cmake_minimum_required(VERSION 3.24)

if(PULSEFORGE_TEST_CONFIGURATIONS)
	string(REPLACE "," ";" CMAKE_CONFIGURATION_TYPES "${PULSEFORGE_TEST_CONFIGURATIONS}")
	set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Probe configurations" FORCE)
endif()

if(PULSEFORGE_TEST_OVERRIDE_FLAGS)
	set(CMAKE_C_FLAGS_DIST "PulseForgeCustomCFlags" CACHE STRING "Probe override" FORCE)
	set(CMAKE_CXX_FLAGS_DIST "PulseForgeCustomCxxFlags" CACHE STRING "Probe override" FORCE)
	set(CMAKE_EXE_LINKER_FLAGS_DIST "PulseForgeCustomExeLinkerFlags" CACHE STRING "Probe override" FORCE)
endif()

project(PulseForgeCustomConfigurationProbe LANGUAGES C CXX)

option(PULSEFORGE_DIST_INHERIT_RELEASE_FLAGS "Use Release flags for Dist when no override is supplied" ON)
include("${PULSEFORGE_CONFIGURATION_POLICY}")

if(CMAKE_CONFIGURATION_TYPES)
	if(NOT "Dist" IN_LIST CMAKE_CONFIGURATION_TYPES)
		message(FATAL_ERROR "The custom Dist configuration was not added before language initialization")
	endif()
elseif(NOT CMAKE_BUILD_TYPE STREQUAL "Dist")
	message(FATAL_ERROR "The single-config probe must use the Dist build type")
endif()

if(PULSEFORGE_TEST_CONFIGURATIONS AND NOT "Custom" IN_LIST CMAKE_CONFIGURATION_TYPES)
	message(FATAL_ERROR "The user-supplied configuration list was not preserved")
endif()

foreach(VariableName IN ITEMS
		CMAKE_C_FLAGS_DIST
		CMAKE_CXX_FLAGS_DIST
		CMAKE_EXE_LINKER_FLAGS_DIST
		CMAKE_SHARED_LINKER_FLAGS_DIST
		CMAKE_MODULE_LINKER_FLAGS_DIST
		CMAKE_STATIC_LINKER_FLAGS_DIST)
	if(NOT DEFINED ${VariableName})
		message(FATAL_ERROR "CMake did not initialize ${VariableName} for Dist")
	endif()
endforeach()

if(PULSEFORGE_TEST_OVERRIDE_FLAGS AND
	(NOT CMAKE_C_FLAGS_DIST STREQUAL "PulseForgeCustomCFlags" OR
		NOT CMAKE_CXX_FLAGS_DIST STREQUAL "PulseForgeCustomCxxFlags" OR
		NOT CMAKE_EXE_LINKER_FLAGS_DIST STREQUAL "PulseForgeCustomExeLinkerFlags"))
	message(FATAL_ERROR "The custom Dist compiler/linker flag overrides were not preserved")
endif()

if(PULSEFORGE_DIST_INHERIT_RELEASE_FLAGS AND NOT PULSEFORGE_TEST_OVERRIDE_FLAGS)
	foreach(FlagKind IN ITEMS C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
		set(DistFlagsVariable "CMAKE_${FlagKind}_FLAGS_DIST")
		set(ReleaseFlagsVariable "CMAKE_${FlagKind}_FLAGS_RELEASE")
		if(NOT "${${DistFlagsVariable}}" STREQUAL "${${ReleaseFlagsVariable}}")
			message(FATAL_ERROR "Dist ${FlagKind} flags did not inherit the active Release value")
		endif()
	endforeach()
endif()

foreach(FlagKind IN ITEMS C CXX EXE_LINKER SHARED_LINKER MODULE_LINKER STATIC_LINKER)
	set(DistFlagsVariable "CMAKE_${FlagKind}_FLAGS_DIST")
	set(ReleaseFlagsVariable "CMAKE_${FlagKind}_FLAGS_RELEASE")
	file(WRITE "${CMAKE_BINARY_DIR}/${FlagKind}FlagsDist.txt" "${${DistFlagsVariable}}")
	file(WRITE "${CMAKE_BINARY_DIR}/${FlagKind}FlagsRelease.txt" "${${ReleaseFlagsVariable}}")
endforeach()

add_library(PulseForgeConfigurationIdentity OBJECT configuration_identity.cpp)
pulseforge_apply_configuration_identity(PulseForgeConfigurationIdentity)
file(GENERATE
	OUTPUT "${CMAKE_BINARY_DIR}/configuration-identity-$<CONFIG>.txt"
	CONTENT "$<TARGET_PROPERTY:PulseForgeConfigurationIdentity,COMPILE_DEFINITIONS>")

add_library(PulseForgeCConfigurationIdentity OBJECT configuration_identity.c)
pulseforge_apply_configuration_identity(PulseForgeCConfigurationIdentity)
file(GENERATE
	OUTPUT "${CMAKE_BINARY_DIR}/c-configuration-identity-$<CONFIG>.txt"
	CONTENT "$<TARGET_PROPERTY:PulseForgeCConfigurationIdentity,COMPILE_DEFINITIONS>")
