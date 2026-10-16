if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS)
	add_executable(PulseForgeGpuValidation
		Tests/GpuRendererValidation.cpp
		Tests/GpuValidation/GpuLightingValidation.cpp
		Tests/GpuValidation/GpuSceneScenario.cpp
		Tests/GpuValidation/GpuSceneOutputValidation.cpp
		Tests/GpuValidation/GpuProjectSceneValidation.cpp
		Tests/GpuValidation/PointShadowImageValidation.cpp
		Tests/GpuValidation/ActiveFrameUploadValidation.cpp
		Tests/GpuValidation/EditorUiUnormValidation.cpp
		Tests/GpuValidation/ValidationShader.cpp)
	target_include_directories(PulseForgeGpuValidation PRIVATE Tests)
	target_compile_definitions(PulseForgeGpuValidation PRIVATE
		PF_PLATFORM_WINDOWS
		PF_GPU_VALIDATION_SHADER_DIRECTORY="${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
		PF_GPU_VALIDATION_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/PulseForgeGame.pfproj"
		PF_PBR_DEMO_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Samples/VisualValidation/PBRMaterials/PBRMaterials.pfproj"
		PF_TRANSPARENCY_DEMO_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Samples/VisualValidation/Transparency/Transparency.pfproj"
		PF_POINT_SHADOW_DEMO_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Samples/VisualValidation/PointLightShadows/PointLightShadows.pfproj"
		PF_GPU_VALIDATION_SCENE_SHADER_DIRECTORY="${PULSEFORGE_SHADER_OUTPUT_DIR}")
	target_link_libraries(PulseForgeGpuValidation PRIVATE PulseForge PulseForgeGpuValidationAnalysis)
	add_dependencies(PulseForgeGpuValidation PulseForgeGpuValidationShaders PulseForgeSceneShaderAssets)
	set_property(TARGET PulseForgeGpuValidation PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeGpuValidation>")
	if(BUILD_TESTING)
		add_test(NAME PulseForge.GPU.RGBA16F COMMAND PulseForgeGpuValidation)
		add_test(NAME PulseForge.GPU.SceneHDR COMMAND PulseForgeGpuValidation --scene-hdr)
		add_test(NAME PulseForge.GPU.SceneToneMapping COMMAND PulseForgeGpuValidation --scene-tonemap)
		add_test(NAME PulseForge.GPU.EditorUIUnorm COMMAND PulseForgeGpuValidation --editor-ui-unorm)
		add_test(NAME PulseForge.GPU.LocalLights COMMAND PulseForgeGpuValidation --scene-local-lights)
		add_test(NAME PulseForge.GPU.SpotlightShadows COMMAND PulseForgeGpuValidation --scene-spot-shadows)
		add_test(NAME PulseForge.GPU.PointShadows COMMAND PulseForgeGpuValidation --scene-point-shadows)
		add_test(NAME PulseForge.GPU.PBRMaterials COMMAND PulseForgeGpuValidation --scene-pbr-materials)
		add_test(NAME PulseForge.GPU.Transparency COMMAND PulseForgeGpuValidation --scene-transparency)
		add_test(NAME PulseForge.GPU.PointShadowImages
			COMMAND PulseForgeGpuValidation --scene-point-shadow-images
				"${CMAKE_BINARY_DIR}/Diagnostics/PointShadowDirection")
		add_test(NAME PulseForge.GPU.ActiveFrameUploads COMMAND PulseForgeGpuValidation --active-frame-resource-upload)
		set_tests_properties(
			PulseForge.GPU.RGBA16F
			PulseForge.GPU.SceneHDR
			PulseForge.GPU.SceneToneMapping
			PulseForge.GPU.EditorUIUnorm
			PulseForge.GPU.LocalLights
			PulseForge.GPU.SpotlightShadows
			PulseForge.GPU.PointShadows
			PulseForge.GPU.PBRMaterials
			PulseForge.GPU.Transparency
			PulseForge.GPU.PointShadowImages
			PulseForge.GPU.ActiveFrameUploads
			PROPERTIES RUN_SERIAL TRUE TIMEOUT 180 LABELS "gpu;renderer")
	endif()
endif()
