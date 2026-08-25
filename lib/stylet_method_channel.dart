import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';
import 'package:stylet/src/stylus_capabilities.dart';
import 'package:stylet/src/stylus_event.dart';
import 'package:stylet/stylet_platform_interface.dart';

/// Stylet backend implemented with Flutter platform channels.
class MethodChannelStylet extends StyletPlatform {
  /// Name shared by every native method channel implementation.
  static const String _methodChannelName = 'dev.focale.stylet/methods';

  /// Name shared by every native event channel implementation.
  static const String _eventChannelName = 'dev.focale.stylet/events';

  /// Channel used for request-response operations.
  @visibleForTesting
  final MethodChannel methodChannel;

  /// Channel used for continuous native input.
  @visibleForTesting
  final EventChannel eventChannel;

  /// Lazily created stream decoded from [eventChannel].
  Stream<StyletEvent>? _events;

  /// Creates a platform-channel backend with optionally injected test channels.
  MethodChannelStylet({
    this.methodChannel = const MethodChannel(_methodChannelName),
    this.eventChannel = const EventChannel(_eventChannelName),
  });

  @override
  Stream<StyletEvent> get events => _events ??= eventChannel.receiveBroadcastStream().expand(decodeStyletEvents);

  @override
  Future<StylusCapabilities> getCapabilities() async {
    try {
      final List<Object?>? values = await methodChannel.invokeListMethod<Object?>('getCapabilities');
      if (values == null) {
        return StylusCapabilities.flutter;
      }
      final Iterable<String> names = values.whereType<String>();
      return StylusCapabilities.flutter.merge(StylusCapabilities.fromNames(names: names));
    } on MissingPluginException {
      return StylusCapabilities.flutter;
    }
  }
}

/// Decodes one event-channel message, including historical event batches.
@visibleForTesting
Iterable<StyletEvent> decodeStyletEvents(Object? value) sync* {
  if (value is List<Object?>) {
    for (final Object? item in value) {
      yield decodeStyletEvent(item);
    }
    return;
  }
  final Map<Object?, Object?> map = _requiredMap(value, context: 'event');
  if (_requiredString(map, 'type') == 'batch') {
    for (final Object? item in _requiredList(map, 'events')) {
      yield decodeStyletEvent(item);
    }
    return;
  }
  yield decodeStyletEvent(map);
}

/// Decodes one platform-channel map into its strongly typed event.
@visibleForTesting
StyletEvent decodeStyletEvent(Object? value) {
  final Map<Object?, Object?> map = _requiredMap(value, context: 'event');
  return switch (_requiredString(map, 'type')) {
    'motion' => _decodeMotionEvent(map),
    'action' => _decodeActionEvent(map),
    'device' => _decodeDeviceEvent(map),
    'pad' => _decodePadEvent(map),
    'batch' => throw const FormatException('Use decodeStyletEvents to decode a Stylet batch.'),
    final String type => throw FormatException('Unknown Stylet event type "$type".'),
  };
}

/// Decodes a native motion sample.
StylusMotionEvent _decodeMotionEvent(Map<Object?, Object?> map) {
  final Offset position = Offset(_requiredDouble(map, 'x'), _requiredDouble(map, 'y'));
  final StylusPhase phase = _phaseFromName(_requiredString(map, 'phase'));
  return StylusMotionEvent(
    timeStamp: Duration(microseconds: _requiredInt(map, 'timestampMicros')),
    source: StyletEventSource.native,
    phase: phase,
    tool: _toolFromName(_optionalString(map, 'tool') ?? 'unknown'),
    pointerIdentifier: _optionalInt(map, 'pointerIdentifier'),
    deviceIdentifier: _optionalInt(map, 'deviceIdentifier'),
    nativeDeviceIdentifier: _optionalString(map, 'nativeDeviceIdentifier'),
    embedderIdentifier: _optionalInt(map, 'embedderIdentifier'),
    position: position,
    localPosition: position,
    delta: Offset(_optionalDouble(map, 'deltaX') ?? 0, _optionalDouble(map, 'deltaY') ?? 0),
    buttons: _optionalInt(map, 'buttons') ?? 0,
    isDown: _optionalBool(map, 'isDown') ?? phase == StylusPhase.down || phase == StylusPhase.move,
    pressure: _optionalDouble(map, 'pressure'),
    pressureMinimum: _optionalDouble(map, 'pressureMinimum'),
    pressureMaximum: _optionalDouble(map, 'pressureMaximum'),
    distance: _optionalDouble(map, 'distance'),
    distanceMaximum: _optionalDouble(map, 'distanceMaximum'),
    tilt: _optionalDouble(map, 'tilt'),
    orientation: _optionalDouble(map, 'orientation'),
    tiltX: _optionalDouble(map, 'tiltX'),
    tiltY: _optionalDouble(map, 'tiltY'),
    barrelRotation: _optionalDouble(map, 'barrelRotation'),
    tangentialPressure: _optionalDouble(map, 'tangentialPressure'),
    wheelDelta: _optionalDouble(map, 'wheelDelta'),
    features: _featuresFromValue(map['features']),
  );
}

