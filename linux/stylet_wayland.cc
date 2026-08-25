#include "stylet_wayland.h"

#include <gdk/gdkwayland.h>
#include <linux/input-event-codes.h>
#include <wayland-client.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "generated/tablet-unstable-v2-client-protocol.h"

namespace {

/** Number of microseconds in a Wayland millisecond timestamp. */
constexpr gint64 kMicrosecondsPerMillisecond = 1000;

/** Newest tablet-v2 interface version understood by the bundled protocol. */
constexpr uint32_t kMaximumTabletProtocolVersion = 2;

/** Flutter button bit representing tip contact. */
constexpr gint64 kFlutterPrimaryButton = 1;

/** Flutter button bit representing the first stylus side button. */
constexpr gint64 kFlutterPrimaryStylusButton = 2;

/** Flutter button bit representing the second stylus side button. */
constexpr gint64 kFlutterSecondaryStylusButton = 4;

/** Forward declaration for a tablet description owned by the backend. */
struct TabletState;

/** Forward declaration for a tablet tool owned by the backend. */
struct ToolState;

/** Forward declaration for a tablet pad owned by the backend. */
struct PadState;

/** Forward declaration for a tablet pad group owned by the backend. */
struct PadGroupState;

/** Forward declaration for a tablet pad ring owned by the backend. */
struct RingState;

/** Forward declaration for a tablet pad strip owned by the backend. */
struct StripState;

/** Forward declaration for a tablet pad dial owned by the backend. */
struct DialState;

/** Explicit lifecycle stage accumulated until a Wayland tool frame. */
enum class PendingToolPhase {
  /** No explicit lifecycle change occurred in the frame. */
  none,

  /** The tool entered proximity above the Flutter surface. */
  added,

  /** The tool began touching the tablet surface. */
  down,

  /** The tool stopped touching the tablet surface. */
  up,

  /** The tool left proximity or the Flutter surface. */
  removed,
};

/** Metadata and protocol handle for one Wayland graphics tablet. */
struct TabletState {
  /** Backend that owns this state. */
  StyletWaylandBackend* backend;

  /** Wayland tablet protocol object. */
  zwp_tablet_v2* proxy;

  /** Human-readable compositor-provided tablet name. */
  std::string name;

  /** Compositor-provided local path used as the preferred identifier. */
  std::string path;

  /** USB vendor identifier, or zero when unavailable. */
  uint32_t vendor_id = 0;

  /** USB product identifier, or zero when unavailable. */
  uint32_t product_id = 0;

  /** Whether an added device packet has been emitted. */
  bool announced = false;
};

/** Accumulated per-tool axes and lifecycle state. */
struct ToolState {
  /** Backend that owns this state. */
  StyletWaylandBackend* backend;

  /** Wayland tablet tool protocol object. */
  zwp_tablet_tool_v2* proxy;

  /** Tablet currently used by the tool. */
  TabletState* tablet = nullptr;

  /** Wayland physical tool type. */
  uint32_t type = ZWP_TABLET_TOOL_V2_TYPE_PEN;

  /** Stable hardware serial assembled from the protocol's two words. */
  uint64_t hardware_serial = 0;

  /** Wacom-format hardware identifier assembled from two words. */
  uint64_t hardware_id = 0;

  /** Feature names reported during tool initialization. */
  std::vector<std::string> features;

  /** Whether an added device packet has been emitted. */
  bool announced = false;

  /** Whether the tool is currently focused on the Flutter surface. */
  bool focused = false;

  /** Whether the tool tip is logically touching the tablet. */
  bool is_down = false;

  /** Whether this frame contains data that should be emitted. */
  bool dirty = false;

  /** Explicit lifecycle change accumulated for the current frame. */
  PendingToolPhase pending_phase = PendingToolPhase::none;

  /** Latest surface-local horizontal position. */
  double x = 0;

  /** Latest surface-local vertical position. */
  double y = 0;

  /** Whether the current position has been initialized. */
  bool has_position = false;

  /** Previously emitted horizontal position. */
  double last_x = 0;

  /** Previously emitted vertical position. */
  double last_y = 0;

  /** Whether a prior focused frame can produce a delta. */
  bool has_last_position = false;

  /** Current normalized Wayland pressure. */
  double pressure = 0;

  /** Whether pressure has been reported for the tool. */
  bool has_pressure = false;

  /** Current normalized hover distance. */
  double distance = 0;

  /** Whether distance has been reported for the tool. */
  bool has_distance = false;

  /** Current horizontal tilt component in radians. */
  double tilt_x = 0;

  /** Current vertical tilt component in radians. */
  double tilt_y = 0;

  /** Whether tilt has been reported for the tool. */
  bool has_tilt = false;

  /** Current clockwise barrel rotation in radians. */
  double rotation = 0;

  /** Whether barrel rotation has been reported for the tool. */
  bool has_rotation = false;

  /** Current normalized airbrush slider position. */
  double slider = 0;

  /** Whether an airbrush slider has been reported for the tool. */
  bool has_slider = false;

  /** Relative wheel movement accumulated for the current frame in radians. */
  double wheel_delta = 0;

  /** Flutter-compatible pressed button bit field excluding tip contact. */
  gint64 side_buttons = 0;
};

/** State shared by a pad and its physical controls. */
struct PadState {
  /** Backend that owns this state. */
  StyletWaylandBackend* backend;

  /** Wayland tablet pad protocol object. */
  zwp_tablet_pad_v2* proxy;

  /** Tablet currently associated with the pad. */
  TabletState* tablet = nullptr;

  /** Compositor-provided local path used as the preferred identifier. */
  std::string path;

  /** Number of physical pad buttons. */
  uint32_t button_count = 0;

  /** Number assigned to the next announced ring. */
  uint32_t next_ring_index = 0;

  /** Number assigned to the next announced strip. */
  uint32_t next_strip_index = 0;

  /** Number assigned to the next announced relative dial. */
  uint32_t next_dial_index = 0;

  /** Number assigned to the next announced control group. */
  uint32_t next_group_index = 0;

  /** Control groups indexed by their pad-global button numbers. */
  std::unordered_map<uint32_t, PadGroupState*> button_groups;

  /** Whether the pad currently targets the Flutter surface. */
  bool focused = false;

  /** Whether an added device packet has been emitted. */
  bool announced = false;
};

/** Mapping-mode state for a group of tablet-pad controls. */
struct PadGroupState {
  /** Pad that owns this group. */
  PadState* pad;

  /** Wayland tablet pad group protocol object. */
  zwp_tablet_pad_group_v2* proxy;

  /** Zero-based group index within the pad. */
  uint32_t index;

  /** Current mapping mode. */
  uint32_t mode = 0;

