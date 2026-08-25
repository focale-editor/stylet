# 📰 Stylet changelog

## 0.1.0

- Added a normalized, immutable Dart event and capability model.
- Added passive native stylus backends for Android, iOS, Linux, macOS, and
  Windows.
- Added barrel rotation, tangential pressure, hover, side-button, eraser,
  Apple Pencil double-tap, and Apple Pencil squeeze support where available.
- Added native-to-Flutter sample correlation and the `StyletListener` widget.
- Added native device descriptions, connection tracking, and tablet-pad events.
- Added chronological Windows Ink history batches and optional runtime Wintab
  tangential-pressure enrichment.
- Added a direct optional Wayland tablet-v2 backend with GTK/XInput fallback,
  including version 2 relative pad dials.
- Added AppKit device metadata and high-rate uncoalesced macOS observation.
- Added an interactive example, platform-channel tests, controller tests, and
  native capability tests.
