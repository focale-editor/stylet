#include "include/stylet/stylet_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "stylet_plugin_private.h"

#ifdef STYLET_HAS_WAYLAND
#include "stylet_wayland.h"
#endif

/** Casts a GObject instance to StyletPlugin after checking its runtime type. */
#define STYLET_PLUGIN(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST((obj), stylet_plugin_get_type(), StyletPlugin))

/** Request-response channel shared with the Dart backend. */
static constexpr char kMethodChannelName[] = "dev.focale.stylet/methods";

/** Continuous event channel shared with the Dart backend. */
static constexpr char kEventChannelName[] = "dev.focale.stylet/events";

/** Flutter button bit representing tip contact. */
static constexpr gint64 kFlutterPrimaryButton = 1;

/** Flutter button bit representing the first stylus side button. */
static constexpr gint64 kFlutterPrimaryStylusButton = 2;

/** Flutter button bit representing the second stylus side button. */
static constexpr gint64 kFlutterSecondaryStylusButton = 4;

/** Device-description bit representing pressure support. */
static constexpr guint kFeaturePressure = 1U << 0;

/** Device-description bit representing tilt and orientation support. */
static constexpr guint kFeatureTilt = 1U << 1;

/** Device-description bit representing hover distance support. */
static constexpr guint kFeatureDistance = 1U << 2;

/** Device-description bit representing barrel rotation support. */
static constexpr guint kFeatureRotation = 1U << 3;

/** Device-description bit representing tangential pressure support. */
static constexpr guint kFeatureTangentialPressure = 1U << 4;

/** Device-description bit representing tablet-pad buttons. */
static constexpr guint kFeaturePadButtons = 1U << 5;

/** Device-description bit representing a tablet-pad ring. */
static constexpr guint kFeaturePadRing = 1U << 6;

/** Device-description bit representing a tablet-pad strip. */
static constexpr guint kFeaturePadStrip = 1U << 7;

/** Own feature metadata accumulated for one observed GTK device. */
struct GdkDeviceState {
  /** Stylet feature bits discovered for the device. */
  guint features;

  /** Largest observed one-based pad button count. */
  guint button_count;
};

/** Owns channels, GTK handlers, and the latest position used for deltas. */
struct _StyletPlugin {
  /** Parent GObject instance. */
  GObject parent_instance;

  /** Method channel retained for the plugin lifetime. */
  FlMethodChannel* method_channel;

  /** Event channel retained for the plugin lifetime. */
  FlEventChannel* event_channel;

  /** Flutter view observed for tablet input. */
  FlView* view;

  /** Signal handler for pointer motion. */
  gulong motion_handler;

  /** Signal handler for button presses. */
  gulong button_press_handler;

  /** Signal handler for button releases. */
  gulong button_release_handler;

  /** Signal handler for tablet proximity entry. */
  gulong proximity_in_handler;

  /** Signal handler for tablet proximity exit. */
  gulong proximity_out_handler;

  /** Signal handler for generic GTK tablet-pad events. */
  gulong pad_event_handler;

  /** Whether Dart currently listens to native stylus events. */
  gboolean listening;

  /** Whether the previous motion position can produce a delta. */
  gboolean has_last_position;

  /** Most recently emitted x coordinate in logical pixels. */
  gdouble last_x;

  /** Most recently emitted y coordinate in logical pixels. */
  gdouble last_y;

  /** Tablet tool serial associated with the latest position. */
  guint64 last_tool_serial;

  /** GTK tool and pad descriptions indexed by their native identifiers. */
  GHashTable* announced_devices;

#ifdef STYLET_HAS_WAYLAND
  /** Optional direct tablet-v2 backend used on compatible Wayland sessions. */
  StyletWaylandBackend* wayland_backend;
#endif
};

G_DEFINE_TYPE(StyletPlugin, stylet_plugin, g_object_get_type())

/** Appends one UTF-8 feature name to a standard-codec list. */
static void append_feature(FlValue* features, const gchar* name) {
  fl_value_append_take(features, fl_value_new_string(name));
}

/** Adds one floating-point value to a standard-codec map. */
static void set_float(FlValue* map, const gchar* key, gdouble value) {
  fl_value_set_string_take(map, key, fl_value_new_float(value));
}

