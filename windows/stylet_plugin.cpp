#include "stylet_plugin.h"

#include "stylet_wintab.h"

#include <flutter/event_stream_handler_functions.h>
#include <flutter/standard_method_codec.h>

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace stylet {
namespace {

/** Request-response channel shared with the Dart backend. */
constexpr char kMethodChannelName[] = "dev.focale.stylet/methods";

/** Continuous event channel shared with the Dart backend. */
constexpr char kEventChannelName[] = "dev.focale.stylet/events";

/** Flutter button bit representing tip contact. */
constexpr int64_t kFlutterPrimaryButton = 1;

/** Flutter button bit representing the first stylus side button. */
constexpr int64_t kFlutterPrimaryStylusButton = 2;

/** Flutter button bit representing the second stylus side button. */
constexpr int64_t kFlutterSecondaryStylusButton = 4;

/** Number of radians in one degree. */
constexpr double kDegreesToRadians = 3.14159265358979323846 / 180.0;

/** Inserts one value into an encodable map with a string key. */
void SetValue(flutter::EncodableMap* map, const char* key,
              flutter::EncodableValue value) {
  (*map)[flutter::EncodableValue(key)] = std::move(value);
}

/** Maps a Windows pointer message to Stylet's lifecycle vocabulary. */
const char* PhaseForMessage(UINT message, bool is_down) {
  switch (message) {
    case WM_POINTERENTER:
      return "added";
    case WM_POINTERDOWN:
      return "down";
    case WM_POINTERUPDATE:
      return is_down ? "move" : "hover";
    case WM_POINTERUP:
      return "up";
    case WM_POINTERCAPTURECHANGED:
      return "cancel";
    case WM_POINTERLEAVE:
      return "removed";
    default:
      return nullptr;
  }
}

/** Converts Windows pen flags into Flutter's public button bit field. */
int64_t FlutterButtons(const POINTER_PEN_INFO& pen_info, bool is_down) {
  int64_t buttons = is_down ? kFlutterPrimaryButton : 0;
  if ((pen_info.penFlags & PEN_FLAG_BARREL) != 0) {
    buttons |= kFlutterPrimaryStylusButton;
  }
  if ((pen_info.pointerInfo.pointerFlags & POINTER_FLAG_SECONDBUTTON) != 0) {
    buttons |= kFlutterSecondaryStylusButton;
  }
  return buttons;
}

/** Returns a stable textual identifier for a Windows pointer source device. */
std::string NativeDeviceIdentifier(HANDLE source_device) {
  std::ostringstream output;
  output << "windows:" << std::hex
         << reinterpret_cast<std::uintptr_t>(source_device);
  return output.str();
}

/** Converts a null-terminated Windows UTF-16 string into UTF-8. */
std::string WideStringToUtf8(const wchar_t* value) {
  if (value == nullptr || *value == L'\0') {
    return {};
  }
  const int size = WideCharToMultiByte(CP_UTF8, 0, value, -1, nullptr, 0,
                                       nullptr, nullptr);
  if (size <= 1) {
    return {};
  }
  std::string result(static_cast<size_t>(size), '\0');
  WideCharToMultiByte(CP_UTF8, 0, value, -1, result.data(), size, nullptr,
                      nullptr);
  result.pop_back();
  return result;
}

}  // namespace

StyletPlugin::StyletPlugin(flutter::PluginRegistrarWindows* registrar)
    : registrar_(registrar) {
  if (registrar_->GetView() != nullptr) {
    view_window_ = registrar_->GetView()->GetNativeWindow();
  }
  wintab_backend_ = std::make_unique<WintabBackend>(view_window_);
}

StyletPlugin::~StyletPlugin() {
  event_sink_.reset();
  if (registrar_ != nullptr && window_proc_delegate_id_ >= 0) {
    registrar_->UnregisterTopLevelWindowProcDelegate(window_proc_delegate_id_);
  }
}

void StyletPlugin::RegisterWithRegistrar(
    flutter::PluginRegistrarWindows* registrar) {
  auto plugin = std::make_unique<StyletPlugin>(registrar);
  StyletPlugin* plugin_pointer = plugin.get();
  const auto& codec = flutter::StandardMethodCodec::GetInstance();

  plugin->method_channel_ =
      std::make_unique<flutter::MethodChannel<flutter::EncodableValue>>(
          registrar->messenger(), kMethodChannelName, &codec);
  plugin->method_channel_->SetMethodCallHandler(
      [plugin_pointer](const auto& call, auto result) {
        plugin_pointer->HandleMethodCall(call, std::move(result));
      });

  plugin->event_channel_ =
      std::make_unique<flutter::EventChannel<flutter::EncodableValue>>(
          registrar->messenger(), kEventChannelName, &codec);
  auto stream_handler = std::make_unique<
      flutter::StreamHandlerFunctions<flutter::EncodableValue>>(
      [plugin_pointer](
          const flutter::EncodableValue* /*arguments*/,
          std::unique_ptr<flutter::EventSink<flutter::EncodableValue>>&& events)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        plugin_pointer->wintab_backend_->ClearSamples();
        plugin_pointer->event_sink_ = std::move(events);
        return nullptr;
      },
      [plugin_pointer](const flutter::EncodableValue* /*arguments*/)
          -> std::unique_ptr<
              flutter::StreamHandlerError<flutter::EncodableValue>> {
        plugin_pointer->event_sink_.reset();
        plugin_pointer->last_positions_.clear();
        plugin_pointer->announced_devices_.clear();
        plugin_pointer->wintab_backend_->ClearSamples();
        return nullptr;
      });
  plugin->event_channel_->SetStreamHandler(std::move(stream_handler));

  plugin->window_proc_delegate_id_ =
      registrar->RegisterTopLevelWindowProcDelegate(
          [plugin_pointer](HWND window, UINT message, WPARAM wparam,
                           LPARAM lparam) {
            return plugin_pointer->HandleWindowMessage(window, message, wparam,
                                                       lparam);
          });
  registrar->AddPlugin(std::move(plugin));
}

