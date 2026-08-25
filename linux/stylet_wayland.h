#ifndef FLUTTER_PLUGIN_STYLET_WAYLAND_H_
#define FLUTTER_PLUGIN_STYLET_WAYLAND_H_

#include <flutter_linux/flutter_linux.h>
#include <gtk/gtk.h>

/** Opaque optional backend for the Wayland tablet-v2 protocol. */
typedef struct _StyletWaylandBackend StyletWaylandBackend;

/** Creates a stopped Wayland backend for a Flutter view and event channel. */
StyletWaylandBackend* stylet_wayland_backend_new(
    GtkWidget* view, FlEventChannel* event_channel);

/** Binds tablet-v2 for the current Wayland seat when the protocol is present. */
gboolean stylet_wayland_backend_start(StyletWaylandBackend* backend);

/** Whether tablet-v2 currently supplies motion in place of GDK events. */
gboolean stylet_wayland_backend_is_active(
    const StyletWaylandBackend* backend);

/** Releases tablet-v2 objects while preserving the reusable backend wrapper. */
void stylet_wayland_backend_stop(StyletWaylandBackend* backend);

/** Releases the optional Wayland backend and every retained native object. */
void stylet_wayland_backend_free(StyletWaylandBackend* backend);

#endif  // FLUTTER_PLUGIN_STYLET_WAYLAND_H_
