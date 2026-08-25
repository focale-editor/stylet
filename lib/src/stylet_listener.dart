import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:stylet/src/stylet_controller.dart';
import 'package:stylet/src/stylus_event.dart';

/// Signature for callbacks receiving normalized stylus motion.
typedef StylusMotionCallback = void Function(StylusMotionEvent event);

/// Signature for callbacks receiving a stylus body interaction.
typedef StylusActionCallback = void Function(StylusActionEvent event);

/// Signature for callbacks receiving a replaceable predicted trajectory.
typedef StylusPredictionCallback = void Function(StylusPredictionEvent event);

/// Signature for callbacks receiving an estimated-property correction.
typedef StylusCorrectionCallback = void Function(StylusCorrectionEvent event);

/// Normalizes pointer events for a subtree and exposes auxiliary native events.
class StyletListener extends StatefulWidget {
  /// Subtree whose pointer events should be observed.
  final Widget child;

  /// Callback invoked for each accepted pointer sample.
  final StylusMotionCallback onEvent;

  /// Callback invoked for native double-tap and squeeze interactions.
  final StylusActionCallback? onAction;

  /// Callback invoked whenever a pointer's temporary prediction is replaced.
  final StylusPredictionCallback? onPrediction;

  /// Callback invoked when an initially estimated sample receives new values.
  final StylusCorrectionCallback? onCorrection;

  /// Controller used for normalization and native event correlation.
  final Stylet? stylet;

  /// Whether mouse, touch, and unknown pointers should also be normalized.
  final bool includeNonStylus;

  /// How the underlying raw pointer listener participates in hit testing.
  final HitTestBehavior behavior;

  /// Creates a listener around [child].
  const StyletListener({
    super.key,
    required this.child,
    required this.onEvent,
    this.onAction,
    this.onPrediction,
    this.onCorrection,
    this.stylet,
    this.includeNonStylus = false,
    this.behavior = HitTestBehavior.deferToChild,
  });

  @override
  State<StyletListener> createState() => _StyletListenerState();
}

/// Maintains optional subscriptions to global auxiliary stylus events.
class _StyletListenerState extends State<StyletListener> {
  /// Controller currently serving this listener.
  late Stylet _stylet;

  /// Active body-action subscription, when [StyletListener.onAction] is set.
  StreamSubscription<StylusActionEvent>? _actionSubscription;

  /// Active motion-prediction subscription, when requested by the widget.
  StreamSubscription<StylusPredictionEvent>? _predictionSubscription;

  /// Active correction subscription, when requested by the widget.
  StreamSubscription<StylusCorrectionEvent>? _correctionSubscription;

  @override
  void initState() {
    super.initState();
    _stylet = widget.stylet ?? Stylet.instance;
    _subscribeToAuxiliaryEvents();
  }

  @override
  void didUpdateWidget(covariant StyletListener oldWidget) {
    super.didUpdateWidget(oldWidget);
    final Stylet nextStylet = widget.stylet ?? Stylet.instance;
    if (!identical(_stylet, nextStylet) ||
        oldWidget.onAction != widget.onAction ||
        oldWidget.onPrediction != widget.onPrediction ||
        oldWidget.onCorrection != widget.onCorrection) {
      _cancelAuxiliarySubscriptions();
      _stylet = nextStylet;
      _subscribeToAuxiliaryEvents();
    }
  }

  @override
  void dispose() {
    _cancelAuxiliarySubscriptions();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => Listener(
    behavior: widget.behavior,
    onPointerDown: _handlePointerEvent,
    onPointerMove: _handlePointerEvent,
    onPointerUp: _handlePointerEvent,
    onPointerHover: _handlePointerEvent,
    onPointerCancel: _handlePointerEvent,
    child: widget.child,
  );

  /// Starts forwarding each auxiliary stream with a configured callback.
  void _subscribeToAuxiliaryEvents() {
    if (widget.onAction != null) {
      _actionSubscription = _stylet.actions.listen(_handleAction);
    }
    if (widget.onPrediction != null) {
      _predictionSubscription = _stylet.predictions.listen(_handlePrediction);
    }
    if (widget.onCorrection != null) {
      _correctionSubscription = _stylet.corrections.listen(_handleCorrection);
    }
  }

  /// Cancels every optional native-event subscription owned by this state.
  void _cancelAuxiliarySubscriptions() {
    _actionSubscription?.cancel();
    _predictionSubscription?.cancel();
    _correctionSubscription?.cancel();
    _actionSubscription = null;
    _predictionSubscription = null;
    _correctionSubscription = null;
  }

  /// Normalizes one Flutter event and applies the pointer-kind filter.
  void _handlePointerEvent(PointerEvent event) {
    final StylusMotionEvent normalized = _stylet.convertPointerEvent(
      event: event,
    );
    if (widget.includeNonStylus || normalized.isStylus) {
      widget.onEvent(normalized);
    }
  }

  /// Forwards a body action to the latest widget callback.
  void _handleAction(StylusActionEvent event) => widget.onAction?.call(event);

  /// Forwards a prediction to the latest widget callback.
  void _handlePrediction(StylusPredictionEvent event) =>
      widget.onPrediction?.call(event);

  /// Forwards a correction to the latest widget callback.
  void _handleCorrection(StylusCorrectionEvent event) =>
      widget.onCorrection?.call(event);
}
