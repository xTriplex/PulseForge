if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" OR
	(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
	# Pin the compiler executable and its archive hash instead of relying on a
	# developer-installed DXC or whatever happens to be on PATH. DXC is a
	# build-time tool only; runtime builds consume the generated SPIR-V files.
	set(PULSEFORGE_DXC_VERSION "1.9.2602.24")
	set(PULSEFORGE_DXC_RELEASE_DATE "2026_05_27")
	set(PULSEFORGE_DXC_URL
		"https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${PULSEFORGE_DXC_VERSION}/dxc_${PULSEFORGE_DXC_RELEASE_DATE}.zip")
	set(PULSEFORGE_DXC_SHA512
		"cf331112f1753fca68525c85bb4964714d85dbf7cb5e41af4720bbdffefdfddad1878c7a1a74898e2105dff32957a526d188d9ee3f18f0df310ec8548827a374")
	set(PULSEFORGE_DXC_ROOT "${CMAKE_BINARY_DIR}/_tools/dxc-${PULSEFORGE_DXC_VERSION}")
	set(PULSEFORGE_DXC "${PULSEFORGE_DXC_ROOT}/bin/x64/dxc.exe")
	if(NOT EXISTS "${PULSEFORGE_DXC}")
		set(PULSEFORGE_DXC_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_downloads")
		set(PULSEFORGE_DXC_ARCHIVE "${PULSEFORGE_DXC_DOWNLOAD_DIR}/dxc-${PULSEFORGE_DXC_VERSION}.zip")
		file(MAKE_DIRECTORY "${PULSEFORGE_DXC_DOWNLOAD_DIR}" "${PULSEFORGE_DXC_ROOT}")
		message(STATUS "Downloading pinned Microsoft DXC ${PULSEFORGE_DXC_VERSION} for HLSL-to-SPIR-V compilation")
		file(DOWNLOAD
			"${PULSEFORGE_DXC_URL}"
			"${PULSEFORGE_DXC_ARCHIVE}"
			EXPECTED_HASH "SHA512=${PULSEFORGE_DXC_SHA512}"
			TLS_VERIFY ON
			SHOW_PROGRESS
			STATUS PULSEFORGE_DXC_DOWNLOAD_STATUS
		)
		list(GET PULSEFORGE_DXC_DOWNLOAD_STATUS 0 PULSEFORGE_DXC_DOWNLOAD_CODE)
		if(NOT PULSEFORGE_DXC_DOWNLOAD_CODE EQUAL 0)
			list(GET PULSEFORGE_DXC_DOWNLOAD_STATUS 1 PULSEFORGE_DXC_DOWNLOAD_MESSAGE)
			message(FATAL_ERROR "Could not download pinned DXC ${PULSEFORGE_DXC_VERSION}: ${PULSEFORGE_DXC_DOWNLOAD_MESSAGE}")
		endif()
		file(ARCHIVE_EXTRACT INPUT "${PULSEFORGE_DXC_ARCHIVE}" DESTINATION "${PULSEFORGE_DXC_ROOT}")
	endif()
	if(NOT EXISTS "${PULSEFORGE_DXC}")
		message(FATAL_ERROR "Pinned DXC archive did not contain the expected Windows x64 compiler: ${PULSEFORGE_DXC}")
	endif()

	set(PULSEFORGE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Runtime/Shaders")
	set(PULSEFORGE_TRIANGLE_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Triangle.vs.spv")
	set(PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Triangle.ps.spv")
	set(PULSEFORGE_TRIANGLE_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/Triangle.hlsl")
	set(PULSEFORGE_BACKGROUND_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/EnvironmentBackground.vs.spv")
	set(PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/EnvironmentBackground.ps.spv")
	set(PULSEFORGE_BACKGROUND_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/EnvironmentBackground.hlsl")
	set(PULSEFORGE_SHADOW_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/DirectionalShadow.vs.spv")
	set(PULSEFORGE_SHADOW_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/DirectionalShadow.ps.spv")
	set(PULSEFORGE_SHADOW_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/DirectionalShadow.hlsl")
	set(PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/PointShadow.vs.spv")
	set(PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/PointShadow.ps.spv")
	set(PULSEFORGE_POINT_SHADOW_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/PointShadow.hlsl")
	set(PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoNormalDepth.vs.spv")
	set(PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoNormalDepth.ps.spv")
	set(PULSEFORGE_SSAO_PREPASS_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/SsaoNormalDepth.hlsl")
	set(PULSEFORGE_SSAO_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Ssao.vs.spv")
	set(PULSEFORGE_SSAO_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Ssao.ps.spv")
	set(PULSEFORGE_SSAO_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/Ssao.hlsl")
	set(PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoBlur.vs.spv")
	set(PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoBlur.ps.spv")
	set(PULSEFORGE_SSAO_BLUR_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/SsaoBlur.hlsl")
	set(PULSEFORGE_TONEMAPPING_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Tonemapping.vs.spv")
	set(PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Tonemapping.ps.spv")
	set(PULSEFORGE_TONEMAPPING_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/Tonemapping.hlsl")
	add_custom_command(
		OUTPUT "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}"
		DEPENDS "${PULSEFORGE_TRIANGLE_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge triangle vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 1 -fvk-t-shift 0 1 -fvk-s-shift 128 1 -fvk-b-shift 256 2 -fvk-t-shift 0 2 -fvk-s-shift 128 2 -fvk-t-shift 0 3 -fvk-s-shift 128 3 -fvk-t-shift 0 5 -fvk-s-shift 128 5 -O3 -WX
			-Fo "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}"
		DEPENDS "${PULSEFORGE_TRIANGLE_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge triangle fragment shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "${PULSEFORGE_BACKGROUND_HLSL}"
		DEPENDS "${PULSEFORGE_BACKGROUND_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge environment background vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "${PULSEFORGE_BACKGROUND_HLSL}"
		DEPENDS "${PULSEFORGE_BACKGROUND_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge environment background fragment shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_SHADOW_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 4 -O3 -WX
			-Fo "${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge directional-shadow vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "${PULSEFORGE_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge alpha-tested directional/spot shadow fragment shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 4 -O3 -WX
			-Fo "${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_POINT_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_POINT_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge point-shadow vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 4 -O3 -WX
			-Fo "${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "${PULSEFORGE_POINT_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_POINT_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge point-shadow fragment shader to SPIR-V"
	)
	foreach(PULSEFORGE_SSAO_SHADER IN ITEMS
		"${PULSEFORGE_SSAO_PREPASS_HLSL}|${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}|vs_6_0|VSMain|normal-depth vertex"
		"${PULSEFORGE_SSAO_PREPASS_HLSL}|${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}|ps_6_0|PSMain|normal-depth fragment"
		"${PULSEFORGE_SSAO_HLSL}|${PULSEFORGE_SSAO_VERTEX_SPIRV}|vs_6_0|VSMain|SSAO vertex"
		"${PULSEFORGE_SSAO_HLSL}|${PULSEFORGE_SSAO_FRAGMENT_SPIRV}|ps_6_0|PSMain|SSAO fragment"
		"${PULSEFORGE_SSAO_BLUR_HLSL}|${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}|vs_6_0|VSMain|SSAO blur vertex"
		"${PULSEFORGE_SSAO_BLUR_HLSL}|${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}|ps_6_0|PSMain|SSAO blur fragment")
		string(REPLACE "|" ";" PULSEFORGE_SSAO_FIELDS "${PULSEFORGE_SSAO_SHADER}")
		list(GET PULSEFORGE_SSAO_FIELDS 0 PULSEFORGE_SSAO_SOURCE)
		list(GET PULSEFORGE_SSAO_FIELDS 1 PULSEFORGE_SSAO_OUTPUT)
		list(GET PULSEFORGE_SSAO_FIELDS 2 PULSEFORGE_SSAO_PROFILE)
		list(GET PULSEFORGE_SSAO_FIELDS 3 PULSEFORGE_SSAO_ENTRY)
		list(GET PULSEFORGE_SSAO_FIELDS 4 PULSEFORGE_SSAO_LABEL)
		set(PULSEFORGE_SSAO_REGISTER_SHIFTS
			-fvk-b-shift 256 4 -fvk-t-shift 0 4 -fvk-s-shift 128 4)
		if("${PULSEFORGE_SSAO_OUTPUT}" STREQUAL "${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}")
			list(APPEND PULSEFORGE_SSAO_REGISTER_SHIFTS
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		endif()
		add_custom_command(
			OUTPUT "${PULSEFORGE_SSAO_OUTPUT}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T "${PULSEFORGE_SSAO_PROFILE}" -E "${PULSEFORGE_SSAO_ENTRY}"
				-fspv-target-env=vulkan1.3 ${PULSEFORGE_SSAO_REGISTER_SHIFTS} -O3 -WX
				-Fo "${PULSEFORGE_SSAO_OUTPUT}" "${PULSEFORGE_SSAO_SOURCE}"
			DEPENDS "${PULSEFORGE_SSAO_SOURCE}"
			VERBATIM
			COMMENT "Compiling PulseForge ${PULSEFORGE_SSAO_LABEL} shader to SPIR-V"
		)
	endforeach()
	add_custom_command(
		OUTPUT "${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "${PULSEFORGE_TONEMAPPING_HLSL}"
		DEPENDS "${PULSEFORGE_TONEMAPPING_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge tone-mapping vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "${PULSEFORGE_TONEMAPPING_HLSL}"
		DEPENDS "${PULSEFORGE_TONEMAPPING_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge tone-mapping fragment shader to SPIR-V"
	)
	add_custom_target(PulseForgeSceneShaderAssets
		DEPENDS "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}"
			"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}"
			"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}"
			"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}" "${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SSAO_VERTEX_SPIRV}" "${PULSEFORGE_SSAO_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}" "${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}"
			"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}")
	if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS)
		set(PULSEFORGE_GPU_VALIDATION_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/GpuValidationShaders")
		set(PULSEFORGE_GPU_VALIDATION_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Tests/GpuRendererValidation.hlsl")
		set(PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/GpuRendererValidation.vs.spv")
		set(PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/GpuRendererValidation.color.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/GpuRendererValidation.sample.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGui.vs.spv")
		set(PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGui.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGui.hlsl")
		set(PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGuiPresent.vs.spv")
		set(PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGuiPresent.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGuiPresent.hlsl")
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge GPU-validation vertex shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSColor -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge GPU-validation color shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling production editor ImGui vertex shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling production editor ImGui fragment shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling editor UI presentation vertex shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling editor UI presentation fragment shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSSample -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge GPU-validation sampling shader to SPIR-V"
		)
		add_custom_target(PulseForgeGpuValidationShaders
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}")
	endif()
	if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
		add_dependencies(PulseForgeGame PulseForgeSceneShaderAssets)
		add_custom_command(TARGET PulseForgeGame POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Triangle.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Triangle.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/EnvironmentBackground.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/EnvironmentBackground.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/DirectionalShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/DirectionalShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/PointShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/PointShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoNormalDepth.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoNormalDepth.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Ssao.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Ssao.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoBlur.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoBlur.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Tonemapping.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Tonemapping.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Models"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Models" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Models"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Textures"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Textures" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Textures"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Environments"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Environments" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Environments"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scenes"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scenes" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scenes"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Materials"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Materials" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Materials"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scripts"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scripts" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scripts"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/PulseForgeGame.pfproj" "$<TARGET_FILE_DIR:PulseForgeGame>/PulseForgeGame.pfproj"
			VERBATIM
		)
		set_property(TARGET PulseForgeGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeGame>")
	endif()

	if(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan")
		set(PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Editor/Shaders")
		set(PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGui.vs.spv")
		set(PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGui.ps.spv")
		set(PULSEFORGE_EDITOR_IMGUI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGui.hlsl")
		set(PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGuiPresent.vs.spv")
		set(PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGuiPresent.ps.spv")
		set(PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGuiPresent.hlsl")
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui vertex shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui fragment shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui UNORM presentation vertex shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui UNORM presentation fragment shader to SPIR-V"
		)
		add_custom_target(PulseForgeEditorShaderAssets
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}"
				"${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}")
		add_dependencies(PulseForgeEditor PulseForgeSceneShaderAssets PulseForgeEditorShaderAssets)
		add_custom_command(TARGET PulseForgeEditor POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Triangle.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Triangle.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EnvironmentBackground.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EnvironmentBackground.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/DirectionalShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/DirectionalShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/PointShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/PointShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoNormalDepth.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoNormalDepth.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Ssao.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Ssao.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoBlur.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoBlur.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Tonemapping.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Tonemapping.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGui.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGui.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGuiPresent.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGuiPresent.ps.spv"
			VERBATIM
		)
	endif()
endif()
