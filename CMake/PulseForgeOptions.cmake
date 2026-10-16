set(PULSEFORGE_SAMPLE_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeGame (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_SAMPLE_RENDERER PROPERTY STRINGS Vulkan OpenGL)
option(PULSEFORGE_BUILD_EDITOR "Build the experimental PulseForge editor shell" ON)
option(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS "Build opt-in Vulkan GPU validation smoke tests" OFF)
option(PULSEFORGE_ENABLE_QUALITY_CHECKS "Enable explicitly requested source, architecture, formatting, and analysis checks" OFF)
option(PULSEFORGE_ENABLE_MSVC_ANALYSIS "Enable selected MSVC static analysis checks" OFF)
set(PULSEFORGE_EDITOR_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeEditor (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_EDITOR_RENDERER PROPERTY STRINGS Vulkan OpenGL)

if(PULSEFORGE_BUILD_EDITOR AND NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan" AND
	NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
	message(FATAL_ERROR "PULSEFORGE_EDITOR_RENDERER must be Vulkan or OpenGL")
endif()

if(NOT WIN32)
	message(FATAL_ERROR "PulseForge currently supports Windows only; desktop platform support is being implemented separately.")
endif()

if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS AND
	NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" AND
	(NOT PULSEFORGE_BUILD_EDITOR OR NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
	message(FATAL_ERROR "PULSEFORGE_BUILD_GPU_VALIDATION_TESTS requires the Vulkan renderer and its pinned DXC shader path")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

set(PULSEFORGE_ENGINE_PCH "${CMAKE_CURRENT_SOURCE_DIR}/PulseForge/Source/Private/Core/PulseForgePCH.h")

if(CMAKE_CONFIGURATION_TYPES)
	list(APPEND CMAKE_CONFIGURATION_TYPES Dist)
	list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
endif()