/// Decodes a native double-tap or squeeze interaction.
StylusActionEvent _decodeActionEvent(Map<Object?, Object?> map) {
  final Object? poseValue = map['pose'];
  return StylusActionEvent(
    timeStamp: Duration(microseconds: _requiredInt(map, 'timestampMicros')),
    source: StyletEventSource.native,
    action: _actionFromName(_requiredString(map, 'action')),
    phase: _actionPhaseFromName(_requiredString(map, 'phase')),
    pose: poseValue == null ? null : _decodePose(_requiredMap(poseValue, context: 'pose')),
  );
}

/// Decodes a native tablet, tool, or pad connection change.
StylusDeviceEvent _decodeDeviceEvent(Map<Object?, Object?> map) {
  final int? buttonCount = _optionalInt(map, 'buttonCount');
  if (buttonCount != null && buttonCount < 0) {
    throw const FormatException('A Stylet device button count must be non-negative.');
  }
  return StylusDeviceEvent(
    timeStamp: Duration(microseconds: _requiredInt(map, 'timestampMicros')),
    source: StyletEventSource.native,
    phase: _devicePhaseFromName(_requiredString(map, 'phase')),
    device: StylusDevice(
      identifier: _requiredString(map, 'nativeDeviceIdentifier'),
      kind: _deviceKindFromName(_requiredString(map, 'kind')),
      name: _optionalString(map, 'name'),
      vendorIdentifier: _optionalInt(map, 'vendorIdentifier'),
      productIdentifier: _optionalInt(map, 'productIdentifier'),
      tool: switch (_optionalString(map, 'tool')) {
        final String name => _toolFromName(name),
        null => null,
      },
      buttonCount: buttonCount,
      features: _featuresFromValue(map['features']),
    ),
  );
}

/// Decodes one native graphics-tablet pad control change.
TabletPadEvent _decodePadEvent(Map<Object?, Object?> map) {
  final int controlIndex = _requiredInt(map, 'controlIndex');
  final int? mode = _optionalInt(map, 'mode');
  if (controlIndex < 0 || (mode != null && mode < 0)) {
    throw const FormatException('Stylet pad indices and modes must be non-negative.');
  }
  return TabletPadEvent(
    timeStamp: Duration(microseconds: _requiredInt(map, 'timestampMicros')),
    source: StyletEventSource.native,
    deviceIdentifier: _requiredString(map, 'nativeDeviceIdentifier'),
    control: _padControlFromName(_requiredString(map, 'control')),
    controlIndex: controlIndex,
    phase: _padPhaseFromName(_requiredString(map, 'phase')),
    value: _optionalDouble(map, 'value'),
    mode: mode,
  );
}

/// Decodes the optional hover pose attached to a body interaction.
StylusPose _decodePose(Map<Object?, Object?> map) {
  final double? x = _optionalDouble(map, 'x');
  final double? y = _optionalDouble(map, 'y');
  if ((x == null) != (y == null)) {
    throw const FormatException('A stylus pose must provide both x and y.');
  }
  return StylusPose(
    position: x == null ? null : Offset(x, y!),
    distance: _optionalDouble(map, 'distance'),
    tilt: _optionalDouble(map, 'tilt'),
    orientation: _optionalDouble(map, 'orientation'),
    barrelRotation: _optionalDouble(map, 'barrelRotation'),
  );
}

/// Parses a motion phase while rejecting incompatible native values.
StylusPhase _phaseFromName(String name) => switch (name) {
  'added' => StylusPhase.added,
  'hover' => StylusPhase.hover,
  'down' => StylusPhase.down,
  'move' => StylusPhase.move,
  'up' => StylusPhase.up,
  'cancel' => StylusPhase.cancel,
  'removed' => StylusPhase.removed,
  _ => throw FormatException('Unknown stylus phase "$name".'),
};

/// Parses a tablet tool while rejecting incompatible native values.
StylusTool _toolFromName(String name) => switch (name) {
  'pen' => StylusTool.pen,
  'eraser' => StylusTool.eraser,
  'unknown' => StylusTool.unknown,
  _ => throw FormatException('Unknown stylus tool "$name".'),
};

/// Parses a body action while rejecting incompatible native values.
StylusAction _actionFromName(String name) => switch (name) {
  'doubleTap' => StylusAction.doubleTap,
  'squeeze' => StylusAction.squeeze,
  _ => throw FormatException('Unknown stylus action "$name".'),
};

