#include "stylet_wintab.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace stylet {
namespace {

/** Private Wintab declarations needed for runtime dynamic linking. */
namespace wintab {

struct ContextTag;
using Context = ContextTag*;
using PacketMask = DWORD;
using Fixed = DWORD;

constexpr UINT kMessageBase = 0x7FF0;
constexpr UINT kPacketMessage = kMessageBase;
constexpr UINT kPacketExtensionMessage = kMessageBase + 8;
constexpr UINT kDefaultSystemContext = 4;
constexpr UINT kDevices = 100;
constexpr UINT kExtensions = 300;
constexpr UINT kTangentialPressure = 16;
constexpr UINT kExtensionTag = 2;
constexpr UINT kExtensionMask = 3;
constexpr UINT kTouchStrip = 6;
constexpr UINT kTouchRing = 7;
constexpr UINT kExpressKeys = 8;
constexpr WORD kControlCount = 0;
constexpr WORD kFunctionCount = 1;
constexpr WORD kAvailable = 2;
constexpr WORD kMinimum = 3;
constexpr WORD kMaximum = 4;
constexpr WORD kOverride = 5;
constexpr UINT kMessages = 0x0004;
constexpr PacketMask kContext = 0x0001;
constexpr PacketMask kStatus = 0x0002;
constexpr PacketMask kTime = 0x0004;
constexpr PacketMask kSerialNumber = 0x0010;
constexpr PacketMask kCursor = 0x0020;
constexpr PacketMask kTangentialPressureData = 0x0800;
constexpr PacketMask kPenPacketData =
    kContext | kTime | kCursor | kTangentialPressureData;
constexpr size_t kContextNameLength = 40;
constexpr int kPacketBatchSize = 128;
constexpr int64_t kMaximumMatchAgeMillis = 24;
constexpr uint8_t kMaximumTabletCount = 16;

struct Axis {
  LONG minimum;
  LONG maximum;
  UINT units;
  Fixed resolution;
};

/** ANSI Wintab context layout used by WTInfoA and WTOpenA. */
struct LogContext {
  char name[kContextNameLength];
  UINT options;
  UINT status;
  UINT locks;
  UINT message_base;
  UINT device;
  UINT packet_rate;
  PacketMask packet_data;
  PacketMask packet_mode;
  PacketMask move_mask;
  DWORD button_down_mask;
  DWORD button_up_mask;
  LONG input_origin_x;
  LONG input_origin_y;
  LONG input_origin_z;
  LONG input_extent_x;
  LONG input_extent_y;
  LONG input_extent_z;
  LONG output_origin_x;
  LONG output_origin_y;
  LONG output_origin_z;
  LONG output_extent_x;
  LONG output_extent_y;
  LONG output_extent_z;
  Fixed sensitivity_x;
  Fixed sensitivity_y;
  Fixed sensitivity_z;
  BOOL system_mode;
  int system_origin_x;
  int system_origin_y;
  int system_extent_x;
  int system_extent_y;
  Fixed system_sensitivity_x;
  Fixed system_sensitivity_y;
};

struct PenPacket {
  Context context;
  DWORD time;
  UINT cursor;
  UINT tangential_pressure;
};

struct ExtensionBase {
  Context context;
  UINT status;
  DWORD time;
  UINT serial_number;
};

struct ExpressKeyData {
  BYTE tablet;
  BYTE control;
  BYTE location;
  BYTE reserved;
  DWORD state;
};

struct SliderData {
  BYTE tablet;
  BYTE control;
  BYTE mode;
  BYTE reserved;
  DWORD position;
};

struct ExtensionProperty {
  BYTE version;
  BYTE tablet_index;
  BYTE control_index;
  BYTE function_index;
  WORD property_id;
  WORD reserved;
  DWORD data_size;
  BYTE data[1];
};

static_assert(sizeof(Axis) == 16, "Unexpected Wintab AXIS layout.");
static_assert(sizeof(LogContext) == 172,
              "Unexpected Wintab LOGCONTEXTA layout.");
static_assert(sizeof(PenPacket) == (sizeof(void*) == 8 ? 24 : 16),
              "Unexpected Wintab PACKET layout.");
static_assert(sizeof(ExtensionBase) == (sizeof(void*) == 8 ? 24 : 16),
              "Unexpected Wintab extension packet header layout.");
static_assert(sizeof(ExpressKeyData) == 8,
              "Unexpected Wintab EXPKEYSDATA layout.");
static_assert(sizeof(SliderData) == 8,
              "Unexpected Wintab SLIDERDATA layout.");

using InfoFunction = UINT(WINAPI*)(UINT, UINT, LPVOID);
using OpenFunction = Context(WINAPI*)(HWND, LogContext*, BOOL);
using CloseFunction = BOOL(WINAPI*)(Context);
using EnableFunction = BOOL(WINAPI*)(Context, BOOL);
using PacketFunction = BOOL(WINAPI*)(Context, UINT, LPVOID);
using PacketsGetFunction = int(WINAPI*)(Context, int, LPVOID);
using ExtensionGetFunction = BOOL(WINAPI*)(Context, UINT, LPVOID);
using ExtensionSetFunction = BOOL(WINAPI*)(Context, UINT, LPVOID);

}  // namespace wintab

void SetValue(flutter::EncodableMap* map, const char* key,
              flutter::EncodableValue value) {
  (*map)[flutter::EncodableValue(key)] = std::move(value);
}

int64_t TimestampDifference(DWORD left, DWORD right) {
  return static_cast<int64_t>(static_cast<int32_t>(left - right));
}

uint32_t ControlKey(BYTE tablet, BYTE control) {
  return (static_cast<uint32_t>(tablet) << 8) | control;
}

}  // namespace

