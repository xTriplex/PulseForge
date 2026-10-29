cmake_minimum_required(VERSION 3.24)

foreach(RequiredVariable IN ITEMS
		PULSEFORGE_CONFIGURATION_PROBE_SOURCE
		PULSEFORGE_CONFIGURATION_INITIALIZER
		PULSEFORGE_CONFIGURATION_POLICY
		PULSEFORGE_CONFIGURATION_PROBE_BINARY
		PULSEFORGE_TEST_GENERATOR
		PULSEFORGE_TEST_MULTI_CONFIG)
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
		"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${PULSEFORGE_CONFIGURATION_INITIALIZER}"
		"-DPULSEFORGE_CONFIGURATION_POLICY=${PULSEFORGE_CONFIGURATION_POLICY}")
	if(DEFINED PULSEFORGE_TEST_GENERATOR_PLATFORM AND NOT PULSEFORGE_TEST_GENERATOR_PLATFORM STREQUAL "")
		list(APPEND ConfigureCommand -A "${PULSEFORGE_TEST_GENERATOR_PLATFORM}")
	endif()
	if(DEFINED PULSEFORGE_TEST_GENERATOR_TOOLSET AND NOT PULSEFORGE_TEST_GENERATOR_TOOLSET STREQUAL "")
		list(APPEND ConfigureCommand -T "${PULSEFORGE_TEST_GENERATOR_TOOLSET}")
	endif()
	if(Name STREQUAL "Overrides")
		if(PULSEFORGE_TEST_MULTI_CONFIG)
			list(APPEND ConfigureCommand -DPULSEFORGE_TEST_CONFIGURATIONS=Debug,Release,Custom)
		else()
			list(APPEND ConfigureCommand -DCMAKE_BUILD_TYPE=Dist)
		endif()
		list(APPEND ConfigureCommand -DPULSEFORGE_TEST_OVERRIDE_FLAGS=ON)
	elseif(NOT PULSEFORGE_TEST_MULTI_CONFIG)
		list(APPEND ConfigureCommand -DCMAKE_BUILD_TYPE=Dist)
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

