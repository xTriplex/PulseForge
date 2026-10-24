if(NOT DEFINED PULSEFORGE_SETTINGS_FILE)
	message(FATAL_ERROR "PULSEFORGE_SETTINGS_FILE is required for the legacy compatibility check.")
endif()

file(READ "${PULSEFORGE_SETTINGS_FILE}" PULSEFORGE_SETTINGS_JSON)
string(JSON PULSEFORGE_SETTINGS_COUNT ERROR_VARIABLE PULSEFORGE_SETTINGS_ERROR
	LENGTH "${PULSEFORGE_SETTINGS_JSON}" configurations)
if(NOT PULSEFORGE_SETTINGS_ERROR STREQUAL "NOTFOUND")
	message(FATAL_ERROR "CMakeSettings.json must contain a configurations array: ${PULSEFORGE_SETTINGS_ERROR}")
endif()
if(PULSEFORGE_SETTINGS_COUNT LESS 1)
	message(FATAL_ERROR "CMakeSettings.json must contain at least one configuration.")
endif()

set(PULSEFORGE_SEEN_CONFIGURATION_NAMES)
math(EXPR PULSEFORGE_INITIAL_LAST_INDEX "${PULSEFORGE_SETTINGS_COUNT} - 1")
foreach(PULSEFORGE_INDEX RANGE 0 ${PULSEFORGE_INITIAL_LAST_INDEX})
	string(JSON PULSEFORGE_NAME ERROR_VARIABLE PULSEFORGE_NAME_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
		configurations ${PULSEFORGE_INDEX} name)
	if(NOT PULSEFORGE_NAME_ERROR STREQUAL "NOTFOUND" OR PULSEFORGE_NAME STREQUAL "")
		message(FATAL_ERROR "CMakeSettings configuration at index ${PULSEFORGE_INDEX} needs a nonempty name.")
	endif()
	list(FIND PULSEFORGE_SEEN_CONFIGURATION_NAMES "${PULSEFORGE_NAME}" PULSEFORGE_NAME_INDEX)
	if(NOT PULSEFORGE_NAME_INDEX EQUAL -1)
		message(FATAL_ERROR "CMakeSettings.json contains duplicate configuration '${PULSEFORGE_NAME}'.")
	endif()
	list(APPEND PULSEFORGE_SEEN_CONFIGURATION_NAMES "${PULSEFORGE_NAME}")
endforeach()

function(pulseforge_find_settings_configuration OutputVariable ConfigurationName)
	set(PULSEFORGE_MATCH_COUNT 0)
	math(EXPR PULSEFORGE_LAST_INDEX "${PULSEFORGE_SETTINGS_COUNT} - 1")
	foreach(PULSEFORGE_INDEX RANGE 0 ${PULSEFORGE_LAST_INDEX})
		string(JSON PULSEFORGE_NAME ERROR_VARIABLE PULSEFORGE_NAME_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
			configurations ${PULSEFORGE_INDEX} name)
		if(PULSEFORGE_NAME STREQUAL ConfigurationName)
			math(EXPR PULSEFORGE_MATCH_COUNT "${PULSEFORGE_MATCH_COUNT} + 1")
			set(PULSEFORGE_MATCH_INDEX "${PULSEFORGE_INDEX}")
		endif()
	endforeach()
	if(PULSEFORGE_MATCH_COUNT GREATER 1)
		message(FATAL_ERROR "CMakeSettings.json contains duplicate configuration '${ConfigurationName}'.")
	endif()
	if(PULSEFORGE_MATCH_COUNT EQUAL 1)
		set(${OutputVariable} "${PULSEFORGE_MATCH_INDEX}" PARENT_SCOPE)
	else()
		set(${OutputVariable} -1 PARENT_SCOPE)
	endif()
endfunction()