/** Reads one optional tablet axis from a GTK event. */
static gboolean get_axis(const GdkEvent* event, GdkAxisUse axis,
                         gdouble* value) {
  return gdk_event_get_axis(event, axis, value);
}

/** Converts a driver tilt component into radians when it uses common ranges. */
static gdouble tilt_axis_to_radians(gdouble value) {
  if (std::abs(value) <= 1.0) {
    return value * G_PI_2;
  }
  if (std::abs(value) > G_PI_2) {
    return value * G_PI / 180.0;
  }
  return value;
}

/** Converts a driver rotation axis into clockwise radians. */
static gdouble rotation_to_radians(gdouble value) {
  return std::abs(value) > 2.0 * G_PI ? value * G_PI / 180.0 : value;
}

/** Returns the most specific GTK tablet tool associated with an event. */
static GdkDeviceTool* get_device_tool(const GdkEvent* event) {
  return gdk_event_get_device_tool(event);
}

/** Returns the serial used to associate consecutive events from one tool. */
static guint64 get_tool_serial(const GdkEvent* event) {
  GdkDeviceTool* tool = get_device_tool(event);
  return tool == nullptr ? 0 : gdk_device_tool_get_serial(tool);
}

/** Whether a GTK event originated from a pen or eraser tablet tool. */
static gboolean is_stylus_event(const GdkEvent* event) {
  GdkDeviceTool* tool = get_device_tool(event);
  if (tool != nullptr) {
    const GdkDeviceToolType type = gdk_device_tool_get_tool_type(tool);
    if (type != GDK_DEVICE_TOOL_TYPE_MOUSE && type != GDK_DEVICE_TOOL_TYPE_LENS) {
      return TRUE;
    }
  }
  GdkDevice* device = gdk_event_get_source_device(event);
  if (device == nullptr) {
    return FALSE;
  }
  const GdkInputSource source = gdk_device_get_source(device);
  return source == GDK_SOURCE_PEN || source == GDK_SOURCE_ERASER ||
         source == GDK_SOURCE_CURSOR;
}

/** Whether tablet-v2 currently replaces GTK's lower-fidelity event stream. */
static gboolean uses_wayland_backend(const StyletPlugin* self) {
#ifdef STYLET_HAS_WAYLAND
  return stylet_wayland_backend_is_active(self->wayland_backend);
#else
  return FALSE;
#endif
}

/** Returns `eraser` for an inverted GTK tool and `pen` otherwise. */
static const gchar* tool_name(const GdkEvent* event) {
  GdkDeviceTool* tool = get_device_tool(event);
  if (tool != nullptr &&
      gdk_device_tool_get_tool_type(tool) == GDK_DEVICE_TOOL_TYPE_ERASER) {
    return "eraser";
  }
  GdkDevice* device = gdk_event_get_source_device(event);
  return device != nullptr && gdk_device_get_source(device) == GDK_SOURCE_ERASER
             ? "eraser"
             : "pen";
}

/** Creates a stable descriptive identifier for a GTK source and tablet tool. */
static gchar* native_device_identifier(const GdkEvent* event,
                                       guint64 tool_serial) {
  GdkDevice* device = gdk_event_get_source_device(event);
  const gchar* name = device == nullptr ? "tablet" : gdk_device_get_name(device);
  return g_strdup_printf("%s:%" G_GUINT64_FORMAT, name, tool_serial);
}

/** Creates a stable descriptive identifier for a GTK tablet pad. */
static gchar* native_pad_identifier(const GdkEvent* event) {
  GdkDevice* device = gdk_event_get_source_device(event);
  const gchar* name = device == nullptr ? "tablet-pad" : gdk_device_get_name(device);
  const gchar* vendor = device == nullptr ? nullptr : gdk_device_get_vendor_id(device);
  const gchar* product = device == nullptr ? nullptr : gdk_device_get_product_id(device);
  return g_strdup_printf("gdk-pad:%s:%s:%s", name,
                         vendor == nullptr ? "unknown" : vendor,
                         product == nullptr ? "unknown" : product);
}

