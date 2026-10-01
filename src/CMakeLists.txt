# wwhd-null: Cemu's OS libraries without Latte, with our forks of some of their sources; the null GPU
# and our Vulkan renderer; our OS layer and frontend; the runtime and, when it has been generated, the
# recompiled program with its overrides (D9). Headless, or in a window with SDL3. src/README.md.

option(WWHD_FORKS "Our forks of Cemu's sources (forks.txt) replace Cemu's; OFF keeps Cemu's, to record baselines" ON)
set(WWHD_RECOMP_DIR "${PROJECT_SOURCE_DIR}/build/recomp" CACHE PATH
	"The generated program (tools/recomp/generate.py writes it there); without it the runtime interprets")

# ---- Cemu's libraries, as wwhd-null links them -------------------------------------------------
# Latte goes, except what our side still uses: the address library (gx2 computes surface layouts
# with it) and, for the renderer, the shader decompiler, the fetch- and GS-copy-shader parsers and
# the texture loader (gpu/vk/latte_glue.cpp stands in for the rest of Latte). Each fork in forks.txt
# takes its original's place in the Cemu target that had it: it compiles as Cemu compiles its own
# files (ThinLTO bitcode included), with the original's directory on the include path for its
# relative includes.
set(latte_kept "/LatteAddrLib/" "/LegacyShaderDecompiler/" "/Core/FetchShader.cpp"
	"/Core/LatteGSCopyShaderParser.cpp" "/Core/LatteTextureLoader.cpp")
# With WWHD_FORKS=OFF only the forks that lay out guest memory stay (they change where Cemu's
# objects are, to the reference's addresses, not what the OS does).
set(layout_forks os/common/SysAllocator.cpp)
set(fork_theirs)
set(fork_ours)
file(STRINGS forks.txt lines)
foreach(line IN LISTS lines)
	if(line MATCHES "^([^# \t][^ \t]*)[ \t]+([^ \t]+)")
		if(WWHD_FORKS OR CMAKE_MATCH_1 IN_LIST layout_forks)
			list(APPEND fork_theirs "${CEMU_SRC}/src/${CMAKE_MATCH_2}")
			list(APPEND fork_ours "${CMAKE_CURRENT_SOURCE_DIR}/${CMAKE_MATCH_1}")
		endif()
	endif()
endforeach()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS forks.txt)
set(forked 0)
foreach(target CemuCafe CemuCommon CemuUtil CemuConfig CemuInput CemuAudio CemuComponents)
	get_target_property(sources ${target} SOURCES)
	get_target_property(dir ${target} SOURCE_DIR)
	set(kept)
	foreach(s IN LISTS sources)
		if(s MATCHES "^\\$<")
			list(APPEND kept "${s}")
			continue()
		endif()
		cmake_path(ABSOLUTE_PATH s BASE_DIRECTORY "${dir}" NORMALIZE OUTPUT_VARIABLE path)
		if(path MATCHES "/src/Cafe/HW/Latte/")
			set(keep OFF)
			foreach(k IN LISTS latte_kept)
				string(FIND "${path}" "${k}" at)
				if(at GREATER -1)
					set(keep ON)
				endif()
			endforeach()
			if(NOT keep)
				continue()
			endif()
		endif()
		list(FIND fork_theirs "${path}" i)
		if(i GREATER -1)
			list(GET fork_ours ${i} path)
			cmake_path(GET s PARENT_PATH theirs)
			cmake_path(ABSOLUTE_PATH theirs BASE_DIRECTORY "${dir}" NORMALIZE)
			# -Wno-constant-conversion: Cemu's IMLInstruction.h (not ours) warns in every file that includes it
			set_source_files_properties("${path}" TARGET_DIRECTORY ${target} PROPERTIES
				INCLUDE_DIRECTORIES "${theirs}" COMPILE_OPTIONS "-Wno-constant-conversion")
			math(EXPR forked "${forked} + 1")
		endif()
		list(APPEND kept "${path}")
	endforeach()
	set_property(TARGET ${target} PROPERTY SOURCES "${kept}")
endforeach()
list(LENGTH fork_theirs n)
if(NOT forked EQUAL n)
	message(FATAL_ERROR "forks.txt names ${n} of Cemu's sources, but only ${forked} are in Cemu's targets")
endif()

