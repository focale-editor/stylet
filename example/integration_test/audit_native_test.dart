import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';

/// Verifies the native channel directly, without the portable fallback.
void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();
  testWidgets('native Stylet channel returns Apple-specific features', (
    tester,
  ) async {
    final features = await const MethodChannel(
      'app.focaleeditor.stylet/methods',
    ).invokeListMethod<String>('getCapabilities');
    expect(features, contains('doubleTap'));
    expect(features, contains('pressure'));
  });
}