/** Owns dynamically resolved Wintab functions, contexts, and control state. */
class WintabBackend::Implementation {
 private:
  struct ControlFunction {
    BYTE tablet;
    BYTE control;
    BYTE function;
    DWORD minimum;
    DWORD maximum;
    bool overridden = false;
  };

  struct ExtensionState {
    explicit ExtensionState(UINT extension_tag) : tag(extension_tag) {}

    // WTExtGet/WTExtSet take this stable WTX_* tag. WTInfo separately uses
    // the driver-specific array index discovered for it.
    UINT tag;
    wintab::PacketMask mask = 0;
    std::vector<ControlFunction> functions;
    std::unordered_map<BYTE, DWORD> control_counts;

    bool is_available() const { return !functions.empty(); }
  };

  struct SliderState {
    bool initialized = false;
    bool active = false;
    DWORD position = 0;
    BYTE mode = 0;
  };

  HMODULE module_ = nullptr;
  wintab::Context pen_context_ = nullptr;
  wintab::Context control_context_ = nullptr;
  wintab::InfoFunction info_ = nullptr;
  wintab::OpenFunction open_ = nullptr;
  wintab::CloseFunction close_ = nullptr;
  wintab::EnableFunction enable_ = nullptr;
  wintab::PacketFunction packet_ = nullptr;
  wintab::PacketsGetFunction packets_get_ = nullptr;
  wintab::ExtensionGetFunction extension_get_ = nullptr;
  wintab::ExtensionSetFunction extension_set_ = nullptr;
  wintab::Axis tangential_pressure_axis_ = {};
  std::deque<wintab::PenPacket> samples_;
  EventCallback event_callback_;
  bool listening_ = false;
  bool overrides_enabled_ = false;
  ExtensionState express_keys_{wintab::kExpressKeys};
  ExtensionState touch_strips_{wintab::kTouchStrip};
  ExtensionState touch_rings_{wintab::kTouchRing};
  std::unordered_map<uint32_t, DWORD> button_states_;
  std::unordered_map<uint32_t, SliderState> strip_states_;
  std::unordered_map<uint32_t, SliderState> ring_states_;
  std::unordered_set<BYTE> announced_tablets_;