function(pulseforge_require_shipped_configuration ConfigurationName ConfigurationType)
	pulseforge_find_settings_configuration(PULSEFORGE_INDEX "${ConfigurationName}")
	if(PULSEFORGE_INDEX LESS 0)
		message(FATAL_ERROR "CMakeSettings.json is missing shipped configuration '${ConfigurationName}'.")
	endif()
	string(JSON PULSEFORGE_ACTUAL_TYPE ERROR_VARIABLE PULSEFORGE_TYPE_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
		configurations ${PULSEFORGE_INDEX} configurationType)
	if(NOT PULSEFORGE_TYPE_ERROR STREQUAL "NOTFOUND" OR NOT PULSEFORGE_ACTUAL_TYPE STREQUAL ConfigurationType)
		message(FATAL_ERROR "CMakeSettings configuration '${ConfigurationName}' must use ${ConfigurationType}.")
	endif()
	string(JSON PULSEFORGE_BUILD_ROOT ERROR_VARIABLE PULSEFORGE_BUILD_ROOT_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
		configurations ${PULSEFORGE_INDEX} buildRoot)
	string(JSON PULSEFORGE_INSTALL_ROOT ERROR_VARIABLE PULSEFORGE_INSTALL_ROOT_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
		configurations ${PULSEFORGE_INDEX} installRoot)
	if(NOT PULSEFORGE_BUILD_ROOT_ERROR STREQUAL "NOTFOUND" OR
		NOT PULSEFORGE_INSTALL_ROOT_ERROR STREQUAL "NOTFOUND")
		message(FATAL_ERROR "Shipped CMakeSettings configuration '${ConfigurationName}' needs buildRoot and installRoot.")
	endif()
	string(REPLACE "\\" "/" PULSEFORGE_BUILD_ROOT "${PULSEFORGE_BUILD_ROOT}")
	string(REPLACE "\\" "/" PULSEFORGE_INSTALL_ROOT "${PULSEFORGE_INSTALL_ROOT}")
	if(NOT PULSEFORGE_BUILD_ROOT STREQUAL "\${projectDir}/Out/Build/\${name}" OR
		NOT PULSEFORGE_INSTALL_ROOT STREQUAL "\${projectDir}/Out/Install/\${name}")
		message(FATAL_ERROR "Shipped CMakeSettings configuration '${ConfigurationName}' must retain Out/Build and Out/Install defaults.")
	endif()
endfunction()

pulseforge_require_shipped_configuration(x64-Debug Debug)
pulseforge_require_shipped_configuration(x64-Release Release)
pulseforge_require_shipped_configuration(x64-Dist Dist)

math(EXPR PULSEFORGE_LAST_INDEX "${PULSEFORGE_SETTINGS_COUNT} - 1")
foreach(PULSEFORGE_INDEX RANGE 0 ${PULSEFORGE_LAST_INDEX})
	string(JSON PULSEFORGE_NAME GET "${PULSEFORGE_SETTINGS_JSON}" configurations ${PULSEFORGE_INDEX} name)
	string(JSON PULSEFORGE_BUILD_ROOT ERROR_VARIABLE PULSEFORGE_BUILD_ROOT_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
		configurations ${PULSEFORGE_INDEX} buildRoot)
	string(JSON PULSEFORGE_INSTALL_ROOT ERROR_VARIABLE PULSEFORGE_INSTALL_ROOT_ERROR GET "${PULSEFORGE_SETTINGS_JSON}"
		configurations ${PULSEFORGE_INDEX} installRoot)
	if(NOT PULSEFORGE_BUILD_ROOT_ERROR STREQUAL "NOTFOUND" OR
		NOT PULSEFORGE_INSTALL_ROOT_ERROR STREQUAL "NOTFOUND" OR
		PULSEFORGE_BUILD_ROOT STREQUAL "" OR PULSEFORGE_INSTALL_ROOT STREQUAL "")
		message(FATAL_ERROR "CMakeSettings configuration '${PULSEFORGE_NAME}' needs nonempty buildRoot and installRoot values.")
	endif()
endforeach()

message(STATUS "CMakeSettings shipped Debug, Release and Dist configurations are present; custom roots remain supported.")
