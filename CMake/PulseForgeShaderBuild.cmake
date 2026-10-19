include("${CMAKE_CURRENT_LIST_DIR}/PulseForgeShaderDeployment.cmake")

if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" OR
	(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
	# DXC is a host tool: select its package from the configure host, independently
	# of the architecture being targeted by this build. An explicit executable
	# override is available for controlled toolchain setups.
	set(PULSEFORGE_DXC_VERSION "1.9.2602.24")
	set(PULSEFORGE_DXC_EXECUTABLE "" CACHE FILEPATH
		"Host DXC executable override; on macOS use the compiler supplied by the official LunarG Vulkan SDK")
	if(PULSEFORGE_DXC_EXECUTABLE)
		if(NOT EXISTS "${PULSEFORGE_DXC_EXECUTABLE}")
			message(FATAL_ERROR "PULSEFORGE_DXC_EXECUTABLE does not exist: ${PULSEFORGE_DXC_EXECUTABLE}")
		endif()
		set(PULSEFORGE_DXC "${PULSEFORGE_DXC_EXECUTABLE}")
		if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
			message(STATUS
				"Using explicit host DXC override '${PULSEFORGE_DXC}'. HLSL sources are compiled locally to SPIR-V; "
				"the macOS proof expects the DXC provided by the official LunarG macOS Vulkan SDK.")
		endif()
	else()
		if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
			string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" PULSEFORGE_DXC_HOST_PROCESSOR)
			if(NOT PULSEFORGE_DXC_HOST_PROCESSOR MATCHES "^(amd64|x86_64)$")
				message(FATAL_ERROR
					"Pinned DXC ${PULSEFORGE_DXC_VERSION} is available for Windows x64 hosts only; "
					"set PULSEFORGE_DXC_EXECUTABLE to a compatible host compiler.")
			endif()
			set(PULSEFORGE_DXC_ARCHIVE_NAME "dxc_2026_05_27.zip")
			set(PULSEFORGE_DXC_ARCHIVE_SHA512
				"cf331112f1753fca68525c85bb4964714d85dbf7cb5e41af4720bbdffefdfddad1878c7a1a74898e2105dff32957a526d188d9ee3f18f0df310ec8548827a374")
			set(PULSEFORGE_DXC_RELATIVE_EXECUTABLE "bin/x64/dxc.exe")
		elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
			string(TOLOWER "${CMAKE_HOST_SYSTEM_PROCESSOR}" PULSEFORGE_DXC_HOST_PROCESSOR)
			if(NOT PULSEFORGE_DXC_HOST_PROCESSOR MATCHES "^(amd64|x86_64)$")
				message(FATAL_ERROR
					"Pinned DXC ${PULSEFORGE_DXC_VERSION} is available for Linux x86_64 hosts only; "
					"set PULSEFORGE_DXC_EXECUTABLE to a compatible host compiler.")
			endif()
			set(PULSEFORGE_DXC_ARCHIVE_NAME "linux_dxc_2026_05_26.x86_64.tar.gz")
			set(PULSEFORGE_DXC_ARCHIVE_SHA512
				"3da18b4b32899c65881276315411092fa1a076e25bbea20606bc247c5fa93c83f948785969e007115dc72afcd433ab76e54c392de6d37c11d93d8614a829e40a")
			set(PULSEFORGE_DXC_RELATIVE_EXECUTABLE "bin/dxc")
		else()
			if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
				message(FATAL_ERROR
					"No pinned Microsoft DXC archive is configured for macOS. Set PULSEFORGE_DXC_EXECUTABLE to the "
					"host compiler from the official LunarG macOS Vulkan SDK (for example, $VULKAN_SDK/bin/dxc). "
					"PulseForge compiles checked-in HLSL sources to SPIR-V during this build; it does not use checked-in SPIR-V.")
			endif()
			message(FATAL_ERROR
				"Pinned DXC ${PULSEFORGE_DXC_VERSION} has no package for host '${CMAKE_HOST_SYSTEM_NAME}'; "
				"set PULSEFORGE_DXC_EXECUTABLE to a compatible host compiler.")
		endif()
		set(PULSEFORGE_DXC_ROOT "${CMAKE_BINARY_DIR}/_tools/dxc-${PULSEFORGE_DXC_VERSION}")
		set(PULSEFORGE_DXC "${PULSEFORGE_DXC_ROOT}/${PULSEFORGE_DXC_RELATIVE_EXECUTABLE}")
		if(NOT EXISTS "${PULSEFORGE_DXC}")
			set(PULSEFORGE_DXC_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_downloads")
			set(PULSEFORGE_DXC_ARCHIVE "${PULSEFORGE_DXC_DOWNLOAD_DIR}/${PULSEFORGE_DXC_ARCHIVE_NAME}")
			set(PULSEFORGE_DXC_URL
				"https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${PULSEFORGE_DXC_VERSION}/${PULSEFORGE_DXC_ARCHIVE_NAME}")
			file(MAKE_DIRECTORY "${PULSEFORGE_DXC_DOWNLOAD_DIR}" "${PULSEFORGE_DXC_ROOT}")
			message(STATUS
				"Downloading pinned Microsoft DXC ${PULSEFORGE_DXC_VERSION} for ${CMAKE_HOST_SYSTEM_NAME} "
				"${CMAKE_HOST_SYSTEM_PROCESSOR} host shader compilation")
			file(DOWNLOAD
				"${PULSEFORGE_DXC_URL}"
				"${PULSEFORGE_DXC_ARCHIVE}"
				EXPECTED_HASH "SHA512=${PULSEFORGE_DXC_ARCHIVE_SHA512}"
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
			message(FATAL_ERROR "Pinned DXC archive did not contain the expected host compiler: ${PULSEFORGE_DXC}")
		endif()
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
		pulseforge_attach_shader_deployment(PulseForgeGame "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders"
			ASSET_TARGETS PulseForgeSceneShaderAssets SHADERS ${PULSEFORGE_SCENE_SHADER_OUTPUTS})
		pulseforge_attach_asset_deployment(PulseForgeGame "$<TARGET_FILE_DIR:PulseForgeGame>"
			DIRECTORIES
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Models" "Assets/Models"
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Textures" "Assets/Textures"
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Environments" "Assets/Environments"
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scenes" "Assets/Scenes"
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Materials" "Assets/Materials"
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scripts" "Assets/Scripts"
			FILES
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/PulseForgeGame.pfproj" "PulseForgeGame.pfproj"
		)
		set_property(TARGET PulseForgeGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeGame>")
	endif()

	if(PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF OR
		(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
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
		if(PULSEFORGE_BUILD_EDITOR)
			pulseforge_attach_shader_deployment(PulseForgeEditor "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders"
				ASSET_TARGETS PulseForgeSceneShaderAssets PulseForgeEditorShaderAssets
				SHADERS ${PULSEFORGE_SCENE_SHADER_OUTPUTS} ${PULSEFORGE_EDITOR_SHADER_OUTPUTS})
		endif()
	endif()
endif()
