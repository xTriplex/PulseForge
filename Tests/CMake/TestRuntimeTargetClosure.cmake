if(NOT DEFINED PULSEFORGE_SOURCE_ROOT OR NOT DEFINED PULSEFORGE_BINARY_ROOT OR NOT DEFINED PULSEFORGE_CMAKE_COMMAND)
	message(FATAL_ERROR "Runtime target-closure fixture requires source, binary, and CMake paths")
endif()

set(FixtureSource "${PULSEFORGE_SOURCE_ROOT}/Tests/CMake/RuntimeTargetClosureFixture")
set(FixtureRoot "${PULSEFORGE_BINARY_ROOT}/RuntimeTargetClosureFixture")
set(PositiveBuild "${FixtureRoot}/Positive")
set(NegativeBuild "${FixtureRoot}/Negative")
set(NoConfigBuild "${FixtureRoot}/NoConfig")
file(REMOVE_RECURSE "${FixtureRoot}")

set(GeneratorArguments)
if(DEFINED PULSEFORGE_GENERATOR AND NOT PULSEFORGE_GENERATOR STREQUAL "")
	list(APPEND GeneratorArguments -G "${PULSEFORGE_GENERATOR}")
endif()
if(DEFINED PULSEFORGE_GENERATOR_PLATFORM AND NOT PULSEFORGE_GENERATOR_PLATFORM STREQUAL "")
	list(APPEND GeneratorArguments -A "${PULSEFORGE_GENERATOR_PLATFORM}")
endif()
if(DEFINED PULSEFORGE_GENERATOR_TOOLSET AND NOT PULSEFORGE_GENERATOR_TOOLSET STREQUAL "")
	list(APPEND GeneratorArguments -T "${PULSEFORGE_GENERATOR_TOOLSET}")
endif()

execute_process(
	COMMAND "${PULSEFORGE_CMAKE_COMMAND}" -S "${FixtureSource}" -B "${PositiveBuild}"
		${GeneratorArguments} "-DPULSEFORGE_SOURCE_ROOT=${PULSEFORGE_SOURCE_ROOT}"
		-DPULSEFORGE_EXPECT_FORBIDDEN=OFF
	RESULT_VARIABLE PositiveResult
	OUTPUT_VARIABLE PositiveOutput
	ERROR_VARIABLE PositiveError)
if(NOT PositiveResult EQUAL 0)
	message(FATAL_ERROR "Legitimate runtime/system dependencies were rejected: ${PositiveOutput}${PositiveError}")
endif()

execute_process(
	COMMAND "${PULSEFORGE_CMAKE_COMMAND}" -S "${FixtureSource}" -B "${NegativeBuild}"
		${GeneratorArguments} "-DPULSEFORGE_SOURCE_ROOT=${PULSEFORGE_SOURCE_ROOT}"
		-DPULSEFORGE_EXPECT_FORBIDDEN=ON
	RESULT_VARIABLE NegativeResult
	OUTPUT_VARIABLE NegativeOutput
	ERROR_VARIABLE NegativeError)
if(NegativeResult EQUAL 0)
	message(FATAL_ERROR "Indirect forbidden authoring dependency was not rejected")
endif()
string(FIND "${NegativeOutput}${NegativeError}" "must not link" RejectionIndex)
if(RejectionIndex EQUAL -1)
	message(FATAL_ERROR "Forbidden dependency failed for an unexpected reason: ${NegativeOutput}${NegativeError}")
endif()

execute_process(
	COMMAND "${PULSEFORGE_CMAKE_COMMAND}" -S "${FixtureSource}" -B "${NoConfigBuild}"
		${GeneratorArguments} "-DPULSEFORGE_SOURCE_ROOT=${PULSEFORGE_SOURCE_ROOT}"
		-DPULSEFORGE_EXPECT_NOCONFIG_FORBIDDEN=ON
	RESULT_VARIABLE NoConfigResult
	OUTPUT_VARIABLE NoConfigOutput
	ERROR_VARIABLE NoConfigError)
if(NoConfigResult EQUAL 0)
	message(FATAL_ERROR "Imported NOCONFIG authoring dependency was not rejected")
endif()
string(FIND "${NoConfigOutput}${NoConfigError}" "must not link" NoConfigRejectionIndex)
if(NoConfigRejectionIndex EQUAL -1)
	message(FATAL_ERROR "Imported NOCONFIG dependency failed for an unexpected reason: ${NoConfigOutput}${NoConfigError}")
endif()
