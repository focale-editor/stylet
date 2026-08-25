# Stylet example

An interactive stylus laboratory for the Stylet Flutter plugin. It displays
the active backend capabilities and visualizes pressure, tilt, azimuth, barrel
rotation, side buttons, double-tap, and squeeze events.

Run it on the current desktop or a connected device:

```console
flutter run
```

Values that the current hardware or driver does not expose remain blank. The
example deliberately uses `StyletListener` around a `CustomPaint` canvas so it
also serves as a compact integration reference.
