# Stylet

High-fidelity stylus input for Flutter on Android, iOS, Linux, macOS, and
Windows.

Stylet keeps Flutter's regular `PointerEvent` pipeline intact and enriches it
with native values that Flutter does not expose everywhere: barrel rotation,
tangential pressure, Apple Pencil double-tap and squeeze, native tilt axes, and
device identifiers. It is designed for drawing applications such as Focale,
but does not impose a canvas, brush engine, or state-management solution.

## Installation

Add Stylet to the consuming application's `pubspec.yaml`:

```shell
flutter pub add stylet
```

Then run `flutter pub get`. A hosted or Git dependency can replace the local
path without changing the API.

## Quick start

Wrap an interactive canvas with `StyletListener`. The callback receives a
normalized sample for every stylus pointer event, while the child continues to
receive its original Flutter events.

```dart
import 'package:flutter/widgets.dart';
import 'package:stylet/stylet.dart';

StyletListener(
  behavior: HitTestBehavior.opaque,
  onEvent: (StylusMotionEvent event) {
    final double pressure = event.normalizedPressure ?? 1;
    final double tilt = event.tilt ?? 0;
    final double orientation = event.orientation ?? 0;
    final double barrelRotation = event.barrelRotation ?? 0;

    updateBrush(
      position: event.localPosition,
      pressure: pressure,
      tilt: tilt,
      orientation: orientation,
      barrelRotation: barrelRotation,
    );
  },
  onAction: (StylusActionEvent event) {
    if (event.action == StylusAction.squeeze &&
        event.phase == StylusActionPhase.began) {
      showToolPalette(at: event.pose?.position);
    }
  },
  child: const DocumentCanvas(),
)
```

`StyletListener` ignores mouse and touch input by default. Set
`includeNonStylus: true` when one callback should normalize every pointer kind.
Do not dispose `Stylet.instance`; create and dispose a dedicated `Stylet`
controller only when an isolated lifecycle is needed.

## Integrating an existing `Listener`

If your editor already handles raw events in its document canvas, you can retain that
listener and replace its local pressure/tilt extraction with one conversion:

```dart
final Stylet stylet = Stylet.instance;

void onPointerMove(PointerMoveEvent pointerEvent) {
  final StylusMotionEvent event = stylet.convertPointerEvent(
    event: pointerEvent,
  );
  brushController.extendStroke(
    position: event.localPosition,
    pressure: event.normalizedPressure ?? 1,
    tilt: event.tilt ?? 0,
    orientation: event.orientation ?? 0,
    barrelRotation: event.barrelRotation ?? 0,
  );
}
```

The controller maintains a short native-event cache and correlates samples by
embedder identifier when available, then by timestamp, tool, phase, and
position. `event.source == StyletEventSource.combined` indicates that a Flutter
event received native extensions.

Stylus body interactions are independent of pointer movement:

```dart
final StreamSubscription<StylusActionEvent> subscription =
    Stylet.instance.actions.listen(handleStylusAction);

// Cancel this application-owned subscription with its lifecycle.
await subscription.cancel();
```

## API model

`StylusMotionEvent` uses consistent semantics across platforms:

- positions and deltas are logical pixels relative to the Flutter view;
- pressure and distance retain their device ranges, with normalized getters;
- tilt is the angle in radians away from the surface normal;
- orientation is the direction in radians of the projected stylus axis;
- `tiltX` and `tiltY` are optional signed components in radians;
- barrel rotation is clockwise radians around the stylus axis;
- tangential pressure is normalized from -1 to 1;
- buttons use Flutter's public button bit field; use
  `isSideButtonPressed(number: 1)` instead of hard-coded masks.

`StylusCapabilities` describes what the current backend can potentially
provide. The `features` set on an individual motion sample is more precise and
should be used when behavior depends on a value being present.

## Platform support

| Feature                     | Android          | iOS/iPadOS                  | Linux             | macOS                  | Windows          |
|-----------------------------|------------------|-----------------------------|-------------------|------------------------|------------------|
| Pressure, tilt, orientation | Yes              | Yes                         | Yes               | Yes                    | Yes              |
| Hover pose                  | Yes              | iPadOS 16.1+                | Yes               | Yes                    | Yes              |
| Side buttons                | Yes              | —                           | Yes               | Yes                    | Yes              |
| Eraser tool                 | Yes              | —                           | Yes               | Yes                    | Yes              |
| Barrel rotation             | Driver `AXIS_RZ` | iOS 17.5+                   | GTK rotation axis | AppKit rotation        | Pen API rotation |
| Tangential pressure         | —                | —                           | GTK slider/wheel  | AppKit barrel pressure | —                |
| Double-tap                  | —                | Apple Pencil                | —                 | —                      | —                |
| Squeeze                     | —                | iOS 17.5+, Apple Pencil Pro | —                 | —                      | —                |

Hardware and tablet drivers determine whether an advertised axis produces
meaningful values. Every backend observes input passively and returns the
native event unchanged, so Flutter's gesture arena remains authoritative.

The iOS deployment target is 15.0 and the macOS deployment target is 12.0.
Other targets follow the minimum versions supported by the current Flutter
toolchain.

## Native event contract

Backend maintainers can find channel names, required packet fields, units, and
lifecycle rules in [`docs/platform_contract.md`](docs/platform_contract.md).
The runnable application in [`example/`](example/) visualizes capabilities,
pressure, tilt, barrel angle, side buttons, and body actions.

## Development

```console
dart format .
dart analyze
flutter test
```

Native compilation is additionally checked through the example application on
each supported host platform.
