#include "include/stylet/stylet_plugin.h"

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

#include <cmath>
#include <cstdint>
#include <cstring>

#include "stylet_plugin_private.h"

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
    g_warning("Failed to send a Stylet event: %s", error->message);
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
  const gchar* phase =
      event->type == GDK_PROXIMITY_IN ? "added" : "removed";
  send_motion(self, reinterpret_cast<GdkEvent*>(event), phase,
              self->has_last_position ? self->last_x : 0.0,
              self->has_last_position ? self->last_y : 0.0, 0, FALSE);
  return FALSE;
}

/** Starts native event delivery for the Dart event-channel subscriber. */
static FlMethodErrorResponse* listen_cb(FlEventChannel* /*channel*/,
                                        FlValue* /*args*/,
                                        gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  self->listening = TRUE;
  return nullptr;
}

/** Stops native event delivery after the Dart subscriber cancels. */
static FlMethodErrorResponse* cancel_cb(FlEventChannel* /*channel*/,
                                        FlValue* /*args*/,
                                        gpointer user_data) {
  StyletPlugin* self = STYLET_PLUGIN(user_data);
  self->listening = FALSE;
  self->has_last_position = FALSE;
  return nullptr;
}

FlMethodResponse* stylet_get_capabilities() {
  const gchar* features[] = {
      "pressure",          "tilt",           "orientation",
      "distance",          "barrelRotation", "tangentialPressure",
      "primaryButton",     "secondaryButton", "eraser",
      "hover",             nullptr,
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
  disconnect_handler(self->view, &self->motion_handler);
  disconnect_handler(self->view, &self->button_press_handler);
  disconnect_handler(self->view, &self->button_release_handler);
  disconnect_handler(self->view, &self->proximity_in_handler);
  disconnect_handler(self->view, &self->proximity_out_handler);
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
static void stylet_plugin_init(StyletPlugin* /*self*/) {}

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
    gtk_widget_add_events(
        GTK_WIDGET(view),
        GDK_POINTER_MOTION_MASK | GDK_BUTTON_PRESS_MASK |
            GDK_BUTTON_RELEASE_MASK | GDK_PROXIMITY_IN_MASK |
            GDK_PROXIMITY_OUT_MASK);
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