 public:
  Implementation(HWND window, EventCallback event_callback)
      : event_callback_(std::move(event_callback)) {
    Initialize(window);
  }

  ~Implementation() {
    SetTabletPadOverrideEnabled(false);
    if (control_context_ != nullptr && close_ != nullptr) {
      close_(control_context_);
    }
    if (pen_context_ != nullptr && close_ != nullptr) {
      close_(pen_context_);
    }
    if (module_ != nullptr) {
      FreeLibrary(module_);
    }
  }

  bool supports_tangential_pressure() const { return pen_context_ != nullptr; }
  bool supports_pad_buttons() const { return express_keys_.is_available(); }
  bool supports_pad_rings() const { return touch_rings_.is_available(); }
  bool supports_pad_strips() const { return touch_strips_.is_available(); }

  void SetListening(bool listening) {
    listening_ = listening;
    announced_tablets_.clear();
    if (listening_ && overrides_enabled_) {
      AnnounceAllPads("added");
    }
  }

  bool SetTabletPadOverrideEnabled(bool enabled) {
    if (control_context_ == nullptr || extension_set_ == nullptr ||
        enable_ == nullptr) {
      return false;
    }
    if (enabled == overrides_enabled_) {
      return enabled && HasAnyOverridableControl();
    }
    if (!enabled) {
      if (listening_) {
        AnnounceAllPads("removed");
      }
      SetEveryOverride(false);
      enable_(control_context_, FALSE);
      overrides_enabled_ = false;
      announced_tablets_.clear();
      button_states_.clear();
      strip_states_.clear();
      ring_states_.clear();
      return true;
    }

    if (!enable_(control_context_, TRUE)) {
      return false;
    }
    RefreshControls();
    if (!HasAnyOverridableControl()) {
      enable_(control_context_, FALSE);
      return false;
    }
    if (!SetEveryOverride(true)) {
      enable_(control_context_, FALSE);
      return false;
    }
    overrides_enabled_ = true;
    if (listening_) {
      AnnounceAllPads("added");
    }
    return true;
  }

  bool HandleWindowMessage(UINT message, WPARAM wparam, LPARAM lparam) {
    const auto context = reinterpret_cast<wintab::Context>(lparam);
    if (message == wintab::kPacketMessage && context == pen_context_) {
      DrainPenPackets();
      return true;
    }
    if (message == wintab::kPacketMessage && context == control_context_) {
      // The same serial is also delivered through WT_PACKETEXT. Leave it in
      // the queue so that callback can retrieve the extension payload.
      return true;
    }
    if (message != wintab::kPacketExtensionMessage ||
        context != control_context_ || packet_ == nullptr) {
      return false;
    }
    std::array<BYTE, sizeof(wintab::ExtensionBase) +
                         sizeof(wintab::ExpressKeyData) +
                         sizeof(wintab::SliderData) * 2>
        packet = {};
    if (packet_(control_context_, static_cast<UINT>(wparam), packet.data())) {
      ProcessExtensionPacket(packet.data());
    }
    return true;
  }

  void EnrichPacket(DWORD timestamp_millis, flutter::EncodableMap* packet,
                    flutter::EncodableList* features) {
    DrainPenPackets();
    size_t closest_index = samples_.size();
    int64_t closest_age = std::numeric_limits<int64_t>::max();
    for (size_t index = 0; index < samples_.size(); ++index) {
      const int64_t age =
          std::abs(TimestampDifference(samples_[index].time, timestamp_millis));
      if (age < closest_age) {
        closest_index = index;
        closest_age = age;
      }
    }
    if (closest_index == samples_.size() ||
        closest_age > wintab::kMaximumMatchAgeMillis) {
      return;
    }
    const UINT pressure = samples_[closest_index].tangential_pressure;
    for (size_t index = 0; index <= closest_index; ++index) {
      samples_.pop_front();
    }
    const double minimum = tangential_pressure_axis_.minimum;
    const double extent =
        static_cast<double>(tangential_pressure_axis_.maximum) - minimum;
    if (extent <= 0) {
      return;
    }
    const double normalized = std::clamp(
        ((static_cast<double>(pressure) - minimum) / extent) * 2.0 - 1.0,
        -1.0, 1.0);
    SetValue(packet, "tangentialPressure",
             flutter::EncodableValue(normalized));
    features->emplace_back("tangentialPressure");
  }

