import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart';
import 'package:stylet/src/stylus_capabilities.dart';

/// Identifies where a normalized event obtained its data.
enum StyletEventSource {
  /// Flutter supplied every field.
  flutter,

  /// A platform backend supplied every field.
  native,

  /// Flutter data was enriched with fields from a platform backend.
  combined,
}

/// Describes the lifecycle stage of a stylus pointer.
enum StylusPhase {
  /// The input device entered the application's pointer space.
  added,

  /// The stylus moved without touching the surface.
  hover,

  /// The stylus started touching the surface.
  down,

  /// The stylus moved while touching the surface.
  move,

  /// The stylus stopped touching the surface.
  up,

  /// The current interaction was interrupted.
  cancel,

  /// The input device left the application's pointer space.
  removed,
}

/// Identifies which end or kind of tablet tool produced an event.
enum StylusTool {
  /// A regular pen tip.
  pen,

  /// An inverted tip or dedicated eraser end.
  eraser,

  /// A tablet tool whose exact kind is unavailable.
  unknown,
}

/// Identifies a discrete interaction performed on a stylus body.
enum StylusAction {
  /// A double tap on the stylus body.
  doubleTap,

  /// A squeeze of the stylus body.
  squeeze,
}

/// Describes the lifecycle stage of a stylus body interaction.
enum StylusActionPhase {
  /// A multi-stage interaction started.
  began,

  /// A multi-stage interaction changed while active.
  changed,

  /// A multi-stage interaction completed normally.
  ended,

  /// A multi-stage interaction was interrupted.
  cancelled,

  /// The platform reported the interaction as one atomic action.
  discrete,
}

/// Base type for every event emitted by Stylet.
@immutable
sealed class StyletEvent {
  /// Time elapsed since the platform's monotonic clock origin.
  final Duration timeStamp;

  /// The layer that supplied this event's data.
  final StyletEventSource source;

  /// Creates the shared portion of a normalized event.
  const StyletEvent({required this.timeStamp, required this.source});
}

/// A normalized high-fidelity sample from a stylus pointer.
@immutable
final class StylusMotionEvent extends StyletEvent {
  /// The lifecycle stage represented by this sample.
  final StylusPhase phase;

  /// The tablet tool that produced this sample.
  final StylusTool tool;

  /// Flutter's interaction-scoped pointer identifier, when available.
  final int? pointerIdentifier;

  /// Flutter's stable device identifier, when available.
  final int? deviceIdentifier;

  /// A platform-defined identifier for the physical tablet tool.
  final String? nativeDeviceIdentifier;

  /// The platform event identifier used to correlate native and Flutter data.
  final int? embedderIdentifier;

  /// Position in logical pixels relative to the Flutter view.
  final Offset position;

  /// Position transformed into the receiving widget's coordinate system.
  final Offset localPosition;

  /// Movement since the preceding sample in logical pixels.
  final Offset delta;

  /// Raw Flutter-compatible button bit field.
  final int buttons;

  /// Whether the tip currently touches the input surface.
  final bool isDown;

  /// Raw pressure in the range described by [pressureMinimum] and [pressureMaximum].
  final double? pressure;

  /// Smallest pressure value the current device can report.
  final double? pressureMinimum;

  /// Largest pressure value the current device can report.
  final double? pressureMaximum;

  /// Device-defined hover distance from the input surface.
  final double? distance;

  /// Largest hover distance the current device can report.
  final double? distanceMaximum;

  /// Angle in radians between the stylus and the surface normal.
  final double? tilt;

  /// Direction in radians of the stylus projection across the surface.
  final double? orientation;

  /// Horizontal tilt component in radians, when a backend exposes it.
  final double? tiltX;

  /// Vertical tilt component in radians, when a backend exposes it.
  final double? tiltY;

  /// Clockwise rotation in radians around the stylus' longitudinal axis.
  final double? barrelRotation;

  /// Tangential pressure in the normalized range from -1 to 1.
  final double? tangentialPressure;

  /// Signed relative stylus-wheel movement in radians.
  final double? wheelDelta;

  /// Features known to be represented or supported by this sample.
  final Set<StylusFeature> features;

