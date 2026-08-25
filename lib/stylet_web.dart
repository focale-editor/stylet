@JS()
library;

import 'dart:async';
import 'dart:js_interop';
import 'dart:js_interop_unsafe';
import 'dart:math' as math;
import 'dart:ui';

import 'package:flutter_web_plugins/flutter_web_plugins.dart';
import 'package:stylet/src/stylus_capabilities.dart';
import 'package:stylet/src/stylus_event.dart';
import 'package:stylet/stylet_platform_interface.dart';
import 'package:web/web.dart' as web;

/// Number of radians in one degree.
const double _degreesToRadians = math.pi / 180;

/// DOM button bit representing pen-tip contact.
const int _webPrimaryButton = 1;

/// DOM button bit representing the pen barrel button.
const int _webBarrelButton = 2;

/// DOM button bit representing the pen eraser.
const int _webEraserButton = 32;

/// Flutter button bit representing pen-tip contact.
const int _flutterPrimaryButton = 1;

/// Flutter button bit representing the first stylus side button.
const int _flutterPrimaryStylusButton = 2;

/// Features represented by a DOM pen sample.
const Set<StylusFeature> _webMotionFeatures = {
  StylusFeature.pressure,
  StylusFeature.tilt,
  StylusFeature.orientation,
  StylusFeature.barrelRotation,
  StylusFeature.tangentialPressure,
  StylusFeature.primaryButton,
  StylusFeature.eraser,
  StylusFeature.hover,
};

/// Flutter Web backend built on the browser Pointer Events API.
final class StyletWeb extends StyletPlatform {
  /// Pointer event names observed for every stream subscription.
  static const List<String> _lifecycleEventNames = [
    'pointerenter',
    'pointerdown',
    'pointerup',
    'pointercancel',
    'pointerleave',
  ];

  /// Synchronous event source used to enrich Flutter's matching pointer event.
  late final StreamController<StyletEvent> _controller;

  /// JavaScript callback retained so it can be removed by identity.
  late final web.EventListener _eventListener;

  /// Latest real position for each active DOM pointer identifier.
  final Map<int, Offset> _lastPositions = {};

  /// Pointers whose most recently emitted prediction was non-empty.
  final Set<int> _activePredictions = {};

  /// Pointers whose pen tip or eraser was touching on the latest real sample.
  final Set<int> _contactPointers = {};

  /// Whether the browser exposes uncoalesced samples on pointer events.
  late final bool _supportsCoalescedEvents;

  /// Whether the browser exposes future predicted pointer events.
  late final bool _supportsPredictedEvents;

  /// Whether precise altitude and azimuth angles are exposed by the browser.
  late final bool _supportsSphericalAngles;

  /// Whether a session-scoped physical pointer identifier is exposed.
  late final bool _supportsPersistentDeviceIdentifier;

  /// Whether high-frequency raw pointer updates are available.
  late final bool _supportsRawUpdates;

  /// DOM element associated with the active Flutter view while listening.
  web.Element? _eventTarget;

  /// Creates a browser backend with runtime feature detection.
  StyletWeb() {
    _supportsCoalescedEvents = _pointerEventPrototypeSupports(
      'getCoalescedEvents',
    );
    _supportsPredictedEvents = _pointerEventPrototypeSupports(
      'getPredictedEvents',
    );
    _supportsSphericalAngles =
        _pointerEventPrototypeSupports('altitudeAngle') &&
        _pointerEventPrototypeSupports('azimuthAngle');
    _supportsPersistentDeviceIdentifier = _pointerEventPrototypeSupports(
      'persistentDeviceId',
    );
    _supportsRawUpdates = web.window.has('onpointerrawupdate');
    _eventListener = _handleDomEvent.toJS;
    _controller = StreamController<StyletEvent>.broadcast(
      sync: true,
      onListen: _startListening,
      onCancel: _stopListening,
    );
  }

  /// Registers the browser backend before the Flutter application starts.
  static void registerWith(Registrar registrar) {
    StyletPlatform.instance = StyletWeb();
  }

  @override
  Stream<StyletEvent> get events => _controller.stream;

  @override
  Future<StylusCapabilities> getCapabilities() async {
    final Set<StylusFeature> features = {..._webMotionFeatures};
    if (_supportsCoalescedEvents) {
      features.add(StylusFeature.historicalSamples);
    }
    if (_supportsPredictedEvents) {
      features.add(StylusFeature.predictedSamples);
    }
    return StylusCapabilities.flutter.merge(
      StylusCapabilities(features: Set.unmodifiable(features)),
    );
  }

