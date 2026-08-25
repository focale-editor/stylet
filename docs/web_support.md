# Web stylus input and delegated ink

Stylet registers a federated Web backend automatically. Normal application code
continues to import `package:stylet/stylet.dart` and use `StyletListener`,
`nativeMotions`, and `predictions` on every platform.

## Pointer Events pipeline

The backend filters DOM events to `pointerType == 'pen'` and detects optional
browser members at runtime:

- `getCoalescedEvents()` preserves measured samples grouped by the browser;
- `pointerrawupdate` provides the earliest high-frequency updates in supporting
  secure contexts;
- `getPredictedEvents()` supplies temporary future positions;
- `altitudeAngle` and `azimuthAngle` preserve more precision than integer tilt
  components when the browser exposes them;
- Pointer Events Level 4's `persistentDeviceId`, when non-zero, supplies a
  randomized device identifier that is stable only for the browsing session;
- pressure, `tiltX`, `tiltY`, `twist`, tangential pressure, barrel-button, and
  eraser state enrich the corresponding Flutter pointer sample.

Stylet listens to `pointermove` rather than raw updates when the latter are not
available. When raw updates are active, `pointermove` is retained only to read
its predicted list. This follows the Pointer Events ordering model and prevents
the same measured sample from being emitted twice.

The automatic backend observes the first `flutter-view` present when its stream
gets its first listener. Flutter multi-view applications should use one shared
Stylet controller only after choosing which view owns drawing input; the event
model does not yet carry a Web view identifier.

Each prediction replaces the preceding trajectory for its `pointerId`. Draw it
as a disposable preview and erase it before rendering the next prediction or
the next authoritative stroke segment. Browsers only guarantee future
positions and timestamps; Stylet carries the latest real pressure and pose
across those positions rather than treating default values as future sensor
measurements.

Browser support varies independently for every optional API. Capability checks
are therefore runtime checks:

```dart
import 'dart:async';

import 'package:stylet/stylet.dart';

final StylusCapabilities capabilities = await Stylet.instance.capabilities;

if (capabilities.supports(StylusFeature.predictedSamples)) {
  final StreamSubscription<StylusPredictionEvent> subscription =
      Stylet.instance.predictions.listen(replacePredictedPath);
  // Cancel the subscription with the owning object.
}
```

Without any optional member, Flutter's ordinary pressure, tilt, orientation,
hover, buttons, and eraser events remain available.

`persistentDeviceId` is deliberately exposed only as `deviceIdentifier` and
`nativeDeviceIdentifier` on motion and prediction events. It can initially be
zero and become available later in a stroke, is randomized for the next browser
session, and does not provide a product name or connection lifecycle.

## Delegated Ink API

The browser Ink API reduces apparent latency differently: the operating system
or browser compositor draws a short trail between the last JavaScript-rendered
point and the physical pen. It neither yields extra measurements nor owns the
document stroke. Keep rendering authoritative and predicted paths normally.

Import the Web-only entry point and request a presenter explicitly:

```dart
import 'dart:ui';

import 'package:stylet/stylet_web.dart';

StyletInkTrail? inkTrail;

Future<void> enableDelegatedInk() async {
  if (!StyletInkTrail.isSupported) {
    return;
  }
  inkTrail = await StyletInkTrail.start(
    color: const Color(0xff202124),
    diameter: 4,
  );
}

void changeBrush(Color color, double diameter) {
  inkTrail?.updateStyle(color: color, diameter: diameter);
}

void disposeInk() {
  inkTrail?.dispose();
  inkTrail = null;
}
```

By default the first `flutter-view` element is the presentation area. A
multi-view application should pass its own `web.Element` explicitly. The
diameter uses CSS pixels and must be finite and positive. `start` throws
`UnsupportedError` if support disappears between feature detection and the
request; the presenter's promise can also fail if the browser rejects the
chosen area.

The listener forwards only trusted pen-contact `pointermove` events, as required
by `updateInkTrailStartPoint`. Calling `dispose` is idempotent and removes that
listener. The API is experimental and is not available in every major browser,
so applications must always retain the normal Stylet rendering path.

WebHID and WebUSB are not opened implicitly. They require a user permission
flow, expose vendor report formats rather than portable pointer samples, and can
compete with the operating system's normal tablet driver. A future explicit
vendor extension can build on them without weakening the core Web fallback.

## References

- [Pointer Events specification](https://www.w3.org/TR/pointerevents3/)
- [Pointer Events Level 4 draft](https://www.w3.org/TR/pointerevents4/)
- [Ink API](https://developer.mozilla.org/docs/Web/API/Ink_API)
- [`Navigator.ink`](https://developer.mozilla.org/docs/Web/API/Navigator/ink)