  /// The original Flutter pointer event, when this sample was derived from one.
  final PointerEvent? originalEvent;

  /// Creates a fully specified normalized motion sample.
  const StylusMotionEvent({
    required super.timeStamp,
    required super.source,
    required this.phase,
    required this.tool,
    required this.position,
    required this.localPosition,
    this.pointerIdentifier,
    this.deviceIdentifier,
    this.nativeDeviceIdentifier,
    this.embedderIdentifier,
    this.delta = Offset.zero,
    this.buttons = 0,
    this.isDown = false,
    this.pressure,
    this.pressureMinimum,
    this.pressureMaximum,
    this.distance,
    this.distanceMaximum,
    this.tilt,
    this.orientation,
    this.tiltX,
    this.tiltY,
    this.barrelRotation,
    this.tangentialPressure,
    this.wheelDelta,
    this.features = const {},
    this.originalEvent,
  });

  /// Converts [event] and optionally merges matching platform [enhancement] data.
  factory StylusMotionEvent.fromPointerEvent({required PointerEvent event, StylusMotionEvent? enhancement}) {
    final Set<StylusFeature> features = {..._featuresForPointer(event), ...?enhancement?.features};
    final StylusTool flutterTool = _toolForPointer(event);
    return StylusMotionEvent(
      timeStamp: event.timeStamp,
      source: enhancement == null ? StyletEventSource.flutter : StyletEventSource.combined,
      phase: _phaseForPointer(event),
      tool: flutterTool == StylusTool.unknown ? enhancement?.tool ?? flutterTool : flutterTool,
      pointerIdentifier: event.pointer,
      deviceIdentifier: event.device,
      nativeDeviceIdentifier: enhancement?.nativeDeviceIdentifier,
      embedderIdentifier: event.embedderId == 0 ? enhancement?.embedderIdentifier : event.embedderId,
      position: event.position,
      localPosition: event.localPosition,
      delta: event.delta,
      buttons: event.buttons | (enhancement?.buttons ?? 0),
      isDown: event.down,
      pressure: event.pressure,
      pressureMinimum: event.pressureMin,
      pressureMaximum: event.pressureMax,
      distance: event.distance,
      distanceMaximum: event.distanceMax,
      tilt: event.tilt,
      orientation: event.orientation,
      tiltX: enhancement?.tiltX,
      tiltY: enhancement?.tiltY,
      barrelRotation: enhancement?.barrelRotation,
      tangentialPressure: enhancement?.tangentialPressure,
      wheelDelta: enhancement?.wheelDelta,
      features: Set.unmodifiable(features),
      originalEvent: event,
    );
  }

  /// Pressure mapped linearly to the 0–1 range, or `null` when unavailable.
  double? get normalizedPressure {
    final double? currentPressure = pressure;
    final double? minimum = pressureMinimum;
    final double? maximum = pressureMaximum;
    if (currentPressure == null || minimum == null || maximum == null) {
      return null;
    }
    final double extent = maximum - minimum;
    if (extent <= 0) {
      return currentPressure.clamp(0.0, 1.0);
    }
    return ((currentPressure - minimum) / extent).clamp(0.0, 1.0);
  }

  /// Hover distance mapped linearly to the 0–1 range, or `null` when unavailable.
  double? get normalizedDistance {
    final double? currentDistance = distance;
    final double? maximum = distanceMaximum;
    if (currentDistance == null || maximum == null || maximum <= 0) {
      return null;
    }
    return (currentDistance / maximum).clamp(0.0, 1.0);
  }

  /// Whether this sample came from a pen or eraser rather than another pointer.
  bool get isStylus => tool != StylusTool.unknown;

  /// Whether [feature] is represented or supported by this sample.
  bool supports(StylusFeature feature) => features.contains(feature);

  /// Whether the side button numbered from one is currently pressed.
  bool isSideButtonPressed({required int number}) {
    if (number < 1) {
      throw RangeError.range(number, 1, null, 'number');
    }
    return buttons & nthStylusButton(number) != 0;
  }

  @override
  String toString() => 'StylusMotionEvent(phase: ${phase.name}, tool: ${tool.name}, position: $position, pressure: $normalizedPressure, rotation: $barrelRotation)';
}

