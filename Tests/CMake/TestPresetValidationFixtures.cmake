foreach(PULSEFORGE_REQUIRED_VARIABLE IN ITEMS PULSEFORGE_SOURCE_ROOT PULSEFORGE_BINARY_ROOT PULSEFORGE_CMAKE_COMMAND)
	if(NOT DEFINED ${PULSEFORGE_REQUIRED_VARIABLE})
		message(FATAL_ERROR "${PULSEFORGE_REQUIRED_VARIABLE} is required.")
	endif()
endforeach()

string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef PULSEFORGE_FIXTURE_SUFFIX)
set(PULSEFORGE_FIXTURE_ROOT "${PULSEFORGE_BINARY_ROOT}/PresetValidation-${PULSEFORGE_FIXTURE_SUFFIX}")
set(PULSEFORGE_PRESET_VALIDATOR "${CMAKE_CURRENT_LIST_DIR}/TestPresets.cmake")
set(PULSEFORGE_SETTINGS_VALIDATOR "${CMAKE_CURRENT_LIST_DIR}/TestLegacyCMakeSettings.cmake")

function(pulseforge_make_preset_fixture Name OutputDirectory)
	set(PULSEFORGE_DIRECTORY "${PULSEFORGE_FIXTURE_ROOT}/${Name}")
	file(MAKE_DIRECTORY "${PULSEFORGE_DIRECTORY}")
	file(COPY_FILE "${PULSEFORGE_SOURCE_ROOT}/CMakePresets.json" "${PULSEFORGE_DIRECTORY}/CMakePresets.json")
	set(${OutputDirectory} "${PULSEFORGE_DIRECTORY}" PARENT_SCOPE)
endfunction()

function(pulseforge_run_preset_contract Name Directory ShouldPass)
	execute_process(
		COMMAND "${PULSEFORGE_CMAKE_COMMAND}"
			"-DPULSEFORGE_SOURCE_ROOT=${Directory}"
			"-DPULSEFORGE_PRESETS_FILE=${Directory}/CMakePresets.json"
			"-DPULSEFORGE_PRESET_WORKING_DIRECTORY=${Directory}"
			"-DPULSEFORGE_CMAKE_COMMAND=${PULSEFORGE_CMAKE_COMMAND}"
			-P "${PULSEFORGE_PRESET_VALIDATOR}"
		RESULT_VARIABLE PULSEFORGE_RESULT
		OUTPUT_VARIABLE PULSEFORGE_OUTPUT
		ERROR_VARIABLE PULSEFORGE_ERROR)
	if(ShouldPass AND NOT PULSEFORGE_RESULT EQUAL 0)
		message(FATAL_ERROR "Preset fixture '${Name}' should pass but failed (${PULSEFORGE_RESULT}):\n${PULSEFORGE_OUTPUT}\n${PULSEFORGE_ERROR}")
	elseif(NOT ShouldPass AND PULSEFORGE_RESULT EQUAL 0)
		message(FATAL_ERROR "Preset fixture '${Name}' should fail but passed.")
	endif()
	message(STATUS "Preset fixture '${Name}': ${ShouldPass}")
endfunction()

function(pulseforge_find_json_array_item OutputVariable Json ArrayName ItemName)
	string(JSON PULSEFORGE_COUNT LENGTH "${Json}" "${ArrayName}")
	math(EXPR PULSEFORGE_LAST_INDEX "${PULSEFORGE_COUNT} - 1")
	set(PULSEFORGE_MATCH_INDEX -1)
	foreach(PULSEFORGE_INDEX RANGE 0 ${PULSEFORGE_LAST_INDEX})
		string(JSON PULSEFORGE_NAME GET "${Json}" "${ArrayName}" ${PULSEFORGE_INDEX} name)
		if(PULSEFORGE_NAME STREQUAL ItemName)
			set(PULSEFORGE_MATCH_INDEX "${PULSEFORGE_INDEX}")
			break()
		endif()
	endforeach()
	set(${OutputVariable} "${PULSEFORGE_MATCH_INDEX}" PARENT_SCOPE)
endfunction()

function(pulseforge_append_json_array_item OutputVariable Json ArrayName ItemJson)
	string(JSON PULSEFORGE_COUNT LENGTH "${Json}" "${ArrayName}")
	string(JSON PULSEFORGE_UPDATED SET "${Json}" "${ArrayName}" ${PULSEFORGE_COUNT} "${ItemJson}")
	set(${OutputVariable} "${PULSEFORGE_UPDATED}" PARENT_SCOPE)
endfunction()

