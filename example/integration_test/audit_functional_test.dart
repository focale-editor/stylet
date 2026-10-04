import 'dart:ui';

import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:stylet/stylet.dart';
import 'package:stylet_example/main.dart' as example;

/// Checks native capabilities and the example's response to a stylus gesture.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('example renders and accepts a portable stylus gesture', (
    tester,
  ) async {
    example.main();
    await tester.pumpAndSettle();
    expect(find.text('Stylet input laboratory'), findsOneWidget);
    final capabilities = await Stylet.instance.capabilities;
    expect(capabilities.supports(StylusFeature.pressure), isTrue);
    final center = tester.getCenter(find.byType(StyletListener));
    final gesture = await tester.startGesture(
      center,
      kind: PointerDeviceKind.stylus,
    );
    await gesture.moveBy(const Offset(30, 20));
    await gesture.up();
    await tester.pumpAndSettle();
    expect(tester.takeException(), isNull);
  });
}
