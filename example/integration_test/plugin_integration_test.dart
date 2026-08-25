import 'package:flutter_test/flutter_test.dart';
import 'package:integration_test/integration_test.dart';
import 'package:stylet/stylet.dart';

void main() {
  IntegrationTestWidgetsFlutterBinding.ensureInitialized();

  testWidgets('the native backend reports at least Flutter capabilities', (
    tester,
  ) async {
    final StylusCapabilities capabilities = await Stylet.instance.capabilities;

    expect(capabilities.supports(StylusFeature.pressure), isTrue);
  });
}