  /** Number of modes exposed by the compositor. */
  uint32_t mode_count = 1;
};

/** Accumulated absolute touch-ring state. */
struct RingState {
  /** Group that owns this ring. */
  PadGroupState* group;

  /** Wayland tablet pad ring protocol object. */
  zwp_tablet_pad_ring_v2* proxy;

  /** Zero-based ring index within the pad. */
  uint32_t index;

  /** Latest normalized clockwise ring position. */
  double value = 0;

  /** Whether a ring interaction is currently active. */
  bool active = false;

  /** Whether an angle or stop event awaits the next frame. */
  bool dirty = false;

  /** Whether the pending frame ends the current interaction. */
  bool stopping = false;
};

/** Accumulated absolute touch-strip state. */
struct StripState {
  /** Group that owns this strip. */
  PadGroupState* group;

  /** Wayland tablet pad strip protocol object. */
  zwp_tablet_pad_strip_v2* proxy;

  /** Zero-based strip index within the pad. */
  uint32_t index;

  /** Latest normalized strip position. */
  double value = 0;

  /** Whether a strip interaction is currently active. */
  bool active = false;

  /** Whether a position or stop event awaits the next frame. */
  bool dirty = false;

  /** Whether the pending frame ends the current interaction. */
  bool stopping = false;
};

/** Relative movement accumulated for one tablet-pad dial frame. */
struct DialState {
  /** Group that owns this dial. */
  PadGroupState* group;

  /** Wayland tablet pad dial protocol object. */
  zwp_tablet_pad_dial_v2* proxy;

  /** Zero-based dial index within the pad. */
  uint32_t index;

  /** Signed movement accumulated in logical detents. */
  double delta = 0;
};

}  // namespace

/** Owns optional tablet-v2 objects and per-device state for one Flutter view. */
struct _StyletWaylandBackend {
  /** Flutter widget whose Wayland surface receives tablet focus. */
  GtkWidget* view;

  /** Event channel used to publish normalized native packets. */
  FlEventChannel* event_channel;

  /** Shared Wayland display borrowed from GDK. */
  wl_display* display = nullptr;

  /** Flutter view surface borrowed from GDK. */
  wl_surface* surface = nullptr;

  /** Registry object used to discover the tablet manager. */
  wl_registry* registry = nullptr;

  /** Registry name of the tablet manager. */
  uint32_t manager_name = 0;

  /** Default Wayland seat borrowed from GDK. */
  wl_seat* seat = nullptr;

  /** Tablet manager proxy owned by this backend. */
  zwp_tablet_manager_v2* manager = nullptr;

  /** Per-seat tablet protocol controller owned by this backend. */
  zwp_tablet_seat_v2* tablet_seat = nullptr;

  /** Whether the backend has successfully bound tablet-v2. */
  bool active = false;

  /** Tablets indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_v2*, std::unique_ptr<TabletState>> tablets;

  /** Tools indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_tool_v2*, std::unique_ptr<ToolState>> tools;

  /** Pads indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_pad_v2*, std::unique_ptr<PadState>> pads;

  /** Pad groups indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_pad_group_v2*, std::unique_ptr<PadGroupState>>
      groups;

  /** Rings indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_pad_ring_v2*, std::unique_ptr<RingState>> rings;

  /** Strips indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_pad_strip_v2*, std::unique_ptr<StripState>>
      strips;

  /** Relative dials indexed by their protocol proxy. */
  std::unordered_map<zwp_tablet_pad_dial_v2*, std::unique_ptr<DialState>> dials;
};

