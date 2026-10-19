if(NOT DEFINED PULSEFORGE_RENDERER_EXECUTABLE OR NOT EXISTS "${PULSEFORGE_RENDERER_EXECUTABLE}")
	message(FATAL_ERROR "The PulseForge macOS renderer executable was not provided")
endif()
if(NOT DEFINED PULSEFORGE_POLICY OR NOT PULSEFORGE_POLICY MATCHES "^(auto|require-fences)$")
	message(FATAL_ERROR "Unsupported PulseForge renderer policy: '${PULSEFORGE_POLICY}'")
endif()
if(NOT DEFINED PULSEFORGE_TIMEOUT_SECONDS)
	set(PULSEFORGE_TIMEOUT_SECONDS 180)
endif()
if(NOT DEFINED PULSEFORGE_SCENARIO)
	set(PULSEFORGE_SCENARIO normal)
endif()
set(ShadowSuppression 0)
if(PULSEFORGE_SCENARIO STREQUAL "suppress-shadow-casters")
	set(ShadowSuppression 1)
endif()

set(ENV{PULSEFORGE_VULKAN_PRESENTATION_POLICY} "${PULSEFORGE_POLICY}")
set(ENV{PULSEFORGE_MACOS_RENDERER_TEST_MODE} "${PULSEFORGE_SCENARIO}")
set(ENV{PULSEFORGE_TEST_SUPPRESS_SHADOW_CASTER_DRAWS} "${ShadowSuppression}")
execute_process(
	COMMAND "${PULSEFORGE_RENDERER_EXECUTABLE}" ${PULSEFORGE_RENDERER_ARGUMENTS}
	TIMEOUT "${PULSEFORGE_TIMEOUT_SECONDS}"
	RESULT_VARIABLE RendererResult
	OUTPUT_VARIABLE RendererStdout
	ERROR_VARIABLE RendererStderr)
set(RendererLog "${RendererStdout}\n${RendererStderr}")
message("${RendererLog}")

if(NOT RendererResult MATCHES "^[0-9]+$")
	if(RendererResult MATCHES "[Tt]imeout|[Tt]imed out")
		message("MACOS_RENDERER_POLICY_DRIVER=FAIL_TIMEOUT")
		message(FATAL_ERROR "PulseForge ${PULSEFORGE_POLICY} renderer process timed out: ${RendererResult}")
	else()
		message("MACOS_RENDERER_POLICY_DRIVER=FAIL_LAUNCH")
		message(FATAL_ERROR "PulseForge ${PULSEFORGE_POLICY} renderer process could not be launched: ${RendererResult}")
	endif()
endif()
if(RendererLog MATCHES "VUID-" OR RendererLog MATCHES "MACOS_RENDERER_VALIDATION_DIAGNOSTICS=FAIL")
	message("MACOS_RENDERER_POLICY_DRIVER=FAIL_VALIDATION_DIAGNOSTICS")
	message(FATAL_ERROR "PulseForge ${PULSEFORGE_POLICY} renderer reported a Vulkan/NVRHI validation or shutdown error")
endif()

if(PULSEFORGE_POLICY STREQUAL "require-fences" AND RendererResult EQUAL 77 AND
	RendererLog MATCHES "MACOS_RENDERER_STRICT_POLICY_REJECTED=CAPABILITY; No otherwise suitable Vulkan GPU satisfies PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences")
	message("MACOS_RENDERER_STRICT_POLICY=CAPABILITY_REJECTED; rendering did not run because the exact required fence capability was unavailable")
	return()
endif()
if(NOT RendererResult EQUAL 0)
	if(PULSEFORGE_SCENARIO STREQUAL "suppress-geometry" AND RendererResult EQUAL 1 AND
		RendererLog MATCHES "MACOS_RENDERER_TEST_FAULT=suppress-geometry" AND
		RendererLog MATCHES "MACOS_RENDERER_VALIDATION=FAIL; PBR HDR render did not produce required RGBA16F color, D32 depth, geometry, and environment output" AND
		RendererLog MATCHES "MACOS_RENDERER_PROCESS_SHUTDOWN=PASS")
		message("MACOS_RENDERER_SENSITIVITY=suppress-geometry; EXPECTED_ASSERTION_FAILURE=PASS")
		return()
	elseif(PULSEFORGE_SCENARIO STREQUAL "suppress-shadow-casters" AND RendererResult EQUAL 1 AND
		RendererLog MATCHES "MACOS_RENDERER_TEST_FAULT=suppress-shadow-casters; render targets and clears remain active" AND
		RendererLog MATCHES "MACOS_RENDERER_VALIDATION=FAIL; Rendered shadow depth contained too few written caster pixels" AND
		RendererLog MATCHES "MACOS_RENDERER_PROCESS_SHUTDOWN=PASS")
		message("MACOS_RENDERER_SENSITIVITY=suppress-shadow-casters; EXPECTED_ASSERTION_FAILURE=PASS")
		return()
	endif()
	message(FATAL_ERROR "PulseForge ${PULSEFORGE_POLICY} renderer exited with status ${RendererResult}")
endif()

if(NOT PULSEFORGE_SCENARIO STREQUAL "normal")
	message(FATAL_ERROR "Fault-injection scenario '${PULSEFORGE_SCENARIO}' unexpectedly passed the renderer proof")
endif()

foreach(RequiredMarker IN ITEMS
	MACOS_RENDERER_INITIAL_OUTPUT=PASS
	MACOS_RENDERER_READBACK=PASS
	MACOS_RENDERER_IMGUI_SUBMISSION=PASS
	MACOS_RENDERER_NATIVE_RESIZE=PASS
	MACOS_RENDERER_EXPOSURE_DIFF=PASS
	MACOS_RENDERER_ORDERLY_SHUTDOWN=PASS
	MACOS_RENDERER_PROCESS_SHUTDOWN=PASS
	"Vulkan validation layer enabled"
	"NVRHI validation layer enabled"
	"Selected Vulkan GPU:"
	"Vulkan presentation retirement:")
	string(FIND "${RendererLog}" "${RequiredMarker}" MarkerPosition)
	if(MarkerPosition EQUAL -1)
		message(FATAL_ERROR "PulseForge ${PULSEFORGE_POLICY} renderer omitted required evidence: ${RequiredMarker}")
	endif()
endforeach()
if(PULSEFORGE_POLICY STREQUAL "require-fences" AND
	NOT RendererLog MATCHES "Vulkan presentation retirement: [^\n]*maintenance")
	message(FATAL_ERROR "Strict renderer exited successfully without selecting maintenance presentation fences")
endif()
message("MACOS_RENDERER_POLICY=${PULSEFORGE_POLICY}; RESULT=RENDERED_AND_VALIDATED")
