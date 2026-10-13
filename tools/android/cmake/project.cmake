# tools/android/cmake/project.cmake (CMAKE_PROJECT_INCLUDE, tools/android/build.sh): after each project(), ours and
# Cemu's, the headers Cemu's Linux code expects of the system that the NDK hasn't: Xlib's and xcb's (platform.h and
# vulkan.h's platform defines include them on every Linux, Android too; nothing links them) and log/log.h
# (tools/android/include).
set(_x11 "$ENV{ANDROID_X11_HEADERS}")
if(NOT _x11)
	set(_x11 /wwhd/opt/android/x11/include)
endif()
include_directories(SYSTEM "${_x11}" "${CMAKE_CURRENT_LIST_DIR}/../include")