  void ClearSamples() {
    DrainPenPackets();
    samples_.clear();
  }

 private:
  bool HasAnyOverridableControl() const {
    return supports_pad_buttons() || supports_pad_rings() ||
           supports_pad_strips();
  }

  void DrainPenPackets() {
    if (pen_context_ == nullptr || packets_get_ == nullptr) {
      return;
    }
    wintab::PenPacket packets[wintab::kPacketBatchSize] = {};
    const int count =
        packets_get_(pen_context_, wintab::kPacketBatchSize, packets);
    if (count <= 0) {
      return;
    }
    samples_.insert(samples_.end(), packets, packets + count);
    while (samples_.size() > wintab::kPacketBatchSize) {
      samples_.pop_front();
    }
  }

  bool DiscoverExtension(ExtensionState* extension) {
    for (UINT index = 0;; ++index) {
      UINT tag = 0;
      if (info_(wintab::kExtensions + index, wintab::kExtensionTag, &tag) ==
          0) {
        return false;
      }
      if (tag != extension->tag) {
        continue;
      }
      return info_(wintab::kExtensions + index, wintab::kExtensionMask,
                   &extension->mask) ==
                 static_cast<UINT>(sizeof(extension->mask)) &&
             extension->mask != 0;
    }
  }

  template <typename T>
  bool GetControlProperty(UINT extension_tag, BYTE tablet, BYTE control,
                          BYTE function, WORD property, T* value) const {
    if (control_context_ == nullptr || extension_get_ == nullptr ||
        value == nullptr) {
      return false;
    }
    std::vector<BYTE> buffer(sizeof(wintab::ExtensionProperty) + sizeof(T), 0);
    auto* request =
        reinterpret_cast<wintab::ExtensionProperty*>(buffer.data());
    request->version = 0;
    request->tablet_index = tablet;
    request->control_index = control;
    request->function_index = function;
    request->property_id = property;
    request->data_size = static_cast<DWORD>(sizeof(T));
    if (!extension_get_(control_context_, extension_tag, request) ||
        request->data_size < sizeof(T)) {
      return false;
    }
    std::memcpy(value, request->data, sizeof(T));
    return true;
  }

  template <typename T>
  bool SetControlProperty(UINT extension_tag, BYTE tablet, BYTE control,
                          BYTE function, WORD property, const T& value) const {
    if (control_context_ == nullptr || extension_set_ == nullptr) {
      return false;
    }
    std::vector<BYTE> buffer(sizeof(wintab::ExtensionProperty) + sizeof(T), 0);
    auto* request =
        reinterpret_cast<wintab::ExtensionProperty*>(buffer.data());
    request->version = 0;
    request->tablet_index = tablet;
    request->control_index = control;
    request->function_index = function;
    request->property_id = property;
    request->data_size = static_cast<DWORD>(sizeof(T));
    std::memcpy(request->data, &value, sizeof(T));
    return extension_set_(control_context_, extension_tag, request) != FALSE;
  }

