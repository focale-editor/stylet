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
