set(PULSEFORGE_PRESETS_FILE "${PULSEFORGE_SOURCE_ROOT}/CMakePresets.json")
file(READ "${PULSEFORGE_PRESETS_FILE}" PULSEFORGE_PRESETS_JSON)
string(JSON PULSEFORGE_PRESETS_VERSION GET "${PULSEFORGE_PRESETS_JSON}" version)
if(NOT PULSEFORGE_PRESETS_VERSION EQUAL 5)
	message(FATAL_ERROR "PulseForge presets must retain schema version 5.")
endif()

function(pulseforge_find_preset_index OutputVariable PresetArray PresetName)
	string(JSON PULSEFORGE_PRESET_COUNT LENGTH "${PULSEFORGE_PRESETS_JSON}" "${PresetArray}")
	math(EXPR PULSEFORGE_PRESET_LAST_INDEX "${PULSEFORGE_PRESET_COUNT} - 1")
	foreach(PULSEFORGE_INDEX RANGE 0 ${PULSEFORGE_PRESET_LAST_INDEX})
		string(JSON PULSEFORGE_PRESET_NAME GET "${PULSEFORGE_PRESETS_JSON}"
			"${PresetArray}" ${PULSEFORGE_INDEX} name)
		if(PULSEFORGE_PRESET_NAME STREQUAL PresetName)
			set(${OutputVariable} "${PULSEFORGE_INDEX}" PARENT_SCOPE)
			return()
		endif()
	endforeach()
	set(${OutputVariable} -1 PARENT_SCOPE)
endfunction()

function(pulseforge_require_preset_names PresetArray)
	string(JSON PULSEFORGE_PRESET_COUNT LENGTH "${PULSEFORGE_PRESETS_JSON}" "${PresetArray}")
	math(EXPR PULSEFORGE_EXPECTED_PRESET_COUNT "${ARGC} - 1")
	if(NOT PULSEFORGE_PRESET_COUNT EQUAL PULSEFORGE_EXPECTED_PRESET_COUNT)
		message(FATAL_ERROR "${PresetArray} has an unexpected number of named presets.")
	endif()
	foreach(PULSEFORGE_EXPECTED_NAME IN LISTS ARGN)
		pulseforge_find_preset_index(PULSEFORGE_PRESET_INDEX "${PresetArray}" "${PULSEFORGE_EXPECTED_NAME}")
		if(PULSEFORGE_PRESET_INDEX LESS 0)
			message(FATAL_ERROR "Missing ${PresetArray} identifier '${PULSEFORGE_EXPECTED_NAME}'.")
		endif()
	endforeach()
endfunction()

pulseforge_require_preset_names(configurePresets Windows-Editor Windows-Core Linux-Core)
pulseforge_require_preset_names(buildPresets
	Windows-Editor-Debug Windows-Editor-Release Windows-Editor-Dist Windows-Core-Debug Linux-Core-Debug)
pulseforge_require_preset_names(testPresets
	Windows-Editor-Debug Windows-Editor-Release Windows-Editor-Dist Windows-Core-Debug Linux-Core-Debug)

pulseforge_find_preset_index(PULSEFORGE_WINDOWS_EDITOR_INDEX configurePresets Windows-Editor)
string(JSON PULSEFORGE_WINDOWS_EDITOR_GENERATOR GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_EDITOR_INDEX} generator)
string(JSON PULSEFORGE_WINDOWS_EDITOR_BINARY_DIR GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_EDITOR_INDEX} binaryDir)
string(JSON PULSEFORGE_WINDOWS_EDITOR_DISPLAY_NAME GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_EDITOR_INDEX} displayName)
string(JSON PULSEFORGE_WINDOWS_EDITOR_HOST_OS GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_EDITOR_INDEX} vendor
	"microsoft.com/VisualStudioSettings/CMake/1.0" hostOS 0)
if(NOT PULSEFORGE_WINDOWS_EDITOR_GENERATOR STREQUAL "Visual Studio 17 2022")
	message(FATAL_ERROR "Windows-Editor must use the existing Visual Studio multi-config generator.")
endif()
if(NOT PULSEFORGE_WINDOWS_EDITOR_BINARY_DIR STREQUAL "\${sourceDir}/Build/CMakePresets/windows-editor")
	message(FATAL_ERROR "Windows-Editor must preserve its existing shared build directory.")
