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
		add_test(NAME PulseForge.GPU.LegacyMainRetirementCap
			CONFIGURATIONS Debug
			COMMAND PulseForgeGpuValidation --legacy-main-retirement-cap)
		add_test(NAME PulseForge.GPU.LegacyNativeRetirementCap
			CONFIGURATIONS Debug
			COMMAND PulseForgeGpuValidation --legacy-native-retirement-cap)
		add_test(NAME PulseForge.GPU.PresentationFenceNativeLifetime
			CONFIGURATIONS Debug
			COMMAND PulseForgeGpuValidation --presentation-fence-native-lifetime)
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
		set_tests_properties(PulseForge.GPU.LegacyMainRetirementCap PROPERTIES
			RUN_SERIAL TRUE
			TIMEOUT 60
			LABELS "gpu;renderer"
			ENVIRONMENT
				"PULSEFORGE_VULKAN_FORCE_LEGACY_RETIREMENT=1;PULSEFORGE_VULKAN_TEST_MAIN_OUT_OF_DATE_AFTER_NATIVE_TARGET=1;PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_AFTER_NATIVE_TARGET_COUNT=5;PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_AFTER_NATIVE_TARGET_DELAY_FRAMES=2;PULSEFORGE_VULKAN_TEST_MAIN_CREATE_FAILURE_AT_DEFERRED_COUNT=4")
		set_tests_properties(PulseForge.GPU.LegacyNativeRetirementCap PROPERTIES
			RUN_SERIAL TRUE
			TIMEOUT 60
			LABELS "gpu;renderer"
			ENVIRONMENT
				"PULSEFORGE_VULKAN_FORCE_LEGACY_RETIREMENT=1;PULSEFORGE_VULKAN_TEST_NATIVE_ACQUIRE_OUT_OF_DATE_COUNT=5;PULSEFORGE_VULKAN_TEST_NATIVE_CREATE_FAILURE_AT_DEFERRED_COUNT=4")
		set_tests_properties(PulseForge.GPU.PresentationFenceNativeLifetime PROPERTIES
			RUN_SERIAL TRUE
			TIMEOUT 60
			LABELS "gpu;renderer"
			ENVIRONMENT
				"PULSEFORGE_VULKAN_PRESENTATION_POLICY=require-fences;PULSEFORGE_VULKAN_TEST_NATIVE_ACQUIRE_OUT_OF_DATE_COUNT=1;PULSEFORGE_VULKAN_TEST_MAIN_OUT_OF_DATE_AFTER_NATIVE_TARGET=1;PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_AFTER_NATIVE_TARGET_COUNT=1;PULSEFORGE_VULKAN_TEST_MAIN_ACQUIRE_OUT_OF_DATE_AFTER_NATIVE_TARGET_DELAY_FRAMES=2")
	endif()
endif()
