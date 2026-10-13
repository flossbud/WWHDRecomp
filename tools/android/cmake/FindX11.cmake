# tools/android/cmake/FindX11.cmake: a stand-in for Cemu's find_package(X11 REQUIRED), which it makes on every Linux,
# Android included. Cemu's platform.h includes Xlib's headers there; nothing the app links calls Xlib. The headers
# only (tools/android/setup.sh copies them from Debian's), as interface targets; every target has them on its include path
# (tools/android/cmake/project.cmake).
set(X11_FOUND TRUE)
set(X11_INCLUDE_DIR "$ENV{ANDROID_X11_HEADERS}")
if(NOT X11_INCLUDE_DIR)
	set(X11_INCLUDE_DIR /wwhd/opt/android/x11/include)
endif()
foreach(t X11 Xrender Xutil)
	if(NOT TARGET X11::${t})
		add_library(X11::${t} INTERFACE IMPORTED)
		set_target_properties(X11::${t} PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${X11_INCLUDE_DIR}")
	endif()
endforeach()