/** Converts one optional hexadecimal GTK identifier into a channel integer. */
static guint64 parse_device_identifier(const gchar* value) {
  if (value == nullptr || *value == '\0') {
    return 0;
  }
  gchar* end = nullptr;
  const guint64 result = g_ascii_strtoull(value, &end, 16);
  return end == value ? 0 : result;
}

/** Returns feature bits inferred from the axes of one GTK input device. */
static guint gdk_device_features(GdkDevice* device) {
  if (device == nullptr) {
    return 0;
  }
  guint features = 0;
  gboolean has_tilt = FALSE;
  const gint axis_count = gdk_device_get_n_axes(device);
  for (gint index = 0; index < axis_count; ++index) {
    switch (gdk_device_get_axis_use(device, index)) {
      case GDK_AXIS_PRESSURE:
        features |= kFeaturePressure;
        break;
      case GDK_AXIS_XTILT:
      case GDK_AXIS_YTILT:
        has_tilt = TRUE;
        break;
      case GDK_AXIS_DISTANCE:
        features |= kFeatureDistance;
        break;
      case GDK_AXIS_ROTATION:
        features |= kFeatureRotation;
        break;
      case GDK_AXIS_WHEEL:
      case GDK_AXIS_SLIDER:
        features |= kFeatureTangentialPressure;
        break;
      default:
        break;
    }
  }
  if (has_tilt) {
    features |= kFeatureTilt;
  }
  return features;
}

/** Builds a standard-codec feature list from GTK device-description bits. */
static FlValue* device_feature_list(guint features, gboolean is_pad) {
  FlValue* result = fl_value_new_list();
  append_feature(result, "deviceInfo");
  if (is_pad) {
    if ((features & kFeaturePadButtons) != 0) {
      append_feature(result, "tabletPadButtons");
    }
    if ((features & kFeaturePadRing) != 0) {
      append_feature(result, "tabletPadRing");
    }
    if ((features & kFeaturePadStrip) != 0) {
      append_feature(result, "tabletPadStrip");
    }
    return result;
  }
  append_feature(result, "primaryButton");
  append_feature(result, "secondaryButton");
  append_feature(result, "eraser");
  append_feature(result, "hover");
  if ((features & kFeaturePressure) != 0) {
    append_feature(result, "pressure");
  }
  if ((features & kFeatureTilt) != 0) {
    append_feature(result, "tilt");
    append_feature(result, "orientation");
  }
  if ((features & kFeatureDistance) != 0) {
    append_feature(result, "distance");
  }
  if ((features & kFeatureRotation) != 0) {
    append_feature(result, "barrelRotation");
  }
  if ((features & kFeatureTangentialPressure) != 0) {
    append_feature(result, "tangentialPressure");
  }
  return result;
}

/** Emits one GTK device description or connection change. */
static void send_gdk_device_packet(StyletPlugin* self, const GdkEvent* event,
                                   const gchar* phase, const gchar* kind,
                                   const gchar* identifier, const gchar* tool,
                                   const GdkDeviceState* state) {
  if (!self->listening || self->event_channel == nullptr) {
    return;
  }
  GdkDevice* device = gdk_event_get_source_device(event);
  g_autoptr(FlValue) packet = fl_value_new_map();
  fl_value_set_string_take(packet, "type", fl_value_new_string("device"));
  fl_value_set_string_take(
      packet, "timestampMicros",
      fl_value_new_int(static_cast<gint64>(gdk_event_get_time(event)) * 1000));
  fl_value_set_string_take(packet, "phase", fl_value_new_string(phase));
  fl_value_set_string_take(packet, "kind", fl_value_new_string(kind));
  fl_value_set_string_take(packet, "nativeDeviceIdentifier",
                           fl_value_new_string(identifier));
  if (device != nullptr) {
    fl_value_set_string_take(packet, "name",
                             fl_value_new_string(gdk_device_get_name(device)));
    const guint64 vendor =
        parse_device_identifier(gdk_device_get_vendor_id(device));
    const guint64 product =
        parse_device_identifier(gdk_device_get_product_id(device));
    if (vendor != 0) {
      fl_value_set_string_take(packet, "vendorIdentifier",
                               fl_value_new_int(static_cast<gint64>(vendor)));
    }
    if (product != 0) {
      fl_value_set_string_take(packet, "productIdentifier",
                               fl_value_new_int(static_cast<gint64>(product)));
    }
  }
  if (tool != nullptr) {
    fl_value_set_string_take(packet, "tool", fl_value_new_string(tool));
  }
  if (state->button_count != 0) {
    fl_value_set_string_take(
        packet, "buttonCount",
        fl_value_new_int(static_cast<gint64>(state->button_count)));
  }
  fl_value_set_string_take(
      packet, "features",
      device_feature_list(state->features, strcmp(kind, "pad") == 0));

  g_autoptr(GError) error = nullptr;
  if (!fl_event_channel_send(self->event_channel, packet, nullptr, &error)) {
    g_warning("Failed to send a Stylet device event: %s",
              error == nullptr ? "unknown error" : error->message);
  }
}

