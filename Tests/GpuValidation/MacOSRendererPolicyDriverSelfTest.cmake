if(NOT DEFINED PULSEFORGE_DRIVER_SCRIPT OR NOT EXISTS "${PULSEFORGE_DRIVER_SCRIPT}")
	message(FATAL_ERROR "The macOS renderer policy driver was not provided")
endif()
if(NOT DEFINED PULSEFORGE_TEST_DIRECTORY)
	message(FATAL_ERROR "The macOS renderer policy driver test directory was not provided")
endif()
if(NOT EXISTS "/bin/sh")
	message(FATAL_ERROR "The macOS renderer policy driver timeout test requires the hosted POSIX shell")
endif()

set(FakeRenderer "${PULSEFORGE_TEST_DIRECTORY}/MacOSRendererPolicyDriverFake.cmake")
set(FakeTimeout "${PULSEFORGE_TEST_DIRECTORY}/MacOSRendererPolicyDriverTimeout.sh")
set(FakeRejection "${PULSEFORGE_TEST_DIRECTORY}/MacOSRendererPolicyDriverRejection.sh")
file(WRITE "${FakeRenderer}" [=[
message("MACOS_RENDERER_STRICT_POLICY_REJECTED=CAPABILITY; No otherwise suitable Vulkan GPU satisfies PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences")
message("MACOS_RENDERER_VALIDATION_DIAGNOSTICS=FAIL; synthetic NVRHI validation error")
message(FATAL_ERROR "representative renderer validation failure")
]=])
file(WRITE "${FakeTimeout}" [=[
#!/bin/sh
echo "MACOS_RENDERER_STRICT_POLICY_REJECTED=CAPABILITY; No otherwise suitable Vulkan GPU satisfies PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences"
exec sleep 10
]=])
file(CHMOD "${FakeTimeout}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)
file(WRITE "${FakeRejection}" [=[
#!/bin/sh
echo "MACOS_RENDERER_STRICT_POLICY_REJECTED=CAPABILITY; No otherwise suitable Vulkan GPU satisfies PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences"
exit 77
]=])
file(CHMOD "${FakeRejection}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE GROUP_READ GROUP_EXECUTE WORLD_READ WORLD_EXECUTE)

execute_process(
	COMMAND "${CMAKE_COMMAND}"
		"-DPULSEFORGE_RENDERER_EXECUTABLE=${CMAKE_COMMAND}"
		"-DPULSEFORGE_RENDERER_ARGUMENTS=-DPULSEFORGE_FAKE_MODE=validation-error;-P;${FakeRenderer}"
		-DPULSEFORGE_POLICY=require-fences
		-DPULSEFORGE_SCENARIO=normal
		-DPULSEFORGE_TIMEOUT_SECONDS=5
		-P "${PULSEFORGE_DRIVER_SCRIPT}"
	TIMEOUT 10
	RESULT_VARIABLE ValidationDriverResult
	OUTPUT_VARIABLE ValidationDriverOutput
	ERROR_VARIABLE ValidationDriverError)
if(NOT ValidationDriverResult MATCHES "^[0-9]+$" OR ValidationDriverResult EQUAL 0)
	message(FATAL_ERROR "The policy driver accepted its synthetic validation-error case:\n${ValidationDriverOutput}\n${ValidationDriverError}")
endif()
string(FIND "${ValidationDriverOutput}\n${ValidationDriverError}" "MACOS_RENDERER_POLICY_DRIVER=FAIL_VALIDATION_DIAGNOSTICS" ValidationFailure)
if(ValidationFailure EQUAL -1)
	message(FATAL_ERROR "The policy driver rejected its validation-error case for an unexpected reason")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}"
		-DPULSEFORGE_RENDERER_EXECUTABLE=/bin/sh
		"-DPULSEFORGE_RENDERER_ARGUMENTS=${FakeTimeout}"
		-DPULSEFORGE_POLICY=require-fences
		-DPULSEFORGE_SCENARIO=normal
		-DPULSEFORGE_TIMEOUT_SECONDS=0.2
		-P "${PULSEFORGE_DRIVER_SCRIPT}"
	TIMEOUT 10
	RESULT_VARIABLE TimeoutDriverResult
	OUTPUT_VARIABLE TimeoutDriverOutput
	ERROR_VARIABLE TimeoutDriverError)
if(NOT TimeoutDriverResult MATCHES "^[0-9]+$" OR TimeoutDriverResult EQUAL 0)
	message(FATAL_ERROR "The policy driver accepted its timeout case:\n${TimeoutDriverOutput}\n${TimeoutDriverError}")
endif()
string(FIND "${TimeoutDriverOutput}\n${TimeoutDriverError}" "MACOS_RENDERER_POLICY_DRIVER=FAIL_TIMEOUT" TimeoutFailure)
if(TimeoutFailure EQUAL -1)
	message(FATAL_ERROR "The policy driver rejected its timeout case for an unexpected reason")
endif()

execute_process(
	COMMAND "${CMAKE_COMMAND}"
		-DPULSEFORGE_RENDERER_EXECUTABLE=/bin/sh
		"-DPULSEFORGE_RENDERER_ARGUMENTS=${FakeRejection}"
		-DPULSEFORGE_POLICY=require-fences
		-DPULSEFORGE_SCENARIO=normal
		-DPULSEFORGE_TIMEOUT_SECONDS=5
		-P "${PULSEFORGE_DRIVER_SCRIPT}"
	TIMEOUT 10
	RESULT_VARIABLE RejectionDriverResult
	OUTPUT_VARIABLE RejectionDriverOutput
	ERROR_VARIABLE RejectionDriverError)
if(NOT RejectionDriverResult MATCHES "^[0-9]+$" OR NOT RejectionDriverResult EQUAL 0)
	message(FATAL_ERROR "The policy driver did not accept the exact strict capability rejection:\n${RejectionDriverOutput}\n${RejectionDriverError}")
endif()
string(FIND "${RejectionDriverOutput}\n${RejectionDriverError}" "MACOS_RENDERER_STRICT_POLICY=CAPABILITY_REJECTED" RejectionAccepted)
if(RejectionAccepted EQUAL -1)
	message(FATAL_ERROR "The policy driver did not label the strict result as a capability rejection")
endif()

file(REMOVE "${FakeRenderer}" "${FakeTimeout}" "${FakeRejection}")
message("MACOS_RENDERER_POLICY_DRIVER_SELF_TEST=PASS; validation errors and timeouts override capability rejection, exact strict rejection is accepted")
