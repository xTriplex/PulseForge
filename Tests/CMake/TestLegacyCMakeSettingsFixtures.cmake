function(pulseforge_run_settings_contract Name SettingsFile ShouldPass)
	execute_process(
		COMMAND "${PULSEFORGE_CMAKE_COMMAND}"
			"-DPULSEFORGE_SETTINGS_FILE=${SettingsFile}"
			-P "${PULSEFORGE_SETTINGS_VALIDATOR}"
		RESULT_VARIABLE PULSEFORGE_RESULT
		OUTPUT_VARIABLE PULSEFORGE_OUTPUT
		ERROR_VARIABLE PULSEFORGE_ERROR)
	if(ShouldPass AND NOT PULSEFORGE_RESULT EQUAL 0)
		message(FATAL_ERROR "Legacy settings fixture '${Name}' should pass but failed:\n${PULSEFORGE_OUTPUT}\n${PULSEFORGE_ERROR}")
	elseif(NOT ShouldPass AND PULSEFORGE_RESULT EQUAL 0)
		message(FATAL_ERROR "Legacy settings fixture '${Name}' should fail but passed.")
	endif()
	message(STATUS "Legacy settings fixture '${Name}': ${ShouldPass}")
endfunction()

function(pulseforge_write_settings_json Directory Json)
	file(WRITE "${Directory}/CMakeSettings.json" "${Json}\n")
endfunction()

pulseforge_run_settings_contract(shipped-defaults "${PULSEFORGE_LEGACY_SETTINGS_FILE}" TRUE)

set(PULSEFORGE_SETTINGS_FIXTURE_ROOT "${PULSEFORGE_FIXTURE_ROOT}/legacy-settings")
file(MAKE_DIRECTORY "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}")
file(COPY_FILE "${PULSEFORGE_LEGACY_SETTINGS_FILE}"
	"${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json")
file(READ "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" PULSEFORGE_SETTINGS_JSON)

pulseforge_reverse_json_array(PULSEFORGE_REORDERED_SETTINGS "${PULSEFORGE_SETTINGS_JSON}" configurations)
pulseforge_write_settings_json("${PULSEFORGE_SETTINGS_FIXTURE_ROOT}" "${PULSEFORGE_REORDERED_SETTINGS}")
pulseforge_run_settings_contract(reordered "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" TRUE)

set(PULSEFORGE_CUSTOM_SETTINGS_JSON "${PULSEFORGE_SETTINGS_JSON}")
pulseforge_append_json_array_item(PULSEFORGE_CUSTOM_SETTINGS_JSON "${PULSEFORGE_CUSTOM_SETTINGS_JSON}" configurations
	[=[{"name":"x64-Custom","generator":"Ninja","configurationType":"Release","buildRoot":"${projectDir}/LocalBuild/Custom","installRoot":"${projectDir}/LocalInstall/Custom"}]=])
pulseforge_write_settings_json("${PULSEFORGE_SETTINGS_FIXTURE_ROOT}" "${PULSEFORGE_CUSTOM_SETTINGS_JSON}")
pulseforge_run_settings_contract(custom-roots "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" TRUE)

file(READ "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" PULSEFORGE_SETTINGS_JSON)
pulseforge_find_json_array_item(PULSEFORGE_INDEX "${PULSEFORGE_SETTINGS_JSON}" configurations x64-Debug)
string(JSON PULSEFORGE_SETTINGS_JSON REMOVE "${PULSEFORGE_SETTINGS_JSON}" configurations ${PULSEFORGE_INDEX})
pulseforge_write_settings_json("${PULSEFORGE_SETTINGS_FIXTURE_ROOT}" "${PULSEFORGE_SETTINGS_JSON}")
pulseforge_run_settings_contract(missing-required "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" FALSE)

file(READ "${PULSEFORGE_LEGACY_SETTINGS_FILE}" PULSEFORGE_SETTINGS_JSON)
pulseforge_find_json_array_item(PULSEFORGE_INDEX "${PULSEFORGE_SETTINGS_JSON}" configurations x64-Release)
string(JSON PULSEFORGE_SETTINGS_JSON SET "${PULSEFORGE_SETTINGS_JSON}" configurations ${PULSEFORGE_INDEX} configurationType "\"Debug\"")
pulseforge_write_settings_json("${PULSEFORGE_SETTINGS_FIXTURE_ROOT}" "${PULSEFORGE_SETTINGS_JSON}")
pulseforge_run_settings_contract(wrong-required-type "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" FALSE)

file(READ "${PULSEFORGE_LEGACY_SETTINGS_FILE}" PULSEFORGE_SETTINGS_JSON)
pulseforge_append_json_array_item(PULSEFORGE_SETTINGS_JSON "${PULSEFORGE_SETTINGS_JSON}" configurations
	[=[{"name":"x64-Debug","generator":"Ninja","configurationType":"Debug","buildRoot":"${projectDir}/Duplicate","installRoot":"${projectDir}/DuplicateInstall"}]=])
pulseforge_write_settings_json("${PULSEFORGE_SETTINGS_FIXTURE_ROOT}" "${PULSEFORGE_SETTINGS_JSON}")
pulseforge_run_settings_contract(duplicate-identity "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" FALSE)

file(WRITE "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" "{\"configurations\":[")
pulseforge_run_settings_contract(malformed-json "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" FALSE)

file(READ "${PULSEFORGE_LEGACY_SETTINGS_FILE}" PULSEFORGE_SETTINGS_JSON)
pulseforge_find_json_array_item(PULSEFORGE_INDEX "${PULSEFORGE_SETTINGS_JSON}" configurations x64-Debug)
string(JSON PULSEFORGE_SETTINGS_JSON SET "${PULSEFORGE_SETTINGS_JSON}" configurations ${PULSEFORGE_INDEX} buildRoot
	"\"\${projectDir}/Out/build/\${name}\"")
pulseforge_write_settings_json("${PULSEFORGE_SETTINGS_FIXTURE_ROOT}" "${PULSEFORGE_SETTINGS_JSON}")
pulseforge_run_settings_contract(wrong-shipped-default-path "${PULSEFORGE_SETTINGS_FIXTURE_ROOT}/CMakeSettings.json" FALSE)

message(STATUS "Legacy shipped defaults and custom configuration fixtures passed.")
