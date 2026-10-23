cmake_minimum_required(VERSION 3.24)

if(NOT PULSEFORGE_SOURCE_ROOT OR NOT PULSEFORGE_CMAKE_COMMAND OR NOT PULSEFORGE_MSBUILD_EXECUTABLE OR
	NOT PULSEFORGE_GENERATOR)
	message(FATAL_ERROR "Visual Studio launch registration test inputs are incomplete.")
endif()

set(PULSEFORGE_FIXTURE_SOURCE "${PULSEFORGE_SOURCE_ROOT}/Tests/CMake/VisualStudioLaunchRegistrationFixture")
set(PULSEFORGE_FIXTURE_ROOT "${PULSEFORGE_BINARY_DIR}/CMakeLaunchRegistration")

function(pulseforge_configure_launch_fixture
	FixtureName EditorEnabled GameEnabled TestingEnabled CoreOnly ExpectedTargets ExpectRegistration)
	set(PULSEFORGE_FIXTURE_BINARY "${PULSEFORGE_FIXTURE_ROOT}/${FixtureName}")
	set(PULSEFORGE_CONFIGURE_COMMAND
		"${PULSEFORGE_CMAKE_COMMAND}"
		-S "${PULSEFORGE_FIXTURE_SOURCE}"
		-B "${PULSEFORGE_FIXTURE_BINARY}"
		-G "${PULSEFORGE_GENERATOR}"
		"-DPULSEFORGE_SOURCE_ROOT=${PULSEFORGE_SOURCE_ROOT}"
		"-DPULSEFORGE_FIXTURE_EDITOR=${EditorEnabled}"
		"-DPULSEFORGE_FIXTURE_GAME=${GameEnabled}"
		"-DPULSEFORGE_FIXTURE_CORE_ONLY=${CoreOnly}"
		"-DPULSEFORGE_FIXTURE_TESTING=${TestingEnabled}")
	if(PULSEFORGE_GENERATOR_PLATFORM)
		list(APPEND PULSEFORGE_CONFIGURE_COMMAND -A "${PULSEFORGE_GENERATOR_PLATFORM}")
	endif()
	if(PULSEFORGE_GENERATOR_TOOLSET)
		list(APPEND PULSEFORGE_CONFIGURE_COMMAND -T "${PULSEFORGE_GENERATOR_TOOLSET}")
	endif()

	execute_process(
		COMMAND ${PULSEFORGE_CONFIGURE_COMMAND}
		RESULT_VARIABLE PULSEFORGE_CONFIGURE_RESULT
		OUTPUT_VARIABLE PULSEFORGE_CONFIGURE_OUTPUT
		ERROR_VARIABLE PULSEFORGE_CONFIGURE_ERROR)
	if(NOT PULSEFORGE_CONFIGURE_RESULT EQUAL 0)
		message(FATAL_ERROR
			"Could not generate ${FixtureName} Visual Studio launch registration fixture:\n"
			"${PULSEFORGE_CONFIGURE_ERROR}${PULSEFORGE_CONFIGURE_OUTPUT}")
	endif()

	foreach(PULSEFORGE_TARGET IN ITEMS PulseForgeEditor PulseForgeGame)
		if(PULSEFORGE_TARGET IN_LIST ExpectedTargets)
			if(NOT EXISTS "${PULSEFORGE_FIXTURE_BINARY}/${PULSEFORGE_TARGET}.vcxproj")
				message(FATAL_ERROR "${FixtureName} did not generate expected ${PULSEFORGE_TARGET}.vcxproj.")
			endif()
		elseif(EXISTS "${PULSEFORGE_FIXTURE_BINARY}/${PULSEFORGE_TARGET}.vcxproj")
			message(FATAL_ERROR "${FixtureName} unexpectedly generated ${PULSEFORGE_TARGET}.vcxproj.")
		endif()
	endforeach()

	set(PULSEFORGE_TEST_FILE "${PULSEFORGE_FIXTURE_BINARY}/CTestTestfile.cmake")
	if(ExpectRegistration)
		if(NOT EXISTS "${PULSEFORGE_TEST_FILE}")
			message(FATAL_ERROR "${FixtureName} did not generate its CTest registration file.")
		endif()
		file(READ "${PULSEFORGE_TEST_FILE}" PULSEFORGE_TEST_CONTENT)
		string(FIND "${PULSEFORGE_TEST_CONTENT}" "PulseForge.CMake.VisualStudioLaunchPaths" PULSEFORGE_TEST_POSITION)
		if(PULSEFORGE_TEST_POSITION LESS 0)
			message(FATAL_ERROR "${FixtureName} did not register the Visual Studio launch-path test.")
		endif()
		string(FIND "${PULSEFORGE_TEST_CONTENT}" "-DPULSEFORGE_TARGETS=${ExpectedTargets}"
			PULSEFORGE_ARGUMENT_POSITION)
		if(PULSEFORGE_ARGUMENT_POSITION LESS 0)
			message(FATAL_ERROR "${FixtureName} did not pass its exact applicable target list to the test.")
		endif()
	else()
		if(EXISTS "${PULSEFORGE_TEST_FILE}")
			file(READ "${PULSEFORGE_TEST_FILE}" PULSEFORGE_TEST_CONTENT)
			if(PULSEFORGE_TEST_CONTENT MATCHES "PulseForge\\.CMake\\.VisualStudioLaunchPaths")
				message(FATAL_ERROR "${FixtureName} unexpectedly registered the Visual Studio launch-path test.")
			endif()
		endif()
	endif()
