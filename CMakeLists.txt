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
	add_test(NAME PulseForge.CMake.Presets
		COMMAND "${CMAKE_COMMAND}"
			"-DPULSEFORGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
			"-DPULSEFORGE_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
			"-DPULSEFORGE_CMAKE_COMMAND=${CMAKE_COMMAND}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestPresetValidationFixtures.cmake")
	add_test(NAME PulseForge.CMake.RuntimeTargetClosure
		COMMAND "${CMAKE_COMMAND}"
			"-DPULSEFORGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
			"-DPULSEFORGE_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
			"-DPULSEFORGE_CMAKE_COMMAND=${CMAKE_COMMAND}"
			"-DPULSEFORGE_GENERATOR=${CMAKE_GENERATOR}"
			"-DPULSEFORGE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
			"-DPULSEFORGE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
			-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestRuntimeTargetClosure.cmake")
	if(WIN32)
		add_test(NAME PulseForge.CMake.LegacySettingsCompatibility
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
				"-DPULSEFORGE_BINARY_ROOT=${CMAKE_CURRENT_BINARY_DIR}"
				"-DPULSEFORGE_CMAKE_COMMAND=${CMAKE_COMMAND}"
				"-DPULSEFORGE_LEGACY_SETTINGS_FILE=${CMAKE_CURRENT_SOURCE_DIR}/CMakeSettings.json"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestPresetValidationFixtures.cmake")
	endif()
	if(CMAKE_GENERATOR MATCHES "^Visual Studio" AND NOT PULSEFORGE_BUILD_CORE_ONLY)
		add_test(NAME PulseForge.CMake.VisualStudioLaunchRegistration
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
				"-DPULSEFORGE_BINARY_DIR=${CMAKE_BINARY_DIR}"
				"-DPULSEFORGE_CMAKE_COMMAND=${CMAKE_COMMAND}"
				"-DPULSEFORGE_MSBUILD_EXECUTABLE=${CMAKE_MAKE_PROGRAM}"
				"-DPULSEFORGE_GENERATOR=${CMAKE_GENERATOR}"
				"-DPULSEFORGE_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
				"-DPULSEFORGE_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestVisualStudioLaunchRegistration.cmake")
	endif()
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
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeVisualStudioLaunch.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeEditorTargets.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeShaderBuild.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGpuValidationAnalysis.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeQualityValidation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeCoreTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeGpuValidation.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeTargetPolicies.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeQualityGraph.cmake")

pulseforge_register_visual_studio_launch_paths_test("${CMAKE_CURRENT_SOURCE_DIR}")