# ---- our sources -------------------------------------------------------------------------------
# They compile as CemuCafe's own files do: its include directories, definitions and options, with
# those of the directories it is in, and the usage requirements of everything it links; Cemu's
# precompiled header; at -O2 and without LTO.
get_target_property(cafe_dir CemuCafe SOURCE_DIR)
get_target_property(cafe_bin CemuCafe BINARY_DIR)
get_target_property(libs CemuCafe LINK_LIBRARIES)
set(cafe_libs)           # for their headers: what can be seen from here (libusb's isn't, nor needed)
foreach(l IN LISTS libs)
	if(TARGET ${l} OR NOT l MATCHES "::")
		list(APPEND cafe_libs ${l})
	endif()
endforeach()
get_directory_property(cafe_definitions DIRECTORY "${cafe_dir}" COMPILE_DEFINITIONS)
function(wwhd_like_cemu target)
	foreach(p INCLUDE_DIRECTORIES COMPILE_OPTIONS COMPILE_DEFINITIONS)
		get_target_property(v CemuCafe ${p})
		if(v)
			set_property(TARGET ${target} APPEND PROPERTY ${p} "${v}")
		endif()
	endforeach()
	set_property(TARGET ${target} APPEND PROPERTY COMPILE_DEFINITIONS "${cafe_definitions}")
	# (src/gui: Cemu's WindowSystem interface, which the frontend implements)
	target_include_directories(${target} PRIVATE "${cafe_dir}" "${cafe_bin}" "${CEMU_SRC}/src/gui"
		"${CMAKE_CURRENT_SOURCE_DIR}/frontend" "${PROJECT_SOURCE_DIR}/tools/recomp/runtime")
	target_link_libraries(${target} PRIVATE ${cafe_libs})
	target_precompile_headers(${target} PRIVATE "${CEMU_SRC}/src/Common/precompiled.h")
	target_compile_options(${target} PRIVATE -O2)
endfunction()

# Cemu's main() (it calls WindowSystem::Create(), which our frontend implements), compiled as Cemu
# compiles it
add_library(wwhd_main OBJECT "${CEMU_SRC}/src/main.cpp" "${CEMU_SRC}/src/mainLLE.cpp")
wwhd_like_cemu(wwhd_main)
get_target_property(bin_libs CemuBin LINK_LIBRARIES)     # SDL3 and Cemu's libraries, in Cemu's order
target_link_libraries(wwhd_main PRIVATE ${bin_libs})
target_include_directories(wwhd_main PRIVATE "${CEMU_SRC}/src")
set_target_properties(wwhd_main PROPERTIES INTERPROCEDURAL_OPTIMIZATION ON)
get_target_property(options wwhd_main COMPILE_OPTIONS)
list(REMOVE_ITEM options -O2)                              # Cemu's -O3
set_property(TARGET wwhd_main PROPERTY COMPILE_OPTIONS "${options}")

add_library(wwhd_frontend OBJECT frontend/cemu_boot.cpp frontend/window_system.cpp frontend/audio_sdl.cpp frontend/overlay.cpp)
wwhd_like_cemu(wwhd_frontend)
target_link_libraries(wwhd_frontend PRIVATE SDL3::SDL3 CemuAudio)   # SDL3 for the window, sound and gamepad; CemuAudio: cubeb, to close it
set_source_files_properties(frontend/window_system.cpp PROPERTIES COMPILE_DEFINITIONS WWHD_NULL_GPU)

add_library(wwhd_gpu OBJECT gpu/null_gpu.cpp gpu/vk/vk.cpp gpu/vk/renderer.cpp gpu/vk/present.cpp
	gpu/vk/shader_cache.cpp gpu/vk/shader_list.cpp gpu/vk/draw.cpp gpu/vk/texture.cpp gpu/vk/latte_glue.cpp)
wwhd_like_cemu(wwhd_gpu)
target_include_directories(wwhd_gpu PRIVATE gpu)

add_library(wwhd_runtime OBJECT runtime/dispatch.cpp runtime/imports.cpp runtime/diff.cpp runtime/profile.cpp
	runtime/without_forks.cpp)
wwhd_like_cemu(wwhd_runtime)