namespace {

/** Adds one floating-point entry to a standard-codec map. */
void set_float(FlValue* map, const gchar* key, double value) {
  fl_value_set_string_take(map, key, fl_value_new_float(value));
}

/** Adds one integer entry to a standard-codec map. */
void set_int(FlValue* map, const gchar* key, gint64 value) {
  fl_value_set_string_take(map, key, fl_value_new_int(value));
}

/** Adds one UTF-8 string entry to a standard-codec map. */
void set_string(FlValue* map, const gchar* key, const std::string& value) {
  fl_value_set_string_take(map, key, fl_value_new_string(value.c_str()));
}

/** Sends one owned packet through the backend's event channel. */
void send_packet(StyletWaylandBackend* backend, FlValue* packet) {
  g_autoptr(FlValue) owned_packet = packet;
  if (backend == nullptr || !backend->active ||
      backend->event_channel == nullptr) {
    return;
  }
  g_autoptr(GError) error = nullptr;
  if (!fl_event_channel_send(backend->event_channel, owned_packet, nullptr,
                             &error)) {
    g_warning("Failed to send a Stylet Wayland event: %s",
              error == nullptr ? "unknown error" : error->message);
  }
}

/** Appends a feature name once to a tool feature vector. */
void append_feature(ToolState* tool, const std::string& name) {
  if (std::find(tool->features.begin(), tool->features.end(), name) ==
      tool->features.end()) {
    tool->features.push_back(name);
  }
}

/** Converts two protocol words into a single unsigned identifier. */
uint64_t combine_words(uint32_t high, uint32_t low) {
  return (static_cast<uint64_t>(high) << 32) | low;
}

/** Returns a deterministic identifier for a Wayland tablet. */
std::string tablet_identifier(const TabletState* tablet) {
  if (!tablet->path.empty()) {
    return "wayland-tablet:" + tablet->path;
  }
  return "wayland-tablet:" + std::to_string(tablet->vendor_id) + ":" +
         std::to_string(tablet->product_id) + ":" +
         std::to_string(wl_proxy_get_id(
             reinterpret_cast<wl_proxy*>(tablet->proxy)));
}

/** Returns a deterministic identifier for a Wayland tablet tool. */
std::string tool_identifier(const ToolState* tool) {
  if (tool->hardware_serial != 0) {
    return "wayland-tool:" + std::to_string(tool->hardware_serial);
  }
  if (tool->hardware_id != 0) {
    return "wayland-tool-id:" + std::to_string(tool->hardware_id) + ":" +
           std::to_string(wl_proxy_get_id(
               reinterpret_cast<wl_proxy*>(tool->proxy)));
  }
  return "wayland-tool:" + std::to_string(wl_proxy_get_id(
                                reinterpret_cast<wl_proxy*>(tool->proxy)));
}

/** Returns a deterministic identifier for a Wayland tablet pad. */
std::string pad_identifier(const PadState* pad) {
  if (!pad->path.empty()) {
    return "wayland-pad:" + pad->path;
  }
  return "wayland-pad:" + std::to_string(wl_proxy_get_id(
                               reinterpret_cast<wl_proxy*>(pad->proxy)));
}

/** Converts a Wayland tool type to Stylet's tool vocabulary. */
const gchar* tool_name(uint32_t type) {
  return type == ZWP_TABLET_TOOL_V2_TYPE_ERASER ? "eraser" : "pen";
}

/** Converts an accumulated lifecycle stage into Stylet's phase vocabulary. */
const gchar* phase_name(const ToolState* tool) {
  switch (tool->pending_phase) {
    case PendingToolPhase::added:
      return "added";
    case PendingToolPhase::down:
      return "down";
    case PendingToolPhase::up:
      return "up";
    case PendingToolPhase::removed:
      return "removed";
    case PendingToolPhase::none:
      return tool->is_down ? "move" : "hover";
  }
  return "hover";
}

/** Builds a standard-codec list from one tool's feature names. */
FlValue* tool_feature_list(const ToolState* tool) {
  FlValue* features = fl_value_new_list();
  for (const std::string& feature : tool->features) {
    fl_value_append_take(features, fl_value_new_string(feature.c_str()));
  }
  return features;
}

/** Emits a tablet, tool, or pad description packet. */
void send_device_packet(StyletWaylandBackend* backend, const gchar* phase,
                        const gchar* kind, const std::string& identifier,
                        const std::string& name, uint32_t vendor_id,
                        uint32_t product_id, const gchar* tool,
                        uint32_t button_count, FlValue* features) {
  g_autoptr(FlValue) owned_features = features;
  FlValue* packet = fl_value_new_map();
  fl_value_set_string_take(packet, "type", fl_value_new_string("device"));
  set_int(packet, "timestampMicros", g_get_monotonic_time());
  fl_value_set_string_take(packet, "phase", fl_value_new_string(phase));
  fl_value_set_string_take(packet, "kind", fl_value_new_string(kind));
  set_string(packet, "nativeDeviceIdentifier", identifier);
  if (!name.empty()) {
    set_string(packet, "name", name);
  }
  if (vendor_id != 0) {
    set_int(packet, "vendorIdentifier", vendor_id);
  }
  if (product_id != 0) {
    set_int(packet, "productIdentifier", product_id);
  }
  if (tool != nullptr) {
    fl_value_set_string_take(packet, "tool", fl_value_new_string(tool));
  }
  if (button_count != 0) {
    set_int(packet, "buttonCount", button_count);
  }
  fl_value_set_string_take(packet, "features", fl_value_ref(owned_features));
  send_packet(backend, packet);
}

/** Announces a tablet after its initial description sequence. */
void announce_tablet(TabletState* tablet) {
  if (tablet->announced) {
    return;
  }
  tablet->announced = true;
  FlValue* features = fl_value_new_list();
  fl_value_append_take(features, fl_value_new_string("deviceInfo"));
  send_device_packet(tablet->backend, "added", "tablet",
                     tablet_identifier(tablet), tablet->name,
                     tablet->vendor_id, tablet->product_id, nullptr, 0,
                     features);
}

/** Announces a tool after its initial capability sequence. */
void announce_tool(ToolState* tool) {
  if (tool->announced) {
    return;
  }
  tool->announced = true;
  append_feature(tool, "primaryButton");
  append_feature(tool, "secondaryButton");
  append_feature(tool, "eraser");
  append_feature(tool, "hover");
  append_feature(tool, "deviceInfo");
  send_device_packet(tool->backend, "added", "tool", tool_identifier(tool),
                     {}, 0, 0, tool_name(tool->type), 2,
                     tool_feature_list(tool));
}

/** Builds a feature list for the controls announced by one pad. */
FlValue* pad_feature_list(const PadState* pad) {
  FlValue* features = fl_value_new_list();
  fl_value_append_take(features, fl_value_new_string("deviceInfo"));
  if (pad->button_count > 0) {
    fl_value_append_take(features, fl_value_new_string("tabletPadButtons"));
  }
  if (pad->next_ring_index > 0) {
    fl_value_append_take(features, fl_value_new_string("tabletPadRing"));
  }
  if (pad->next_strip_index > 0) {
    fl_value_append_take(features, fl_value_new_string("tabletPadStrip"));
  }
  if (pad->next_dial_index > 0) {
    fl_value_append_take(features, fl_value_new_string("tabletPadDial"));
  }
  return features;
}

/** Announces a pad after its initial description sequence. */
void announce_pad(PadState* pad) {
  if (pad->announced) {
    return;
  }
  pad->announced = true;
  send_device_packet(pad->backend, "added", "pad", pad_identifier(pad), {},
                     0, 0, nullptr, pad->button_count,
                     pad_feature_list(pad));
}

/** Emits a device removal packet using its most recent description. */
void remove_tablet(TabletState* tablet) {
  if (!tablet->announced) {
    return;
  }
  FlValue* features = fl_value_new_list();
  fl_value_append_take(features, fl_value_new_string("deviceInfo"));
  send_device_packet(tablet->backend, "removed", "tablet",
                     tablet_identifier(tablet), tablet->name,
                     tablet->vendor_id, tablet->product_id, nullptr, 0,
                     features);
}

/** Emits a device removal packet for a tool. */
void remove_tool(ToolState* tool) {
  if (!tool->announced) {
    return;
  }
  send_device_packet(tool->backend, "removed", "tool", tool_identifier(tool),
                     {}, 0, 0, tool_name(tool->type), 2,
                     tool_feature_list(tool));
}

/** Emits a device removal packet for a tablet pad. */
void remove_pad(PadState* pad) {
  if (!pad->announced) {
    return;
  }
  send_device_packet(pad->backend, "removed", "pad", pad_identifier(pad), {},
                     0, 0, nullptr, pad->button_count,
                     pad_feature_list(pad));
}

/** Emits the accumulated state of one focused tool frame. */
void send_tool_frame(ToolState* tool, uint32_t time) {
  if (!tool->dirty || !tool->focused || !tool->has_position) {
    return;
  }
  const gchar* phase = phase_name(tool);
  FlValue* packet = fl_value_new_map();
  fl_value_set_string_take(packet, "type", fl_value_new_string("motion"));
  set_int(packet, "timestampMicros",
          static_cast<gint64>(time) * kMicrosecondsPerMillisecond);
  fl_value_set_string_take(packet, "phase", fl_value_new_string(phase));
  fl_value_set_string_take(packet, "tool",
                           fl_value_new_string(tool_name(tool->type)));
  set_int(packet, "pointerIdentifier",
          wl_proxy_get_id(reinterpret_cast<wl_proxy*>(tool->proxy)));
  set_int(packet, "deviceIdentifier",
          tool->tablet == nullptr
              ? 0
              : wl_proxy_get_id(
                    reinterpret_cast<wl_proxy*>(tool->tablet->proxy)));
  set_string(packet, "nativeDeviceIdentifier", tool_identifier(tool));
  set_float(packet, "x", tool->x);
  set_float(packet, "y", tool->y);
  set_float(packet, "deltaX",
            tool->has_last_position ? tool->x - tool->last_x : 0);
  set_float(packet, "deltaY",
            tool->has_last_position ? tool->y - tool->last_y : 0);
  set_int(packet, "buttons",
          tool->side_buttons |
              (tool->is_down ? kFlutterPrimaryButton : 0));
  fl_value_set_string_take(packet, "isDown",
                           fl_value_new_bool(tool->is_down));
  if (tool->has_pressure) {
    set_float(packet, "pressure", tool->pressure);
    set_float(packet, "pressureMinimum", 0);
    set_float(packet, "pressureMaximum", 1);
  }
  if (tool->has_distance) {
    set_float(packet, "distance", tool->distance);
    set_float(packet, "distanceMaximum", 1);
  }
  if (tool->has_tilt) {
    const double tangent_x = std::tan(tool->tilt_x);
    const double tangent_y = std::tan(tool->tilt_y);
    set_float(packet, "tiltX", tool->tilt_x);
    set_float(packet, "tiltY", tool->tilt_y);
    set_float(packet, "tilt",
              std::atan(std::hypot(tangent_x, tangent_y)));
    set_float(packet, "orientation", std::atan2(tangent_x, -tangent_y));
  }
  if (tool->has_rotation) {
    set_float(packet, "barrelRotation", tool->rotation);
  }
  if (tool->has_slider) {
    set_float(packet, "tangentialPressure", tool->slider);
  }
  if (tool->wheel_delta != 0) {
    set_float(packet, "wheelDelta", tool->wheel_delta);
  }
  fl_value_set_string_take(packet, "features", tool_feature_list(tool));
  send_packet(tool->backend, packet);

  if (tool->pending_phase == PendingToolPhase::removed) {
    tool->has_last_position = false;
  } else {
    tool->last_x = tool->x;
    tool->last_y = tool->y;
    tool->has_last_position = true;
  }
}

/** Clears fields that only apply to one completed Wayland tool frame. */
void finish_tool_frame(ToolState* tool) {
  if (tool->pending_phase == PendingToolPhase::removed) {
    tool->focused = false;
    tool->has_last_position = false;
  }
  tool->pending_phase = PendingToolPhase::none;
  tool->dirty = false;
  tool->wheel_delta = 0;
}

/** Emits one normalized tablet-pad packet. */
void send_pad_event(PadState* pad, uint32_t time, const gchar* control,
                    uint32_t index, const gchar* phase,
                    const double* value, uint32_t mode) {
  if (!pad->focused) {
    return;
  }
  FlValue* packet = fl_value_new_map();
  fl_value_set_string_take(packet, "type", fl_value_new_string("pad"));
  set_int(packet, "timestampMicros",
          static_cast<gint64>(time) * kMicrosecondsPerMillisecond);
  set_string(packet, "nativeDeviceIdentifier", pad_identifier(pad));
  fl_value_set_string_take(packet, "control", fl_value_new_string(control));
  set_int(packet, "controlIndex", index);
  fl_value_set_string_take(packet, "phase", fl_value_new_string(phase));
  if (value != nullptr) {
    set_float(packet, "value", *value);
  }
  set_int(packet, "mode", mode);
  send_packet(pad->backend, packet);
}

/** Returns a tablet state for a protocol proxy, or null after removal. */
TabletState* find_tablet(StyletWaylandBackend* backend,
                         zwp_tablet_v2* proxy) {
  const auto iterator = backend->tablets.find(proxy);
  return iterator == backend->tablets.end() ? nullptr : iterator->second.get();
}

/** Receives a Wayland tablet name. */
void tablet_name_cb(void* data, zwp_tablet_v2* /*proxy*/, const char* name) {
  static_cast<TabletState*>(data)->name = name == nullptr ? "" : name;
}

/** Receives a Wayland tablet USB identifier pair. */
void tablet_id_cb(void* data, zwp_tablet_v2* /*proxy*/, uint32_t vendor_id,
                  uint32_t product_id) {
  TabletState* tablet = static_cast<TabletState*>(data);
  tablet->vendor_id = vendor_id;
  tablet->product_id = product_id;
}

/** Receives a local path that can identify a Wayland tablet. */
void tablet_path_cb(void* data, zwp_tablet_v2* /*proxy*/, const char* path) {
  TabletState* tablet = static_cast<TabletState*>(data);
  if (tablet->path.empty() && path != nullptr) {
    tablet->path = path;
  }
}

/** Finalizes and announces a Wayland tablet description. */
void tablet_done_cb(void* data, zwp_tablet_v2* /*proxy*/) {
  announce_tablet(static_cast<TabletState*>(data));
}

/** Announces removal and destroys a compositor-removed Wayland tablet. */
void tablet_removed_cb(void* data, zwp_tablet_v2* proxy) {
  TabletState* tablet = static_cast<TabletState*>(data);
  StyletWaylandBackend* backend = tablet->backend;
  remove_tablet(tablet);
  for (auto& entry : backend->tools) {
    if (entry.second->tablet == tablet) {
      entry.second->tablet = nullptr;
    }
  }
  for (auto& entry : backend->pads) {
    if (entry.second->tablet == tablet) {
      entry.second->tablet = nullptr;
    }
  }
  zwp_tablet_v2_destroy(proxy);
  backend->tablets.erase(proxy);
}

/** Accepts optional bus metadata not yet represented by Stylet's Dart model. */
void tablet_bustype_cb(void* /*data*/, zwp_tablet_v2* /*proxy*/,
                       uint32_t /*bustype*/) {}

/** Listener shared by every Wayland tablet description object. */
const zwp_tablet_v2_listener kTabletListener = {
    tablet_name_cb,
    tablet_id_cb,
    tablet_path_cb,
    tablet_done_cb,
    tablet_removed_cb,
    tablet_bustype_cb,
};

/** Receives the physical kind of a Wayland tool. */
void tool_type_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                  uint32_t type) {
  static_cast<ToolState*>(data)->type = type;
}