  /// Attaches DOM listeners when the first Dart consumer subscribes.
  void _startListening() {
    final web.Element target = _flutterPresentationArea();
    _eventTarget = target;
    for (final String name in _lifecycleEventNames) {
      target.addEventListener(name, _eventListener, true.toJS);
    }
    if (_supportsRawUpdates) {
      target.addEventListener('pointerrawupdate', _eventListener, true.toJS);
      if (_supportsPredictedEvents) {
        target.addEventListener('pointermove', _eventListener, true.toJS);
      }
    } else {
      target.addEventListener('pointermove', _eventListener, true.toJS);
    }
  }

  /// Removes DOM listeners when the final Dart consumer unsubscribes.
  void _stopListening() {
    final web.Element? target = _eventTarget;
    if (target == null) {
      return;
    }
    for (final String name in _lifecycleEventNames) {
      target.removeEventListener(name, _eventListener, true.toJS);
    }
    if (_supportsRawUpdates) {
      target.removeEventListener('pointerrawupdate', _eventListener, true.toJS);
      if (_supportsPredictedEvents) {
        target.removeEventListener('pointermove', _eventListener, true.toJS);
      }
    } else {
      target.removeEventListener('pointermove', _eventListener, true.toJS);
    }
    _eventTarget = null;
    _lastPositions.clear();
    _activePredictions.clear();
    _contactPointers.clear();
  }

  /// Routes one trusted or synthetic DOM pointer event to the relevant stream.
  void _handleDomEvent(web.Event event) {
    final web.PointerEvent pointerEvent = event as web.PointerEvent;
    if (pointerEvent.pointerType != 'pen') {
      return;
    }

    final bool isMove = pointerEvent.type == 'pointermove';
    final bool isRawUpdate = pointerEvent.type == 'pointerrawupdate';
    if (!isMove || !_supportsRawUpdates) {
      _emitRealSamples(pointerEvent);
    }
    if (_supportsPredictedEvents && isMove) {
      _emitPrediction(pointerEvent);
    }
    if (!isMove && !isRawUpdate && _isTerminal(pointerEvent)) {
      _clearPrediction(pointerEvent);
    }
  }

  /// Emits every coalesced real sample represented by one DOM event.
  void _emitRealSamples(web.PointerEvent event) {
    final web.DOMRect bounds = _eventTarget!.getBoundingClientRect();
    final List<web.PointerEvent> samples = _coalescedEvents(event);
    for (final web.PointerEvent sample in samples) {
      _controller.add(_motionFromPointerEvent(sample, bounds: bounds));
    }
    if (_isTerminal(event)) {
      _lastPositions.remove(event.pointerId);
      _contactPointers.remove(event.pointerId);
    }
  }

  /// Emits a complete replacement for one pointer's predicted trajectory.
  void _emitPrediction(web.PointerEvent event) {
    List<web.PointerEvent> predictedEvents;
    try {
      predictedEvents = event.getPredictedEvents().toDart;
    } catch (_) {
      predictedEvents = const [];
    }
    if (predictedEvents.isEmpty &&
        !_activePredictions.contains(event.pointerId)) {
      return;
    }

    final web.DOMRect bounds = _eventTarget!.getBoundingClientRect();
    final int? deviceIdentifier = _deviceIdentifierFor(event);
    Offset previousPosition = _positionFor(event, bounds);
    final List<StylusMotionEvent> samples = [];
    for (final web.PointerEvent predictedEvent in predictedEvents) {
      final Offset position = _positionFor(predictedEvent, bounds);
      samples.add(
        _motionFromPointerEvent(
          predictedEvent,
          bounds: bounds,
          previousPosition: previousPosition,
          stateEvent: event,
          updateRealPosition: false,
        ),
      );
      previousPosition = position;
    }
    _controller.add(
      StylusPredictionEvent(
        timeStamp: _timeStampFor(event),
        source: StyletEventSource.native,
        pointerIdentifier: event.pointerId,
        deviceIdentifier: deviceIdentifier,
        nativeDeviceIdentifier: _nativeDeviceIdentifier(deviceIdentifier),
        samples: samples,
      ),
    );
    if (samples.isEmpty) {
      _activePredictions.remove(event.pointerId);
    } else {
      _activePredictions.add(event.pointerId);
    }
  }

