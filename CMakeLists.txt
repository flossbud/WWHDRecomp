# WWHD's build: wwhd-null (src/CMakeLists.txt) with Cemu's libraries built from Cemu's source tree,
# which is Cemu at the pinned commit with tools/reference/cemu-patches applied (on the worker,
# tools/worker/setup-volume.sh cemu puts it in /wwhd/opt/cemu-src). src/README.md has the commands.
cmake_minimum_required(VERSION 3.24)

set(CEMU_SRC "/wwhd/opt/cemu-src" CACHE PATH "Cemu's source tree: the pinned commit with tools/reference/cemu-patches applied")
if(NOT EXISTS "${CEMU_SRC}/src/Cafe/CafeSystem.cpp")
	message(FATAL_ERROR "CEMU_SRC=${CEMU_SRC} is not Cemu's source tree (tools/worker/setup-volume.sh cemu)")
endif()
set(WWHD_SDL3 "/wwhd/opt/sdl3" CACHE PATH "SDL3 with video, for the window (tools/worker/setup-volume.sh sdl3); empty: vcpkg's, which Cemu builds for controllers only")

# Cemu's dependencies come from vcpkg with Cemu's manifest. Cemu's own CMakeLists.txt sets that up
# before its project(); as Cemu is our subproject, it happens here, before ours, the same way.
set(THREADS_PREFER_PTHREAD_FLAG true)
set(WWHD_VCPKG_MANIFEST "${CEMU_SRC}" CACHE PATH "vcpkg's manifest directory: Cemu's, or tools/arm64/build.sh's trimmed copy")
set(VCPKG_MANIFEST_DIR "${WWHD_VCPKG_MANIFEST}")
# WWHD_VCPKG_SYSTEM_PORTS=OFF (tools/arm64/build.sh): no Linux overlay, whose ports (libpng, glm, gtk3, cairo) are
# empty stand-ins for the system's packages, which a cross build's sysroot doesn't have
option(WWHD_VCPKG_SYSTEM_PORTS "Cemu's Linux overlay: libpng, glm, gtk3 and cairo from the system" ON)
if(NOT WWHD_VCPKG_SYSTEM_PORTS)
	set(VCPKG_OVERLAY_PORTS "${CEMU_SRC}/dependencies/vcpkg_overlay_ports")
elseif(UNIX AND NOT APPLE)
	set(VCPKG_OVERLAY_PORTS "${CEMU_SRC}/dependencies/vcpkg_overlay_ports_linux;${CEMU_SRC}/dependencies/vcpkg_overlay_ports")
elseif(APPLE)
	set(VCPKG_OVERLAY_PORTS "${CEMU_SRC}/dependencies/vcpkg_overlay_ports_mac;${CEMU_SRC}/dependencies/vcpkg_overlay_ports")
else()
	set(VCPKG_OVERLAY_PORTS "${CEMU_SRC}/dependencies/vcpkg_overlay_ports_win;${CEMU_SRC}/dependencies/vcpkg_overlay_ports")
endif()
set(CMAKE_TOOLCHAIN_FILE "${CEMU_SRC}/dependencies/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")
set(CMAKE_FIND_PACKAGE_PREFER_CONFIG TRUE)
if(WIN32)
	set(VCPKG_TARGET_TRIPLET "x64-windows-static" CACHE STRING "")
endif()
if(WWHD_SDL3)
	set(SDL3_DIR "${WWHD_SDL3}/lib/cmake/SDL3" CACHE PATH "")
endif()

project(wwhd C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The same float results and char on every ISA, Cemu's targets included (docs/research/android-plan.md):
# arm64 has fused multiply-add and clang would contract a*b+c with it (x86-64 has none at its baseline,
# so this changes nothing there), and char is unsigned on Linux and Android arm64.
add_compile_options(-ffp-contract=off -fsigned-char)

# Cemu as its own build configures it, without its wxWidgets GUI: our frontend implements its
# WindowSystem interface. Only what wwhd-null links is built.
# Its find_package() targets are global, so src/ can link what Cemu's targets link.
# Nor its Wiimote support (HIDAPI, BlueZ), as tools/arm64/build.sh and tools/android/build.sh already leave it out: our
# frontend reads controllers through SDL itself. Cemu's Wiimote provider's connection thread (HidapiWiimote::get_devices,
# SDL_hid_init) and its SDL provider's thread (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) both start in CemuCommonInit and both
# set up SDL's udev, whose SDL_UDEV_Init has no lock: it publishes its state before loading libudev's functions, so the
# second thread could get them still null. Under load wwhd-play died at start, a call of udev_new through null in
# SDL_hid_device_change_count (B97, about 1 start in 20).
set(ENABLE_WXWIDGETS OFF CACHE BOOL "" FORCE)
set(ENABLE_HIDAPI OFF CACHE BOOL "" FORCE)
set(ENABLE_BLUEZ OFF CACHE BOOL "" FORCE)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
add_subdirectory("${CEMU_SRC}" cemu EXCLUDE_FROM_ALL)

add_subdirectory(src)
