# Stylet example

An interactive stylus laboratory for the Stylet Flutter plugin. It displays
the active backend capabilities and visualizes pressure, tilt, azimuth, barrel
rotation, stylus wheels, side buttons, double-tap, squeeze, native device
changes, and tablet-pad controls.

Run it on the current desktop or a connected device:

```console
flutter run
```

Values that the current hardware or driver does not expose remain blank. The
example deliberately uses `StyletListener` around a `CustomPaint` canvas so it
also serves as a compact integration reference.