  /// Emits an empty prediction when an active pointer interaction terminates.
  void _clearPrediction(web.PointerEvent event) {
    if (!_activePredictions.remove(event.pointerId)) {
      return;
    }
    final int? deviceIdentifier = _deviceIdentifierFor(event);
    _controller.add(
      StylusPredictionEvent(
        timeStamp: _timeStampFor(event),
        source: StyletEventSource.native,
        pointerIdentifier: event.pointerId,
        deviceIdentifier: deviceIdentifier,
        nativeDeviceIdentifier: _nativeDeviceIdentifier(deviceIdentifier),
        samples: const [],
      ),
    );
  }

  /// Returns uncoalesced updates or the parent event as a compatibility fallback.
  List<web.PointerEvent> _coalescedEvents(web.PointerEvent event) {
    if (_supportsCoalescedEvents &&
        (event.type == 'pointermove' || event.type == 'pointerrawupdate')) {
      try {
        final List<web.PointerEvent> samples = event
            .getCoalescedEvents()
            .toDart;
        if (samples.isNotEmpty) {
          return samples;
        }
      } catch (_) {
        // A partial browser implementation still has a safe parent event.
      }
    }
    return [event];
  }

  /// Converts a DOM pointer event into one normalized native motion sample.
  StylusMotionEvent _motionFromPointerEvent(
    web.PointerEvent event, {
    required web.DOMRect bounds,
    Offset? previousPosition,
    web.PointerEvent? stateEvent,
    bool updateRealPosition = true,
  }) {
    final Offset position = _positionFor(event, bounds);
    final Offset previous =
        previousPosition ?? _lastPositions[event.pointerId] ?? position;
    final web.PointerEvent sensorEvent = stateEvent ?? event;
    final double tiltX = sensorEvent.tiltX * _degreesToRadians;
    final double tiltY = sensorEvent.tiltY * _degreesToRadians;
    final double tangentX = math.tan(tiltX);
    final double tangentY = math.tan(tiltY);
    final bool isDown = _isContact(sensorEvent);
    final bool isEraser = _isEraser(sensorEvent);
    final bool wasDown = _contactPointers.contains(event.pointerId);
    final int? deviceIdentifier = _deviceIdentifierFor(sensorEvent);
    final double tilt = _supportsSphericalAngles
        ? (math.pi / 2 - _numericProperty(sensorEvent, 'altitudeAngle')).clamp(
            0.0,
            math.pi / 2,
          )
        : math.atan(math.sqrt(tangentX * tangentX + tangentY * tangentY));
    final double orientation = _supportsSphericalAngles
        ? _normalizeOrientation(
            _numericProperty(sensorEvent, 'azimuthAngle') + math.pi / 2,
          )
        : math.atan2(tangentX, -tangentY);
    if (updateRealPosition) {
      _lastPositions[event.pointerId] = position;
      if (isDown) {
        _contactPointers.add(event.pointerId);
      } else {
        _contactPointers.remove(event.pointerId);
      }
    }
    return StylusMotionEvent(
      timeStamp: _timeStampFor(event),
      source: StyletEventSource.native,
      phase: _phaseFor(event, isDown: isDown, wasDown: wasDown),
      tool: isEraser ? StylusTool.eraser : StylusTool.pen,
      pointerIdentifier: event.pointerId,
      deviceIdentifier: deviceIdentifier,
      nativeDeviceIdentifier: _nativeDeviceIdentifier(deviceIdentifier),
      position: position,
      localPosition: position,
      delta: position - previous,
      buttons: _flutterButtons(sensorEvent, isDown: isDown),
      isDown: isDown,
      pressure: sensorEvent.pressure,
      pressureMinimum: 0,
      pressureMaximum: 1,
      tilt: tilt,
      orientation: orientation,
      tiltX: tiltX,
      tiltY: tiltY,
      barrelRotation: sensorEvent.twist * _degreesToRadians,
      tangentialPressure: sensorEvent.tangentialPressure,
      features: _webMotionFeatures,
    );
  }

  /// Returns a non-zero Pointer Events Level 4 device identifier when present.
  int? _deviceIdentifierFor(web.PointerEvent event) {
    if (!_supportsPersistentDeviceIdentifier) {
      return null;
    }
    final JSNumber? value = event['persistentDeviceId'] as JSNumber?;
    if (value == null) {
      return null;
    }
    final int identifier = value.toDartDouble.toInt();
    return identifier == 0 ? null : identifier;
  }
}

