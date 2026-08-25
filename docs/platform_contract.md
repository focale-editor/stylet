# Native platform contract

This document defines the standard-codec protocol between Stylet's native
backends and `MethodChannelStylet`. Keep it synchronized with
`lib/stylet_method_channel.dart` when extending the event model.

The federated Web backend runs in the same Dart isolate and constructs model
objects directly, so it does not encode these maps. It must preserve the same
field meanings, units, replacement rules, and lifecycle semantics.

## Channels

- Method channel: `dev.focale.stylet/methods`
- Event channel: `dev.focale.stylet/events`
- Codec: Flutter standard method codec

The method channel currently accepts `getCapabilities` with no arguments. A
successful response is a list of names from `StylusFeature`. Unknown methods
must return Flutter's not-implemented response.

An event-channel message can be one event map, a top-level list of event maps,
or a map whose `type` is `batch` and whose required `events` field is a list of
event maps. Backends should use a list when one native callback represents
multiple chronologically ordered hardware samples. A batch cannot contain
another batch. Predictions use their own atomic packet because every new
prediction replaces the preceding one instead of extending it.

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
| `sampleIdentifier` | string | Identifier used by a later correction packet. |
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
| `wheelDelta` | number | Signed relative stylus-wheel movement in radians. |
| `features` | list of strings | Values known to be supported by this sample. |
| `estimatedProperties` | list of strings | Values in this packet that are currently estimates. |
| `propertiesExpectingUpdates` | list of strings | Estimated values for which a correction is expected. |

All numeric values must be finite. Backends omit an unavailable value instead
of sending a sentinel or `null`. They clear delta state after `cancel` and
`removed`; a new pointer or tool begins with a zero delta.

Flutter button bits currently used by Stylet are 1 for tip contact, 2 for the
first side button, and 4 for the second side button. Native backends should use
Flutter's public constants where they are available rather than duplicating
these values.

The estimated-property lists accept `position`, `pressure`, `tilt`,
`orientation`, and `barrelRotation`. A backend only supplies a
`sampleIdentifier` when the platform provides a reliable correlation key.

## Prediction packets

A prediction atomically replaces the temporary trajectory for one pointer:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Always `prediction`. |
| `timestampMicros` | integer | Timestamp of the real sample from which the prediction was generated. |
| `pointerIdentifier` | integer | Optional interaction-scoped pointer identifier. |
| `deviceIdentifier` | integer | Optional platform device identifier. |
| `nativeDeviceIdentifier` | string | Optional stable native tool identifier. |
| `samples` | list of maps | Future motion packets in chronological order. |

Each item in `samples` is a complete motion packet whose `type` is `motion`.
An empty list clears the pointer's preceding preview. Predicted samples are
never inserted into the authoritative motion stream and must not be committed
to a document until matching real input arrives.

## Correction packets

A correction supplies the latest complete representation of an earlier sample:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Always `correction`. |
| `timestampMicros` | integer | Time at which the correction reached the application. |
| `sampleIdentifier` | string | Identifier copied from the original motion packet. |
| `correctedProperties` | list of strings | Properties whose pending update has resolved. |
| `sample` | map | Complete corrected motion packet. |

The nested sample repeats the same `sampleIdentifier`. It can still contain
estimated properties when the hardware resolved only part of the pending data.

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

## Device packets

A device event describes a tablet, tool, or pad connection or metadata change:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Always `device`. |
| `timestampMicros` | integer | Microseconds on the platform monotonic clock. |
| `phase` | string | `added`, `changed`, or `removed`. |
| `kind` | string | `tablet`, `tool`, `pad`, or `unknown`. |
| `nativeDeviceIdentifier` | string | Stable identifier for this platform session. |
| `name` | string | Optional human-readable model or tool name. |
| `vendorIdentifier` | integer | Optional USB or platform vendor identifier. |
| `productIdentifier` | integer | Optional USB or platform product identifier. |
| `tool` | string | Optional `pen`, `eraser`, or `unknown` tool kind. |
| `buttonCount` | integer | Optional non-negative physical button count. |
| `features` | list of strings | Optional device-specific feature names. |

An `added` or `changed` packet contains the complete metadata known at that
time, not a partial patch. A `removed` packet repeats the latest description so
consumers can identify the departing device. Motion and pad packets should use
the identifier from their corresponding tool or pad description.

## Tablet-pad packets

A graphics-tablet pad event contains:

| Field | Type | Meaning |
| --- | --- | --- |
| `type` | string | Always `pad`. |
| `timestampMicros` | integer | Microseconds on the platform monotonic clock. |
| `nativeDeviceIdentifier` | string | Identifier of the pad described by a device packet. |
| `control` | string | `button`, `ring`, `strip`, `dial`, or `mode`. |
| `controlIndex` | integer | Non-negative, zero-based control or group index. |
| `phase` | string | `began`, `changed`, `ended`, or `discrete`. |
| `value` | number | Optional 0–1 ring/strip position or signed dial movement in logical detents. |
| `mode` | integer | Optional non-negative active mapping mode. |

Button presses use `began` and releases use `ended`. Backends that only expose
standalone absolute pad samples use `changed`; atomic mode switches use
`discrete`.

## Correlation

The Dart controller keeps at most 64 recent native motions. It first matches a
nonzero `embedderIdentifier`; otherwise it accepts compatible tool and phase
samples no more than 120 milliseconds and 8 logical pixels apart. Native clocks
therefore need to share the monotonic basis used by Flutter's embedder on that
platform. On the Web, both Flutter and Stylet derive durations from the same DOM
`Event.timeStamp` value.

For a matched sample, Flutter remains authoritative for lifecycle, position,
local position, and delta. A non-null native pressure, range, distance, tilt, or
orientation replaces the portable counterpart; native-only axes and identifiers
are then added. This lets a backend preserve higher sensor precision without
changing Flutter's hit testing or gesture ordering.

Native dependency selection and runtime fallbacks are described in
[`native_backends.md`](native_backends.md).
