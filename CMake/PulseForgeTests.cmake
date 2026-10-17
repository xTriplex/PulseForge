if(BUILD_TESTING)
	if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" OR
		(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
		add_test(NAME PulseForge.RuntimeAssetDeployment
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_FIXTURE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/RuntimeAssetDeploymentFixture"
				"-DPULSEFORGE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/Tests/RuntimeAssetDeployment"
				"-DPULSEFORGE_TEST_GENERATOR=${CMAKE_GENERATOR}"
				"-DPULSEFORGE_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
				"-DPULSEFORGE_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
				"-DPULSEFORGE_TEST_CONFIGURATION=$<CONFIG>"
				"-DPULSEFORGE_DEPLOYMENT_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeAssetDeployment.cmake"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestRuntimeAssetDeployment.cmake")
		add_test(NAME PulseForge.ShaderDeployment
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_FIXTURE_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/ShaderDeploymentFixture"
				"-DPULSEFORGE_TEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/Tests/ShaderDeployment"
				"-DPULSEFORGE_TEST_GENERATOR=${CMAKE_GENERATOR}"
				"-DPULSEFORGE_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
				"-DPULSEFORGE_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
				"-DPULSEFORGE_DEPLOYMENT_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeShaderDeployment.cmake"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestShaderDeployment.cmake")
	endif()

	if(NOT PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_test(NAME PulseForge.CMakeBuildMode
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_BUILD_TREE=${CMAKE_BINARY_DIR}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/CheckNormalBuildMode.cmake")
	endif()

	add_executable(PulseForgeRendererTests
		Tests/PulseForgeRendererTests.cpp
		Tests/RendererPolicyTests.cpp
		Tests/RendererContractTests.cpp
		Tests/RendererTests.cpp
	)
	target_include_directories(PulseForgeRendererTests PRIVATE Tests)
	target_compile_definitions(PulseForgeRendererTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeRendererTests PRIVATE PulseForgeRenderer)
	add_test(NAME PulseForge.Renderer COMMAND PulseForgeRendererTests)

	add_executable(PulseForgeRenderMathTests
		Tests/PulseForgeRenderMathTests.cpp
		Tests/RenderMathTests.cpp
	)
	target_include_directories(PulseForgeRenderMathTests PRIVATE Tests)
	target_compile_definitions(PulseForgeRenderMathTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeRenderMathTests PRIVATE PulseForgeRenderMath)
	add_test(NAME PulseForge.RenderMath COMMAND PulseForgeRenderMathTests)

	add_executable(PulseForgeGpuValidationAnalysisTests
		Tests/PulseForgeGpuValidationAnalysisTests.cpp
		Tests/GpuValidation/PointShadowImageAnalysisTests.cpp
		Tests/GpuValidation/TransparencyImageAnalysisTests.cpp
	)
	target_include_directories(PulseForgeGpuValidationAnalysisTests PRIVATE Tests)
	target_compile_definitions(PulseForgeGpuValidationAnalysisTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeGpuValidationAnalysisTests PRIVATE PulseForgeGpuValidationAnalysis)
	add_test(NAME PulseForge.GpuValidationAnalysis COMMAND PulseForgeGpuValidationAnalysisTests)

	add_executable(PulseForgeCoreTests
		Tests/PulseForgeTests.cpp
		Tests/CoreTests.cpp
	)
	target_include_directories(PulseForgeCoreTests PRIVATE Tests)
	target_compile_definitions(PulseForgeCoreTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeCoreTests PRIVATE PulseForgeCore)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeCoreTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Core COMMAND PulseForgeCoreTests)

	add_executable(PulseForgeSceneTests
		Tests/PulseForgeSceneTests.cpp
		Tests/SceneTests.cpp
	)
	target_include_directories(PulseForgeSceneTests PRIVATE Tests)
	target_compile_definitions(PulseForgeSceneTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeSceneTests PRIVATE
		PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath PulseForgePhysics)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeSceneTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Scene COMMAND PulseForgeSceneTests)

	add_executable(PulseForgeAssetTests
		Tests/PulseForgeAssetTests.cpp
		Tests/AssetsTests.cpp
		Tests/SceneAssetTests.cpp
		Tests/AssetRegistryTests.cpp
		Tests/AssetOperationsTests.cpp
	)
	target_include_directories(PulseForgeAssetTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeAssetTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeAssetTests PRIVATE
		PulseForgeAssetCatalog PulseForgeAssetServices PulseForgeSceneSerialization)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAssetTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Assets COMMAND PulseForgeAssetTests)

	add_executable(PulseForgeAssetImportTests
		Tests/PulseForgeAssetImportTests.cpp
		Tests/GltfMeshImportTests.cpp
		Tests/ImageAssetImportTests.cpp
	)
	target_include_directories(PulseForgeAssetImportTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeAssetImportTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeAssetImportTests PRIVATE PulseForgeAssetImporters PulseForgeAssetServices)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAssetImportTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.AssetImports COMMAND PulseForgeAssetImportTests)

	add_executable(PulseForgeAssetImporterConsumer Tests/ModuleConsumers/AssetImporterConsumer.cpp)
	target_compile_definitions(PulseForgeAssetImporterConsumer PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeAssetImporterConsumer PRIVATE PulseForgeAssetImporters)
	add_test(NAME PulseForge.AssetImporterConsumer COMMAND PulseForgeAssetImporterConsumer)

	add_executable(PulseForgeSceneRendererConsumer Tests/ModuleConsumers/SceneRendererConsumer.cpp)
	target_compile_definitions(PulseForgeSceneRendererConsumer PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeSceneRendererConsumer PRIVATE PulseForgeSceneRenderer)
	add_test(NAME PulseForge.SceneRendererConsumer COMMAND PulseForgeSceneRendererConsumer)

	add_executable(PulseForgeEditorCoreConsumer Tests/ModuleConsumers/EditorCoreConsumer.cpp)
	target_compile_definitions(PulseForgeEditorCoreConsumer PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorCoreConsumer PRIVATE PulseForgeEditorCore)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeEditorCoreConsumer PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.EditorCoreConsumer COMMAND PulseForgeEditorCoreConsumer)

	add_executable(PulseForgeAudioRuntimeTests
		Tests/PulseForgeAudioRuntimeTests.cpp
		Tests/AudioRuntimeTests.cpp
	)
	target_include_directories(PulseForgeAudioRuntimeTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeAudioRuntimeTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeAudioRuntimeTests PRIVATE
		PulseForgeAssetServices
		PulseForgeAudioAssets
		PulseForgeAudioScene
		PulseForgeScripting
	)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAudioRuntimeTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.AudioRuntime COMMAND PulseForgeAudioRuntimeTests)

	add_executable(PulseForgeScriptingRuntimeTests
		Tests/PulseForgeScriptingRuntimeTests.cpp
		Tests/ScriptRuntimeTests.cpp
	)
	target_include_directories(PulseForgeScriptingRuntimeTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeScriptingRuntimeTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeScriptingRuntimeTests PRIVATE PulseForgeRuntime PulseForgeAssetServices)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeScriptingRuntimeTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.ScriptingRuntime COMMAND PulseForgeScriptingRuntimeTests)

	add_executable(PulseForgeEditorCoreTests
		Tests/PulseForgeEditorCoreTests.cpp
		Tests/EditorDocumentCloseRequestTests.cpp
		Tests/EditorDocumentTabInteractionTests.cpp
		Tests/EditorSceneEntityCommandTests.cpp
		Tests/EditorSceneViewportInteractionTests.cpp
		Tests/EditorViewportGizmoGeometryTests.cpp
		Tests/EditorViewportGizmoInteractionTests.cpp
	)
	target_include_directories(PulseForgeEditorCoreTests PRIVATE Tests)
	target_compile_definitions(PulseForgeEditorCoreTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorCoreTests PRIVATE PulseForgeCore PulseForgeEditorCore)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeEditorCoreTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.EditorCore COMMAND PulseForgeEditorCoreTests)

	add_executable(PulseForgeCommandHistoryFailureTests Tests/EditorCommandHistoryFailureTests.cpp)
	target_include_directories(PulseForgeCommandHistoryFailureTests PRIVATE PulseForgeEditor/Source/Public)
	if(MSVC)
		# This isolated allocator-failure executable replaces global new/delete to inject failures.
		# Keep it on the non-debug CRT so MSVC's debug-heap diagnostics do not intercept that probe.
		set_property(TARGET PulseForgeCommandHistoryFailureTests PROPERTY MSVC_RUNTIME_LIBRARY MultiThreaded)
	endif()
	add_test(NAME PulseForge.CommandHistoryFailures COMMAND PulseForgeCommandHistoryFailureTests)

	if(PULSEFORGE_BUILD_EDITOR)
		set(PULSEFORGE_EDITOR_INTEGRATION_LIBRARIES
			PulseForgeCore PulseForgeWindow PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath
			PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAudio PulseForgeAudioAssets
			PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics PulseForgeScripting PulseForgeRuntime
			PulseForgeRenderer PulseForgeApplication PulseForgeSceneRenderer
		)
		add_executable(PulseForgeEditorTests
			Tests/PulseForgeEditorTests.cpp
			Tests/EditorTests.cpp
			Tests/EditorViewportTests.cpp
			Tests/EditorViewportSceneRenderViewTests.cpp
			Tests/EditorAssetPreviewStressHarnessTests.cpp
			Tests/EditorDocumentHostTestEditorTests.cpp
			Tests/EditorStateTests.cpp
			Tests/EditorPanelStateTests.cpp
			Tests/EditorSceneStateTests.cpp
			Tests/EditorDocumentManagerTests.cpp
			Tests/EditorDocumentEditorRegistryTests.cpp
			Tests/EditorDocumentWorkspaceTests.cpp
			Tests/EditorAssetImportOperationTests.cpp
			Tests/EditorMaterialProjectTests.cpp
			Tests/EditorProjectLoadingTests.cpp
			Tests/EditorAssetReferenceTests.cpp
			Tests/ProjectRuntimeTests.cpp
			Tests/EditorHistoryTests.cpp
		)
		target_include_directories(PulseForgeEditorTests PRIVATE
			Tests
			PulseForgeEditor/Source/Public
			PulseForge/Vendor/json/include
		)
		target_compile_definitions(PulseForgeEditorTests PRIVATE PF_PLATFORM_WINDOWS)
		if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
			target_compile_definitions(PulseForgeEditorTests PRIVATE PF_EDITOR_RENDERER_OPENGL)
		endif()
		target_link_libraries(PulseForgeEditorTests PRIVATE
			PulseForgeEditorCore PulseForgeEditorUI ${PULSEFORGE_EDITOR_INTEGRATION_LIBRARIES} PulseForgeImGui spdlog::spdlog)
		if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
			add_dependencies(PulseForgeEditorTests PulseForgePublicHeaderChecks)
		endif()
		add_test(NAME PulseForge.Editor COMMAND PulseForgeEditorTests)
		add_executable(PulseForgeEditorWorkspacePresentationTests
			Tests/EditorWorkspacePresentationTestMain.cpp
			Tests/EditorWorkspacePresentationTests.cpp
		)
		target_include_directories(PulseForgeEditorWorkspacePresentationTests PRIVATE
			Tests
			PulseForgeEditor/Source/Public
			PulseForge/Vendor/json/include
		)
		target_compile_definitions(PulseForgeEditorWorkspacePresentationTests PRIVATE PF_PLATFORM_WINDOWS)
		target_link_libraries(PulseForgeEditorWorkspacePresentationTests PRIVATE
			PulseForgeEditorCore PulseForgeEditorUI ${PULSEFORGE_EDITOR_INTEGRATION_LIBRARIES} PulseForgeImGui spdlog::spdlog)
		if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
			add_dependencies(PulseForgeEditorWorkspacePresentationTests PulseForgePublicHeaderChecks)
		endif()
		add_test(NAME PulseForge.EditorWorkspacePresentation COMMAND PulseForgeEditorWorkspacePresentationTests)
	endif()

	add_executable(PulseForgeBufferUpdateTests Tests/BufferUpdateValidationTests.cpp)
	target_compile_definitions(PulseForgeBufferUpdateTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeBufferUpdateTests PRIVATE PulseForgeRenderer)
	add_test(NAME PulseForge.BufferUpdates COMMAND PulseForgeBufferUpdateTests)

	set(PULSEFORGE_TEST_EXECUTABLE_TARGETS
		PulseForgeRendererTests
		PulseForgeRenderMathTests
		PulseForgeGpuValidationAnalysisTests
		PulseForgeCoreTests
		PulseForgeSceneTests
		PulseForgeAssetTests
		PulseForgeAssetImportTests
		PulseForgeAssetImporterConsumer
		PulseForgeSceneRendererConsumer
		PulseForgeEditorCoreConsumer
		PulseForgeAudioRuntimeTests
		PulseForgeScriptingRuntimeTests
		PulseForgeEditorCoreTests
		PulseForgeCommandHistoryFailureTests
		PulseForgeBufferUpdateTests
	)
	if(PULSEFORGE_BUILD_EDITOR)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS
			PulseForgeEditorTests
			PulseForgeEditorWorkspacePresentationTests
		)
	endif()
	if(TARGET PulseForgeGpuValidation)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeGpuValidation)
	endif()

	add_custom_target(PulseForgeTests)
	add_dependencies(PulseForgeTests ${PULSEFORGE_TEST_EXECUTABLE_TARGETS})
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeTests PulseForgePublicHeaderChecks)
	endif()
endif()