/** Receives the stable serial of a Wayland tool. */
void tool_serial_cb(void* data, zwp_tablet_tool_v2* /*proxy*/, uint32_t high,
                    uint32_t low) {
  static_cast<ToolState*>(data)->hardware_serial = combine_words(high, low);
}

/** Receives the Wacom-format hardware identifier of a Wayland tool. */
void tool_hardware_id_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                         uint32_t high, uint32_t low) {
  static_cast<ToolState*>(data)->hardware_id = combine_words(high, low);
}

/** Records one capability advertised by a Wayland tool. */
void tool_capability_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                        uint32_t capability) {
  ToolState* tool = static_cast<ToolState*>(data);
  switch (capability) {
    case ZWP_TABLET_TOOL_V2_CAPABILITY_TILT:
      append_feature(tool, "tilt");
      append_feature(tool, "orientation");
      break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE:
      append_feature(tool, "pressure");
      break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE:
      append_feature(tool, "distance");
      break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_ROTATION:
      append_feature(tool, "barrelRotation");
      break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_SLIDER:
      append_feature(tool, "tangentialPressure");
      break;
    case ZWP_TABLET_TOOL_V2_CAPABILITY_WHEEL:
      append_feature(tool, "wheel");
      break;
  }
}

/** Finalizes and announces a Wayland tool description. */
void tool_done_cb(void* data, zwp_tablet_tool_v2* /*proxy*/) {
  announce_tool(static_cast<ToolState*>(data));
}