flutter::EncodableList StyletPlugin::GetCapabilities() {
  return {
      flutter::EncodableValue("pressure"),
      flutter::EncodableValue("tilt"),
      flutter::EncodableValue("orientation"),
      flutter::EncodableValue("barrelRotation"),
      flutter::EncodableValue("primaryButton"),
      flutter::EncodableValue("secondaryButton"),
      flutter::EncodableValue("eraser"),
      flutter::EncodableValue("hover"),
      flutter::EncodableValue("historicalSamples"),
      flutter::EncodableValue("deviceInfo"),
  };
}

void StyletPlugin::HandleMethodCall(
    const flutter::MethodCall<flutter::EncodableValue>& method_call,
    std::unique_ptr<flutter::MethodResult<flutter::EncodableValue>> result) {
  if (method_call.method_name() == "getCapabilities") {
    result->Success(flutter::EncodableValue(GetCurrentCapabilities()));
    return;
  }
  result->NotImplemented();
}

std::optional<LRESULT> StyletPlugin::HandleWindowMessage(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  if (wintab_backend_->HandleWindowMessage(message, wparam, lparam)) {
    return std::nullopt;
  }
  const char* phase = PhaseForMessage(message, false);
  if (phase == nullptr || event_sink_ == nullptr) {
    return std::nullopt;
  }

  const UINT32 pointer_id = GET_POINTERID_WPARAM(wparam);
  POINTER_INPUT_TYPE pointer_type = PT_POINTER;
  if (!GetPointerType(pointer_id, &pointer_type) || pointer_type != PT_PEN) {
    return std::nullopt;
  }
  EmitPenEvents(window, message, pointer_id);
  return std::nullopt;
}

std::vector<POINTER_PEN_INFO> StyletPlugin::ReadPenHistory(
    UINT32 pointer_id) const {
  UINT32 count = 0;
  if (GetPointerPenInfoHistory(pointer_id, &count, nullptr) && count > 0) {
    const UINT32 capacity = count;
    std::vector<POINTER_PEN_INFO> history(capacity);
    if (GetPointerPenInfoHistory(pointer_id, &count, history.data())) {
      history.resize(std::min(count, capacity));
      std::reverse(history.begin(), history.end());
      return history;
    }
  }

  POINTER_PEN_INFO current = {};
  if (GetPointerPenInfo(pointer_id, &current)) {
    return {current};
  }
  return {};
}