endif()
if(NOT PULSEFORGE_WINDOWS_EDITOR_DISPLAY_NAME STREQUAL "Windows Editor")
	message(FATAL_ERROR "Windows-Editor must have a coherent capitalized display name.")
endif()
if(NOT PULSEFORGE_WINDOWS_EDITOR_HOST_OS STREQUAL "Windows")
	message(FATAL_ERROR "Windows-Editor must be limited to Windows Visual Studio target systems.")
endif()
string(JSON PULSEFORGE_WINDOWS_EDITOR_BUILD_TYPE ERROR_VARIABLE PULSEFORGE_WINDOWS_EDITOR_BUILD_TYPE_ERROR
	GET "${PULSEFORGE_PRESETS_JSON}" configurePresets ${PULSEFORGE_WINDOWS_EDITOR_INDEX}
	cacheVariables CMAKE_BUILD_TYPE)
if(PULSEFORGE_WINDOWS_EDITOR_BUILD_TYPE_ERROR STREQUAL "NOTFOUND")
	message(FATAL_ERROR "The multi-configuration Windows-Editor preset must not set CMAKE_BUILD_TYPE.")
endif()

pulseforge_find_preset_index(PULSEFORGE_WINDOWS_CORE_INDEX configurePresets Windows-Core)
string(JSON PULSEFORGE_WINDOWS_CORE_PARENT GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_CORE_INDEX} inherits)
string(JSON PULSEFORGE_WINDOWS_CORE_BINARY_DIR GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_CORE_INDEX} binaryDir)
string(JSON PULSEFORGE_WINDOWS_CORE_HOST_OS GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_CORE_INDEX} vendor
	"microsoft.com/VisualStudioSettings/CMake/1.0" hostOS 0)
if(NOT PULSEFORGE_WINDOWS_CORE_PARENT STREQUAL "Windows-Editor" OR
	NOT PULSEFORGE_WINDOWS_CORE_BINARY_DIR STREQUAL "\${sourceDir}/Build/CMakePresets/windows-core" OR
	NOT PULSEFORGE_WINDOWS_CORE_HOST_OS STREQUAL "Windows")
	message(FATAL_ERROR "Windows-Core must preserve its parent, build directory and host filter.")
endif()
string(JSON PULSEFORGE_WINDOWS_CORE_DISPLAY_NAME GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_WINDOWS_CORE_INDEX} displayName)
if(NOT PULSEFORGE_WINDOWS_CORE_DISPLAY_NAME STREQUAL "Windows Core and CPU Tests")
	message(FATAL_ERROR "Windows-Core must have a coherent capitalized display name.")
endif()

pulseforge_find_preset_index(PULSEFORGE_LINUX_CORE_INDEX configurePresets Linux-Core)
string(JSON PULSEFORGE_LINUX_CORE_GENERATOR GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_LINUX_CORE_INDEX} generator)
string(JSON PULSEFORGE_LINUX_CORE_BINARY_DIR GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_LINUX_CORE_INDEX} binaryDir)
string(JSON PULSEFORGE_LINUX_CORE_HOST_OS GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_LINUX_CORE_INDEX} vendor
	"microsoft.com/VisualStudioSettings/CMake/1.0" hostOS 0)
string(JSON PULSEFORGE_LINUX_CORE_BUILD_TYPE GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_LINUX_CORE_INDEX} cacheVariables CMAKE_BUILD_TYPE)
if(NOT PULSEFORGE_LINUX_CORE_GENERATOR STREQUAL "Unix Makefiles" OR
	NOT PULSEFORGE_LINUX_CORE_BINARY_DIR STREQUAL "\${sourceDir}/Build/CMakePresets/linux-core" OR
	NOT PULSEFORGE_LINUX_CORE_HOST_OS STREQUAL "Linux" OR
	NOT PULSEFORGE_LINUX_CORE_BUILD_TYPE STREQUAL "Debug")
	message(FATAL_ERROR "Linux-Core must preserve its Linux configure behavior and binary directory.")
endif()
string(JSON PULSEFORGE_LINUX_CORE_DISPLAY_NAME GET "${PULSEFORGE_PRESETS_JSON}"
	configurePresets ${PULSEFORGE_LINUX_CORE_INDEX} displayName)
if(NOT PULSEFORGE_LINUX_CORE_DISPLAY_NAME STREQUAL "Linux Core and CPU Tests")
	message(FATAL_ERROR "Linux-Core must have a coherent capitalized display name.")
