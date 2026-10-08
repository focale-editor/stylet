import 'dart:io';

import 'package:checks/checks.dart';
import 'package:flutter_test/flutter_test.dart';

/// Compiles and runs the real Objective-C bridge on macOS without a tablet.
void main() {
  test(
    'Wacom bridge compiles and preserves address and timestamp contracts',
    () async {
      final Directory temporary = await Directory.systemTemp.createTemp(
        'stylet-wacom-test-',
      );
      addTearDown(() => temporary.delete(recursive: true));
      final String executable = '${temporary.path}/stylet-wacom-test';
      final ProcessResult compilation = await Process.run('xcrun', [
        '--sdk',
        'macosx',
        'clang',
        '-fobjc-arc',
        '-fmodules',
        '-Werror=implicit-function-declaration',
        '-framework',
        'AppKit',
        '-framework',
        'ApplicationServices',
        'test/native/stylet_wacom_bridge_test.m',
        '-o',
        executable,
      ]);
      check(
        compilation.exitCode,
        because: '${compilation.stdout}\n${compilation.stderr}',
      ).equals(0);
      final ProcessResult execution = await Process.run(executable, []);
      check(
        execution.exitCode,
        because: '${execution.stdout}\n${execution.stderr}',
      ).equals(0);
    },
    skip: !Platform.isMacOS ? 'Requires the macOS SDK and runtime.' : false,
  );
}