/** Announces removal and destroys a compositor-removed Wayland tool. */
void tool_removed_cb(void* data, zwp_tablet_tool_v2* proxy) {
  ToolState* tool = static_cast<ToolState*>(data);
  StyletWaylandBackend* backend = tool->backend;
  remove_tool(tool);
  zwp_tablet_tool_v2_destroy(proxy);
  backend->tools.erase(proxy);
}

/** Begins a Wayland tool proximity frame for the Flutter surface. */
void tool_proximity_in_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                          uint32_t /*serial*/, zwp_tablet_v2* tablet_proxy,
                          wl_surface* surface) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->tablet = find_tablet(tool->backend, tablet_proxy);
  tool->focused = surface == tool->backend->surface;
  tool->pending_phase = PendingToolPhase::added;
  tool->dirty = true;
}

/** Marks a Wayland tool as leaving the Flutter surface at frame end. */
void tool_proximity_out_cb(void* data, zwp_tablet_tool_v2* /*proxy*/) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->pending_phase = PendingToolPhase::removed;
  tool->dirty = true;
}

/** Marks the tool tip as touching the tablet. */
void tool_down_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                  uint32_t /*serial*/) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->is_down = true;
  tool->pending_phase = PendingToolPhase::down;
  tool->dirty = true;
}

/** Marks the tool tip as no longer touching the tablet. */
void tool_up_cb(void* data, zwp_tablet_tool_v2* /*proxy*/) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->is_down = false;
  tool->pending_phase = PendingToolPhase::up;
  tool->dirty = true;
}

/** Updates one Wayland tool's surface-local position. */
void tool_motion_cb(void* data, zwp_tablet_tool_v2* /*proxy*/, wl_fixed_t x,
                    wl_fixed_t y) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->x = wl_fixed_to_double(x);
  tool->y = wl_fixed_to_double(y);
  tool->has_position = true;
  tool->dirty = true;
}

/** Updates one Wayland tool's normalized pressure. */
void tool_pressure_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                      uint32_t pressure) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->pressure = static_cast<double>(pressure) / 65535.0;
  tool->has_pressure = true;
  tool->dirty = true;
}

/** Updates one Wayland tool's normalized hover distance. */
void tool_distance_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                      uint32_t distance) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->distance = static_cast<double>(distance) / 65535.0;
  tool->has_distance = true;
  tool->dirty = true;
}

/** Updates signed Wayland tilt components after converting degrees to radians. */
void tool_tilt_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                  wl_fixed_t tilt_x, wl_fixed_t tilt_y) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->tilt_x = wl_fixed_to_double(tilt_x) * G_PI / 180.0;
  tool->tilt_y = wl_fixed_to_double(tilt_y) * G_PI / 180.0;
  tool->has_tilt = true;
  tool->dirty = true;
}

/** Updates clockwise Wayland tool rotation after converting degrees to radians. */
void tool_rotation_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                      wl_fixed_t degrees) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->rotation = wl_fixed_to_double(degrees) * G_PI / 180.0;
  tool->has_rotation = true;
  tool->dirty = true;
}

/** Updates a Wayland airbrush slider normalized to the signed unit range. */
void tool_slider_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                    int32_t position) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->slider = std::clamp(static_cast<double>(position) / 65535.0, -1.0,
                            1.0);
  tool->has_slider = true;
  tool->dirty = true;
}

/** Accumulates relative Wayland stylus-wheel movement in radians. */
void tool_wheel_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                   wl_fixed_t degrees, int32_t /*clicks*/) {
  ToolState* tool = static_cast<ToolState*>(data);
  tool->wheel_delta += wl_fixed_to_double(degrees) * G_PI / 180.0;
  tool->dirty = true;
}

/** Updates Flutter-compatible side-button bits for a Wayland tool. */
void tool_button_cb(void* data, zwp_tablet_tool_v2* /*proxy*/,
                    uint32_t /*serial*/, uint32_t button, uint32_t state) {
  ToolState* tool = static_cast<ToolState*>(data);
  const bool pressed = state == ZWP_TABLET_TOOL_V2_BUTTON_STATE_PRESSED;
  const gint64 bit = button == BTN_STYLUS
                         ? kFlutterPrimaryStylusButton
                         : button == BTN_STYLUS2
                               ? kFlutterSecondaryStylusButton
                               : 0;
  if (pressed) {
    tool->side_buttons |= bit;
  } else {
    tool->side_buttons &= ~bit;
  }
  tool->dirty = true;
}

/** Emits and clears one complete Wayland tool frame. */
void tool_frame_cb(void* data, zwp_tablet_tool_v2* /*proxy*/, uint32_t time) {
  ToolState* tool = static_cast<ToolState*>(data);
  send_tool_frame(tool, time);
  finish_tool_frame(tool);
}

/** Listener shared by every Wayland tablet tool object. */
const zwp_tablet_tool_v2_listener kToolListener = {
    tool_type_cb,          tool_serial_cb,       tool_hardware_id_cb,
    tool_capability_cb,    tool_done_cb,         tool_removed_cb,
    tool_proximity_in_cb,  tool_proximity_out_cb, tool_down_cb,
    tool_up_cb,            tool_motion_cb,       tool_pressure_cb,
    tool_distance_cb,      tool_tilt_cb,         tool_rotation_cb,
    tool_slider_cb,        tool_wheel_cb,        tool_button_cb,
    tool_frame_cb,
};

/** Ignores the optional physical source of a ring interaction. */
void ring_source_cb(void* /*data*/, zwp_tablet_pad_ring_v2* /*proxy*/,
                    uint32_t /*source*/) {}

/** Stores one absolute ring angle normalized to the unit range. */
void ring_angle_cb(void* data, zwp_tablet_pad_ring_v2* /*proxy*/,
                   wl_fixed_t degrees) {
  RingState* ring = static_cast<RingState*>(data);
  double normalized = std::fmod(wl_fixed_to_double(degrees), 360.0) / 360.0;
  if (normalized < 0) {
    normalized += 1;
  }
  ring->value = normalized;
  ring->dirty = true;
  ring->stopping = false;
}

