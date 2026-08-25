#
# To learn more about a Podspec see http://guides.cocoapods.org/syntax/podspec.html.
# Run `pod lib lint stylet.podspec` to validate before publishing.
#
Pod::Spec.new do |s|
  s.name             = 'stylet'
  s.version          = '0.1.0'
  s.summary          = 'High-fidelity stylus input for Flutter.'
  s.description      = <<-DESC
Pressure, tilt, barrel rotation, and tablet controls for Flutter.
                       DESC
  s.homepage         = 'https://github.com/focale-editor/stylet'
  s.license          = { :file => '../LICENSE' }
  s.author           = 'Hugo Delaunay'

  s.source           = { :path => '.' }
  s.source_files = 'stylet/Sources/stylet/**/*'

  # If your plugin requires a privacy manifest, for example if it collects user
  # data, update the PrivacyInfo.xcprivacy file to describe your plugin's
  # privacy impact, and then uncomment this line. For more information,
  # see https://developer.apple.com/documentation/bundleresources/privacy_manifest_files
  # s.resource_bundles = {'stylet_privacy' => ['stylet/Sources/stylet/PrivacyInfo.xcprivacy']}

  s.dependency 'FlutterMacOS'

  s.platform = :osx, '12.0'
  s.pod_target_xcconfig = { 'DEFINES_MODULE' => 'YES' }
  s.swift_version = '5.0'
end
