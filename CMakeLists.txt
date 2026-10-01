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
set(VCPKG_MANIFEST_DIR "${CEMU_SRC}")
if(UNIX AND NOT APPLE)
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

# Cemu as its own build configures it, without its wxWidgets GUI: our frontend implements its
# WindowSystem interface. Only what wwhd-null links is built.
# Its find_package() targets are global, so src/ can link what Cemu's targets link.
set(ENABLE_WXWIDGETS OFF CACHE BOOL "" FORCE)
set(CMAKE_FIND_PACKAGE_TARGETS_GLOBAL ON)
add_subdirectory("${CEMU_SRC}" cemu EXCLUDE_FROM_ALL)

add_subdirectory(src)
