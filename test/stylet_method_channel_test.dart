import 'package:checks/checks.dart';
import 'package:flutter/gestures.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:stylet/stylet.dart';
import 'package:stylet/stylet_method_channel.dart';

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  const MethodChannel methodChannel = MethodChannel('dev.focale.stylet/methods');
  final TestDefaultBinaryMessenger messenger = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;

  tearDown(() => messenger.setMockMethodCallHandler(methodChannel, null));

  test('decodes native motion data without losing advanced axes', () {
    final StyletEvent decoded = decodeStyletEvent({
      'type': 'motion',
      'timestampMicros': 1234,
      'phase': 'move',
      'tool': 'eraser',
      'pointerIdentifier': 7,
      'nativeDeviceIdentifier': 'tablet:42',
      'x': 10.5,
      'y': 20,
      'buttons': kPrimaryStylusButton,
      'isDown': true,
      'pressure': 0.75,
      'pressureMinimum': 0,
      'pressureMaximum': 1,
      'tiltX': 0.1,
      'tiltY': -0.2,
      'barrelRotation': 2.4,
      'tangentialPressure': -0.5,
      'wheelDelta': 0.125,
      'features': ['pressure', 'barrelRotation', 'tangentialPressure', 'wheel'],
    });

    check(decoded).isA<StylusMotionEvent>()
      ..has((event) => event.phase, 'phase').equals(StylusPhase.move)
      ..has((event) => event.tool, 'tool').equals(StylusTool.eraser)
      ..has((event) => event.position, 'position').equals(const Offset(10.5, 20))
      ..has((event) => event.barrelRotation, 'barrelRotation').equals(2.4)
      ..has((event) => event.tangentialPressure, 'tangentialPressure').equals(-0.5)
      ..has((event) => event.wheelDelta, 'wheelDelta').equals(0.125)
      ..has((event) => event.nativeDeviceIdentifier, 'nativeDeviceIdentifier').equals('tablet:42');
  });

  test('decodes a squeeze with its hover pose', () {
    final StyletEvent decoded = decodeStyletEvent({
      'type': 'action',
      'timestampMicros': 9000,
      'action': 'squeeze',
      'phase': 'ended',
      'pose': {'x': 4, 'y': 8, 'distance': 0.3, 'tilt': 0.4, 'orientation': 0.5, 'barrelRotation': 0.6},
    });

    check(decoded).isA<StylusActionEvent>()
      ..has((event) => event.action, 'action').equals(StylusAction.squeeze)
      ..has((event) => event.phase, 'phase').equals(StylusActionPhase.ended)
      ..has((event) => event.pose?.position, 'pose.position').equals(const Offset(4, 8))
      ..has((event) => event.pose?.barrelRotation, 'pose.barrelRotation').equals(0.6);
  });

  test('expands historical batches and decodes device and pad events', () {
    final List<StyletEvent> decoded = decodeStyletEvents({
      'type': 'batch',
      'events': [
        {
          'type': 'device',
          'timestampMicros': 100,
          'phase': 'added',
          'kind': 'tablet',
          'nativeDeviceIdentifier': 'usb:056a:0357',
          'name': 'Drawing tablet',
          'vendorIdentifier': 0x056a,
          'productIdentifier': 0x0357,
          'buttonCount': 8,
          'features': ['deviceInfo', 'tabletPadButtons', 'tabletPadRing'],
        },
        {
          'type': 'pad',
          'timestampMicros': 200,
          'nativeDeviceIdentifier': 'usb:056a:0357:pad',
          'control': 'ring',
          'controlIndex': 0,
          'phase': 'changed',
          'value': 0.25,
          'mode': 1,
        },
        {
          'type': 'pad',
          'timestampMicros': 300,
          'nativeDeviceIdentifier': 'usb:056a:0357:pad',
          'control': 'dial',
          'controlIndex': 0,
          'phase': 'discrete',
          'value': -0.5,
          'mode': 1,
        },
      ],
    }).toList();

    check(decoded).length.equals(3);
    check(decoded.first).isA<StylusDeviceEvent>()
      ..has((event) => event.device.name, 'device.name').equals('Drawing tablet')
      ..has((event) => event.device.buttonCount, 'device.buttonCount').equals(8)
      ..has((event) => event.device.features, 'device.features').contains(StylusFeature.tabletPadRing);
    check(decoded[1]).isA<TabletPadEvent>()
      ..has((event) => event.control, 'control').equals(TabletPadControl.ring)
      ..has((event) => event.value, 'value').equals(0.25)
      ..has((event) => event.mode, 'mode').equals(1);
    check(decoded.last).isA<TabletPadEvent>()
      ..has((event) => event.control, 'control').equals(TabletPadControl.dial)
      ..has((event) => event.phase, 'phase').equals(TabletPadPhase.discrete)
      ..has((event) => event.value, 'value').equals(-0.5);
  });

  test('expands a top-level native event list', () {
    final List<StyletEvent> decoded = decodeStyletEvents([
      {'type': 'motion', 'timestampMicros': 1, 'phase': 'hover', 'x': 1, 'y': 2},
      {'type': 'motion', 'timestampMicros': 2, 'phase': 'move', 'x': 2, 'y': 3},
    ]).toList();

    check(decoded).length.equals(2);
    check(decoded.first).isA<StylusMotionEvent>().has((event) => event.phase, 'phase').equals(StylusPhase.hover);
    check(decoded.last).isA<StylusMotionEvent>().has((event) => event.phase, 'phase').equals(StylusPhase.move);
  });

  test('rejects malformed native values', () {
    check(
      () => decodeStyletEvent({'type': 'motion', 'timestampMicros': 1, 'phase': 'impossible', 'x': 0, 'y': 0}),
    ).throws<FormatException>();
    check(
      () => decodeStyletEvent({
        'type': 'action',
        'timestampMicros': 1,
        'action': 'squeeze',
        'phase': 'ended',
        'pose': {'x': 1},
      }),
    ).throws<FormatException>();
    check(
      () => decodeStyletEvent({
        'type': 'pad',
        'timestampMicros': 1,
        'nativeDeviceIdentifier': 'pad',
        'control': 'button',
        'controlIndex': -1,
        'phase': 'began',
      }),
    ).throws<FormatException>();
    check(
      () => decodeStyletEvent({
        'type': 'device',
        'timestampMicros': 1,
        'phase': 'added',
        'kind': 'pad',
        'nativeDeviceIdentifier': 'pad',
        'buttonCount': -1,
      }),
    ).throws<FormatException>();
  });

  test('combines native capabilities with portable Flutter input', () async {
    messenger.setMockMethodCallHandler(methodChannel, (call) async {
      check(call.method).equals('getCapabilities');
      return ['barrelRotation', 'squeeze'];
    });
    final MethodChannelStylet platform = MethodChannelStylet(methodChannel: methodChannel);

    final StylusCapabilities capabilities = await platform.getCapabilities();

    check(capabilities.supports(StylusFeature.pressure)).isTrue();
    check(capabilities.supports(StylusFeature.barrelRotation)).isTrue();
    check(capabilities.supports(StylusFeature.squeeze)).isTrue();
  });

  test('falls back to portable capabilities without a native plugin', () async {
    final MethodChannelStylet platform = MethodChannelStylet(methodChannel: methodChannel);

    final StylusCapabilities capabilities = await platform.getCapabilities();

    check(capabilities).equals(StylusCapabilities.flutter);
  });
}