/** Marks the current ring interaction as ending at the next frame. */
void ring_stop_cb(void* data, zwp_tablet_pad_ring_v2* /*proxy*/) {
  RingState* ring = static_cast<RingState*>(data);
  ring->dirty = true;
  ring->stopping = true;
}

/** Emits one complete Wayland touch-ring frame. */
void ring_frame_cb(void* data, zwp_tablet_pad_ring_v2* /*proxy*/,
                   uint32_t time) {
  RingState* ring = static_cast<RingState*>(data);
  if (!ring->dirty) {
    return;
  }
  const gchar* phase = ring->stopping
                           ? "ended"
                           : ring->active ? "changed" : "began";
  const double* value = ring->stopping ? nullptr : &ring->value;
  send_pad_event(ring->group->pad, time, "ring", ring->index, phase, value,
                 ring->group->mode);
  ring->active = !ring->stopping;
  ring->dirty = false;
  ring->stopping = false;
}

/** Listener shared by every Wayland tablet-pad ring. */
const zwp_tablet_pad_ring_v2_listener kRingListener = {
    ring_source_cb,
    ring_angle_cb,
    ring_stop_cb,
    ring_frame_cb,
};

/** Ignores the optional physical source of a strip interaction. */
void strip_source_cb(void* /*data*/, zwp_tablet_pad_strip_v2* /*proxy*/,
                     uint32_t /*source*/) {}

/** Stores one absolute strip position normalized to the unit range. */
void strip_position_cb(void* data, zwp_tablet_pad_strip_v2* /*proxy*/,
                       uint32_t position) {
  StripState* strip = static_cast<StripState*>(data);
  strip->value = static_cast<double>(position) / 65535.0;
  strip->dirty = true;
  strip->stopping = false;
}

/** Marks the current strip interaction as ending at the next frame. */
void strip_stop_cb(void* data, zwp_tablet_pad_strip_v2* /*proxy*/) {
  StripState* strip = static_cast<StripState*>(data);
  strip->dirty = true;
  strip->stopping = true;
}

/** Emits one complete Wayland touch-strip frame. */
void strip_frame_cb(void* data, zwp_tablet_pad_strip_v2* /*proxy*/,
                    uint32_t time) {
  StripState* strip = static_cast<StripState*>(data);
  if (!strip->dirty) {
    return;
  }
  const gchar* phase = strip->stopping
                           ? "ended"
                           : strip->active ? "changed" : "began";
  const double* value = strip->stopping ? nullptr : &strip->value;
  send_pad_event(strip->group->pad, time, "strip", strip->index, phase, value,
                 strip->group->mode);
  strip->active = !strip->stopping;
  strip->dirty = false;
  strip->stopping = false;
}

/** Listener shared by every Wayland tablet-pad strip. */
const zwp_tablet_pad_strip_v2_listener kStripListener = {
    strip_source_cb,
    strip_position_cb,
    strip_stop_cb,
    strip_frame_cb,
};

/** Accumulates one relative Wayland pad-dial movement in logical detents. */
void dial_delta_cb(void* data, zwp_tablet_pad_dial_v2* /*proxy*/,
                   int32_t value120) {
  DialState* dial = static_cast<DialState*>(data);
  dial->delta += static_cast<double>(value120) / 120.0;
}

/** Emits and resets one complete Wayland tablet-pad dial frame. */
void dial_frame_cb(void* data, zwp_tablet_pad_dial_v2* /*proxy*/,
                   uint32_t time) {
  DialState* dial = static_cast<DialState*>(data);
  if (dial->delta == 0) {
    return;
  }
  send_pad_event(dial->group->pad, time, "dial", dial->index, "discrete",
                 &dial->delta, dial->group->mode);
  dial->delta = 0;
}

/** Listener shared by every relative Wayland tablet-pad dial. */
const zwp_tablet_pad_dial_v2_listener kDialListener = {
    dial_delta_cb,
    dial_frame_cb,
};

/** Associates pad-global button indices with their mapping-mode group. */
void group_buttons_cb(void* data, zwp_tablet_pad_group_v2* /*proxy*/,
                      wl_array* buttons) {
  PadGroupState* group = static_cast<PadGroupState*>(data);
  if (buttons == nullptr || buttons->data == nullptr) {
    return;
  }
  const auto* indices = static_cast<const uint32_t*>(buttons->data);
  const size_t count = buttons->size / sizeof(uint32_t);
  for (size_t index = 0; index < count; ++index) {
    group->pad->button_groups[indices[index]] = group;
  }
}

/** Registers one touch ring announced for a Wayland pad group. */
void group_ring_cb(void* data, zwp_tablet_pad_group_v2* /*proxy*/,
                   zwp_tablet_pad_ring_v2* proxy) {
  PadGroupState* group = static_cast<PadGroupState*>(data);
  auto ring = std::make_unique<RingState>(
      RingState{group, proxy, group->pad->next_ring_index++});
  RingState* ring_pointer = ring.get();
  group->pad->backend->rings.emplace(proxy, std::move(ring));
  zwp_tablet_pad_ring_v2_add_listener(proxy, &kRingListener, ring_pointer);
}

/** Registers one touch strip announced for a Wayland pad group. */
void group_strip_cb(void* data, zwp_tablet_pad_group_v2* /*proxy*/,
                    zwp_tablet_pad_strip_v2* proxy) {
  PadGroupState* group = static_cast<PadGroupState*>(data);
  auto strip = std::make_unique<StripState>(
      StripState{group, proxy, group->pad->next_strip_index++});
  StripState* strip_pointer = strip.get();
  group->pad->backend->strips.emplace(proxy, std::move(strip));
  zwp_tablet_pad_strip_v2_add_listener(proxy, &kStripListener, strip_pointer);
}

/** Registers one relative dial announced for a Wayland pad group. */
void group_dial_cb(void* data, zwp_tablet_pad_group_v2* /*proxy*/,
                   zwp_tablet_pad_dial_v2* proxy) {
  PadGroupState* group = static_cast<PadGroupState*>(data);
  auto dial = std::make_unique<DialState>(
      DialState{group, proxy, group->pad->next_dial_index++});
  DialState* dial_pointer = dial.get();
  group->pad->backend->dials.emplace(proxy, std::move(dial));
  zwp_tablet_pad_dial_v2_add_listener(proxy, &kDialListener, dial_pointer);
}

/** Records the number of mapping modes supported by a pad group. */
void group_modes_cb(void* data, zwp_tablet_pad_group_v2* /*proxy*/,
                    uint32_t modes) {
  static_cast<PadGroupState*>(data)->mode_count = std::max(modes, 1U);
}

/** Accepts completion of a pad group description. */
void group_done_cb(void* /*data*/, zwp_tablet_pad_group_v2* /*proxy*/) {}