/// A stylus pose attached to a body gesture such as squeeze.
@immutable
final class StylusPose {
  /// Position in logical pixels relative to the Flutter view.
  final Offset? position;

  /// Device-defined hover distance from the input surface.
  final double? distance;

  /// Angle in radians between the stylus and the surface normal.
  final double? tilt;

  /// Direction in radians of the stylus projection across the surface.
  final double? orientation;

  /// Clockwise rotation in radians around the stylus' longitudinal axis.
  final double? barrelRotation;

  /// Creates the optional pose supplied with a stylus body interaction.
  const StylusPose({this.position, this.distance, this.tilt, this.orientation, this.barrelRotation});

  @override
  String toString() => 'StylusPose(position: $position, distance: $distance, tilt: $tilt, orientation: $orientation, rotation: $barrelRotation)';
}

/// A double-tap or squeeze reported by a supported stylus.
@immutable
final class StylusActionEvent extends StyletEvent {
  /// The interaction performed on the stylus body.
  final StylusAction action;

  /// The lifecycle stage of this interaction.
  final StylusActionPhase phase;

  /// The stylus pose captured with the interaction, when available.
  final StylusPose? pose;

  /// Creates a normalized stylus body interaction.
  const StylusActionEvent({required super.timeStamp, required super.source, required this.action, required this.phase, this.pose});

  @override
  String toString() => 'StylusActionEvent(action: ${action.name}, phase: ${phase.name}, pose: $pose)';
}

/// Identifies the native object described by a device event.
enum StylusDeviceKind {
  /// A complete graphics tablet or integrated digitizer.
  tablet,

  /// A pen, eraser, airbrush, or other tablet tool.
  tool,

  /// The buttons, ring, strips, and dials attached to a tablet.
  pad,

  /// A device whose more specific role is unavailable.
  unknown,
}

/// Describes a change in a native input device's lifetime.
enum StylusDevicePhase {
  /// The device became available to the application.
  added,

  /// New metadata or capabilities became available for the device.
  changed,

  /// The device is no longer available to the application.
  removed,
}

/// Describes one native tablet, tool, or tablet pad.
@immutable
final class StylusDevice {
  /// Stable native identifier within the current platform session.
  final String identifier;

  /// Role played by this device in the tablet input stack.
  final StylusDeviceKind kind;

  /// Human-readable model or tool name, when available.
  final String? name;

  /// USB or platform vendor identifier, when available.
  final int? vendorIdentifier;

  /// USB or platform product identifier, when available.
  final int? productIdentifier;

  /// Physical tool kind for a [StylusDeviceKind.tool] device.
  final StylusTool? tool;

  /// Number of physical pad or tool buttons, when known.
  final int? buttonCount;

  /// Immutable features reported specifically for this device.
  final Set<StylusFeature> features;

  /// Creates a native input device description.
  StylusDevice({
    required this.identifier,
    required this.kind,
    this.name,
    this.vendorIdentifier,
    this.productIdentifier,
    this.tool,
    this.buttonCount,
    Set<StylusFeature> features = const {},
  }) : features = Set.unmodifiable(features);

  @override
  bool operator ==(Object other) =>
      identical(this, other) ||
      other is StylusDevice &&
          identifier == other.identifier &&
          kind == other.kind &&
          name == other.name &&
          vendorIdentifier == other.vendorIdentifier &&
          productIdentifier == other.productIdentifier &&
          tool == other.tool &&
          buttonCount == other.buttonCount &&
          setEquals(features, other.features);

  @override
  int get hashCode => Object.hash(identifier, kind, name, vendorIdentifier, productIdentifier, tool, buttonCount, Object.hashAllUnordered(features));

  @override
  String toString() => 'StylusDevice(identifier: $identifier, kind: ${kind.name}, name: $name, features: ${features.map((feature) => feature.name).join(', ')})';
}

/// A native tablet, tool, or pad connection and metadata change.
@immutable
final class StylusDeviceEvent extends StyletEvent {
  /// Lifetime change represented by this event.
  final StylusDevicePhase phase;

