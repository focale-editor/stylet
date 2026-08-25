import 'dart:async';

import 'package:checks/checks.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/widgets.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:stylet/stylet.dart';
import 'package:stylet/stylet_platform_interface.dart';

/// In-memory backend used to drive controller and widget tests synchronously.
class _FakeStyletPlatform extends StyletPlatform {
  /// Stream controller that represents the native event channel.
  final StreamController<StyletEvent> _controller = StreamController.broadcast(sync: true);

  /// Capabilities returned to the controller under test.
  final StylusCapabilities capabilities;

  /// Creates a fake backend with optional [capabilities].
  _FakeStyletPlatform({this.capabilities = StylusCapabilities.flutter});

  @override
  Stream<StyletEvent> get events => _controller.stream;

  @override
  Future<StylusCapabilities> getCapabilities() async => capabilities;

  /// Emits [event] as if it came from native code.
  void emit(StyletEvent event) => _controller.add(event);

  /// Releases the fake event stream.
  Future<void> dispose() => _controller.close();
}

void main() {
  group('StylusMotionEvent', () {
    test('normalizes pressure, distance, and side buttons', () {
      const PointerMoveEvent pointer = PointerMoveEvent(
        timeStamp: Duration(milliseconds: 10),
        pointer: 4,
        kind: PointerDeviceKind.stylus,
        position: Offset(20, 30),
        buttons: kPrimaryButton | kPrimaryStylusButton,
        pressure: 550,
        pressureMin: 100,
        pressureMax: 1000,
        distanceMax: 8,
        tilt: 0.2,
        orientation: 0.7,
      );

      final StylusMotionEvent event = StylusMotionEvent.fromPointerEvent(event: pointer);

      check(event.normalizedPressure).isNotNull().equals(0.5);
      check(event.isSideButtonPressed(number: 1)).isTrue();
      check(event.isSideButtonPressed(number: 2)).isFalse();
      check(event.supports(StylusFeature.pressure)).isTrue();
      check(event.supports(StylusFeature.distance)).isTrue();
      check(event.tool).equals(StylusTool.pen);

      const PointerHoverEvent hover = PointerHoverEvent(kind: PointerDeviceKind.stylus, distance: 4, distanceMax: 8);
      check(StylusMotionEvent.fromPointerEvent(event: hover).normalizedDistance).isNotNull().equals(0.5);
    });

    test('recognizes the eraser and validates button numbers', () {
      const PointerHoverEvent pointer = PointerHoverEvent(kind: PointerDeviceKind.invertedStylus);
      final StylusMotionEvent event = StylusMotionEvent.fromPointerEvent(event: pointer);

      check(event.tool).equals(StylusTool.eraser);
      check(() => event.isSideButtonPressed(number: 0)).throws<RangeError>();
    });
  });

  group('Stylet', () {
    test('enriches a matching Flutter event with native barrel data', () async {
      final _FakeStyletPlatform platform = _FakeStyletPlatform();
      final Stylet stylet = Stylet(platform: platform);
      const StylusMotionEvent native = StylusMotionEvent(
        timeStamp: Duration(milliseconds: 20),
        source: StyletEventSource.native,
        phase: StylusPhase.move,
        tool: StylusTool.pen,
        embedderIdentifier: 83,
        position: Offset(100, 80),
        localPosition: Offset(100, 80),
        barrelRotation: 1.25,
        tangentialPressure: -0.4,
        features: {StylusFeature.barrelRotation, StylusFeature.tangentialPressure},
      );
      platform.emit(native);

      const PointerMoveEvent pointer = PointerMoveEvent(
        timeStamp: Duration(milliseconds: 20),
        pointer: 2,
        kind: PointerDeviceKind.stylus,
        position: Offset(100, 80),
        embedderId: 83,
      );
      final StylusMotionEvent event = stylet.convertPointerEvent(event: pointer);

      check(event.source).equals(StyletEventSource.combined);
      check(event.barrelRotation).equals(1.25);
      check(event.tangentialPressure).equals(-0.4);
      check(event.originalEvent).isNotNull().identicalTo(pointer);
      await stylet.dispose();
      await platform.dispose();
    });

    test('does not merge a stale nearby sample', () async {
      final _FakeStyletPlatform platform = _FakeStyletPlatform();
      final Stylet stylet = Stylet(platform: platform);
      platform.emit(
        const StylusMotionEvent(
          timeStamp: Duration(milliseconds: 1),
          source: StyletEventSource.native,
          phase: StylusPhase.move,
          tool: StylusTool.pen,
          position: Offset.zero,
          localPosition: Offset.zero,
          barrelRotation: 2,
        ),
      );

      const PointerMoveEvent pointer = PointerMoveEvent(
        timeStamp: Duration(seconds: 1),
        kind: PointerDeviceKind.stylus,
        position: Offset.zero,
      );
      final StylusMotionEvent event = stylet.convertPointerEvent(event: pointer);

      check(event.source).equals(StyletEventSource.flutter);
      check(event.barrelRotation).isNull();
      await stylet.dispose();
      await platform.dispose();
    });

    test('forwards action events and platform capabilities', () async {
      const StylusCapabilities capabilities = StylusCapabilities(features: {StylusFeature.barrelRotation, StylusFeature.squeeze});
      final _FakeStyletPlatform platform = _FakeStyletPlatform(capabilities: capabilities);
      final Stylet stylet = Stylet(platform: platform);
      final Future<StylusActionEvent> nextAction = stylet.actions.first;

      platform.emit(
        const StylusActionEvent(
          timeStamp: Duration(seconds: 1),
          source: StyletEventSource.native,
          action: StylusAction.squeeze,
          phase: StylusActionPhase.ended,
        ),
      );

      check((await nextAction).action).equals(StylusAction.squeeze);
      check(await stylet.capabilities).equals(capabilities);
      await stylet.dispose();
      await platform.dispose();
    });

    test('rejects conversion after disposal', () async {
      final _FakeStyletPlatform platform = _FakeStyletPlatform();
      final Stylet stylet = Stylet(platform: platform);
      await stylet.dispose();

      check(() => stylet.convertPointerEvent(event: const PointerHoverEvent(kind: PointerDeviceKind.stylus))).throws<StateError>();
      await platform.dispose();
    });
  });

  testWidgets('StyletListener filters non-stylus input and forwards actions', (tester) async {
    final _FakeStyletPlatform platform = _FakeStyletPlatform();
    final Stylet stylet = Stylet(platform: platform);
    addTearDown(stylet.dispose);
    addTearDown(platform.dispose);
    final List<StylusMotionEvent> motions = [];
    final List<StylusActionEvent> actions = [];
    await tester.pumpWidget(
      Directionality(
        textDirection: TextDirection.ltr,
        child: StyletListener(
          stylet: stylet,
          onEvent: motions.add,
          onAction: actions.add,
          behavior: HitTestBehavior.opaque,
          child: const SizedBox.expand(),
        ),
      ),
    );

    final TestGesture mouse = await tester.createGesture(kind: PointerDeviceKind.mouse, pointer: 1);
    await mouse.down(const Offset(10, 10));
    await mouse.up();
    await mouse.removePointer();
    final TestGesture stylus = await tester.createGesture(kind: PointerDeviceKind.stylus, pointer: 2);
    await stylus.down(const Offset(10, 10));
    await stylus.up();
    await stylus.removePointer();
    platform.emit(
      const StylusActionEvent(
        timeStamp: Duration.zero,
        source: StyletEventSource.native,
        action: StylusAction.doubleTap,
        phase: StylusActionPhase.discrete,
      ),
    );
    check(motions).length.equals(2);
    check(motions.first.tool).equals(StylusTool.pen);
    check(actions).length.equals(1);
    check(actions.single.action).equals(StylusAction.doubleTap);
    await tester.pumpWidget(const SizedBox.shrink());
    await tester.pump();
  });
}