add_library(wwhd_os OBJECT os/coreinit_memory.cpp os/coreinit_queue_wait.cpp os/coreinit_thread_state.cpp
	os/coreinit_time.cpp os/erreula.cpp os/input.cpp os/install.cpp os/nn_offline.cpp os/swkbd.cpp
	os/system_flags.cpp
	os/gx2/blit.cpp os/gx2/context_state.cpp os/gx2/draw.cpp os/gx2/memory.cpp os/gx2/misc.cpp
	os/gx2/query.cpp os/gx2/render_target.cpp os/gx2/resource.cpp os/gx2/shader.cpp
	os/gx2/shader_legacy.cpp os/gx2/state.cpp os/gx2/streamout.cpp os/gx2/surface.cpp
	os/gx2/surface_copy.cpp os/gx2/texture.cpp os/gx2/tiling_aperture.cpp)
wwhd_like_cemu(wwhd_os)

set(objects $<TARGET_OBJECTS:wwhd_main> $<TARGET_OBJECTS:wwhd_frontend> $<TARGET_OBJECTS:wwhd_gpu>
	$<TARGET_OBJECTS:wwhd_runtime> $<TARGET_OBJECTS:wwhd_os>)

# the recompiled program (M3) and the overrides (D9): strict FP to match the interpreter, no strict
# aliasing (D11), the runtime header precompiled after Cemu's
if(WWHD_RECOMP_DIR AND EXISTS "${WWHD_RECOMP_DIR}/func_table.cpp")
	file(GLOB shards CONFIGURE_DEPENDS "${WWHD_RECOMP_DIR}/shard_*.cpp")
	list(LENGTH shards n)
	message(STATUS "wwhd-null: the recompiled program from ${WWHD_RECOMP_DIR} (${n} shards)")
	add_library(wwhd_recomp OBJECT ${shards} "${WWHD_RECOMP_DIR}/func_table.cpp" "${WWHD_RECOMP_DIR}/imports.cpp")
	wwhd_like_cemu(wwhd_recomp)
	target_include_directories(wwhd_recomp PRIVATE "${WWHD_RECOMP_DIR}")
	target_precompile_headers(wwhd_recomp PRIVATE "${PROJECT_SOURCE_DIR}/tools/recomp/runtime/ppc_ops.h")
	target_compile_options(wwhd_recomp PRIVATE -ffp-contract=off -fno-strict-aliasing -Wno-unused-label -Wno-unused-variable)
	add_library(wwhd_overrides OBJECT overrides/task_loop.cpp overrides/gamepad_view.cpp overrides/tick.cpp)
	wwhd_like_cemu(wwhd_overrides)
	target_include_directories(wwhd_overrides PRIVATE "${WWHD_RECOMP_DIR}")
	target_compile_options(wwhd_overrides PRIVATE -ffp-contract=off -fno-strict-aliasing)
	list(APPEND objects $<TARGET_OBJECTS:wwhd_recomp> $<TARGET_OBJECTS:wwhd_overrides>)
else()
	message(STATUS "wwhd-null: without the recompiled program (the runtime interprets)")
endif()

# ---- the executable ----------------------------------------------------------------------------
# Linked as Cemu links its own (its link options, SDL3 and its libraries in its order, ThinLTO),
# plus its resources (the system font, which coreinit hands the game; Cemu's GUI links them) and
# glslang's default limits for the renderer. Link order doesn't matter: Cemu's SysAllocators take
# the guest addresses the reference gives them (os/common/SysAllocator.cpp, sysalloc_layout.h), not
# their static constructors' order. Where the linker is GNU ld with LLVM's plugin or lld, a ThinLTO
# cache spares a relink most of its code generation.
get_target_property(bin_options CemuBin LINK_OPTIONS)
add_executable(wwhd-null ${objects})
set_target_properties(wwhd-null PROPERTIES LINKER_LANGUAGE CXX INTERPROCEDURAL_OPTIMIZATION ON
	RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
if(bin_options)
	set_property(TARGET wwhd-null PROPERTY LINK_OPTIONS "${bin_options}")
endif()
if(CMAKE_SYSTEM_NAME MATCHES "Linux|Android")
	target_link_options(wwhd-null PRIVATE "-Wl,--plugin-opt,cache-dir=${CMAKE_BINARY_DIR}/thinlto-cache")
endif()
target_link_libraries(wwhd-null PRIVATE ${bin_libs} CemuResource glslang::glslang-default-resource-limits)