/** Announces or updates one GTK tablet tool and handles proximity removal. */
static void update_gdk_tool(StyletPlugin* self, const GdkEvent* event,
                            const gchar* identifier, const gchar* phase) {
  GdkDeviceState* state = static_cast<GdkDeviceState*>(
      g_hash_table_lookup(self->announced_devices, identifier));
  if (strcmp(phase, "removed") == 0) {
    if (state != nullptr) {
      send_gdk_device_packet(self, event, "removed", "tool", identifier,
                             tool_name(event), state);
      g_hash_table_remove(self->announced_devices, identifier);
    }
    return;
  }

  const guint features =
      gdk_device_features(gdk_event_get_source_device(event));
  if (state == nullptr) {
    state = g_new0(GdkDeviceState, 1);
    state->features = features;
    state->button_count = 2;
    g_hash_table_insert(self->announced_devices, g_strdup(identifier), state);
    send_gdk_device_packet(self, event, "added", "tool", identifier,
                           tool_name(event), state);
  } else if ((features & ~state->features) != 0) {
    state->features |= features;
    send_gdk_device_packet(self, event, "changed", "tool", identifier,
                           tool_name(event), state);
  }
}

/** Converts GTK button state to Flutter's public stylus button bit field. */
static gint64 flutter_buttons(guint state, gboolean is_down) {
  gint64 buttons = is_down ? kFlutterPrimaryButton : 0;
  if ((state & GDK_BUTTON2_MASK) != 0) {
    buttons |= kFlutterPrimaryStylusButton;
  }
  if ((state & GDK_BUTTON3_MASK) != 0) {
    buttons |= kFlutterSecondaryStylusButton;
  }
  return buttons;
}

