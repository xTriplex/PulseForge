cmake_minimum_required(VERSION 3.24)

foreach(RequiredVariable IN ITEMS
		PULSEFORGE_CONFIGURATION_PROBE_SOURCE
		PULSEFORGE_CONFIGURATION_INITIALIZER
		PULSEFORGE_CONFIGURATION_PROBE_BINARY
		PULSEFORGE_TEST_GENERATOR)
	if(NOT DEFINED ${RequiredVariable} OR "${${RequiredVariable}}" STREQUAL "")
		message(FATAL_ERROR "${RequiredVariable} must be provided")
	endif()
endforeach()

cmake_path(IS_PREFIX PULSEFORGE_BUILD_TREE "${PULSEFORGE_CONFIGURATION_PROBE_BINARY}" NORMALIZE IsProbeInsideBuildTree)
if(NOT IsProbeInsideBuildTree)
	message(FATAL_ERROR "The configuration probe must remain under the generated build tree")
endif()

file(REMOVE_RECURSE "${PULSEFORGE_CONFIGURATION_PROBE_BINARY}")

function(ConfigureProbe Name)
	set(BinaryDirectory "${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/${Name}")
	set(ConfigureCommand
		"${CMAKE_COMMAND}"
		-S "${PULSEFORGE_CONFIGURATION_PROBE_SOURCE}"
		-B "${BinaryDirectory}"
		-G "${PULSEFORGE_TEST_GENERATOR}"
		"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${PULSEFORGE_CONFIGURATION_INITIALIZER}")
	if(DEFINED PULSEFORGE_TEST_GENERATOR_PLATFORM AND NOT PULSEFORGE_TEST_GENERATOR_PLATFORM STREQUAL "")
		list(APPEND ConfigureCommand -A "${PULSEFORGE_TEST_GENERATOR_PLATFORM}")
	endif()
	if(DEFINED PULSEFORGE_TEST_GENERATOR_TOOLSET AND NOT PULSEFORGE_TEST_GENERATOR_TOOLSET STREQUAL "")
		list(APPEND ConfigureCommand -T "${PULSEFORGE_TEST_GENERATOR_TOOLSET}")
	endif()
	if(Name STREQUAL "Overrides")
		list(APPEND ConfigureCommand
			-DPULSEFORGE_TEST_CONFIGURATIONS=Debug,Release,Custom
			-DPULSEFORGE_TEST_OVERRIDE_FLAGS=ON)
	endif()

	execute_process(
		COMMAND ${ConfigureCommand}
		RESULT_VARIABLE ConfigureResult
		OUTPUT_VARIABLE ConfigureOutput
		ERROR_VARIABLE ConfigureError)
	if(NOT ConfigureResult EQUAL 0)
		message(FATAL_ERROR "${Name} custom-configuration probe failed:\n${ConfigureOutput}\n${ConfigureError}")
	endif()
endfunction()

ConfigureProbe(Default)
ConfigureProbe(Overrides)
message(STATUS "Fresh multi-config probes initialized Dist and preserved explicit configuration/flag overrides")
