# vcpkg's arm64-android for the app (m-android A2): static libraries linked into libmain.so, built by the NDK's clang
# through vcpkg's Android toolchain (ANDROID_NDK_HOME; tools/android/build.sh sets it), API 33 as the app's minimum
# (Android 13: bionic's backtrace(), which Cemu's exception handler calls).
set(VCPKG_TARGET_ARCHITECTURE arm64)
set(VCPKG_CRT_LINKAGE dynamic)
set(VCPKG_LIBRARY_LINKAGE static)
set(VCPKG_CMAKE_SYSTEM_NAME Android)
set(VCPKG_CMAKE_SYSTEM_VERSION 33)
set(VCPKG_MAKE_BUILD_TRIPLET "--host=aarch64-linux-android")
set(VCPKG_CMAKE_CONFIGURE_OPTIONS -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-33)
set(VCPKG_BUILD_TYPE release)
set(VCPKG_ENV_PASSTHROUGH ANDROID_NDK_HOME)