/** Emits a mapping-mode change for a focused tablet pad. */
void group_mode_switch_cb(void* data, zwp_tablet_pad_group_v2* /*proxy*/,
                          uint32_t time, uint32_t /*serial*/, uint32_t mode) {
  PadGroupState* group = static_cast<PadGroupState*>(data);
  group->mode = mode;
  send_pad_event(group->pad, time, "mode", group->index, "discrete", nullptr,
                 mode);
}

/** Listener shared by every Wayland tablet-pad control group. */
const zwp_tablet_pad_group_v2_listener kGroupListener = {
    group_buttons_cb, group_ring_cb,  group_strip_cb,       group_modes_cb,
    group_done_cb,    group_mode_switch_cb, group_dial_cb,
};

/** Registers one control group announced for a Wayland tablet pad. */
void pad_group_cb(void* data, zwp_tablet_pad_v2* /*proxy*/,
                  zwp_tablet_pad_group_v2* proxy) {
  PadState* pad = static_cast<PadState*>(data);
  const uint32_t index = pad->next_group_index++;
  auto group = std::make_unique<PadGroupState>(PadGroupState{pad, proxy, index});
  PadGroupState* group_pointer = group.get();
  pad->backend->groups.emplace(proxy, std::move(group));
  zwp_tablet_pad_group_v2_add_listener(proxy, &kGroupListener, group_pointer);
}

/** Receives a local path that can identify a Wayland tablet pad. */
void pad_path_cb(void* data, zwp_tablet_pad_v2* /*proxy*/, const char* path) {
  PadState* pad = static_cast<PadState*>(data);
  if (pad->path.empty() && path != nullptr) {
    pad->path = path;
  }
}

/** Records the physical button count of a Wayland tablet pad. */
void pad_buttons_cb(void* data, zwp_tablet_pad_v2* /*proxy*/,
                    uint32_t buttons) {
  static_cast<PadState*>(data)->button_count = buttons;
}

/** Finalizes and announces a Wayland tablet pad description. */
void pad_done_cb(void* data, zwp_tablet_pad_v2* /*proxy*/) {
  announce_pad(static_cast<PadState*>(data));
}

/** Emits a physical Wayland tablet-pad button state change. */
void pad_button_cb(void* data, zwp_tablet_pad_v2* /*proxy*/, uint32_t time,
                   uint32_t button, uint32_t state) {
  PadState* pad = static_cast<PadState*>(data);
  const gchar* phase = state == ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED
                           ? "began"
                           : "ended";
  const auto group = pad->button_groups.find(button);
  const uint32_t mode =
      group == pad->button_groups.end() ? 0 : group->second->mode;
  send_pad_event(pad, time, "button", button, phase, nullptr, mode);
}

/** Focuses a Wayland tablet pad on the Flutter surface. */
void pad_enter_cb(void* data, zwp_tablet_pad_v2* /*proxy*/,
                  uint32_t /*serial*/, zwp_tablet_v2* tablet_proxy,
                  wl_surface* surface) {
  PadState* pad = static_cast<PadState*>(data);
  pad->tablet = find_tablet(pad->backend, tablet_proxy);
  pad->focused = surface == pad->backend->surface;
}

/** Removes focus from a Wayland tablet pad leaving the Flutter surface. */
void pad_leave_cb(void* data, zwp_tablet_pad_v2* /*proxy*/,
                  uint32_t /*serial*/, wl_surface* /*surface*/) {
  static_cast<PadState*>(data)->focused = false;
}

/** Destroys every child protocol object owned by one tablet pad. */
void destroy_pad_controls(PadState* pad) {
  StyletWaylandBackend* backend = pad->backend;
  for (auto iterator = backend->dials.begin();
       iterator != backend->dials.end();) {
    if (iterator->second->group->pad != pad) {
      ++iterator;
      continue;
    }
    zwp_tablet_pad_dial_v2_destroy(iterator->first);
    iterator = backend->dials.erase(iterator);
  }
  for (auto iterator = backend->rings.begin();
       iterator != backend->rings.end();) {
    if (iterator->second->group->pad != pad) {
      ++iterator;
      continue;
    }
    zwp_tablet_pad_ring_v2_destroy(iterator->first);
    iterator = backend->rings.erase(iterator);
  }
  for (auto iterator = backend->strips.begin();
       iterator != backend->strips.end();) {
    if (iterator->second->group->pad != pad) {
      ++iterator;
      continue;
    }
    zwp_tablet_pad_strip_v2_destroy(iterator->first);
    iterator = backend->strips.erase(iterator);
  }
  for (auto iterator = backend->groups.begin();
       iterator != backend->groups.end();) {
    if (iterator->second->pad != pad) {
      ++iterator;
      continue;
    }
    zwp_tablet_pad_group_v2_destroy(iterator->first);
    iterator = backend->groups.erase(iterator);
  }
  pad->button_groups.clear();
}

/** Announces removal and destroys a compositor-removed Wayland tablet pad. */
void pad_removed_cb(void* data, zwp_tablet_pad_v2* proxy) {
  PadState* pad = static_cast<PadState*>(data);
  StyletWaylandBackend* backend = pad->backend;
  remove_pad(pad);
  pad->focused = false;
  destroy_pad_controls(pad);
  zwp_tablet_pad_v2_destroy(proxy);
  backend->pads.erase(proxy);
}

/** Listener shared by every Wayland tablet-pad object. */
const zwp_tablet_pad_v2_listener kPadListener = {
    pad_group_cb, pad_path_cb, pad_buttons_cb, pad_done_cb,
    pad_button_cb, pad_enter_cb, pad_leave_cb,   pad_removed_cb,
};

/** Registers one newly announced Wayland tablet. */
void seat_tablet_added_cb(void* data, zwp_tablet_seat_v2* /*seat*/,
                          zwp_tablet_v2* proxy) {
  StyletWaylandBackend* backend = static_cast<StyletWaylandBackend*>(data);
  auto tablet = std::make_unique<TabletState>(TabletState{backend, proxy});
  TabletState* tablet_pointer = tablet.get();
  backend->tablets.emplace(proxy, std::move(tablet));
  zwp_tablet_v2_add_listener(proxy, &kTabletListener, tablet_pointer);
}

/** Registers one newly announced Wayland tablet tool. */
void seat_tool_added_cb(void* data, zwp_tablet_seat_v2* /*seat*/,
                        zwp_tablet_tool_v2* proxy) {
  StyletWaylandBackend* backend = static_cast<StyletWaylandBackend*>(data);
  auto tool = std::make_unique<ToolState>(ToolState{backend, proxy});
  ToolState* tool_pointer = tool.get();
  backend->tools.emplace(proxy, std::move(tool));
  zwp_tablet_tool_v2_add_listener(proxy, &kToolListener, tool_pointer);
}

