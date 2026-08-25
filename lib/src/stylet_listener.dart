import 'dart:async';

import 'package:flutter/widgets.dart';
import 'package:stylet/src/stylet_controller.dart';
import 'package:stylet/src/stylus_event.dart';

/// Signature for callbacks receiving normalized stylus motion.
typedef StylusMotionCallback = void Function(StylusMotionEvent event);

/// Signature for callbacks receiving a stylus body interaction.
typedef StylusActionCallback = void Function(StylusActionEvent event);

/// Normalizes pointer events for a widget subtree and exposes stylus actions.
class StyletListener extends StatefulWidget {
  /// Subtree whose pointer events should be observed.
  final Widget child;

  /// Callback invoked for each accepted pointer sample.
  final StylusMotionCallback onEvent;

  /// Callback invoked for native double-tap and squeeze interactions.
  final StylusActionCallback? onAction;

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
    this.stylet,
    this.includeNonStylus = false,
    this.behavior = HitTestBehavior.deferToChild,
  });

  @override
  State<StyletListener> createState() => _StyletListenerState();
}

/// Maintains the optional subscription to global stylus body actions.
class _StyletListenerState extends State<StyletListener> {
  /// Controller currently serving this listener.
  late Stylet _stylet;

  /// Active body-action subscription, when [StyletListener.onAction] is set.
  StreamSubscription<StylusActionEvent>? _actionSubscription;

  @override
  void initState() {
    super.initState();
    _stylet = widget.stylet ?? Stylet.instance;
    _subscribeToActions();
  }

  @override
  void didUpdateWidget(covariant StyletListener oldWidget) {
    super.didUpdateWidget(oldWidget);
    final Stylet nextStylet = widget.stylet ?? Stylet.instance;
    if (!identical(_stylet, nextStylet) || oldWidget.onAction != widget.onAction) {
      _actionSubscription?.cancel();
      _actionSubscription = null;
      _stylet = nextStylet;
      _subscribeToActions();
    }
  }

  @override
  void dispose() {
    _actionSubscription?.cancel();
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

  /// Starts forwarding body actions when a callback is configured.
  void _subscribeToActions() {
    if (widget.onAction != null) {
      _actionSubscription = _stylet.actions.listen(_handleAction);
    }
  }

  /// Normalizes one Flutter event and applies the pointer-kind filter.
  void _handlePointerEvent(PointerEvent event) {
    final StylusMotionEvent normalized = _stylet.convertPointerEvent(event: event);
    if (widget.includeNonStylus || normalized.isStylus) {
      widget.onEvent(normalized);
    }
  }

  /// Forwards a body action to the latest widget callback.
  void _handleAction(StylusActionEvent event) => widget.onAction?.call(event);
}
