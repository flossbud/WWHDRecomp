# tools/arm64/cmake/FindGTK3.cmake: a stand-in for Cemu's find_package(GTK3 REQUIRED) on Linux. Only Cemu's wxWidgets
# GUI links GTK3, and wwhd-null builds without it; an arm64 GTK3 would be hundreds of packages in the sysroot.
set(GTK3_FOUND TRUE)
if(NOT TARGET GTK3::gtk)
	add_library(GTK3::gtk INTERFACE IMPORTED)
endif()
