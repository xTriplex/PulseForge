cmake_minimum_required(VERSION 3.24)

project(PulseForge LANGUAGES C CXX)

include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeSourceLayout.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeOptions.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeDependencies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEngineTargets.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGameTarget.cmake")
include(CTest)
if(BUILD_TESTING)
	add_test(NAME PulseForge.SourceLayout
		COMMAND "${CMAKE_COMMAND}"
			"-DPULSEFORGE_SOURCE_LAYOUT_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeSourceLayout.cmake")
endif()
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEditorCoreTarget.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeAssetDeployment.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEditorTargets.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeShaderBuild.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGpuValidationAnalysis.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeQualityValidation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGpuValidation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTargetPolicies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeQualityGraph.cmake")
