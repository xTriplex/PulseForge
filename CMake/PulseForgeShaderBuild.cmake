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

	function(pulseforge_add_spirv_shader Output Source Profile Entry)
		cmake_parse_arguments(PARSE_ARGV 4 Shader "" "COMMENT" "OPTIONS")
		if(NOT Output OR NOT Source OR NOT Profile OR NOT Entry OR NOT Shader_COMMENT)
			message(FATAL_ERROR "pulseforge_add_spirv_shader requires output, source, profile, entry, and COMMENT")
		endif()
		if(Shader_UNPARSED_ARGUMENTS)
			message(FATAL_ERROR "Unexpected arguments to pulseforge_add_spirv_shader: ${Shader_UNPARSED_ARGUMENTS}")
		endif()

		get_filename_component(ShaderOutputDirectory "${Output}" DIRECTORY)
		add_custom_command(
			OUTPUT "${Output}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${ShaderOutputDirectory}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T "${Profile}" -E "${Entry}" -fspv-target-env=vulkan1.3
				${Shader_OPTIONS} -O3 -WX -Fo "${Output}" "${Source}"
			DEPENDS "${Source}"
			VERBATIM
			COMMENT "${Shader_COMMENT}"
		)
	endfunction()

	function(pulseforge_attach_shader_deployment Target OutputDirectory)
		if(NOT TARGET "${Target}")
			message(FATAL_ERROR "Cannot attach shader deployment to missing target '${Target}'")
		endif()
		set(ShaderDeploymentCommands COMMAND "${CMAKE_COMMAND}" -E make_directory "${OutputDirectory}")
		foreach(ShaderOutput IN LISTS ARGN)
			get_filename_component(ShaderFileName "${ShaderOutput}" NAME)
			list(APPEND ShaderDeploymentCommands COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${ShaderOutput}" "${OutputDirectory}/${ShaderFileName}")
		endforeach()
		add_custom_command(TARGET "${Target}" POST_BUILD ${ShaderDeploymentCommands} VERBATIM)
	endfunction()

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
	pulseforge_add_spirv_shader("${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}" vs_6_0 VSMain
		COMMENT "Compiling PulseForge triangle vertex shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
	pulseforge_add_spirv_shader("${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}" ps_6_0 PSMain
		COMMENT "Compiling PulseForge triangle fragment shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0
			-fvk-b-shift 256 1 -fvk-t-shift 0 1 -fvk-s-shift 128 1
			-fvk-b-shift 256 2 -fvk-t-shift 0 2 -fvk-s-shift 128 2
			-fvk-t-shift 0 3 -fvk-s-shift 128 3 -fvk-t-shift 0 5 -fvk-s-shift 128 5)
	pulseforge_add_spirv_shader("${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "${PULSEFORGE_BACKGROUND_HLSL}" vs_6_0 VSMain
		COMMENT "Compiling PulseForge environment background vertex shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
	pulseforge_add_spirv_shader("${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "${PULSEFORGE_BACKGROUND_HLSL}" ps_6_0 PSMain
		COMMENT "Compiling PulseForge environment background fragment shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
	pulseforge_add_spirv_shader("${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_SHADOW_HLSL}" vs_6_0 VSMain
		COMMENT "Compiling PulseForge directional-shadow vertex shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 4)
	pulseforge_add_spirv_shader("${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "${PULSEFORGE_SHADOW_HLSL}" ps_6_0 PSMain
		COMMENT "Compiling PulseForge alpha-tested directional/spot shadow fragment shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
	pulseforge_add_spirv_shader("${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_POINT_SHADOW_HLSL}" vs_6_0 VSMain
		COMMENT "Compiling PulseForge point-shadow vertex shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 4)
	pulseforge_add_spirv_shader("${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "${PULSEFORGE_POINT_SHADOW_HLSL}" ps_6_0 PSMain
		COMMENT "Compiling PulseForge point-shadow fragment shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 4)
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
		pulseforge_add_spirv_shader("${PULSEFORGE_SSAO_OUTPUT}" "${PULSEFORGE_SSAO_SOURCE}" "${PULSEFORGE_SSAO_PROFILE}"
			"${PULSEFORGE_SSAO_ENTRY}" COMMENT "Compiling PulseForge ${PULSEFORGE_SSAO_LABEL} shader to SPIR-V"
			OPTIONS ${PULSEFORGE_SSAO_REGISTER_SHIFTS})
	endforeach()
	pulseforge_add_spirv_shader("${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "${PULSEFORGE_TONEMAPPING_HLSL}" vs_6_0 VSMain
		COMMENT "Compiling PulseForge tone-mapping vertex shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
	pulseforge_add_spirv_shader("${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "${PULSEFORGE_TONEMAPPING_HLSL}" ps_6_0 PSMain
		COMMENT "Compiling PulseForge tone-mapping fragment shader to SPIR-V"
		OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
	set(PULSEFORGE_SCENE_SHADER_OUTPUTS
		"${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}"
		"${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}"
		"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}"
		"${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}"
		"${PULSEFORGE_SHADOW_VERTEX_SPIRV}"
		"${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}"
		"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}"
		"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}"
		"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}"
		"${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}"
		"${PULSEFORGE_SSAO_VERTEX_SPIRV}"
		"${PULSEFORGE_SSAO_FRAGMENT_SPIRV}"
		"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}"
		"${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}"
		"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}"
		"${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}")
	add_custom_target(PulseForgeSceneShaderAssets DEPENDS ${PULSEFORGE_SCENE_SHADER_OUTPUTS})
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
		set(PULSEFORGE_GPU_VALIDATION_SHADER_OUTPUTS
			"${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}"
			"${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}"
			"${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}"
			"${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}"
			"${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}"
			"${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}"
			"${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}")
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}" vs_6_0 VSMain
			COMMENT "Compiling PulseForge GPU-validation vertex shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}" ps_6_0 PSColor
			COMMENT "Compiling PulseForge GPU-validation color shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			vs_6_0 VSMain COMMENT "Compiling production editor ImGui vertex shader for GPU validation"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			ps_6_0 PSMain COMMENT "Compiling production editor ImGui fragment shader for GPU validation"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			vs_6_0 VSMain COMMENT "Compiling editor UI presentation vertex shader for GPU validation"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			ps_6_0 PSMain COMMENT "Compiling editor UI presentation fragment shader for GPU validation"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}" ps_6_0 PSSample
			COMMENT "Compiling PulseForge GPU-validation sampling shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		add_custom_target(PulseForgeGpuValidationShaders
			DEPENDS ${PULSEFORGE_GPU_VALIDATION_SHADER_OUTPUTS})
	endif()
	if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
		add_dependencies(PulseForgeGame PulseForgeSceneShaderAssets)
		pulseforge_attach_shader_deployment(PulseForgeGame "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders"
			${PULSEFORGE_SCENE_SHADER_OUTPUTS})
		add_custom_command(TARGET PulseForgeGame POST_BUILD
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
		set(PULSEFORGE_EDITOR_SHADER_OUTPUTS
			"${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}"
			"${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}"
			"${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}"
			"${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}")
		pulseforge_add_spirv_shader("${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_HLSL}" vs_6_0 VSMain
			COMMENT "Compiling PulseForge editor ImGui vertex shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_HLSL}" ps_6_0 PSMain
			COMMENT "Compiling PulseForge editor ImGui fragment shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}" vs_6_0 VSMain
			COMMENT "Compiling PulseForge editor ImGui UNORM presentation vertex shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		pulseforge_add_spirv_shader("${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}" ps_6_0 PSMain
			COMMENT "Compiling PulseForge editor ImGui UNORM presentation fragment shader to SPIR-V"
			OPTIONS -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		add_custom_target(PulseForgeEditorShaderAssets
			DEPENDS ${PULSEFORGE_EDITOR_SHADER_OUTPUTS})
		add_dependencies(PulseForgeEditor PulseForgeSceneShaderAssets PulseForgeEditorShaderAssets)
		pulseforge_attach_shader_deployment(PulseForgeEditor "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders"
			${PULSEFORGE_SCENE_SHADER_OUTPUTS} ${PULSEFORGE_EDITOR_SHADER_OUTPUTS})
	endif()
endif()