void StyletPlugin::AnnounceDevice(const POINTER_PEN_INFO& pen_info) {
  if (event_sink_ == nullptr) {
    return;
  }
  const HANDLE source_device = pen_info.pointerInfo.sourceDevice;
  const std::string identifier = NativeDeviceIdentifier(source_device);
  if (!announced_devices_.insert(identifier).second) {
    return;
  }

  POINTER_DEVICE_INFO device_info = {};
  const bool has_device_info = GetPointerDevice(source_device, &device_info);
  flutter::EncodableList features = {
      flutter::EncodableValue("pressure"),
      flutter::EncodableValue("tilt"),
      flutter::EncodableValue("orientation"),
      flutter::EncodableValue("barrelRotation"),
      flutter::EncodableValue("primaryButton"),
      flutter::EncodableValue("secondaryButton"),
      flutter::EncodableValue("eraser"),
      flutter::EncodableValue("hover"),
      flutter::EncodableValue("historicalSamples"),
      flutter::EncodableValue("deviceInfo"),
  };
  if (wintab_backend_->supports_tangential_pressure()) {
    features.emplace_back("tangentialPressure");
  }
  flutter::EncodableMap packet;
  SetValue(&packet, "type", flutter::EncodableValue("device"));
  SetValue(&packet, "timestampMicros",
           flutter::EncodableValue(
               static_cast<int64_t>(pen_info.pointerInfo.dwTime) * 1000));
  SetValue(&packet, "phase", flutter::EncodableValue("added"));
  SetValue(&packet, "kind", flutter::EncodableValue("tablet"));
  SetValue(&packet, "nativeDeviceIdentifier",
           flutter::EncodableValue(identifier));
  if (has_device_info) {
    const std::string product_name =
        WideStringToUtf8(device_info.productString);
    if (!product_name.empty()) {
      SetValue(&packet, "name", flutter::EncodableValue(product_name));
    }
  }
  SetValue(&packet, "features", flutter::EncodableValue(features));
  event_sink_->Success(flutter::EncodableValue(packet));
}

void StyletPlugin::EmitPenEvents(HWND window, UINT message, UINT32 pointer_id) {
  if (event_sink_ == nullptr) {
    return;
  }
  const std::vector<POINTER_PEN_INFO> history = ReadPenHistory(pointer_id);
  if (history.empty()) {
    return;
  }
  AnnounceDevice(history.back());

  flutter::EncodableList packets;
  packets.reserve(history.size());
  for (size_t index = 0; index < history.size(); ++index) {
    const UINT sample_message =
        index + 1 == history.size() ? message : WM_POINTERUPDATE;
    packets.emplace_back(BuildPenPacket(window, sample_message, pointer_id,
                                        history[index]));
  }
  event_sink_->Success(flutter::EncodableValue(packets));
}