endif()

foreach(PULSEFORGE_CONFIGURATION IN ITEMS Debug Release Dist)
	set(PULSEFORGE_PRESET_NAME "Windows-Editor-${PULSEFORGE_CONFIGURATION}")
	pulseforge_find_preset_index(PULSEFORGE_BUILD_PRESET_INDEX buildPresets "${PULSEFORGE_PRESET_NAME}")
	string(JSON PULSEFORGE_BUILD_PRESET_CONFIGURE GET "${PULSEFORGE_PRESETS_JSON}"
		buildPresets ${PULSEFORGE_BUILD_PRESET_INDEX} configurePreset)
	string(JSON PULSEFORGE_BUILD_PRESET_CONFIGURATION GET "${PULSEFORGE_PRESETS_JSON}"
		buildPresets ${PULSEFORGE_BUILD_PRESET_INDEX} configuration)
	string(JSON PULSEFORGE_BUILD_PRESET_DISPLAY_NAME GET "${PULSEFORGE_PRESETS_JSON}"
		buildPresets ${PULSEFORGE_BUILD_PRESET_INDEX} displayName)
	if(NOT PULSEFORGE_BUILD_PRESET_CONFIGURE STREQUAL "Windows-Editor")
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} must use Windows-Editor.")
	endif()
	if(NOT PULSEFORGE_BUILD_PRESET_CONFIGURATION STREQUAL PULSEFORGE_CONFIGURATION)
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} must select ${PULSEFORGE_CONFIGURATION} exactly.")
	endif()
	if(NOT PULSEFORGE_BUILD_PRESET_DISPLAY_NAME STREQUAL "Windows Editor ${PULSEFORGE_CONFIGURATION}")
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} must have a matching capitalized display name.")
	endif()
	string(JSON PULSEFORGE_BUILD_PRESET_HAS_TARGETS ERROR_VARIABLE PULSEFORGE_TARGETS_ERROR
		GET "${PULSEFORGE_PRESETS_JSON}" buildPresets ${PULSEFORGE_BUILD_PRESET_INDEX} targets)
	if(PULSEFORGE_TARGETS_ERROR STREQUAL "NOTFOUND")
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} must build the normal target set.")
	endif()

	pulseforge_find_preset_index(PULSEFORGE_TEST_PRESET_INDEX testPresets "${PULSEFORGE_PRESET_NAME}")
	string(JSON PULSEFORGE_TEST_PRESET_CONFIGURE GET "${PULSEFORGE_PRESETS_JSON}"
		testPresets ${PULSEFORGE_TEST_PRESET_INDEX} configurePreset)
	string(JSON PULSEFORGE_TEST_PRESET_CONFIGURATION GET "${PULSEFORGE_PRESETS_JSON}"
		testPresets ${PULSEFORGE_TEST_PRESET_INDEX} configuration)
	string(JSON PULSEFORGE_TEST_PRESET_DISPLAY_NAME GET "${PULSEFORGE_PRESETS_JSON}"
		testPresets ${PULSEFORGE_TEST_PRESET_INDEX} displayName)
	if(NOT PULSEFORGE_TEST_PRESET_CONFIGURE STREQUAL "Windows-Editor" OR
		NOT PULSEFORGE_TEST_PRESET_CONFIGURATION STREQUAL PULSEFORGE_CONFIGURATION OR
		NOT PULSEFORGE_TEST_PRESET_DISPLAY_NAME STREQUAL "Windows Editor ${PULSEFORGE_CONFIGURATION}")
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} test preset does not match its build configuration.")
	endif()
endforeach()