endfunction()

pulseforge_configure_launch_fixture(EditorAndGame ON ON ON OFF "PulseForgeEditor;PulseForgeGame" TRUE)
pulseforge_configure_launch_fixture(GameOnly OFF ON ON OFF PulseForgeGame TRUE)
pulseforge_configure_launch_fixture(CoreOnly OFF OFF ON ON "" FALSE)
pulseforge_configure_launch_fixture(TestingDisabled ON ON OFF OFF "PulseForgeEditor;PulseForgeGame" FALSE)
pulseforge_configure_launch_fixture(NoDesktopTargets OFF OFF ON OFF "" FALSE)

set(PULSEFORGE_MISSING_PROJECT_COMMAND
	"${PULSEFORGE_CMAKE_COMMAND}"
	"-DPULSEFORGE_BINARY_DIR=${PULSEFORGE_FIXTURE_ROOT}/EditorAndGame"
	"-DPULSEFORGE_MSBUILD_EXECUTABLE=${PULSEFORGE_MSBUILD_EXECUTABLE}"
	"-DPULSEFORGE_PLATFORM=${PULSEFORGE_GENERATOR_PLATFORM}"
	-DPULSEFORGE_TARGETS=PulseForgeMissing
	-P "${PULSEFORGE_SOURCE_ROOT}/Tests/CMake/TestVisualStudioLaunchPaths.cmake")
execute_process(
	COMMAND ${PULSEFORGE_MISSING_PROJECT_COMMAND}
	RESULT_VARIABLE PULSEFORGE_MISSING_PROJECT_RESULT
	OUTPUT_VARIABLE PULSEFORGE_MISSING_PROJECT_OUTPUT
	ERROR_VARIABLE PULSEFORGE_MISSING_PROJECT_ERROR)
if(PULSEFORGE_MISSING_PROJECT_RESULT EQUAL 0 OR
	NOT "${PULSEFORGE_MISSING_PROJECT_ERROR}${PULSEFORGE_MISSING_PROJECT_OUTPUT}"
	MATCHES "Generated Visual Studio project is missing")
	message(FATAL_ERROR "An explicitly expected missing project did not fail the launch-path check.")
endif()

message(STATUS "Visual Studio launch registration matches enabled executable targets and testing options.")