/** Sends one normalized motion packet to Dart when the stream is active. */
static void send_motion(StyletPlugin* self, const GdkEvent* event,
                        const gchar* phase, gdouble x, gdouble y, guint state,
                        gboolean is_down) {
  if (!self->listening || self->event_channel == nullptr ||
      !is_stylus_event(event)) {
    return;
  }

  const guint64 tool_serial = get_tool_serial(event);
  const gboolean same_tool =
      self->has_last_position && self->last_tool_serial == tool_serial;
  g_autoptr(FlValue) packet = fl_value_new_map();
  fl_value_set_string_take(packet, "type", fl_value_new_string("motion"));
  fl_value_set_string_take(
      packet, "timestampMicros",
      fl_value_new_int(static_cast<gint64>(gdk_event_get_time(event)) * 1000));
  fl_value_set_string_take(packet, "phase", fl_value_new_string(phase));
  fl_value_set_string_take(packet, "tool", fl_value_new_string(tool_name(event)));
  fl_value_set_string_take(packet, "pointerIdentifier",
                           fl_value_new_int(static_cast<gint64>(tool_serial)));
  GdkDevice* device = gdk_event_get_source_device(event);
  fl_value_set_string_take(
      packet, "deviceIdentifier",
      fl_value_new_int(device == nullptr
                           ? 0
                           : static_cast<gint64>(g_str_hash(gdk_device_get_name(device)))));
  g_autofree gchar* identifier = native_device_identifier(event, tool_serial);
  fl_value_set_string_take(packet, "nativeDeviceIdentifier",
                           fl_value_new_string(identifier));
  const gboolean is_removing = strcmp(phase, "removed") == 0;
  if (!is_removing) {
    update_gdk_tool(self, event, identifier, phase);
  }
  set_float(packet, "x", x);
  set_float(packet, "y", y);
  set_float(packet, "deltaX", same_tool ? x - self->last_x : 0.0);
  set_float(packet, "deltaY", same_tool ? y - self->last_y : 0.0);
  fl_value_set_string_take(packet, "buttons",
                           fl_value_new_int(flutter_buttons(state, is_down)));
  fl_value_set_string_take(packet, "isDown", fl_value_new_bool(is_down));

  g_autoptr(FlValue) features = fl_value_new_list();
  append_feature(features, "primaryButton");
  append_feature(features, "secondaryButton");
  append_feature(features, "eraser");
  append_feature(features, "hover");

  gdouble pressure = 0;
  if (get_axis(event, GDK_AXIS_PRESSURE, &pressure)) {
    set_float(packet, "pressure", pressure);
    set_float(packet, "pressureMinimum", 0.0);
    set_float(packet, "pressureMaximum", 1.0);
    append_feature(features, "pressure");
  }

  gdouble tilt_x_value = 0;
  gdouble tilt_y_value = 0;
  const gboolean has_tilt_x = get_axis(event, GDK_AXIS_XTILT, &tilt_x_value);
  const gboolean has_tilt_y = get_axis(event, GDK_AXIS_YTILT, &tilt_y_value);
  if (has_tilt_x || has_tilt_y) {
    const gdouble tilt_x = tilt_axis_to_radians(tilt_x_value);
    const gdouble tilt_y = tilt_axis_to_radians(tilt_y_value);
    set_float(packet, "tiltX", tilt_x);
    set_float(packet, "tiltY", tilt_y);
    set_float(packet, "tilt", std::atan(std::hypot(std::tan(tilt_x),
                                                    std::tan(tilt_y))));
    set_float(packet, "orientation", std::atan2(std::tan(tilt_x),
                                                 -std::tan(tilt_y)));
    append_feature(features, "tilt");
    append_feature(features, "orientation");
  }

  gdouble distance = 0;
  if (get_axis(event, GDK_AXIS_DISTANCE, &distance)) {
    set_float(packet, "distance", distance);
    set_float(packet, "distanceMaximum", 1.0);
    append_feature(features, "distance");
  }

  gdouble rotation = 0;
  if (get_axis(event, GDK_AXIS_ROTATION, &rotation)) {
    set_float(packet, "barrelRotation", rotation_to_radians(rotation));
    append_feature(features, "barrelRotation");
  }

  gdouble slider = 0;
  if (get_axis(event, GDK_AXIS_SLIDER, &slider) ||
      get_axis(event, GDK_AXIS_WHEEL, &slider)) {
    set_float(packet, "tangentialPressure", CLAMP(slider, -1.0, 1.0));
    append_feature(features, "tangentialPressure");
  }
  fl_value_set_string_take(packet, "features", fl_value_ref(features));

  g_autoptr(GError) error = nullptr;
  if (!fl_event_channel_send(self->event_channel, packet, nullptr, &error)) {
    g_warning("Failed to send a Stylet event: %s",
              error == nullptr ? "unknown error" : error->message);
  }
  if (is_removing) {
    update_gdk_tool(self, event, identifier, phase);
  }

  if (strcmp(phase, "up") == 0 || strcmp(phase, "cancel") == 0 ||
      strcmp(phase, "removed") == 0) {
    self->has_last_position = FALSE;
  } else {
    self->has_last_position = TRUE;
    self->last_x = x;
    self->last_y = y;
    self->last_tool_serial = tool_serial;
  }
}

/** Observes a GTK tablet motion event without consuming it. */
static gboolean motion_event_cb(GtkWidget* /*widget*/, GdkEventMotion* event,
                                gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  if (uses_wayland_backend(self)) {
    return FALSE;
  }
  const gboolean is_down = (event->state & GDK_BUTTON1_MASK) != 0;
  send_motion(self, reinterpret_cast<GdkEvent*>(event),
              is_down ? "move" : "hover", event->x, event->y, event->state,
              is_down);
  return FALSE;
}

