import 'dart:io';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:stylet_example/main.dart';

/// Verifies the native channel directly, without the portable fallback.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('native Stylet channel returns platform capabilities', (
    tester,
  ) async {
    await tester.pumpWidget(const StyletExampleApp());
    await tester.pumpAndSettle();
    final List<String>? features = await const MethodChannel(
      'app.focaleeditor.stylet/methods',
    ).invokeListMethod<String>('getCapabilities');
    expect(features, contains('pressure'));
    if (Platform.isWindows) {
      expect(
        features,
        containsAll(['barrelRotation', 'historicalSamples', 'deviceInfo']),
      );
    } else if (Platform.isIOS || Platform.isMacOS) {
      expect(features, contains('doubleTap'));
    }
  });
}