function(pulseforge_reverse_json_array OutputVariable Json ArrayName)
	string(JSON PULSEFORGE_COUNT LENGTH "${Json}" "${ArrayName}")
	set(PULSEFORGE_ARRAY_JSON "[")
	math(EXPR PULSEFORGE_INDEX "${PULSEFORGE_COUNT} - 1")
	while(PULSEFORGE_INDEX GREATER_EQUAL 0)
		string(JSON PULSEFORGE_ITEM GET "${Json}" "${ArrayName}" ${PULSEFORGE_INDEX})
		if(NOT PULSEFORGE_ARRAY_JSON STREQUAL "[")
			string(APPEND PULSEFORGE_ARRAY_JSON ",")
		endif()
		string(APPEND PULSEFORGE_ARRAY_JSON "${PULSEFORGE_ITEM}")
		math(EXPR PULSEFORGE_INDEX "${PULSEFORGE_INDEX} - 1")
	endwhile()
	string(APPEND PULSEFORGE_ARRAY_JSON "]")
	string(JSON PULSEFORGE_UPDATED SET "${Json}" "${ArrayName}" "${PULSEFORGE_ARRAY_JSON}")
	set(${OutputVariable} "${PULSEFORGE_UPDATED}" PARENT_SCOPE)
endfunction()

function(pulseforge_write_preset_json Directory Json)
	file(WRITE "${Directory}/CMakePresets.json" "${Json}\n")
endfunction()

if(DEFINED PULSEFORGE_LEGACY_SETTINGS_FILE)
	include("${CMAKE_CURRENT_LIST_DIR}/TestLegacyCMakeSettingsFixtures.cmake")
	return()
endif()

pulseforge_make_preset_fixture(defaults PULSEFORGE_DEFAULTS_DIRECTORY)
pulseforge_run_preset_contract(defaults "${PULSEFORGE_DEFAULTS_DIRECTORY}" TRUE)