/** Observes a GTK tablet button event without consuming it. */
static gboolean button_event_cb(GtkWidget* /*widget*/, GdkEventButton* event,
                                gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  if (uses_wayland_backend(self)) {
    return FALSE;
  }
  const gboolean pressed = event->type == GDK_BUTTON_PRESS;
  guint state = event->state;
  if (event->button == 1 && pressed) {
    state |= GDK_BUTTON1_MASK;
  } else if (event->button == 1) {
    state &= ~GDK_BUTTON1_MASK;
  } else if (event->button == 2 && pressed) {
    state |= GDK_BUTTON2_MASK;
  } else if (event->button == 2) {
    state &= ~GDK_BUTTON2_MASK;
  } else if (event->button == 3 && pressed) {
    state |= GDK_BUTTON3_MASK;
  } else if (event->button == 3) {
    state &= ~GDK_BUTTON3_MASK;
  }
  const gboolean is_down = (state & GDK_BUTTON1_MASK) != 0;
  const gchar* phase =
      event->button == 1 ? (pressed ? "down" : "up")
                         : (is_down ? "move" : "hover");
  send_motion(self, reinterpret_cast<GdkEvent*>(event), phase, event->x,
              event->y, state, is_down);
  return FALSE;
}

/** Observes GTK tablet proximity changes without consuming them. */
static gboolean proximity_event_cb(GtkWidget* /*widget*/, GdkEventProximity* event,
                                   gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  if (uses_wayland_backend(self)) {
    return FALSE;
  }
  const gchar* phase =
      event->type == GDK_PROXIMITY_IN ? "added" : "removed";
  send_motion(self, reinterpret_cast<GdkEvent*>(event), phase,
              self->has_last_position ? self->last_x : 0.0,
              self->has_last_position ? self->last_y : 0.0, 0, FALSE);
  return FALSE;
}

/** Announces a GTK tablet pad or expands its known control description. */
static GdkDeviceState* update_gdk_pad(StyletPlugin* self,
                                      const GdkEvent* event,
                                      const gchar* identifier,
                                      guint feature, guint button_count) {
  GdkDeviceState* state = static_cast<GdkDeviceState*>(
      g_hash_table_lookup(self->announced_devices, identifier));
  const gboolean is_new = state == nullptr;
  if (is_new) {
    state = g_new0(GdkDeviceState, 1);
    g_hash_table_insert(self->announced_devices, g_strdup(identifier), state);
  }
  const gboolean changed =
      (feature & ~state->features) != 0 || button_count > state->button_count;
  state->features |= feature;
  state->button_count = MAX(state->button_count, button_count);
  if (is_new || changed) {
    send_gdk_device_packet(self, event, is_new ? "added" : "changed", "pad",
                           identifier, nullptr, state);
  }
  return state;
}

/** Sends one GTK graphics-tablet pad control packet. */
static void send_gdk_pad_packet(StyletPlugin* self, const GdkEvent* event,
                                const gchar* identifier, const gchar* control,
                                guint index, const gchar* phase,
                                const gdouble* value, guint mode) {
  g_autoptr(FlValue) packet = fl_value_new_map();
  fl_value_set_string_take(packet, "type", fl_value_new_string("pad"));
  fl_value_set_string_take(
      packet, "timestampMicros",
      fl_value_new_int(static_cast<gint64>(gdk_event_get_time(event)) * 1000));
  fl_value_set_string_take(packet, "nativeDeviceIdentifier",
                           fl_value_new_string(identifier));
  fl_value_set_string_take(packet, "control", fl_value_new_string(control));
  fl_value_set_string_take(packet, "controlIndex",
                           fl_value_new_int(static_cast<gint64>(index)));
  fl_value_set_string_take(packet, "phase", fl_value_new_string(phase));
  if (value != nullptr) {
    set_float(packet, "value", CLAMP(*value, 0.0, 1.0));
  }
  fl_value_set_string_take(packet, "mode",
                           fl_value_new_int(static_cast<gint64>(mode)));
  g_autoptr(GError) error = nullptr;
  if (!fl_event_channel_send(self->event_channel, packet, nullptr, &error)) {
    g_warning("Failed to send a Stylet tablet-pad event: %s",
              error == nullptr ? "unknown error" : error->message);
  }
}