flutter::EncodableMap StyletPlugin::BuildPenPacket(
    HWND window, UINT message, UINT32 pointer_id,
    const POINTER_PEN_INFO& pen_info) {
  const bool is_down =
      (pen_info.pointerInfo.pointerFlags & POINTER_FLAG_INCONTACT) != 0;
  const char* phase = PhaseForMessage(message, is_down);
  // Every caller filters unsupported messages before building a packet.
  assert(phase != nullptr);

  POINT position = pen_info.pointerInfo.ptPixelLocation;
  HWND coordinate_window = view_window_ == nullptr ? window : view_window_;
  ScreenToClient(coordinate_window, &position);
  const UINT dpi = GetDpiForWindow(coordinate_window);
  const double scale = dpi == 0 ? 1.0 : static_cast<double>(dpi) / 96.0;
  const double x = static_cast<double>(position.x) / scale;
  const double y = static_cast<double>(position.y) / scale;
  const auto previous = last_positions_.find(pointer_id);

  flutter::EncodableMap packet;
  SetValue(&packet, "type", flutter::EncodableValue("motion"));
  SetValue(&packet, "timestampMicros",
           flutter::EncodableValue(
               static_cast<int64_t>(pen_info.pointerInfo.dwTime) * 1000));
  SetValue(&packet, "phase", flutter::EncodableValue(phase));
  const bool is_eraser =
      (pen_info.penFlags & (PEN_FLAG_ERASER | PEN_FLAG_INVERTED)) != 0;
  SetValue(&packet, "tool",
           flutter::EncodableValue(is_eraser ? "eraser" : "pen"));
  SetValue(&packet, "pointerIdentifier",
           flutter::EncodableValue(static_cast<int64_t>(pointer_id)));
  SetValue(&packet, "deviceIdentifier",
           flutter::EncodableValue(static_cast<int64_t>(
               reinterpret_cast<std::intptr_t>(pen_info.pointerInfo.sourceDevice))));
  SetValue(&packet, "nativeDeviceIdentifier",
           flutter::EncodableValue(
               NativeDeviceIdentifier(pen_info.pointerInfo.sourceDevice)));
  SetValue(&packet, "x", flutter::EncodableValue(x));
  SetValue(&packet, "y", flutter::EncodableValue(y));
  SetValue(&packet, "deltaX",
           flutter::EncodableValue(previous == last_positions_.end()
                                        ? 0.0
                                        : x - previous->second.first));
  SetValue(&packet, "deltaY",
           flutter::EncodableValue(previous == last_positions_.end()
                                        ? 0.0
                                        : y - previous->second.second));
  SetValue(&packet, "buttons",
           flutter::EncodableValue(FlutterButtons(pen_info, is_down)));
  SetValue(&packet, "isDown", flutter::EncodableValue(is_down));

  flutter::EncodableList features = {
      flutter::EncodableValue("primaryButton"),
      flutter::EncodableValue("secondaryButton"),
      flutter::EncodableValue("eraser"),
      flutter::EncodableValue("hover"),
  };
  if ((pen_info.penMask & PEN_MASK_PRESSURE) != 0) {
    SetValue(&packet, "pressure",
             flutter::EncodableValue(static_cast<double>(pen_info.pressure)));
    SetValue(&packet, "pressureMinimum", flutter::EncodableValue(0.0));
    SetValue(&packet, "pressureMaximum", flutter::EncodableValue(1024.0));
    features.emplace_back("pressure");
  }

  const bool has_tilt_x = (pen_info.penMask & PEN_MASK_TILT_X) != 0;
  const bool has_tilt_y = (pen_info.penMask & PEN_MASK_TILT_Y) != 0;
  if (has_tilt_x || has_tilt_y) {
    const double tilt_x =
        static_cast<double>(pen_info.tiltX) * kDegreesToRadians;
    const double tilt_y =
        static_cast<double>(pen_info.tiltY) * kDegreesToRadians;
    SetValue(&packet, "tiltX", flutter::EncodableValue(tilt_x));
    SetValue(&packet, "tiltY", flutter::EncodableValue(tilt_y));
    SetValue(&packet, "tilt",
             flutter::EncodableValue(std::atan(std::hypot(
                 std::tan(tilt_x), std::tan(tilt_y)))));
    SetValue(&packet, "orientation",
             flutter::EncodableValue(std::atan2(std::tan(tilt_x),
                                                -std::tan(tilt_y))));
    features.emplace_back("tilt");
    features.emplace_back("orientation");
  }

  if ((pen_info.penMask & PEN_MASK_ROTATION) != 0) {
    SetValue(&packet, "barrelRotation",
             flutter::EncodableValue(static_cast<double>(pen_info.rotation) *
                                     kDegreesToRadians));
    features.emplace_back("barrelRotation");
  }
  wintab_backend_->EnrichPacket(pen_info.pointerInfo.dwTime, &packet,
                                &features);
  SetValue(&packet, "features", flutter::EncodableValue(features));

  if (message == WM_POINTERUP || message == WM_POINTERLEAVE ||
      message == WM_POINTERCAPTURECHANGED) {
    last_positions_.erase(pointer_id);
  } else {
    last_positions_[pointer_id] = std::make_pair(x, y);
  }
  return packet;
}

flutter::EncodableList StyletPlugin::GetCurrentCapabilities() const {
  flutter::EncodableList capabilities = GetCapabilities();
  if (wintab_backend_->supports_tangential_pressure()) {
    capabilities.emplace_back("tangentialPressure");
  }
  return capabilities;
}

}  // namespace stylet