  void DiscoverControls(ExtensionState* extension) {
    if (extension->mask == 0) {
      return;
    }
    for (BYTE tablet = 0; tablet < wintab::kMaximumTabletCount; ++tablet) {
      DWORD control_count = 0;
      if (!GetControlProperty(extension->tag, tablet, 0, 0,
                              wintab::kControlCount, &control_count) ||
          control_count == 0) {
        continue;
      }
      control_count = std::min<DWORD>(control_count, 256);
      extension->control_counts[tablet] = control_count;
      for (DWORD control = 0; control < control_count; ++control) {
        DWORD function_count = 0;
        if (!GetControlProperty(extension->tag, tablet,
                                static_cast<BYTE>(control), 0,
                                wintab::kFunctionCount, &function_count)) {
          continue;
        }
        function_count = std::min<DWORD>(function_count, 256);
        for (DWORD function = 0; function < function_count; ++function) {
          BOOL available = FALSE;
          if (!GetControlProperty(extension->tag, tablet,
                                  static_cast<BYTE>(control),
                                  static_cast<BYTE>(function),
                                  wintab::kAvailable, &available) ||
              !available) {
            continue;
          }
          DWORD minimum = 0;
          DWORD maximum = 1;
          GetControlProperty(extension->tag, tablet,
                             static_cast<BYTE>(control),
                             static_cast<BYTE>(function), wintab::kMinimum,
                             &minimum);
          GetControlProperty(extension->tag, tablet,
                             static_cast<BYTE>(control),
                             static_cast<BYTE>(function), wintab::kMaximum,
                             &maximum);
          extension->functions.push_back(ControlFunction{
              tablet, static_cast<BYTE>(control),
              static_cast<BYTE>(function), minimum, maximum});
        }
      }
    }
  }

  void RefreshControls() {
    express_keys_.functions.clear();
    express_keys_.control_counts.clear();
    touch_strips_.functions.clear();
    touch_strips_.control_counts.clear();
    touch_rings_.functions.clear();
    touch_rings_.control_counts.clear();
    DiscoverControls(&express_keys_);
    DiscoverControls(&touch_strips_);
    DiscoverControls(&touch_rings_);
  }

  bool SetOverrides(ExtensionState* extension, bool enabled) {
    bool changed = false;
    const BOOL value = enabled ? TRUE : FALSE;
    for (ControlFunction& function : extension->functions) {
      if (!enabled && !function.overridden) {
        continue;
      }
      if (SetControlProperty(extension->tag, function.tablet,
                             function.control, function.function,
                             wintab::kOverride, value)) {
        function.overridden = enabled;
        changed = true;
      }
    }
    return changed;
  }

  bool SetEveryOverride(bool enabled) {
    const bool buttons = SetOverrides(&express_keys_, enabled);
    const bool strips = SetOverrides(&touch_strips_, enabled);
    const bool rings = SetOverrides(&touch_rings_, enabled);
    return buttons || strips || rings;
  }

  bool TabletHasActiveControls(const ExtensionState& extension,
                               BYTE tablet) const {
    return std::any_of(
        extension.functions.begin(), extension.functions.end(),
        [tablet](const ControlFunction& function) {
          return function.tablet == tablet && function.overridden;
        });
  }

  void AnnounceAllPads(const char* phase) {
    std::unordered_set<BYTE> tablets;
    for (const ControlFunction& function : express_keys_.functions) {
      if (function.overridden) {
        tablets.insert(function.tablet);
      }
    }
    for (const ControlFunction& function : touch_strips_.functions) {
      if (function.overridden) {
        tablets.insert(function.tablet);
      }
    }
    for (const ControlFunction& function : touch_rings_.functions) {
      if (function.overridden) {
        tablets.insert(function.tablet);
      }
    }
    for (BYTE tablet : tablets) {
      EmitPadDevice(tablet, phase);
    }
  }

  void AnnouncePadIfNeeded(BYTE tablet) {
    if (announced_tablets_.insert(tablet).second) {
      EmitPadDevice(tablet, "added");
    }
  }