/** Observes GTK tablet-pad events without consuming them. */
static gboolean pad_event_cb(GtkWidget* /*widget*/, GdkEvent* event,
                             gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  if (!self->listening || uses_wayland_backend(self)) {
    return FALSE;
  }
  if (event->type != GDK_PAD_BUTTON_PRESS &&
      event->type != GDK_PAD_BUTTON_RELEASE &&
      event->type != GDK_PAD_RING && event->type != GDK_PAD_STRIP &&
      event->type != GDK_PAD_GROUP_MODE) {
    return FALSE;
  }

  g_autofree gchar* identifier = native_pad_identifier(event);
  if (event->type == GDK_PAD_BUTTON_PRESS ||
      event->type == GDK_PAD_BUTTON_RELEASE) {
    const GdkEventPadButton* pad = &event->pad_button;
    update_gdk_pad(self, event, identifier, kFeaturePadButtons,
                   pad->button + 1);
    send_gdk_pad_packet(self, event, identifier, "button", pad->button,
                        event->type == GDK_PAD_BUTTON_PRESS ? "began" : "ended",
                        nullptr, pad->mode);
  } else if (event->type == GDK_PAD_RING || event->type == GDK_PAD_STRIP) {
    const GdkEventPadAxis* pad = &event->pad_axis;
    const gboolean is_ring = event->type == GDK_PAD_RING;
    update_gdk_pad(self, event, identifier,
                   is_ring ? kFeaturePadRing : kFeaturePadStrip, 0);
    send_gdk_pad_packet(self, event, identifier,
                        is_ring ? "ring" : "strip", pad->index, "changed",
                        &pad->value, pad->mode);
  } else {
    const GdkEventPadGroupMode* pad = &event->pad_group_mode;
    update_gdk_pad(self, event, identifier, 0, 0);
    send_gdk_pad_packet(self, event, identifier, "mode", pad->group,
                        "discrete", nullptr, pad->mode);
  }
  return FALSE;
}

/** Starts native event delivery for the Dart event-channel subscriber. */
static FlMethodErrorResponse* listen_cb(FlEventChannel* /*channel*/,
                                        FlValue* /*args*/,
                                        gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  self->listening = TRUE;
#ifdef STYLET_HAS_WAYLAND
  if (self->wayland_backend != nullptr) {
    stylet_wayland_backend_start(self->wayland_backend);
  }
#endif
  return nullptr;
}

/** Stops native event delivery after the Dart subscriber cancels. */
static FlMethodErrorResponse* cancel_cb(FlEventChannel* /*channel*/,
                                        FlValue* /*args*/,
                                        gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  self->listening = FALSE;
  self->has_last_position = FALSE;
  g_hash_table_remove_all(self->announced_devices);
#ifdef STYLET_HAS_WAYLAND
  stylet_wayland_backend_stop(self->wayland_backend);
#endif
  return nullptr;
}

FlMethodResponse* stylet_get_capabilities() {
  const gchar* features[] = {
      "pressure",          "tilt",           "orientation",
      "distance",          "barrelRotation", "tangentialPressure",
      "primaryButton",     "secondaryButton", "eraser",
      "hover",             "deviceInfo",     "tabletPadButtons",
      "tabletPadRing",     "tabletPadStrip",
#ifdef STYLET_HAS_WAYLAND
      "wheel",             "tabletPadDial",
#endif
      nullptr,
  };
  g_autoptr(FlValue) result = fl_value_new_list_from_strv(features);
  return FL_METHOD_RESPONSE(fl_method_success_response_new(result));
}

/** Handles one request from Stylet's Dart method channel. */
static void handle_method_call(StyletPlugin* self,
                               FlMethodCall* method_call) {
  g_autoptr(FlMethodResponse) response = nullptr;
  if (strcmp(fl_method_call_get_name(method_call), "getCapabilities") == 0) {
    response = stylet_get_capabilities();
  } else {
    response = FL_METHOD_RESPONSE(fl_method_not_implemented_response_new());
  }
  fl_method_call_respond(method_call, response, nullptr);
}

/** Adapts the method-channel callback to the plugin instance. */
static void method_call_cb(FlMethodChannel* /*channel*/,
                           FlMethodCall* method_call,
                           gpointer user_data) {
  handle_method_call(STYLET_PLUGIN(user_data), method_call);
}