/// Opt-in delegated ink trail drawn by a supporting browser compositor.
final class StyletInkTrail {
  /// Browser presenter responsible for the delegated compositor trail.
  final _DelegatedInkTrailPresenter _presenter;

  /// Element bounding the area where delegated ink may be displayed.
  final web.Element presentationArea;

  /// JavaScript listener retained so it can be removed by identity.
  late final web.EventListener _eventListener;

  /// Current trail color.
  Color _color;

  /// Current trail diameter in CSS pixels.
  double _diameter;

  /// Whether [dispose] has already detached the browser listener.
  bool _isDisposed = false;

  /// Attaches an already resolved delegated-ink presenter.
  StyletInkTrail._(
    this._presenter,
    this.presentationArea,
    this._color,
    this._diameter,
  ) {
    _eventListener = _handlePointerMove.toJS;
    presentationArea.addEventListener('pointermove', _eventListener, true.toJS);
  }

  /// Whether the current browser exposes the experimental Ink API.
  static bool get isSupported => _browserInk() != null;

  /// Current delegated trail color.
  Color get color => _color;

  /// Current delegated trail diameter in CSS pixels.
  double get diameter => _diameter;

  /// Requests a compositor presenter and starts forwarding trusted pen moves.
  static Future<StyletInkTrail> start({
    required Color color,
    required double diameter,
    web.Element? presentationArea,
  }) async {
    _validateDiameter(diameter);
    final _Ink? ink = _browserInk();
    if (ink == null) {
      throw UnsupportedError('The browser does not support the Ink API.');
    }
    final web.Element area = presentationArea ?? _flutterPresentationArea();
    final _DelegatedInkTrailPresenter presenter = await ink
        .requestPresenter(_InkPresenterOptions(presentationArea: area))
        .toDart;
    return StyletInkTrail._(presenter, area, color, diameter);
  }

  /// Changes the style used by subsequent delegated trail updates.
  void updateStyle({Color? color, double? diameter}) {
    if (_isDisposed) {
      throw StateError('This delegated ink trail has been disposed.');
    }
    if (diameter != null) {
      _validateDiameter(diameter);
      _diameter = diameter;
    }
    if (color != null) {
      _color = color;
    }
  }

  /// Stops forwarding pointer events to the browser compositor.
  void dispose() {
    if (_isDisposed) {
      return;
    }
    _isDisposed = true;
    presentationArea.removeEventListener(
      'pointermove',
      _eventListener,
      true.toJS,
    );
  }

  /// Supplies the latest trusted contact event and brush style to the browser.
  void _handlePointerMove(web.Event event) {
    final web.PointerEvent pointerEvent = event as web.PointerEvent;
    if (!pointerEvent.isTrusted ||
        pointerEvent.pointerType != 'pen' ||
        !_isContact(pointerEvent)) {
      return;
    }
    try {
      _presenter.updateInkTrailStartPoint(
        pointerEvent,
        _InkTrailStyle(color: _cssColor(_color), diameter: _diameter),
      );
    } catch (_) {
      // A compositor rejection must not interrupt authoritative input.
    }
  }
}

/// JavaScript projection of `navigator.ink`.
extension type _Ink._(JSObject _) implements JSObject {
  /// Requests a delegated presenter for one presentation area.
  external JSPromise<_DelegatedInkTrailPresenter> requestPresenter(
    _InkPresenterOptions options,
  );
}

/// JavaScript projection of a delegated ink trail presenter.
extension type _DelegatedInkTrailPresenter._(JSObject _) implements JSObject {
  /// Moves the beginning of the compositor-owned trail to a trusted event.
  external void updateInkTrailStartPoint(
    web.PointerEvent event,
    _InkTrailStyle style,
  );
}

/// Options object passed to `Ink.requestPresenter`.
extension type _InkPresenterOptions._(JSObject _) implements JSObject {
  /// Creates options scoped to [presentationArea].
  external factory _InkPresenterOptions({
    required web.Element presentationArea,
  });
}

/// Style object passed to `updateInkTrailStartPoint`.
extension type _InkTrailStyle._(JSObject _) implements JSObject {
  /// Creates a CSS-colored circular delegated ink style.
  external factory _InkTrailStyle({
    required String color,
    required double diameter,
  });
}

