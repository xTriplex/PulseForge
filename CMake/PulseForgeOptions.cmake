set(PULSEFORGE_SAMPLE_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeGame (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_SAMPLE_RENDERER PROPERTY STRINGS Vulkan OpenGL)
option(PULSEFORGE_BUILD_CORE_ONLY "Build only portable Core and CPU consumer tests" OFF)
option(PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF
	"Enable the explicit macOS Vulkan renderer proof configuration" OFF)
if(PULSEFORGE_BUILD_CORE_ONLY OR PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF)
	set(PULSEFORGE_EDITOR_DEFAULT OFF)
else()
	set(PULSEFORGE_EDITOR_DEFAULT ON)
endif()
option(PULSEFORGE_BUILD_EDITOR "Build the experimental PulseForge editor shell" ${PULSEFORGE_EDITOR_DEFAULT})
option(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS "Build opt-in Vulkan GPU validation smoke tests" OFF)
option(PULSEFORGE_ENABLE_QUALITY_CHECKS "Enable explicitly requested source, architecture, formatting, and analysis checks" OFF)
option(PULSEFORGE_ENABLE_MSVC_ANALYSIS "Enable selected MSVC static analysis checks" OFF)
set(PULSEFORGE_EDITOR_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeEditor (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_EDITOR_RENDERER PROPERTY STRINGS Vulkan OpenGL)

if(PULSEFORGE_BUILD_EDITOR AND NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan" AND
	NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
	message(FATAL_ERROR "PULSEFORGE_EDITOR_RENDERER must be Vulkan or OpenGL")
endif()

if(WIN32)
	if(NOT CMAKE_C_COMPILER_ID STREQUAL "MSVC" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
		message(FATAL_ERROR
			"Windows builds currently require the MSVC C and C++ toolchains. Selected C=${CMAKE_C_COMPILER_ID}, "
			"CXX=${CMAKE_CXX_COMPILER_ID}.")
	endif()
	set(PULSEFORGE_PLATFORM_WINDOWS TRUE)
	set(PULSEFORGE_PLATFORM_LINUX FALSE)
	set(PULSEFORGE_PLATFORM_MACOS FALSE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	if(NOT CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang)$" OR
		NOT CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
		message(FATAL_ERROR
			"Linux Core-only builds require GCC or Clang C and C++ toolchains. Selected C=${CMAKE_C_COMPILER_ID}, "
			"CXX=${CMAKE_CXX_COMPILER_ID}.")
	endif()
	set(PULSEFORGE_PLATFORM_WINDOWS FALSE)
	set(PULSEFORGE_PLATFORM_LINUX TRUE)
	set(PULSEFORGE_PLATFORM_MACOS FALSE)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	if(NOT PULSEFORGE_BUILD_CORE_ONLY AND NOT PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF)
		message(FATAL_ERROR
			"macOS defaults to the Core portability probe. The renderer proof is opt-in with "
			"PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF=ON.")
	endif()
	if(PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF)
		if(PULSEFORGE_BUILD_CORE_ONLY)
			message(FATAL_ERROR "PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF cannot be combined with PULSEFORGE_BUILD_CORE_ONLY.")
		endif()
		if(PULSEFORGE_BUILD_EDITOR)
			message(FATAL_ERROR "The macOS Vulkan proof does not include the editor. Set PULSEFORGE_BUILD_EDITOR=OFF.")
		endif()
		if(NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
			message(FATAL_ERROR "The macOS Vulkan proof requires PULSEFORGE_SAMPLE_RENDERER=Vulkan.")
		endif()
		if(NOT PULSEFORGE_BUILD_GPU_VALIDATION_TESTS)
			message(FATAL_ERROR "The macOS Vulkan proof requires PULSEFORGE_BUILD_GPU_VALIDATION_TESTS=ON.")
		endif()
	endif()
	if(NOT CMAKE_C_COMPILER_ID MATCHES "^(AppleClang|Clang)$" OR
		NOT CMAKE_CXX_COMPILER_ID MATCHES "^(AppleClang|Clang)$")
		message(FATAL_ERROR
			"macOS builds require Apple Clang or Clang C and C++ toolchains. Selected C=${CMAKE_C_COMPILER_ID}, "
			"CXX=${CMAKE_CXX_COMPILER_ID}.")
	endif()
	set(PULSEFORGE_PLATFORM_WINDOWS FALSE)
	set(PULSEFORGE_PLATFORM_LINUX FALSE)
	set(PULSEFORGE_PLATFORM_MACOS TRUE)
else()
	message(FATAL_ERROR
		"Unsupported PulseForge operating system '${CMAKE_SYSTEM_NAME}'. This revision supports Windows desktop "
		"builds, Linux desktop/runtime/Core builds, and macOS Core portability or opt-in Vulkan proof builds.")
endif()

if(PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF AND NOT CMAKE_SYSTEM_NAME STREQUAL "Darwin")
	message(FATAL_ERROR "PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF is only valid when configuring for macOS.")
endif()

if(PULSEFORGE_PLATFORM_WINDOWS)
	set(PULSEFORGE_PLATFORM_COMPILE_DEFINITION PF_PLATFORM_WINDOWS)
elseif(PULSEFORGE_PLATFORM_MACOS)
	set(PULSEFORGE_PLATFORM_COMPILE_DEFINITION PF_PLATFORM_MACOS)
else()
	set(PULSEFORGE_PLATFORM_COMPILE_DEFINITION PF_PLATFORM_LINUX)
endif()

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
	message(FATAL_ERROR "PulseForge currently supports 64-bit Windows, Linux, and macOS Core targets only.")
endif()

if(PULSEFORGE_BUILD_CORE_ONLY AND PULSEFORGE_BUILD_EDITOR)
	message(FATAL_ERROR "PULSEFORGE_BUILD_CORE_ONLY cannot be combined with PULSEFORGE_BUILD_EDITOR=ON.")
endif()

if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS AND
	NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" AND
	(NOT PULSEFORGE_BUILD_EDITOR OR NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
	message(FATAL_ERROR "PULSEFORGE_BUILD_GPU_VALIDATION_TESTS requires the Vulkan renderer and its pinned DXC shader path")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC)
	set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

include(CheckCXXSourceCompiles)
check_cxx_source_compiles("#include <expected>
int main() { std::expected<int, int> value = 1; return *value - 1; }
" PULSEFORGE_HAS_STD_EXPECTED)
if(NOT PULSEFORGE_HAS_STD_EXPECTED)
	message(FATAL_ERROR
		"The selected C++ compiler/standard library cannot compile std::expected in C++23 mode. "
		"Use a toolchain with C++23 expected support (for example, a recent MSVC, GCC/libstdc++, or Clang/libc++ release).")
endif()
check_cxx_source_compiles("#include <format>
#include <string>
int main() { return std::format(\"{}\", 23) == \"23\" ? 0 : 1; }
" PULSEFORGE_HAS_STD_FORMAT)
if(NOT PULSEFORGE_HAS_STD_FORMAT)
	message(FATAL_ERROR
		"The selected C++ compiler/standard library cannot compile std::format in C++23 mode. "
		"PulseForge requires a toolchain with C++23 formatting support; this is a standard-library capability, not just a compiler flag.")
endif()

if(CMAKE_GENERATOR STREQUAL "Visual Studio 17 2022" AND
	CMAKE_CXX_COMPILER_ID STREQUAL "MSVC" AND
	CMAKE_CXX_COMPILER_VERSION VERSION_GREATER_EQUAL "19.44" AND
	CMAKE_CXX_COMPILER_VERSION VERSION_LESS "19.45")
	set(PulseForgeBuildInParallelConfigured FALSE)
	foreach(PulseForgeVSGlobal IN LISTS CMAKE_VS_GLOBALS)
		if(PulseForgeVSGlobal MATCHES "^BuildInParallel=")
			set(PulseForgeBuildInParallelConfigured TRUE)
			break()
		endif()
	endforeach()
	if(NOT PulseForgeBuildInParallelConfigured)
		list(APPEND CMAKE_VS_GLOBALS "BuildInParallel=false")
	endif()
	unset(PulseForgeBuildInParallelConfigured)
	unset(PulseForgeVSGlobal)
endif()

set(PULSEFORGE_ENGINE_PCH "${CMAKE_CURRENT_SOURCE_DIR}/PulseForge/Source/Public/Core/PulseForgePCH.h")
