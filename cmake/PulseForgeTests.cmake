if(BUILD_TESTING)
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

	add_custom_target(PulseForgeTests)
	add_dependencies(PulseForgeTests PulseForgeCoreTests PulseForgeSceneTests PulseForgeAssetTests PulseForgeRendererTests)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeTests PulseForgePublicHeaderChecks)
	endif()

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

	add_executable(PulseForgeEditorCoreTests
		Tests/PulseForgeEditorCoreTests.cpp
		Tests/EditorSceneEntityCommandTests.cpp
		Tests/EditorSceneViewportInteractionTests.cpp
		Tests/EditorViewportGizmoGeometryTests.cpp
		Tests/EditorViewportGizmoInteractionTests.cpp
	)
	target_include_directories(PulseForgeEditorCoreTests PRIVATE Tests PulseForgeEditor/Source/Private)
	target_compile_definitions(PulseForgeEditorCoreTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorCoreTests PRIVATE PulseForgeEditorCore)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeEditorCoreTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.EditorCore COMMAND PulseForgeEditorCoreTests)

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
			Tests/ScriptRuntimeTests.cpp
			Tests/ProjectRuntimeTests.cpp
			Tests/EditorHistoryTests.cpp
		)
		target_include_directories(PulseForgeEditorTests PRIVATE
			Tests
			PulseForgeEditor/Source/Public
			PulseForgeEditor/Source/Private
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
			PulseForgeEditor/Source/Private
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
endif()