  void EmitPadDevice(BYTE tablet, const char* phase) {
    if (!listening_ || event_callback_ == nullptr) {
      return;
    }
    if (std::strcmp(phase, "added") == 0) {
      announced_tablets_.insert(tablet);
    } else if (std::strcmp(phase, "removed") == 0 &&
               announced_tablets_.erase(tablet) == 0) {
      return;
    }
    flutter::EncodableList features = {
        flutter::EncodableValue("deviceInfo")};
    if (TabletHasActiveControls(express_keys_, tablet)) {
      features.emplace_back("tabletPadButtons");
    }
    if (TabletHasActiveControls(touch_rings_, tablet)) {
      features.emplace_back("tabletPadRing");
    }
    if (TabletHasActiveControls(touch_strips_, tablet)) {
      features.emplace_back("tabletPadStrip");
    }
    flutter::EncodableMap packet;
    SetValue(&packet, "type", flutter::EncodableValue("device"));
    SetValue(&packet, "timestampMicros",
             flutter::EncodableValue(
                 static_cast<int64_t>(GetTickCount64()) * 1000));
    SetValue(&packet, "phase", flutter::EncodableValue(phase));
    SetValue(&packet, "kind", flutter::EncodableValue("pad"));
    SetValue(&packet, "nativeDeviceIdentifier",
             flutter::EncodableValue("wintab-pad:" +
                                      std::to_string(tablet)));
    SetValue(&packet, "name",
             flutter::EncodableValue("Wintab tablet controls"));
    const auto buttons = express_keys_.control_counts.find(tablet);
    if (buttons != express_keys_.control_counts.end()) {
      SetValue(&packet, "buttonCount",
               flutter::EncodableValue(
                   static_cast<int64_t>(buttons->second)));
    }
    SetValue(&packet, "features", flutter::EncodableValue(features));
    event_callback_(std::move(packet));
  }

  void EmitPadEvent(BYTE tablet, const char* control, BYTE control_index,
                    const char* phase, const double* value,
                    const BYTE* mode, DWORD timestamp) {
    if (!listening_ || !overrides_enabled_ || event_callback_ == nullptr) {
      return;
    }
    AnnouncePadIfNeeded(tablet);
    flutter::EncodableMap packet;
    SetValue(&packet, "type", flutter::EncodableValue("pad"));
    SetValue(&packet, "timestampMicros",
             flutter::EncodableValue(static_cast<int64_t>(timestamp) * 1000));
    SetValue(&packet, "nativeDeviceIdentifier",
             flutter::EncodableValue("wintab-pad:" +
                                      std::to_string(tablet)));
    SetValue(&packet, "control", flutter::EncodableValue(control));
    SetValue(&packet, "controlIndex",
             flutter::EncodableValue(static_cast<int64_t>(control_index)));
    SetValue(&packet, "phase", flutter::EncodableValue(phase));
    if (value != nullptr) {
      SetValue(&packet, "value", flutter::EncodableValue(*value));
    }
    if (mode != nullptr) {
      SetValue(&packet, "mode",
               flutter::EncodableValue(static_cast<int64_t>(*mode)));
    }
    event_callback_(std::move(packet));
  }

  const ControlFunction* FindFunction(const ExtensionState& extension,
                                      BYTE tablet, BYTE control,
                                      BYTE function) const {
    const auto result = std::find_if(
        extension.functions.begin(), extension.functions.end(),
        [tablet, control, function](const ControlFunction& candidate) {
          return candidate.tablet == tablet && candidate.control == control &&
                 candidate.function == function;
        });
    return result == extension.functions.end() ? nullptr : &*result;
  }

  double NormalizeSlider(const ExtensionState& extension,
                         const wintab::SliderData& data) const {
    const ControlFunction* function =
        FindFunction(extension, data.tablet, data.control, data.mode);
    if (function == nullptr || function->maximum <= function->minimum) {
      return 0;
    }
    return std::clamp(
        (static_cast<double>(data.position) - function->minimum) /
            (static_cast<double>(function->maximum) - function->minimum),
        0.0, 1.0);
  }

  void ProcessButton(const wintab::ExpressKeyData& data, DWORD timestamp) {
    if (!TabletHasActiveControls(express_keys_, data.tablet)) {
      return;
    }
    const uint32_t key = ControlKey(data.tablet, data.control);
    const auto previous = button_states_.find(key);
    const bool pressed = data.state != 0;
    if (previous == button_states_.end()) {
      button_states_[key] = data.state;
      if (!pressed) {
        return;
      }
    } else {
      const bool was_pressed = previous->second != 0;
      previous->second = data.state;
      if (was_pressed == pressed) {
        return;
      }
    }
    EmitPadEvent(data.tablet, "button", data.control,
                 pressed ? "began" : "ended", nullptr, nullptr, timestamp);
  }

