import 'package:flutter_test/flutter_test.dart';
import 'package:stylet_example/main.dart';

void main() {
  testWidgets('the example presents the stylus laboratory', (tester) async {
    await tester.pumpWidget(const StyletExampleApp());
    await tester.pump();

    expect(find.text('Stylet input laboratory'), findsOneWidget);
    expect(find.text('Pressure'), findsOneWidget);
    expect(find.text('Barrel'), findsOneWidget);
  });
}