foreach(PULSEFORGE_PRESET_NAME IN ITEMS Windows-Core-Debug Linux-Core-Debug)
	if(PULSEFORGE_PRESET_NAME STREQUAL "Windows-Core-Debug")
		set(PULSEFORGE_CONFIGURE_PRESET_NAME Windows-Core)
		set(PULSEFORGE_CONFIGURATION Debug)
	else()
		set(PULSEFORGE_CONFIGURE_PRESET_NAME Linux-Core)
		set(PULSEFORGE_CONFIGURATION Debug)
	endif()
	pulseforge_find_preset_index(PULSEFORGE_BUILD_PRESET_INDEX buildPresets "${PULSEFORGE_PRESET_NAME}")
	string(JSON PULSEFORGE_BUILD_PRESET_CONFIGURE GET "${PULSEFORGE_PRESETS_JSON}"
		buildPresets ${PULSEFORGE_BUILD_PRESET_INDEX} configurePreset)
	if(NOT PULSEFORGE_BUILD_PRESET_CONFIGURE STREQUAL PULSEFORGE_CONFIGURE_PRESET_NAME)
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} must use ${PULSEFORGE_CONFIGURE_PRESET_NAME}.")
	endif()
	if(PULSEFORGE_PRESET_NAME STREQUAL "Windows-Core-Debug")
		string(JSON PULSEFORGE_BUILD_PRESET_CONFIGURATION GET "${PULSEFORGE_PRESETS_JSON}"
			buildPresets ${PULSEFORGE_BUILD_PRESET_INDEX} configuration)
		if(NOT PULSEFORGE_BUILD_PRESET_CONFIGURATION STREQUAL PULSEFORGE_CONFIGURATION)
			message(FATAL_ERROR "Windows-Core-Debug must select Debug.")
		endif()
	endif()

	pulseforge_find_preset_index(PULSEFORGE_TEST_PRESET_INDEX testPresets "${PULSEFORGE_PRESET_NAME}")
	string(JSON PULSEFORGE_TEST_PRESET_CONFIGURE GET "${PULSEFORGE_PRESETS_JSON}"
		testPresets ${PULSEFORGE_TEST_PRESET_INDEX} configurePreset)
	if(NOT PULSEFORGE_TEST_PRESET_CONFIGURE STREQUAL PULSEFORGE_CONFIGURE_PRESET_NAME)
		message(FATAL_ERROR "${PULSEFORGE_PRESET_NAME} test preset must use ${PULSEFORGE_CONFIGURE_PRESET_NAME}.")
	endif()
	if(PULSEFORGE_PRESET_NAME STREQUAL "Windows-Core-Debug")
		string(JSON PULSEFORGE_TEST_PRESET_CONFIGURATION GET "${PULSEFORGE_PRESETS_JSON}"
			testPresets ${PULSEFORGE_TEST_PRESET_INDEX} configuration)
		if(NOT PULSEFORGE_TEST_PRESET_CONFIGURATION STREQUAL PULSEFORGE_CONFIGURATION)
			message(FATAL_ERROR "Windows-Core-Debug test preset must select Debug.")
		endif()
	endif()
endforeach()

execute_process(
	COMMAND "${PULSEFORGE_CMAKE_COMMAND}" --list-presets=all
	WORKING_DIRECTORY "${PULSEFORGE_SOURCE_ROOT}"
	RESULT_VARIABLE PULSEFORGE_LIST_PRESETS_RESULT
	OUTPUT_VARIABLE PULSEFORGE_LIST_PRESETS_OUTPUT
	ERROR_VARIABLE PULSEFORGE_LIST_PRESETS_ERROR)
if(NOT PULSEFORGE_LIST_PRESETS_RESULT EQUAL 0)
	message(FATAL_ERROR "cmake --list-presets=all failed: ${PULSEFORGE_LIST_PRESETS_ERROR}")
endif()
foreach(PULSEFORGE_REQUIRED_PRESET IN ITEMS
	Windows-Editor Windows-Core Linux-Core Windows-Editor-Debug Windows-Editor-Release Windows-Editor-Dist
	Windows-Core-Debug Linux-Core-Debug)
	if(NOT PULSEFORGE_LIST_PRESETS_OUTPUT MATCHES "${PULSEFORGE_REQUIRED_PRESET}")
		message(FATAL_ERROR "cmake --list-presets=all omitted ${PULSEFORGE_REQUIRED_PRESET}.")
	endif()
endforeach()
foreach(PULSEFORGE_FORBIDDEN_PRESET IN ITEMS
	windows-editor windows-core linux-core windows-editor-debug windows-editor-release windows-editor-dist
	windows-core-debug linux-core-debug)
	if(PULSEFORGE_LIST_PRESETS_OUTPUT MATCHES "${PULSEFORGE_FORBIDDEN_PRESET}")
		message(FATAL_ERROR "cmake --list-presets=all exposed obsolete identifier ${PULSEFORGE_FORBIDDEN_PRESET}.")
	endif()
endforeach()

message(STATUS "Windows Editor Debug, Release and Dist presets share ${PULSEFORGE_WINDOWS_EDITOR_BINARY_DIR}.")
