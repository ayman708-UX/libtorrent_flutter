Pod::Spec.new do |s|
  s.name             = 'libtorrent_flutter'
  s.version          = '1.0.0'
  s.summary          = 'Flutter plugin for libtorrent with built-in streaming server.'
  s.description      = <<-DESC
  Native libtorrent 2.0 bindings for Flutter with an integrated HTTP streaming server.
                       DESC
  s.homepage         = 'https://github.com/ayman708-UX/libtorrent_flutter'
  s.license          = { :type => 'MIT', :file => '../LICENSE' }
  s.author           = { 'ayman708-UX' => 'ayman@example.com' }
  s.source           = { :path => '.' }

  s.dependency 'FlutterMacOS'
  s.platform = :osx, '10.14'
  s.osx.deployment_target = '10.14'
  s.swift_version = '5.0'

  # Check for prebuilt dylib first
  prebuilt_relative = '../prebuilt/macos/universal/liblibtorrent_flutter.dylib'
  prebuilt_absolute = File.join(__dir__, prebuilt_relative)

  if File.exist?(prebuilt_absolute)
    # Use prebuilt — just bundle the dylib, no compilation needed
    s.vendored_libraries = prebuilt_relative
    s.source_files = 'Classes/**/*.swift'
  else
    # Build from source — needs Homebrew libtorrent
    s.source_files = 'Classes/**/*'
    s.pod_target_xcconfig = {
      'DEFINES_MODULE' => 'YES',
      'HEADER_SEARCH_PATHS' => [
        '"$(PODS_TARGET_SRCROOT)/../src"',
        '"/opt/homebrew/include"',
        '"/usr/local/include"',
      ].join(' '),
      'LIBRARY_SEARCH_PATHS' => [
        '"/opt/homebrew/lib"',
        '"/usr/local/lib"',
      ].join(' '),
      'OTHER_LDFLAGS' => '-ltorrent-rasterbar -lboost_system -lssl -lcrypto',
      'OTHER_CPLUSPLUSFLAGS' => '-std=c++17 -DTORRENT_BRIDGE_EXPORTS -DTORRENT_NO_DEPRECATE',
      'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    }
  end
end
