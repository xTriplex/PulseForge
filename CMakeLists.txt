cmake_minimum_required(VERSION 3.24)

# Add custom multi-configurations before CMake enables languages and initializes
# per-configuration compiler and linker flags. Keep any toolchain-provided hooks.
list(APPEND CMAKE_PROJECT_TOP_LEVEL_INCLUDES
	"${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeConfigurations.cmake")

project(PulseForge LANGUAGES C CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeSourceLayout.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeOptions.cmake")
if(PULSEFORGE_PLATFORM_MACOS AND PULSEFORGE_BUILD_EDITOR)
	# NativeFileDialog's Cocoa source needs Objective-C's compiler rules during
	# generation. Keep this language initialization in the top-level directory.
	enable_language(OBJC)
endif()
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