pulseforge_make_preset_fixture(reordered PULSEFORGE_REORDERED_DIRECTORY)
file(READ "${PULSEFORGE_REORDERED_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
foreach(PULSEFORGE_ARRAY IN ITEMS configurePresets buildPresets testPresets)
	pulseforge_reverse_json_array(PULSEFORGE_JSON "${PULSEFORGE_JSON}" "${PULSEFORGE_ARRAY}")
endforeach()
pulseforge_write_preset_json("${PULSEFORGE_REORDERED_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(reordered "${PULSEFORGE_REORDERED_DIRECTORY}" TRUE)

pulseforge_make_preset_fixture(extensions PULSEFORGE_EXTENSIONS_DIRECTORY)
file(READ "${PULSEFORGE_EXTENSIONS_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
pulseforge_append_json_array_item(PULSEFORGE_JSON "${PULSEFORGE_JSON}" configurePresets [=[{"name":"Local-Core","inherits":"Linux-Core","binaryDir":"${sourceDir}/LocalCoreBuild"}]=])
pulseforge_append_json_array_item(PULSEFORGE_JSON "${PULSEFORGE_JSON}" buildPresets [=[{"name":"Local-Core-Debug","configurePreset":"Local-Core","configuration":"Debug"}]=])
pulseforge_append_json_array_item(PULSEFORGE_JSON "${PULSEFORGE_JSON}" testPresets [=[{"name":"Local-Core-Debug","configurePreset":"Local-Core","configuration":"Debug"}]=])
pulseforge_write_preset_json("${PULSEFORGE_EXTENSIONS_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(extensions "${PULSEFORGE_EXTENSIONS_DIRECTORY}" TRUE)

pulseforge_make_preset_fixture(user-inheritance PULSEFORGE_USER_DIRECTORY)
file(WRITE "${PULSEFORGE_USER_DIRECTORY}/CMakeLists.txt"
	"cmake_minimum_required(VERSION 3.24)\nproject(PulseForgePresetFixture LANGUAGES NONE)\n")
if(WIN32)
	set(PULSEFORGE_USER_PARENT Windows-Editor)
else()
	set(PULSEFORGE_USER_PARENT Linux-Core)
endif()
file(WRITE "${PULSEFORGE_USER_DIRECTORY}/CMakeUserPresets.json"
"{\n  \"version\": 5,\n  \"configurePresets\": [{\"name\": \"Developer-Local\", \"inherits\": \"${PULSEFORGE_USER_PARENT}\", \"binaryDir\": \"\${sourceDir}/DeveloperBuild\"}]\n}\n")
pulseforge_run_preset_contract(user-inheritance "${PULSEFORGE_USER_DIRECTORY}" TRUE)
execute_process(
	COMMAND "${PULSEFORGE_CMAKE_COMMAND}" --preset Developer-Local
	WORKING_DIRECTORY "${PULSEFORGE_USER_DIRECTORY}"
	RESULT_VARIABLE PULSEFORGE_USER_CONFIGURE_RESULT
	OUTPUT_VARIABLE PULSEFORGE_USER_CONFIGURE_OUTPUT
	ERROR_VARIABLE PULSEFORGE_USER_CONFIGURE_ERROR)
if(NOT PULSEFORGE_USER_CONFIGURE_RESULT EQUAL 0 OR
	NOT EXISTS "${PULSEFORGE_USER_DIRECTORY}/DeveloperBuild/CMakeCache.txt")
	message(FATAL_ERROR "A user preset inheriting ${PULSEFORGE_USER_PARENT} did not configure in its custom binaryDir:\n${PULSEFORGE_USER_CONFIGURE_OUTPUT}\n${PULSEFORGE_USER_CONFIGURE_ERROR}")
endif()
message(STATUS "User preset inheritance configured into DeveloperBuild.")

pulseforge_make_preset_fixture(missing-required PULSEFORGE_MISSING_DIRECTORY)
file(READ "${PULSEFORGE_MISSING_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
pulseforge_find_json_array_item(PULSEFORGE_INDEX "${PULSEFORGE_JSON}" configurePresets Windows-Core)
string(JSON PULSEFORGE_JSON REMOVE "${PULSEFORGE_JSON}" configurePresets ${PULSEFORGE_INDEX})
pulseforge_write_preset_json("${PULSEFORGE_MISSING_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(missing-required "${PULSEFORGE_MISSING_DIRECTORY}" FALSE)

pulseforge_make_preset_fixture(duplicate-identity PULSEFORGE_DUPLICATE_DIRECTORY)
file(READ "${PULSEFORGE_DUPLICATE_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
pulseforge_append_json_array_item(PULSEFORGE_JSON "${PULSEFORGE_JSON}" configurePresets
	[=[{"name":"Windows-Core","inherits":"Windows-Editor","binaryDir":"${sourceDir}/DuplicateCore"}]=])
pulseforge_write_preset_json("${PULSEFORGE_DUPLICATE_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(duplicate-identity "${PULSEFORGE_DUPLICATE_DIRECTORY}" FALSE)

pulseforge_make_preset_fixture(wrong-release PULSEFORGE_WRONG_RELEASE_DIRECTORY)
file(READ "${PULSEFORGE_WRONG_RELEASE_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
pulseforge_find_json_array_item(PULSEFORGE_INDEX "${PULSEFORGE_JSON}" buildPresets Windows-Editor-Release)
string(JSON PULSEFORGE_JSON SET "${PULSEFORGE_JSON}" buildPresets ${PULSEFORGE_INDEX} configuration "\"Debug\"")
pulseforge_write_preset_json("${PULSEFORGE_WRONG_RELEASE_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(wrong-release "${PULSEFORGE_WRONG_RELEASE_DIRECTORY}" FALSE)

pulseforge_make_preset_fixture(bad-reference PULSEFORGE_BAD_REFERENCE_DIRECTORY)
file(READ "${PULSEFORGE_BAD_REFERENCE_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
pulseforge_append_json_array_item(PULSEFORGE_JSON "${PULSEFORGE_JSON}" buildPresets [=[{"name":"Broken-Reference","configurePreset":"Does-Not-Exist"}]=])
pulseforge_write_preset_json("${PULSEFORGE_BAD_REFERENCE_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(bad-reference "${PULSEFORGE_BAD_REFERENCE_DIRECTORY}" FALSE)

pulseforge_make_preset_fixture(wrong-default-path PULSEFORGE_WRONG_PATH_DIRECTORY)
file(READ "${PULSEFORGE_WRONG_PATH_DIRECTORY}/CMakePresets.json" PULSEFORGE_JSON)
pulseforge_find_json_array_item(PULSEFORGE_INDEX "${PULSEFORGE_JSON}" configurePresets Windows-Editor)
string(JSON PULSEFORGE_JSON SET "${PULSEFORGE_JSON}" configurePresets ${PULSEFORGE_INDEX} binaryDir
	"\"\${sourceDir}/Build/CMakePresets/Windowseditor\"")
pulseforge_write_preset_json("${PULSEFORGE_WRONG_PATH_DIRECTORY}" "${PULSEFORGE_JSON}")
pulseforge_run_preset_contract(wrong-default-path "${PULSEFORGE_WRONG_PATH_DIRECTORY}" FALSE)

pulseforge_make_preset_fixture(malformed-json PULSEFORGE_MALFORMED_DIRECTORY)
file(WRITE "${PULSEFORGE_MALFORMED_DIRECTORY}/CMakePresets.json" "{\"version\":\n")
pulseforge_run_preset_contract(malformed-json "${PULSEFORGE_MALFORMED_DIRECTORY}" FALSE)

if(PULSEFORGE_FIXTURE_ROOT MATCHES "^${PULSEFORGE_BINARY_ROOT}/PresetValidation-[0-9a-f]+$")
	file(REMOVE_RECURSE "${PULSEFORGE_FIXTURE_ROOT}")
endif()

message(STATUS "Project preset defaults, extension, ordering, and failure fixtures passed.")