  /// Current description of the affected device.
  final StylusDevice device;

  /// Creates a normalized native device change.
  const StylusDeviceEvent({
    required super.timeStamp,
    required super.source,
    required this.phase,
    required this.device,
  });

  @override
  String toString() => 'StylusDeviceEvent(phase: ${phase.name}, device: $device)';
}

/// Identifies a physical control on a graphics-tablet pad.
enum TabletPadControl {
  /// A momentary physical button.
  button,

  /// An absolute circular touch ring.
  ring,

  /// An absolute linear touch strip.
  strip,

  /// A relative rotary dial or wheel.
  dial,

  /// A change to the active control mapping mode.
  mode,
}

/// Describes the lifecycle of a tablet-pad interaction.
enum TabletPadPhase {
  /// The user began interacting with the control.
  began,

  /// The control value changed while the interaction remained active.
  changed,

  /// The user stopped interacting with the control.
  ended,

  /// The hardware reported one atomic change without a lifecycle.
  discrete,
}

/// A button, ring, strip, dial, or mode event from a graphics-tablet pad.
@immutable
final class TabletPadEvent extends StyletEvent {
  /// Native identifier of the pad that owns the control.
  final String deviceIdentifier;

  /// Kind of physical or logical control that changed.
  final TabletPadControl control;

  /// Zero-based index of the control within the pad.
  final int controlIndex;

  /// Lifecycle stage represented by this event.
  final TabletPadPhase phase;

  /// Current normalized position or relative delta, when applicable.
  ///
  /// Rings and strips use the 0–1 range. Dials use signed logical detents.
  final double? value;

  /// Active mapping mode for the control group, when available.
  final int? mode;

  /// Creates a normalized graphics-tablet pad event.
  const TabletPadEvent({
    required super.timeStamp,
    required super.source,
    required this.deviceIdentifier,
    required this.control,
    required this.controlIndex,
    required this.phase,
    this.value,
    this.mode,
  });

  /// Whether a button lifecycle phase establishes a pressed state.
  bool? get isPressed => switch ((control, phase)) {
    (TabletPadControl.button, TabletPadPhase.began) => true,
    (TabletPadControl.button, TabletPadPhase.ended) => false,
    _ => null,
  };

  @override
  String toString() => 'TabletPadEvent(device: $deviceIdentifier, control: ${control.name}[$controlIndex], phase: ${phase.name}, value: $value, mode: $mode)';
}

/// Returns the normalized lifecycle stage for a Flutter pointer event.
StylusPhase _phaseForPointer(PointerEvent event) => switch (event) {
  PointerAddedEvent() || PointerEnterEvent() => StylusPhase.added,
  PointerHoverEvent() => StylusPhase.hover,
  PointerDownEvent() => StylusPhase.down,
  PointerMoveEvent() => StylusPhase.move,
  PointerUpEvent() => StylusPhase.up,
  PointerCancelEvent() => StylusPhase.cancel,
  PointerRemovedEvent() || PointerExitEvent() => StylusPhase.removed,
  _ => event.down ? StylusPhase.move : StylusPhase.hover,
};

/// Returns the tablet tool represented by a Flutter pointer event.
StylusTool _toolForPointer(PointerEvent event) => switch (event.kind) {
  PointerDeviceKind.stylus => StylusTool.pen,
  PointerDeviceKind.invertedStylus => StylusTool.eraser,
  _ => StylusTool.unknown,
};

/// Infers the portable features carried by a Flutter pointer event.
Set<StylusFeature> _featuresForPointer(PointerEvent event) {
  if (event.kind != PointerDeviceKind.stylus && event.kind != PointerDeviceKind.invertedStylus) {
    return const {};
  }
  final Set<StylusFeature> features = {
    StylusFeature.tilt,
    StylusFeature.orientation,
    StylusFeature.primaryButton,
    StylusFeature.secondaryButton,
    StylusFeature.eraser,
  };
  if (event.pressureMax > event.pressureMin) {
    features.add(StylusFeature.pressure);
  }
  if (event.distanceMax > 0) {
    features.addAll(const {StylusFeature.distance, StylusFeature.hover});
  }
  return features;
}