/// Returns whether the PointerEvent prototype exposes [member].
bool _pointerEventPrototypeSupports(String member) {
  if (!globalContext.has('PointerEvent')) {
    return false;
  }
  final JSObject constructor = globalContext['PointerEvent']! as JSObject;
  final JSObject prototype = constructor['prototype']! as JSObject;
  return prototype.has(member);
}

/// Returns the browser Ink entry point when it is exposed.
_Ink? _browserInk() {
  if (!web.window.navigator.has('ink')) {
    return null;
  }
  try {
    return web.window.navigator['ink'] as _Ink?;
  } catch (_) {
    return null;
  }
}

/// Resolves the first Flutter view, with a document-root compatibility fallback.
web.Element _flutterPresentationArea() =>
    web.document.querySelector('flutter-view') ??
    web.document.body ??
    web.document.documentElement!;

/// Converts one DOM high-resolution timestamp from milliseconds to microseconds.
Duration _timeStampFor(web.PointerEvent event) =>
    Duration(microseconds: (event.timeStamp * 1000).round());

/// Converts DOM viewport coordinates into Flutter-view logical coordinates.
Offset _positionFor(web.PointerEvent event, web.DOMRect bounds) => Offset(
  _numericProperty(event, 'clientX') - bounds.left,
  _numericProperty(event, 'clientY') - bounds.top,
);

/// Reads a JavaScript numeric property without truncating fractional pixels.
double _numericProperty(JSObject object, String name) =>
    (object[name] as JSNumber).toDartDouble;

/// Wraps a browser azimuth conversion into Flutter's orientation interval.
double _normalizeOrientation(double value) {
  final double wrapped = (value + math.pi) % (2 * math.pi) - math.pi;
  return wrapped == -math.pi ? math.pi : wrapped;
}

/// Whether a DOM pen event represents tip or eraser contact.
bool _isContact(web.PointerEvent event) =>
    event.buttons & (_webPrimaryButton | _webEraserButton) != 0;

/// Whether a DOM pen event comes from the eraser end or eraser button.
bool _isEraser(web.PointerEvent event) =>
    event.buttons & _webEraserButton != 0 || event.button == 5;

/// Converts DOM pen buttons into Flutter's stylus button bit field.
int _flutterButtons(web.PointerEvent event, {required bool isDown}) {
  int buttons = isDown ? _flutterPrimaryButton : 0;
  if (event.buttons & _webBarrelButton != 0) {
    buttons |= _flutterPrimaryStylusButton;
  }
  return buttons;
}

/// Maps one DOM pointer event to Stylet's motion lifecycle.
StylusPhase _phaseFor(
  web.PointerEvent event, {
  required bool isDown,
  required bool wasDown,
}) => switch (event.type) {
  'pointerenter' => StylusPhase.added,
  'pointerdown' => isDown ? StylusPhase.down : StylusPhase.hover,
  'pointerup' when event.button == 0 || event.button == 5 => StylusPhase.up,
  'pointercancel' => StylusPhase.cancel,
  'pointerleave' => StylusPhase.removed,
  _ when isDown && !wasDown => StylusPhase.down,
  _ when !isDown && wasDown => StylusPhase.up,
  _ => isDown ? StylusPhase.move : StylusPhase.hover,
};

/// Whether a DOM event ends retained position and prediction state.
bool _isTerminal(web.PointerEvent event) =>
    event.type == 'pointercancel' ||
    event.type == 'pointerleave' ||
    (event.type == 'pointerup' && (event.button == 0 || event.button == 5));

/// Validates a delegated trail diameter supplied by application code.
void _validateDiameter(double diameter) {
  if (!diameter.isFinite || diameter <= 0) {
    throw RangeError.value(
      diameter,
      'diameter',
      'The delegated ink diameter must be finite and positive.',
    );
  }
}

/// Converts a Flutter color into a CSS rgba() value.
String _cssColor(Color color) {
  final int argb = color.toARGB32();
  final int alpha = argb >> 24 & 0xff;
  final int red = argb >> 16 & 0xff;
  final int green = argb >> 8 & 0xff;
  final int blue = argb & 0xff;
  return 'rgba($red, $green, $blue, ${alpha / 255})';
}

/// Encodes a browser's session-scoped pointer device identifier for clients.
String? _nativeDeviceIdentifier(int? identifier) =>
    identifier == null ? null : 'web-pointer-device:$identifier';
