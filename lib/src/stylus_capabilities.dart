import 'package:flutter/foundation.dart';

/// A stylus input feature that a backend or device can report.
enum StylusFeature {
  /// Variable tip pressure.
  pressure,

  /// The angle between the stylus and the surface normal.
  tilt,

  /// The direction in which the stylus leans across the surface.
  orientation,

  /// Hover distance above the input surface.
  distance,

  /// Rotation around the stylus' longitudinal axis.
  barrelRotation,

  /// Pressure applied to a tangential control such as an airbrush wheel.
  tangentialPressure,

  /// A primary button on the side of the stylus.
  primaryButton,

  /// A secondary button on the side of the stylus.
  secondaryButton,

  /// An inverted tip or dedicated eraser end.
  eraser,

  /// Pointer movement while the stylus is above the surface.
  hover,

  /// A double-tap interaction on the stylus body.
  doubleTap,

  /// A squeeze interaction on the stylus body.
  squeeze,
}

/// Describes the stylus features available through an input backend.
@immutable
class StylusCapabilities {
  /// Capabilities exposed by Flutter's portable pointer event model.
  static const StylusCapabilities flutter = StylusCapabilities(
    features: {
      StylusFeature.pressure,
      StylusFeature.tilt,
      StylusFeature.orientation,
      StylusFeature.distance,
      StylusFeature.primaryButton,
      StylusFeature.secondaryButton,
      StylusFeature.eraser,
      StylusFeature.hover,
    },
  );

  /// The immutable set of features the backend can provide.
  final Set<StylusFeature> features;

  /// Creates a capability description from [features].
  const StylusCapabilities({this.features = const {}});

  /// Creates a capability description from platform-channel feature names.
  factory StylusCapabilities.fromNames({required Iterable<String> names}) {
    final Set<StylusFeature> features = names.map(_featureFromName).whereType<StylusFeature>().toSet();
    return StylusCapabilities(features: Set.unmodifiable(features));
  }

  /// Whether [feature] can be reported by this backend.
  bool supports(StylusFeature feature) => features.contains(feature);

  /// Returns a description containing the features from both values.
  StylusCapabilities merge(StylusCapabilities other) => StylusCapabilities(features: Set.unmodifiable(features.followedBy(other.features).toSet()));

  @override
  bool operator ==(Object other) => identical(this, other) || other is StylusCapabilities && setEquals(features, other.features);

  @override
  int get hashCode => Object.hashAllUnordered(features);

  @override
  String toString() => 'StylusCapabilities(${features.map((feature) => feature.name).join(', ')})';
}

/// Converts a platform-channel name into a known feature.
StylusFeature? _featureFromName(String name) {
  for (final StylusFeature feature in StylusFeature.values) {
    if (feature.name == name) {
      return feature;
    }
  }
  return null;
}