/// Parses a body action phase while rejecting incompatible native values.
StylusActionPhase _actionPhaseFromName(String name) => switch (name) {
  'began' => StylusActionPhase.began,
  'changed' => StylusActionPhase.changed,
  'ended' => StylusActionPhase.ended,
  'cancelled' => StylusActionPhase.cancelled,
  'discrete' => StylusActionPhase.discrete,
  _ => throw FormatException('Unknown stylus action phase "$name".'),
};

/// Parses a native device kind while rejecting incompatible values.
StylusDeviceKind _deviceKindFromName(String name) => switch (name) {
  'tablet' => StylusDeviceKind.tablet,
  'tool' => StylusDeviceKind.tool,
  'pad' => StylusDeviceKind.pad,
  'unknown' => StylusDeviceKind.unknown,
  _ => throw FormatException('Unknown stylus device kind "$name".'),
};

/// Parses a native device lifetime phase.
StylusDevicePhase _devicePhaseFromName(String name) => switch (name) {
  'added' => StylusDevicePhase.added,
  'changed' => StylusDevicePhase.changed,
  'removed' => StylusDevicePhase.removed,
  _ => throw FormatException('Unknown stylus device phase "$name".'),
};

/// Parses a native tablet-pad control kind.
TabletPadControl _padControlFromName(String name) => switch (name) {
  'button' => TabletPadControl.button,
  'ring' => TabletPadControl.ring,
  'strip' => TabletPadControl.strip,
  'dial' => TabletPadControl.dial,
  'mode' => TabletPadControl.mode,
  _ => throw FormatException('Unknown tablet-pad control "$name".'),
};

/// Parses a native tablet-pad interaction phase.
TabletPadPhase _padPhaseFromName(String name) => switch (name) {
  'began' => TabletPadPhase.began,
  'changed' => TabletPadPhase.changed,
  'ended' => TabletPadPhase.ended,
  'discrete' => TabletPadPhase.discrete,
  _ => throw FormatException('Unknown tablet-pad phase "$name".'),
};

/// Converts an optional list of names into an immutable feature set.
Set<StylusFeature> _featuresFromValue(Object? value) {
  if (value == null) {
    return const {};
  }
  if (value is! List<Object?>) {
    throw const FormatException('The Stylet features value must be a list.');
  }
  final Iterable<String> names = value.whereType<String>();
  return StylusCapabilities.fromNames(names: names).features;
}

/// Reads a required channel map.
Map<Object?, Object?> _requiredMap(Object? value, {required String context}) {
  if (value is Map<Object?, Object?>) {
    return value;
  }
  throw FormatException('The Stylet $context must be a map.');
}

/// Reads a required list field.
List<Object?> _requiredList(Map<Object?, Object?> map, String key) {
  final Object? value = map[key];
  if (value is List<Object?>) {
    return value;
  }
  throw FormatException('Stylet field "$key" must be a list.');
}

/// Reads a required string field.
String _requiredString(Map<Object?, Object?> map, String key) {
  final String? value = _optionalString(map, key);
  if (value == null) {
    throw FormatException('Missing or invalid Stylet field "$key".');
  }
  return value;
}

/// Reads an optional string field and validates its type.
String? _optionalString(Map<Object?, Object?> map, String key) {
  final Object? value = map[key];
  if (value == null || value is String) {
    return value as String?;
  }
  throw FormatException('Stylet field "$key" must be a string.');
}

/// Reads a required integer field.
int _requiredInt(Map<Object?, Object?> map, String key) {
  final int? value = _optionalInt(map, key);
  if (value == null) {
    throw FormatException('Missing or invalid Stylet field "$key".');
  }
  return value;
}

/// Reads an optional integer field and validates that it is integral.
int? _optionalInt(Map<Object?, Object?> map, String key) {
  final Object? value = map[key];
  if (value == null) {
    return null;
  }
  if (value is int) {
    return value;
  }
  if (value is num && value.isFinite && value == value.roundToDouble()) {
    return value.toInt();
  }
  throw FormatException('Stylet field "$key" must be an integer.');
}

/// Reads a required finite floating-point field.
double _requiredDouble(Map<Object?, Object?> map, String key) {
  final double? value = _optionalDouble(map, key);
  if (value == null) {
    throw FormatException('Missing or invalid Stylet field "$key".');
  }
  return value;
}

/// Reads an optional finite floating-point field.
double? _optionalDouble(Map<Object?, Object?> map, String key) {
  final Object? value = map[key];
  if (value == null) {
    return null;
  }
  if (value is num && value.isFinite) {
    return value.toDouble();
  }
  throw FormatException('Stylet field "$key" must be a finite number.');
}

/// Reads an optional Boolean field and validates its type.
bool? _optionalBool(Map<Object?, Object?> map, String key) {
  final Object? value = map[key];
  if (value == null || value is bool) {
    return value as bool?;
  }
  throw FormatException('Stylet field "$key" must be a Boolean.');
}
