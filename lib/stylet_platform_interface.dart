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

  /// Every motion, prediction, correction, body action, device, and pad event.
  Stream<StyletEvent> get events => const Stream.empty();

  /// Returns every feature this backend can potentially expose.
  Future<StylusCapabilities> getCapabilities() async =>
      StylusCapabilities.flutter;

  /// Enables or releases vendor-driver ownership of tablet-pad controls.
  ///
  /// Windows and macOS drivers require an application to override the user's
  /// normal ExpressKey, ring, and strip mappings before they deliver raw
  /// control events. Implementations return whether at least one compatible
  /// control accepted the requested state. Platforms with passive pad input
  /// leave this disabled and return false.
  Future<bool> setTabletPadOverrideEnabled({required bool enabled}) async =>
      false;
}