  void ProcessSlider(const ExtensionState& extension,
                     const wintab::SliderData& data, const char* control,
                     std::unordered_map<uint32_t, SliderState>* states,
                     DWORD timestamp) {
    if (!TabletHasActiveControls(extension, data.tablet)) {
      return;
    }
    const uint32_t key = ControlKey(data.tablet, data.control);
    SliderState& state = (*states)[key];
    const bool active = static_cast<int32_t>(data.position) >= 0;
    if (!state.initialized) {
      state.initialized = true;
      state.active = active;
      state.position = data.position;
      state.mode = data.mode;
      if (!active) {
        return;
      }
      const double value = NormalizeSlider(extension, data);
      EmitPadEvent(data.tablet, control, data.control, "began", &value,
                   &data.mode, timestamp);
      return;
    }
    if (state.mode != data.mode) {
      EmitPadEvent(data.tablet, "mode", data.control, "discrete", nullptr,
                   &data.mode, timestamp);
    }
    if (!active) {
      if (state.active) {
        EmitPadEvent(data.tablet, control, data.control, "ended", nullptr,
                     &data.mode, timestamp);
      }
    } else if (!state.active || state.position != data.position ||
               state.mode != data.mode) {
      const double value = NormalizeSlider(extension, data);
      EmitPadEvent(data.tablet, control, data.control,
                   state.active ? "changed" : "began", &value, &data.mode,
                   timestamp);
    }
    state.active = active;
    state.position = data.position;
    state.mode = data.mode;
  }

  void ProcessExtensionPacket(const BYTE* bytes) {
    if (!overrides_enabled_) {
      return;
    }
    wintab::ExtensionBase base = {};
    std::memcpy(&base, bytes, sizeof(base));
    size_t offset = sizeof(base);
    const DWORD timestamp = base.time == 0
                                ? static_cast<DWORD>(GetTickCount64())
                                : base.time;
    if (express_keys_.mask != 0) {
      wintab::ExpressKeyData data = {};
      std::memcpy(&data, bytes + offset, sizeof(data));
      offset += sizeof(data);
      ProcessButton(data, timestamp);
    }
    if (touch_strips_.mask != 0) {
      wintab::SliderData data = {};
      std::memcpy(&data, bytes + offset, sizeof(data));
      offset += sizeof(data);
      ProcessSlider(touch_strips_, data, "strip", &strip_states_, timestamp);
    }
    if (touch_rings_.mask != 0) {
      wintab::SliderData data = {};
      std::memcpy(&data, bytes + offset, sizeof(data));
      ProcessSlider(touch_rings_, data, "ring", &ring_states_, timestamp);
    }
  }

  void LoadFunctions() {
    info_ = reinterpret_cast<wintab::InfoFunction>(
        GetProcAddress(module_, "WTInfoA"));
    open_ = reinterpret_cast<wintab::OpenFunction>(
        GetProcAddress(module_, "WTOpenA"));
    close_ = reinterpret_cast<wintab::CloseFunction>(
        GetProcAddress(module_, "WTClose"));
    enable_ = reinterpret_cast<wintab::EnableFunction>(
        GetProcAddress(module_, "WTEnable"));
    packet_ = reinterpret_cast<wintab::PacketFunction>(
        GetProcAddress(module_, "WTPacket"));
    packets_get_ = reinterpret_cast<wintab::PacketsGetFunction>(
        GetProcAddress(module_, "WTPacketsGet"));
    extension_get_ = reinterpret_cast<wintab::ExtensionGetFunction>(
        GetProcAddress(module_, "WTExtGet"));
    extension_set_ = reinterpret_cast<wintab::ExtensionSetFunction>(
        GetProcAddress(module_, "WTExtSet"));
  }

