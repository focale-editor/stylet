# Native platform contract

This document defines the standard-codec protocol between Stylet's native
backends and `MethodChannelStylet`. Keep it synchronized with
`lib/stylet_method_channel.dart` when extending the event model.

## Channels

- Method channel: `dev.focale.stylet/methods`
- Event channel: `dev.focale.stylet/events`
- Codec: Flutter standard method codec

The method channel currently accepts `getCapabilities` with no arguments. A
successful response is a list of names from `StylusFeature`. Unknown methods
must return Flutter's not-implemented response.

Native observers must remain passive. Android and desktop callbacks return the
source event unchanged, and iOS recognizers must not cancel touches or claim a
gesture Flutter would otherwise receive.

## Motion packets

A motion event is a map with these required fields:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Always `motion`. |
| `timestampMicros` | integer | Microseconds on the platform monotonic clock. |
| `phase` | string | `added`, `hover`, `down`, `move`, `up`, `cancel`, or `removed`. |
| `x`, `y` | number | Logical pixels from the Flutter view's top-left corner. |

Optional common fields are:

| Field | Type | Meaning |
| --- | --- | --- |
| `tool` | string | `pen`, `eraser`, or `unknown`. |
| `pointerIdentifier` | integer | Interaction-scoped native pointer identifier. |
| `deviceIdentifier` | integer | Platform device identifier. |
| `nativeDeviceIdentifier` | string | Stable descriptive tool identifier where available. |
| `embedderIdentifier` | integer | Identifier shared with Flutter's `PointerEvent.embedderId`. |
| `deltaX`, `deltaY` | number | Logical-pixel movement since the previous sample. |
| `buttons` | integer | Flutter-compatible pointer button bit field. |
| `isDown` | Boolean | Whether the tip is in contact. |
| `pressure` | number | Raw pressure in the accompanying range. |
| `pressureMinimum`, `pressureMaximum` | number | Device pressure limits. |
| `distance`, `distanceMaximum` | number | Raw hover distance and its maximum. |
| `tilt` | number | Radians away from the surface normal. |
| `orientation` | number | Projected-axis direction in radians. |
| `tiltX`, `tiltY` | number | Signed tilt components in radians. |
| `barrelRotation` | number | Clockwise radians around the stylus axis. |
| `tangentialPressure` | number | Barrel pressure normalized from -1 to 1. |
| `features` | list of strings | Values known to be supported by this sample. |

All numeric values must be finite. Backends omit an unavailable value instead
of sending a sentinel or `null`. They clear delta state after `cancel` and
`removed`; a new pointer or tool begins with a zero delta.

Flutter button bits currently used by Stylet are 1 for tip contact, 2 for the
first side button, and 4 for the second side button. Native backends should use
Flutter's public constants where they are available rather than duplicating
these values.

## Body-action packets

Double-tap and squeeze packets contain:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Always `action`. |
| `timestampMicros` | integer | Microseconds on the platform monotonic clock. |
| `action` | string | `doubleTap` or `squeeze`. |
| `phase` | string | `began`, `changed`, `ended`, `cancelled`, or `discrete`. |
| `pose` | map | Optional hover pose described below. |

An optional `pose` can contain `x`, `y`, `distance`, `tilt`, `orientation`, and
`barrelRotation`, with the same units as a motion packet. A pose must provide
both position coordinates or neither.

## Correlation

The Dart controller keeps at most 64 recent native motions. It first matches a
nonzero `embedderIdentifier`; otherwise it accepts compatible tool and phase
samples no more than 120 milliseconds and 8 logical pixels apart. Native clocks
therefore need to share the monotonic basis used by Flutter's embedder on that
platform.
