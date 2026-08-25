#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint stylet.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'stylet'
  s.version          = '0.1.0'
  s.summary          = 'High-fidelity stylus input for Flutter.'
  s.description      = <<-DESC
Pressure, tilt, barrel rotation, double-tap, and squeeze input for Flutter.
                       DESC
  s.homepage         = 'https://github.com/focale-editor/stylet'
  s.license          = { :file => '../LICENSE' }
  s.author           = 'Hugo Delaunay'
  s.source           = { :path => '.' }
  s.source_files = 'stylet/Sources/stylet/**/*'
  s.dependency 'Flutter'
  s.platform = :ios, '15.0'

  # Flutter.framework does not contain a i386 slice.
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES', 'EXCLUDED_ARCHS[sdk=iphonesimulator*]' => 'i386' }
  s.swift_version = '5.0'

  # If your plugin requires a privacy manifest, for example if it uses any
  # required reason APIs, update the PrivacyInfo.xcprivacy file to describe your
  # plugin's privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'stylet_privacy' => ['stylet/Sources/stylet/PrivacyInfo.xcprivacy']}
end
