cmake_minimum_required(VERSION 3.24)

if(PULSEFORGE_TEST_CONFIGURATIONS)
	string(REPLACE "," ";" CMAKE_CONFIGURATION_TYPES "${PULSEFORGE_TEST_CONFIGURATIONS}")
	set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Probe configurations" FORCE)
endif()

if(PULSEFORGE_TEST_OVERRIDE_FLAGS)
	set(CMAKE_CXX_FLAGS_DIST "PulseForgeCustomCxxFlags" CACHE STRING "Probe override" FORCE)
	set(CMAKE_EXE_LINKER_FLAGS_DIST "PulseForgeCustomExeLinkerFlags" CACHE STRING "Probe override" FORCE)
endif()

project(PulseForgeCustomConfigurationProbe LANGUAGES C CXX)

if(NOT "Dist" IN_LIST CMAKE_CONFIGURATION_TYPES)
	message(FATAL_ERROR "The custom Dist configuration was not added before language initialization")
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
	(NOT CMAKE_CXX_FLAGS_DIST STREQUAL "PulseForgeCustomCxxFlags" OR
		NOT CMAKE_EXE_LINKER_FLAGS_DIST STREQUAL "PulseForgeCustomExeLinkerFlags"))
	message(FATAL_ERROR "The custom Dist compiler/linker flag overrides were not preserved")
endif()
