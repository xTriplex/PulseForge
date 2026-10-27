if(NOT DEFINED PULSEFORGE_COOK_EXECUTABLE OR NOT DEFINED PULSEFORGE_TEST_ROOT)
	message(FATAL_ERROR "Cook configuration CLI fixture requires executable and test-root paths")
endif()

set(ProjectFile "${PULSEFORGE_TEST_ROOT}/cook-configuration-missing-project.pfproj")
foreach(Configuration IN ITEMS Debug Release Dist)
	execute_process(
		COMMAND "${PULSEFORGE_COOK_EXECUTABLE}" --project "${ProjectFile}" --output "${PULSEFORGE_TEST_ROOT}/unused-${Configuration}"
			--target Windows --configuration "${Configuration}"
		RESULT_VARIABLE Result
		OUTPUT_VARIABLE Output
		ERROR_VARIABLE Error)
	if(NOT Result EQUAL 3 OR "${Output}${Error}" MATCHES "Cook configuration must be")
		message(FATAL_ERROR "Cook CLI did not accept supported configuration ${Configuration}: ${Output}${Error}")
	endif()
endforeach()

execute_process(
	COMMAND "${PULSEFORGE_COOK_EXECUTABLE}" --project "${ProjectFile}" --output "${PULSEFORGE_TEST_ROOT}/unused-invalid"
		--target Windows --configuration Profile
	RESULT_VARIABLE InvalidResult
	OUTPUT_VARIABLE InvalidOutput
	ERROR_VARIABLE InvalidError)
if(NOT InvalidResult EQUAL 2 OR NOT "${InvalidOutput}${InvalidError}" MATCHES "Cook configuration must be Debug, Release, or Dist")
	message(FATAL_ERROR "Cook CLI did not reject an unsupported configuration precisely: ${InvalidOutput}${InvalidError}")
endif()