function(ReconfigureProbeWithChangedReleaseFlags Name)
	set(BinaryDirectory "${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/${Name}")
	set(ConfigureCommand
		"${CMAKE_COMMAND}"
		-S "${PULSEFORGE_CONFIGURATION_PROBE_SOURCE}"
		-B "${BinaryDirectory}"
		-G "${PULSEFORGE_TEST_GENERATOR}"
		"-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=${PULSEFORGE_CONFIGURATION_INITIALIZER}"
		"-DPULSEFORGE_CONFIGURATION_POLICY=${PULSEFORGE_CONFIGURATION_POLICY}")
	if(DEFINED PULSEFORGE_TEST_GENERATOR_PLATFORM AND NOT PULSEFORGE_TEST_GENERATOR_PLATFORM STREQUAL "")
		list(APPEND ConfigureCommand -A "${PULSEFORGE_TEST_GENERATOR_PLATFORM}")
	endif()
	if(DEFINED PULSEFORGE_TEST_GENERATOR_TOOLSET AND NOT PULSEFORGE_TEST_GENERATOR_TOOLSET STREQUAL "")
		list(APPEND ConfigureCommand -T "${PULSEFORGE_TEST_GENERATOR_TOOLSET}")
	endif()
	if(NOT PULSEFORGE_TEST_MULTI_CONFIG)
		list(APPEND ConfigureCommand -DCMAKE_BUILD_TYPE=Dist)
	endif()
	foreach(FlagKind IN ITEMS C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
		list(APPEND ConfigureCommand "-DCMAKE_${FlagKind}_FLAGS_RELEASE=PulseForgeUpdated${FlagKind}ReleaseFlags")
	endforeach()
	execute_process(
		COMMAND ${ConfigureCommand}
		RESULT_VARIABLE ConfigureResult
		OUTPUT_VARIABLE ConfigureOutput
		ERROR_VARIABLE ConfigureError)
	if(NOT ConfigureResult EQUAL 0)
		message(FATAL_ERROR "${Name} Release-flag refresh probe failed:\n${ConfigureOutput}\n${ConfigureError}")
	endif()
endfunction()

ConfigureProbe(Default)
ConfigureProbe(Overrides)
ReconfigureProbeWithChangedReleaseFlags(Default)
ReconfigureProbeWithChangedReleaseFlags(Overrides)

function(ReadProbeFile BinaryDirectory Name Output)
	set(Path "${BinaryDirectory}/${Name}")
	if(NOT EXISTS "${Path}")
		message(FATAL_ERROR "Configuration probe did not generate '${Path}'")
	endif()
	file(READ "${Path}" Contents)
	set(${Output} "${Contents}" PARENT_SCOPE)
endfunction()

if(PULSEFORGE_TEST_MULTI_CONFIG)
	set(ProbeConfigurations Debug Release Dist)
	set(ConfigurationMessage "multi-config")
	set(OverrideMessage "preserved explicit configuration list")
else()
	set(ProbeConfigurations Dist)
	set(ConfigurationMessage "single-config Dist")
	set(OverrideMessage "preserved explicit flag overrides")
endif()

foreach(Configuration IN LISTS ProbeConfigurations)
	ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Default"
		"configuration-identity-${Configuration}.txt" Definitions)
	ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Default"
		"c-configuration-identity-${Configuration}.txt" CDefinitions)
	if(Configuration STREQUAL "Debug")
		set(ExpectedDefinition PF_DEBUG)
	elseif(Configuration STREQUAL "Release")
		set(ExpectedDefinition PF_RELEASE)
	else()
		set(ExpectedDefinition PF_DIST)
	endif()
	if(NOT Definitions STREQUAL ExpectedDefinition)
		message(FATAL_ERROR "${Configuration} identity was '${Definitions}', expected '${ExpectedDefinition}'")
	endif()
	if(NOT CDefinitions STREQUAL ExpectedDefinition)
		message(FATAL_ERROR "C ${Configuration} identity was '${CDefinitions}', expected '${ExpectedDefinition}'")
	endif()
endforeach()

foreach(FlagKind IN ITEMS C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
	ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Default" "${FlagKind}FlagsDist.txt" DistFlags)
	ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Default" "${FlagKind}FlagsRelease.txt" ReleaseFlags)
	if(NOT DistFlags STREQUAL ReleaseFlags)
		message(FATAL_ERROR "Default Dist ${FlagKind} flags do not match Release flags")
	endif()
endforeach()

ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Overrides" "CFlagsDist.txt" OverrideCFlags)
ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Overrides" "CXXFlagsDist.txt" OverrideCxxFlags)
if(NOT OverrideCFlags STREQUAL "PulseForgeCustomCFlags" OR
	NOT OverrideCxxFlags STREQUAL "PulseForgeCustomCxxFlags")
	message(FATAL_ERROR "Explicit Dist compiler flag customization was not preserved")
endif()
ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Overrides" "EXE_LINKERFlagsDist.txt" OverrideExeLinkerFlags)
if(NOT OverrideExeLinkerFlags STREQUAL "PulseForgeCustomExeLinkerFlags")
	message(FATAL_ERROR "Explicit Dist executable-linker customization was not preserved")
endif()
foreach(FlagKind IN ITEMS SHARED_LINKER MODULE_LINKER STATIC_LINKER)
	ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Overrides" "${FlagKind}FlagsDist.txt" DistFlags)
	ReadProbeFile("${PULSEFORGE_CONFIGURATION_PROBE_BINARY}/Overrides" "${FlagKind}FlagsRelease.txt" ReleaseFlags)
	if(NOT DistFlags STREQUAL ReleaseFlags)
		message(FATAL_ERROR "Custom Dist ${FlagKind} flags did not track the updated Release defaults")
	endif()
endforeach()

message(STATUS "Fresh ${ConfigurationMessage} probes initialized Dist and ${OverrideMessage}")
