set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(PulseForge/Vendor/Glfw)
find_package(OpenGL REQUIRED)

# Build only Jolt's CPU physics library. PulseForge owns the engine-facing
# integration; Jolt remains a private implementation dependency.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_DXC OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_AVX2 OFF CACHE BOOL "" FORCE)
set(USE_AVX OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT OFF CACHE BOOL "" FORCE)
set(USE_TZCNT OFF CACHE BOOL "" FORCE)
set(USE_F16C OFF CACHE BOOL "" FORCE)
set(USE_FMADD OFF CACHE BOOL "" FORCE)
add_subdirectory(PulseForge/Vendor/JoltPhysics/Build "${CMAKE_BINARY_DIR}/_deps/JoltPhysics-build" EXCLUDE_FROM_ALL)

# NVRHI is an implementation dependency of the Vulkan renderer backend. Keep
# its upstream source revision pinned and build only the backend PulseForge
# currently uses.
include(FetchContent)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS ON CACHE BOOL "" FORCE)
set(NVRHI_VULKAN_HEADERS_GIT_TAG v1.4.352 CACHE STRING "" FORCE)
FetchContent_Declare(PulseForgeNVRHI
	GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
	GIT_TAG 6b96fb03e07539f08327aea76c56d55f1de9d906
)
FetchContent_MakeAvailable(PulseForgeNVRHI)

# Lua is embedded through PulseForge's narrow C API bindings. Pin the official
# release archive and verify it before compiling the VM as a private dependency.
FetchContent_Declare(PulseForgeLua
	URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
	URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(PulseForgeLua)
set(PULSEFORGE_LUA_SOURCE_DIR "${pulseforgelua_SOURCE_DIR}/src")
add_library(PulseForgeLua STATIC
	${PULSEFORGE_LUA_SOURCE_DIR}/lapi.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lcode.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lctype.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ldebug.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ldo.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ldump.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lfunc.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lgc.c
	${PULSEFORGE_LUA_SOURCE_DIR}/llex.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lmem.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lobject.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lopcodes.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lparser.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lstate.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lstring.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ltable.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ltm.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lundump.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lvm.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lzio.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lauxlib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lbaselib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lmathlib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lstrlib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ltablib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lutf8lib.c
)
target_include_directories(PulseForgeLua PUBLIC "${PULSEFORGE_LUA_SOURCE_DIR}")
set_target_properties(PulseForgeLua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

add_library(PulseForgeGlad STATIC PulseForge/Vendor/Glad/src/glad.c)
target_include_directories(PulseForgeGlad PUBLIC PulseForge/Vendor/Glad/include)
target_compile_options(PulseForgeGlad PRIVATE
	$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
)

if(PULSEFORGE_BUILD_EDITOR)
	if(PULSEFORGE_PLATFORM_MACOS)
		# NativeFileDialog calls project() in its own directory. Enable Objective-C
		# after that call so CMake retains the language rules alongside its .m target.
		set(CMAKE_PROJECT_nativefiledialog-extended_INCLUDE
			"${CMAKE_CURRENT_LIST_DIR}/PulseForgeEnableNFDObjectiveC.cmake")
	endif()
	set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
	set(NFD_BUILD_GLFW3_TESTS OFF CACHE BOOL "" FORCE)
	set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
	add_subdirectory(PulseForge/Vendor/nativefiledialog)

	# The current Font Awesome Free desktop font uses CFF outlines in an OTF.
	# Reuse the repository's pinned FreeType copy for the editor's font atlas;
	# compressed-font features are disabled because bundled assets are plain local TTF/OTF files.
	set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
	add_subdirectory(PulseForge/Vendor/freetype "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)
endif()

if(PULSEFORGE_BUILD_EDITOR OR PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF)
	set(PULSEFORGE_IMGUI_SOURCES
		PulseForge/Vendor/imgui/imgui.cpp
		PulseForge/Vendor/imgui/imgui_draw.cpp
		PulseForge/Vendor/imgui/imgui_tables.cpp
		PulseForge/Vendor/imgui/imgui_widgets.cpp
		PulseForge/Vendor/imgui/imgui_demo.cpp
		PulseForge/Vendor/imgui/misc/cpp/imgui_stdlib.cpp
		PulseForge/Vendor/imgui/backends/imgui_impl_glfw.cpp
	)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		list(APPEND PULSEFORGE_IMGUI_SOURCES PulseForge/Vendor/imgui/backends/imgui_impl_opengl3.cpp)
	endif()
	add_library(PulseForgeImGui STATIC ${PULSEFORGE_IMGUI_SOURCES})
	target_include_directories(PulseForgeImGui PUBLIC
		PulseForge/Vendor/imgui
		PulseForge/Vendor/imgui/backends
	)
	target_link_libraries(PulseForgeImGui PUBLIC glfw)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_link_libraries(PulseForgeImGui PUBLIC OpenGL::GL)
	endif()
	target_compile_options(PulseForgeImGui PRIVATE
		$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
		$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
	)
endif()