  void Initialize(HWND window) {
    if (window == nullptr) {
      return;
    }
    module_ = LoadLibraryExW(L"Wintab32.dll", nullptr,
                             LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (module_ == nullptr) {
      return;
    }
    LoadFunctions();
    if (info_ == nullptr || open_ == nullptr || close_ == nullptr ||
        packet_ == nullptr || packets_get_ == nullptr ||
        info_(0, 0, nullptr) == 0) {
      return;
    }

    wintab::LogContext base_context = {};
    if (info_(wintab::kDefaultSystemContext, 0, &base_context) !=
        static_cast<UINT>(sizeof(base_context))) {
      return;
    }

    const UINT device = base_context.device == std::numeric_limits<UINT>::max()
                            ? 0
                            : base_context.device;
    if (info_(wintab::kDevices + device, wintab::kTangentialPressure,
              &tangential_pressure_axis_) ==
            static_cast<UINT>(sizeof(tangential_pressure_axis_)) &&
        tangential_pressure_axis_.maximum >
            tangential_pressure_axis_.minimum) {
      wintab::LogContext pen_context = base_context;
      pen_context.options |= wintab::kMessages;
      pen_context.message_base = wintab::kMessageBase;
      pen_context.packet_data = wintab::kPenPacketData;
      pen_context.packet_mode = 0;
      pen_context.move_mask = wintab::kPenPacketData;
      pen_context_ = open_(window, &pen_context, TRUE);
    }

    if (enable_ == nullptr || extension_get_ == nullptr ||
        extension_set_ == nullptr) {
      return;
    }
    DiscoverExtension(&express_keys_);
    DiscoverExtension(&touch_strips_);
    DiscoverExtension(&touch_rings_);
    const wintab::PacketMask extension_mask =
        express_keys_.mask | touch_strips_.mask | touch_rings_.mask;
    if (extension_mask == 0) {
      return;
    }
    wintab::LogContext control_context = base_context;
    control_context.options |= wintab::kMessages;
    control_context.message_base = wintab::kMessageBase;
    // Keep this mask in the same order as ExtensionBase followed by the
    // extension payloads parsed in ProcessExtensionPacket.
    control_context.packet_data =
        wintab::kContext | wintab::kStatus | wintab::kTime |
        wintab::kSerialNumber | extension_mask;
    control_context.packet_mode = 0;
    control_context.move_mask = control_context.packet_data;
    control_context_ = open_(window, &control_context, FALSE);
    if (control_context_ == nullptr) {
      return;
    }
    if (!enable_(control_context_, TRUE)) {
      close_(control_context_);
      control_context_ = nullptr;
      return;
    }
    RefreshControls();
    enable_(control_context_, FALSE);
  }
};

WintabBackend::WintabBackend(HWND window, EventCallback event_callback)
    : implementation_(
          std::make_unique<Implementation>(window, std::move(event_callback))) {}

WintabBackend::~WintabBackend() = default;

bool WintabBackend::supports_tangential_pressure() const {
  return implementation_->supports_tangential_pressure();
}

bool WintabBackend::supports_pad_buttons() const {
  return implementation_->supports_pad_buttons();
}

bool WintabBackend::supports_pad_rings() const {
  return implementation_->supports_pad_rings();
}

bool WintabBackend::supports_pad_strips() const {
  return implementation_->supports_pad_strips();
}

bool WintabBackend::SetTabletPadOverrideEnabled(bool enabled) {
  return implementation_->SetTabletPadOverrideEnabled(enabled);
}

void WintabBackend::SetListening(bool listening) {
  implementation_->SetListening(listening);
}

bool WintabBackend::HandleWindowMessage(UINT message, WPARAM wparam,
                                        LPARAM lparam) {
  return implementation_->HandleWindowMessage(message, wparam, lparam);
}

void WintabBackend::EnrichPacket(DWORD timestamp_millis,
                                 flutter::EncodableMap* packet,
                                 flutter::EncodableList* features) {
  implementation_->EnrichPacket(timestamp_millis, packet, features);
}

void WintabBackend::ClearSamples() { implementation_->ClearSamples(); }

}  // namespace stylet
