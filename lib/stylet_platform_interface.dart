import 'package:plugin_platform_interface/plugin_platform_interface.dart';
import 'package:stylet/src/stylus_capabilities.dart';
import 'package:stylet/src/stylus_event.dart';
import 'package:stylet/stylet_method_channel.dart';

/// Contract implemented by Stylet's platform backends.
abstract class StyletPlatform extends PlatformInterface {
  /// Token that prevents accidental implementation without extending this class.
  static final Object _token = Object();

  /// Active backend used by newly created Stylet controllers.
  static StyletPlatform _instance = MethodChannelStylet();

  /// Creates a token-verified Stylet platform backend.
  StyletPlatform() : super(token: _token);

  /// The active platform backend.
  static StyletPlatform get instance => _instance;

  /// Replaces the active backend, primarily for federated plugins and tests.
  static set instance(StyletPlatform instance) {
    PlatformInterface.verifyToken(instance, _token);
    _instance = instance;
  }

  /// Native stylus samples and body interactions from this backend.
  Stream<StyletEvent> get events => const Stream.empty();

  /// Returns every feature this backend can potentially expose.
  Future<StylusCapabilities> getCapabilities() async => StylusCapabilities.flutter;
}
