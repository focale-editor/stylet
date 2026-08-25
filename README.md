# Stylet

High-fidelity stylus input for Flutter on Android, iOS, Linux, macOS, Web, and
Windows.

Stylet keeps Flutter's regular `PointerEvent` pipeline intact and enriches it
with native values that Flutter does not expose everywhere: barrel rotation,
tangential pressure, Apple Pencil double-tap and squeeze, native tilt axes, and
device identifiers. Replaceable motion predictions and corrections for
initially estimated Apple Pencil values are exposed separately from definitive
input. On the Web, Pointer Events add raw/coalesced samples, tangential pressure,
rotation, and browser-generated predictions; the experimental Ink API is an
optional compositor trail. Stylet is designed for drawing applications such as
Focale, but does not impose a canvas, brush engine, or state-management
solution.

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
  onPrediction: (StylusPredictionEvent event) {
    replacePredictedPath(event.pointerIdentifier, event.samples);
  },
  onCorrection: (StylusCorrectionEvent event) {
    replaceRecordedSample(event.sampleIdentifier, event.correctedSample);
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
final StreamSubscription<StylusActionEvent> subscription = Stylet.instance.actions.listen(handleStylusAction);

// Cancel this application-owned subscription with its lifecycle.
await subscription.cancel();
```

Predictions are temporary previews. Each event replaces the preceding preview
for its pointer, including an empty sample list that clears it:

```dart
final StreamSubscription<StylusPredictionEvent> predictions =
    Stylet.instance.predictions.listen((event) {
  replacePredictedPath(
    pointerIdentifier: event.pointerIdentifier,
    samples: event.samples,
  );
});

final StreamSubscription<StylusCorrectionEvent> corrections =
    Stylet.instance.corrections.listen((event) {
  replaceRecordedSample(
    identifier: event.sampleIdentifier,
    sample: event.correctedSample,
  );
});
```

Cancel both application-owned subscriptions with their lifecycle. Prediction
positions use Flutter-view coordinates like other native events; transform them
through the receiving render object when drawing inside a nested widget.

Web applications can additionally opt into a browser-composited delegated ink
trail. It is a visual latency optimization, not a replacement for authoritative
`StyletListener` samples. See [`docs/web_support.md`](docs/web_support.md) for
feature detection, setup, and lifecycle details.

## API model

`StylusMotionEvent` uses consistent semantics across platforms:

- positions and deltas are logical pixels relative to the Flutter view;
- pressure and distance retain their device ranges, with normalized getters;
- tilt is the angle in radians away from the surface normal;
- orientation is the direction in radians of the projected stylus axis;
- `tiltX` and `tiltY` are optional signed components in radians;
- barrel rotation is clockwise radians around the stylus axis;
- tangential pressure is normalized from -1 to 1;
- `wheelDelta` is signed relative stylus-wheel movement in radians;
- buttons use Flutter's public button bit field; use
  `isSideButtonPressed(number: 1)` instead of hard-coded masks;
- `sampleIdentifier` correlates an estimated sample with a later correction;
- estimated properties and properties still expecting updates are distinct,
  because a value can remain estimated after the platform stops refining it.

`StylusCapabilities` describes what the current backend can potentially
provide. The `features` set on an individual motion sample is more precise and
should be used when behavior depends on a value being present.

Native device and tablet-pad events are available independently of motion:

```dart
final StreamSubscription<StylusDeviceEvent> devices = Stylet.instance.deviceEvents.listen(handleDeviceChange);
final StreamSubscription<TabletPadEvent> controls = Stylet.instance.padEvents.listen(handleTabletControl);

final Map<String, StylusDevice> connected = Stylet.instance.connectedDevices;
```

Cancel application-owned subscriptions with their lifecycle. Linux reports
pad buttons, rings, strips, and mapping modes through Wayland tablet-v2 or GTK;
relative dials require tablet-v2 version 2.

## Platform support

| Feature                     | Android          | iOS/iPadOS                  | Linux                       | macOS                  | Web                         | Windows                     |
|-----------------------------|------------------|-----------------------------|-----------------------------|------------------------|-----------------------------|-----------------------------|
| Pressure, tilt, orientation | Yes              | Yes                         | Yes                         | Yes                    | Pointer Events              | Yes                         |
| Hover pose                  | Yes              | iPadOS 16.1+                | Yes                         | Yes                    | Pointer Events              | Yes                         |
| Side buttons                | Yes              | —                           | Yes                         | Yes                    | First barrel button         | Yes                         |
| Eraser tool                 | Yes              | —                           | Yes                         | Yes                    | Pointer Events              | Yes                         |
| Barrel rotation             | Driver `AXIS_RZ` | iOS 17.5+                   | GTK or tablet-v2            | AppKit rotation        | `PointerEvent.twist`        | Windows Ink                 |
| Tangential pressure         | —                | —                           | GTK or tablet-v2            | AppKit barrel pressure | Pointer Events              | Wintab driver, when present |
| Stylus wheel                | —                | —                           | Wayland tablet-v2           | —                      | —                           | —                           |
| High-rate sample delivery   | Motion history   | Coalesced touches            | Protocol frames             | Coalescing disabled    | Coalesced/raw updates       | Windows Ink history batches |
| Predicted trajectories      | AndroidX Input   | UIKit                       | —                           | —                      | Browser, when available     | Experimental opt-in         |
| Delegated compositor trail  | —                | —                           | —                           | —                      | Experimental Ink API       | —                           |
| Estimated-value corrections | —                | UIKit                       | —                           | —                      | —                           | —                           |
| Device identity / metadata  | InputManager     | —                           | GTK or tablet-v2            | AppKit                 | PE4 session ID, if available | Windows Ink              |
| Pad buttons, rings, strips  | —                | —                           | GTK or tablet-v2            | —                      | —                           | —                           |
| Relative pad dials          | —                | —                           | Tablet-v2 version 2         | —                      | —                           | —                           |
| Double-tap                  | —                | Apple Pencil                | —                           | —                      | —                           | —                           |
| Squeeze                     | —                | iOS 17.5+, Apple Pencil Pro | —                           | —                      | —                           | —                           |

Hardware and tablet drivers determine whether an advertised axis produces
meaningful values. Every backend observes input passively and returns the
native event unchanged, so Flutter's gesture arena remains authoritative.

The iOS deployment target is 15.0 and the macOS deployment target is 12.0.
Other targets follow the minimum versions supported by the current Flutter
toolchain.

## Native event contract

Backend maintainers can find channel names, required packet fields, units, and
lifecycle rules in [`docs/platform_contract.md`](docs/platform_contract.md).
Native dependencies and fallback behavior are detailed in
[`docs/native_backends.md`](docs/native_backends.md).
Web Pointer Events and delegated ink behavior are detailed in
[`docs/web_support.md`](docs/web_support.md).
The runnable application in [`example/`](example/) visualizes capabilities,
pressure, tilt, barrel angle, stylus-wheel movement, side buttons, body
actions, predictions, corrections, native devices, and tablet-pad controls.
