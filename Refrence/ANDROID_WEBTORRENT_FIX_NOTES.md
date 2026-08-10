# Android WebTorrent / OpenSSL repair

This patch addresses the Android-only `wss://` tracker failure in three layers:

1. `assets/cacert.pem` is now actually shipped with the Flutter package.
2. Dart copies the bundle atomically into the app cache and calls
   `lt_set_ssl_cert_path()` before constructing the native session.
3. The Android bridge validates that OpenSSL can parse the bundle before
   creating libtorrent, and leaves `validate_https_trackers` enabled.

The Android CI build also passes `-Dwebtorrent=ON` explicitly and verifies that
libdatachannel, libjuice, and usrsctp were produced.

## Important

The `.so` files already present in `prebuilt/android/` were compiled before the
native diagnostics in this patch. The bundled-asset/Dart portion can be tested
with them, because they already export `lt_set_ssl_cert_path`.

To test the complete native patch, push this branch and run the GitHub Actions
`build-android` job, then replace the three `prebuilt/android/<abi>/` libraries
with the newly built artifacts (or publish a new release version).

## Android verification

Run the app and filter Logcat:

```bash
adb logcat -c
adb logcat -s libtorrent_native flutter
```

Expected native lines after a rebuilt `.so`:

```text
SSL_CERT_FILE set to /data/user/0/.../cache/libtorrent_flutter/cacert.pem
Validated OpenSSL CA bundle: /data/user/0/.../cacert.pem (... bytes)
```

Then test a magnet containing at least one `wss://` tracker. Keep the complete
Logcat if metadata still times out; at that point the certificate path is no
longer a guess and the tracker/WebRTC alerts can be diagnosed directly.
