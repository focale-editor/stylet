import 'dart:async';
import 'dart:developer' as developer;

import 'package:flutter/gestures.dart';
import 'package:stylet/src/stylus_capabilities.dart';
import 'package:stylet/src/stylus_event.dart';
import 'package:stylet/stylet_platform_interface.dart';

/// Coordinates portable Flutter pointer data with native stylus extensions.
class Stylet {
  /// Shared controller suitable for most applications.
  static final Stylet instance = Stylet();

  /// Largest accepted clock difference between matching native and Flutter samples.
  static const Duration _maximumEnhancementAge = Duration(milliseconds: 120);

  /// Largest accepted squared position difference between matching samples.
  static const double _maximumEnhancementDistanceSquared = 64;

  /// Maximum native samples retained for short-lived correlation.
  static const int _maximumCachedMotions = 64;

  /// Platform backend supplying native extensions.
  final StyletPlatform _platform;

  /// Broadcast form of the backend event stream.
  late final Stream<StyletEvent> _events;

  /// Subscription that maintains the native correlation cache.
  StreamSubscription<StyletEvent>? _cacheSubscription;

  /// Recent native motion samples ordered from oldest to newest.
  final List<StylusMotionEvent> _recentNativeMotions = [];

  /// Whether this controller has released its platform subscription.
  bool _isDisposed = false;

  /// Creates a controller backed by [platform] or the registered backend.
  Stylet({StyletPlatform? platform}) : _platform = platform ?? StyletPlatform.instance {
    final Stream<StyletEvent> platformEvents = _platform.events;
    _events = platformEvents.isBroadcast ? platformEvents : platformEvents.asBroadcastStream();
    _cacheSubscription = _events.listen(_rememberNativeEvent, onError: _reportPlatformError);
  }

  /// Native motion samples and body interactions reported by the platform.
  Stream<StyletEvent> get events => _events;

  /// Native motion samples, including axes Flutter does not expose directly.
  Stream<StylusMotionEvent> get nativeMotions => _events.where((event) => event is StylusMotionEvent).cast<StylusMotionEvent>();

  /// Double-tap and squeeze interactions reported by supported styluses.
  Stream<StylusActionEvent> get actions => _events.where((event) => event is StylusActionEvent).cast<StylusActionEvent>();

  /// Features the active platform backend can potentially provide.
  Future<StylusCapabilities> get capabilities => _platform.getCapabilities();

  /// Normalizes [event] and merges a recent matching native sample when possible.
  StylusMotionEvent convertPointerEvent({required PointerEvent event}) {
    if (_isDisposed) {
      throw StateError('This Stylet controller has been disposed.');
    }
    return StylusMotionEvent.fromPointerEvent(event: event, enhancement: _matchingEnhancement(event));
  }

  /// Releases this controller's native event subscription.
  ///
  /// Applications should not dispose the shared [instance].
  Future<void> dispose() async {
    if (_isDisposed) {
      return;
    }
    _isDisposed = true;
    final StreamSubscription<StyletEvent>? subscription = _cacheSubscription;
    _cacheSubscription = null;
    _recentNativeMotions.clear();
    await subscription?.cancel();
  }

  /// Retains native motion samples used to enrich Flutter pointer events.
  void _rememberNativeEvent(StyletEvent event) {
    if (event is! StylusMotionEvent) {
      return;
    }
    _recentNativeMotions.add(event);
    if (_recentNativeMotions.length > _maximumCachedMotions) {
      _recentNativeMotions.removeRange(0, _recentNativeMotions.length - _maximumCachedMotions);
    }
  }

  /// Finds the closest native sample representing [event].
  StylusMotionEvent? _matchingEnhancement(PointerEvent event) {
    final int embedderIdentifier = event.embedderId;
    if (embedderIdentifier != 0) {
      for (final StylusMotionEvent motion in _recentNativeMotions.reversed) {
        if (motion.embedderIdentifier == embedderIdentifier) {
          return motion;
        }
      }
    }

    final StylusTool expectedTool = switch (event.kind) {
      PointerDeviceKind.stylus => StylusTool.pen,
      PointerDeviceKind.invertedStylus => StylusTool.eraser,
      _ => StylusTool.unknown,
    };
    for (final StylusMotionEvent motion in _recentNativeMotions.reversed) {
      if (expectedTool != StylusTool.unknown && motion.tool != expectedTool) {
        continue;
      }
      if (!_phasesCanMatch(event: event, motion: motion)) {
        continue;
      }
      final Duration difference = event.timeStamp - motion.timeStamp;
      if (difference.inMicroseconds.abs() > _maximumEnhancementAge.inMicroseconds) {
        continue;
      }
      final Offset positionDifference = event.position - motion.position;
      if (positionDifference.distanceSquared <= _maximumEnhancementDistanceSquared) {
        return motion;
      }
    }
    return null;
  }

  /// Whether a Flutter event and native sample can describe the same lifecycle stage.
  bool _phasesCanMatch({required PointerEvent event, required StylusMotionEvent motion}) => switch (event) {
    PointerDownEvent() => motion.phase == StylusPhase.down,
    PointerMoveEvent() => motion.phase == StylusPhase.move,
    PointerUpEvent() => motion.phase == StylusPhase.up,
    PointerCancelEvent() => motion.phase == StylusPhase.cancel,
    PointerHoverEvent() => motion.phase == StylusPhase.hover,
    _ => true,
  };

  /// Reports backend stream failures without terminating independent consumers.
  void _reportPlatformError(Object error, StackTrace stackTrace) {
    developer.log('The native stylus event stream reported an error.', name: 'stylet', error: error, stackTrace: stackTrace);
  }
}
