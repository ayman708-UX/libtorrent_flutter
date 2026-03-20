Pod::Spec.new do |s|
  s.name             = 'libtorrent_flutter'
  s.version          = '1.2.0'
  s.summary          = 'Flutter plugin for libtorrent with built-in streaming server.'
  s.description      = <<-DESC
  Native libtorrent 2.0 bindings for Flutter with an integrated HTTP streaming server.
                       DESC
  s.homepage         = 'https://github.com/ayman708-UX/libtorrent_flutter'
  s.license          = { :type => 'GPL-3.0', :file => '../LICENSE' }
  s.author           = { 'ayman708-UX' => 'ayman@example.com' }
  s.source           = { :path => '.' }

  s.dependency 'Flutter'
  s.frameworks = 'SystemConfiguration'
  s.platform = :ios, '13.0'
  s.ios.deployment_target = '13.0'
  s.swift_version = '5.0'

  # On iOS we must statically link — Apple does not allow dynamic libraries
  # in App Store submissions. The prebuilt .a is a fat static lib containing
  # libtorrent + torrent_bridge compiled for arm64.
  prebuilt_library  = 'liblibtorrent_flutter.a'
  prebuilt_absolute = File.join(__dir__, prebuilt_library)

  if File.exist?(prebuilt_absolute)
    # Use prebuilt static library — no build tools needed
    s.vendored_libraries = prebuilt_library
    s.source_files = 'Classes/**/*.swift'
    s.pod_target_xcconfig = {
      'DEFINES_MODULE' => 'YES',
      # Force the linker to include all symbols from the static lib
      # so DynamicLibrary.process() can find them via dlsym()
      'OTHER_LDFLAGS' => '-force_load "$(PODS_TARGET_SRCROOT)/liblibtorrent_flutter.a"',
    }
  else
    # Fallback: build from source (requires libtorrent headers + libs)
    s.source_files = 'Classes/**/*'
    s.pod_target_xcconfig = {
      'DEFINES_MODULE' => 'YES',
      'HEADER_SEARCH_PATHS' => '"$(PODS_TARGET_SRCROOT)/../src"',
      'OTHER_CPLUSPLUSFLAGS' => '-std=c++17 -DTORRENT_BRIDGE_EXPORTS -DTORRENT_NO_DEPRECATE -DTORRENT_USE_SSL=0',
      'CLANG_CXX_LANGUAGE_STANDARD' => 'c++17',
    }
  end
end