/** Disconnects one GTK signal handler if it is active. */
static void disconnect_handler(gpointer instance, gulong* handler) {
  if (instance != nullptr && *handler != 0) {
    g_signal_handler_disconnect(instance, *handler);
    *handler = 0;
  }
}

/** Releases channels, the retained view, and every GTK signal handler. */
static void stylet_plugin_dispose(GObject* object) {
  StyletPlugin* self = STYLET_PLUGIN(object);
#ifdef STYLET_HAS_WAYLAND
  stylet_wayland_backend_free(self->wayland_backend);
  self->wayland_backend = nullptr;
#endif
  disconnect_handler(self->view, &self->motion_handler);
  disconnect_handler(self->view, &self->button_press_handler);
  disconnect_handler(self->view, &self->button_release_handler);
  disconnect_handler(self->view, &self->proximity_in_handler);
  disconnect_handler(self->view, &self->proximity_out_handler);
  disconnect_handler(self->view, &self->pad_event_handler);
  g_clear_pointer(&self->announced_devices, g_hash_table_unref);
  g_clear_object(&self->view);
  g_clear_object(&self->method_channel);
  g_clear_object(&self->event_channel);
  G_OBJECT_CLASS(stylet_plugin_parent_class)->dispose(object);
}

/** Installs the GObject disposal implementation. */
static void stylet_plugin_class_init(StyletPluginClass* klass) {
  G_OBJECT_CLASS(klass)->dispose = stylet_plugin_dispose;
}

/** Initializes nullable handles and correlation state. */
static void stylet_plugin_init(StyletPlugin* self) {
  self->announced_devices =
      g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
}

void stylet_plugin_register_with_registrar(FlPluginRegistrar* registrar) {
  StyletPlugin* plugin =
      STYLET_PLUGIN(g_object_new(stylet_plugin_get_type(), nullptr));
  FlBinaryMessenger* messenger = fl_plugin_registrar_get_messenger(registrar);
  g_autoptr(FlStandardMethodCodec) method_codec =
      fl_standard_method_codec_new();

  plugin->method_channel = fl_method_channel_new(
      messenger, kMethodChannelName, FL_METHOD_CODEC(method_codec));
  fl_method_channel_set_method_call_handler(
      plugin->method_channel, method_call_cb, g_object_ref(plugin),
      g_object_unref);

  plugin->event_channel = fl_event_channel_new(
      messenger, kEventChannelName, FL_METHOD_CODEC(method_codec));
  fl_event_channel_set_stream_handlers(plugin->event_channel, listen_cb,
                                       cancel_cb, g_object_ref(plugin),
                                       g_object_unref);

  FlView* view = fl_plugin_registrar_get_view(registrar);
  if (view != nullptr) {
    plugin->view = FL_VIEW(g_object_ref(view));
#ifdef STYLET_HAS_WAYLAND
    plugin->wayland_backend =
        stylet_wayland_backend_new(GTK_WIDGET(view), plugin->event_channel);
#endif
    gtk_widget_add_events(
        GTK_WIDGET(view),
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
            GDK_BUTTON_RELEASE_MASK | GDK_PROXIMITY_IN_MASK |
            GDK_PROXIMITY_OUT_MASK | GDK_TABLET_PAD_MASK);
    plugin->pad_event_handler =
        g_signal_connect(view, "event", G_CALLBACK(pad_event_cb), plugin);
    plugin->motion_handler =
        g_signal_connect(view, "motion-notify-event", G_CALLBACK(motion_event_cb),
                         plugin);
    plugin->button_press_handler =
        g_signal_connect(view, "button-press-event", G_CALLBACK(button_event_cb),
                         plugin);
    plugin->button_release_handler =
        g_signal_connect(view, "button-release-event",
                         G_CALLBACK(button_event_cb), plugin);
    plugin->proximity_in_handler =
        g_signal_connect(view, "proximity-in-event",
                         G_CALLBACK(proximity_event_cb), plugin);
    plugin->proximity_out_handler =
        g_signal_connect(view, "proximity-out-event",
                         G_CALLBACK(proximity_event_cb), plugin);
  }

  g_object_unref(plugin);
}