/** Registers one newly announced Wayland tablet pad. */
void seat_pad_added_cb(void* data, zwp_tablet_seat_v2* /*seat*/,
                       zwp_tablet_pad_v2* proxy) {
  StyletWaylandBackend* backend = static_cast<StyletWaylandBackend*>(data);
  auto pad = std::make_unique<PadState>(PadState{backend, proxy});
  PadState* pad_pointer = pad.get();
  backend->pads.emplace(proxy, std::move(pad));
  zwp_tablet_pad_v2_add_listener(proxy, &kPadListener, pad_pointer);
}

/** Listener that receives every tablet object associated with a seat. */
const zwp_tablet_seat_v2_listener kTabletSeatListener = {
    seat_tablet_added_cb,
    seat_tool_added_cb,
    seat_pad_added_cb,
};

/** Creates the per-seat tablet controller once both globals are available. */
void create_tablet_seat(StyletWaylandBackend* backend) {
  if (backend->tablet_seat != nullptr || backend->manager == nullptr ||
      backend->seat == nullptr) {
    return;
  }
  backend->tablet_seat =
      zwp_tablet_manager_v2_get_tablet_seat(backend->manager, backend->seat);
  zwp_tablet_seat_v2_add_listener(backend->tablet_seat, &kTabletSeatListener,
                                  backend);
}

/** Binds the tablet-v2 manager advertised by the Wayland compositor. */
void registry_global_cb(void* data, wl_registry* registry, uint32_t name,
                        const char* interface, uint32_t version) {
  StyletWaylandBackend* backend = static_cast<StyletWaylandBackend*>(data);
  if (strcmp(interface, zwp_tablet_manager_v2_interface.name) == 0 &&
      backend->manager == nullptr) {
    backend->manager_name = name;
    backend->manager = static_cast<zwp_tablet_manager_v2*>(wl_registry_bind(
        registry, name, &zwp_tablet_manager_v2_interface,
        std::min(version, kMaximumTabletProtocolVersion)));
  }
  create_tablet_seat(backend);
}

/** Stops the backend when a required Wayland global disappears. */
void registry_global_remove_cb(void* data, wl_registry* /*registry*/,
                               uint32_t name) {
  StyletWaylandBackend* backend = static_cast<StyletWaylandBackend*>(data);
  if (name == backend->manager_name) {
    backend->active = false;
  }
}

/** Registry listener used during optional tablet-v2 discovery. */
const wl_registry_listener kRegistryListener = {
    registry_global_cb,
    registry_global_remove_cb,
};

}  // namespace

StyletWaylandBackend* stylet_wayland_backend_new(
    GtkWidget* view, FlEventChannel* event_channel) {
  g_return_val_if_fail(GTK_IS_WIDGET(view), nullptr);
  g_return_val_if_fail(FL_IS_EVENT_CHANNEL(event_channel), nullptr);
  return new StyletWaylandBackend{
      view,
      FL_EVENT_CHANNEL(g_object_ref(event_channel)),
  };
}

gboolean stylet_wayland_backend_start(StyletWaylandBackend* backend) {
  g_return_val_if_fail(backend != nullptr, FALSE);
  if (backend->active) {
    return TRUE;
  }
  if (backend->registry != nullptr) {
    stylet_wayland_backend_stop(backend);
  }
  GdkDisplay* gdk_display = gtk_widget_get_display(backend->view);
  GdkWindow* gdk_window = gtk_widget_get_window(backend->view);
  if (!GDK_IS_WAYLAND_DISPLAY(gdk_display) || gdk_window == nullptr ||
      !GDK_IS_WAYLAND_WINDOW(gdk_window)) {
    return FALSE;
  }
  GdkSeat* gdk_seat = gdk_display_get_default_seat(gdk_display);
  if (gdk_seat == nullptr) {
    return FALSE;
  }
  backend->display = gdk_wayland_display_get_wl_display(gdk_display);
  backend->surface = gdk_wayland_window_get_wl_surface(gdk_window);
  backend->seat = gdk_wayland_seat_get_wl_seat(gdk_seat);
  if (backend->display == nullptr || backend->surface == nullptr ||
      backend->seat == nullptr) {
    return FALSE;
  }
  backend->active = true;
  backend->registry = wl_display_get_registry(backend->display);
  wl_registry_add_listener(backend->registry, &kRegistryListener, backend);
  if (wl_display_roundtrip(backend->display) < 0) {
    stylet_wayland_backend_stop(backend);
    return FALSE;
  }
  create_tablet_seat(backend);
  if (backend->tablet_seat == nullptr ||
      wl_display_roundtrip(backend->display) < 0) {
    stylet_wayland_backend_stop(backend);
    return FALSE;
  }
  return TRUE;
}

gboolean stylet_wayland_backend_is_active(
    const StyletWaylandBackend* backend) {
  return backend != nullptr && backend->active;
}

void stylet_wayland_backend_stop(StyletWaylandBackend* backend) {
  if (backend == nullptr) {
    return;
  }
  backend->active = false;
  for (auto& entry : backend->dials) {
    zwp_tablet_pad_dial_v2_destroy(entry.first);
  }
  backend->dials.clear();
  for (auto& entry : backend->rings) {
    zwp_tablet_pad_ring_v2_destroy(entry.first);
  }
  backend->rings.clear();
  for (auto& entry : backend->strips) {
    zwp_tablet_pad_strip_v2_destroy(entry.first);
  }
  backend->strips.clear();
  for (auto& entry : backend->groups) {
    zwp_tablet_pad_group_v2_destroy(entry.first);
  }
  backend->groups.clear();
  for (auto& entry : backend->pads) {
    zwp_tablet_pad_v2_destroy(entry.first);
  }
  backend->pads.clear();
  for (auto& entry : backend->tools) {
    zwp_tablet_tool_v2_destroy(entry.first);
  }
  backend->tools.clear();
  for (auto& entry : backend->tablets) {
    zwp_tablet_v2_destroy(entry.first);
  }
  backend->tablets.clear();
  if (backend->tablet_seat != nullptr) {
    zwp_tablet_seat_v2_destroy(backend->tablet_seat);
    backend->tablet_seat = nullptr;
  }
  if (backend->manager != nullptr) {
    zwp_tablet_manager_v2_destroy(backend->manager);
    backend->manager = nullptr;
  }
  backend->seat = nullptr;
  if (backend->registry != nullptr) {
    wl_registry_destroy(backend->registry);
    backend->registry = nullptr;
  }
  backend->display = nullptr;
  backend->surface = nullptr;
  backend->manager_name = 0;
}

void stylet_wayland_backend_free(StyletWaylandBackend* backend) {
  if (backend == nullptr) {
    return;
  }
  stylet_wayland_backend_stop(backend);
  g_clear_object(&backend->event_channel);
  delete backend;
}
