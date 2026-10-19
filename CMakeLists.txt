cmake_minimum_required(VERSION 3.24)

# Add custom multi-configurations before CMake enables languages and initializes
# per-configuration compiler and linker flags. Keep any toolchain-provided hooks.
list(APPEND CMAKE_PROJECT_TOP_LEVEL_INCLUDES
	"${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeConfigurations.cmake")

set(PULSEFORGE_PROJECT_LANGUAGES C CXX)
if((CMAKE_SYSTEM_NAME STREQUAL "Darwin" OR CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin") AND
	NOT PULSEFORGE_BUILD_CORE_ONLY AND NOT PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF AND
	(NOT DEFINED PULSEFORGE_BUILD_EDITOR OR PULSEFORGE_BUILD_EDITOR))
	# NativeFileDialog adds an Objective-C Cocoa source when the editor is enabled.
	# Enable Objective-C with the top-level project so its compiler rules remain
	# available across the dependency's nested project() call.
	list(APPEND PULSEFORGE_PROJECT_LANGUAGES OBJC)
endif()
project(PulseForge LANGUAGES ${PULSEFORGE_PROJECT_LANGUAGES})

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeSourceLayout.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeOptions.cmake")
include(CTest)
if(BUILD_TESTING)
	add_test(NAME PulseForge.SourceLayout
		COMMAND "${CMAKE_COMMAND}"
			"-DPULSEFORGE_SOURCE_LAYOUT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeSourceLayout.cmake")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeCoreDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeCoreTarget.cmake")

if(PULSEFORGE_BUILD_CORE_ONLY)
	include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeCoreTests.cmake")
	include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTargetPolicies.cmake")
	return()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEngineTargets.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGameTarget.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEditorCoreTarget.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeAssetDeployment.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEditorTargets.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeShaderBuild.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGpuValidationAnalysis.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeQualityValidation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeCoreTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGpuValidation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTargetPolicies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeQualityGraph.cmake")
