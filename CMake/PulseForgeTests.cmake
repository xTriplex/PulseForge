if(BUILD_TESTING)
	if(CMAKE_CONFIGURATION_TYPES)
		add_test(NAME PulseForge.CMake.CustomConfigurationInitialization
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_BUILD_TREE=${CMAKE_BINARY_DIR}"
				"-DPULSEFORGE_CONFIGURATION_PROBE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/CustomConfigurationProbe"
				"-DPULSEFORGE_CONFIGURATION_INITIALIZER=${CMAKE_CURRENT_SOURCE_DIR}/CMake/PulseForgeConfigurations.cmake"
				"-DPULSEFORGE_CONFIGURATION_PROBE_BINARY=${CMAKE_CURRENT_BINARY_DIR}/Tests/CustomConfigurationInitialization"
				"-DPULSEFORGE_TEST_GENERATOR=${CMAKE_GENERATOR}"
				"-DPULSEFORGE_TEST_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
				"-DPULSEFORGE_TEST_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/TestCustomConfigurationInitialization.cmake")
	endif()

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
				"-DPULSEFORGE_TARGET_SYSTEM_NAME=${CMAKE_SYSTEM_NAME}"
				"-DPULSEFORGE_BUILD_TREE=${CMAKE_BINARY_DIR}"
				"-DPULSEFORGE_BUILD_EDITOR=${PULSEFORGE_BUILD_EDITOR}"
				"-DPULSEFORGE_BUILD_CORE_ONLY=${PULSEFORGE_BUILD_CORE_ONLY}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/CheckNormalBuildMode.cmake")
	endif()

	add_executable(PulseForgeRendererTests
		Tests/PulseForgeRendererTests.cpp
		Tests/RendererPolicyTests.cpp
		Tests/RendererContractTests.cpp
		Tests/RendererTests.cpp
	)
	target_include_directories(PulseForgeRendererTests PRIVATE Tests)
	target_link_libraries(PulseForgeRendererTests PRIVATE PulseForgeRenderer PulseForgeScene nvrhi_vk nvrhi Vulkan-Headers)
	add_test(NAME PulseForge.Renderer COMMAND PulseForgeRendererTests)

	add_executable(PulseForgeRenderMathTests
		Tests/PulseForgeRenderMathTests.cpp
		Tests/RenderMathTests.cpp
	)
	target_include_directories(PulseForgeRenderMathTests PRIVATE Tests)
	target_link_libraries(PulseForgeRenderMathTests PRIVATE PulseForgeRenderMath)
	add_test(NAME PulseForge.RenderMath COMMAND PulseForgeRenderMathTests)

	add_executable(PulseForgeGpuValidationAnalysisTests
		Tests/PulseForgeGpuValidationAnalysisTests.cpp
		Tests/GpuValidation/PointShadowImageAnalysisTests.cpp
		Tests/GpuValidation/TransparencyImageAnalysisTests.cpp
	)
	target_include_directories(PulseForgeGpuValidationAnalysisTests PRIVATE Tests)
	target_link_libraries(PulseForgeGpuValidationAnalysisTests PRIVATE PulseForgeGpuValidationAnalysis)
	add_test(NAME PulseForge.GpuValidationAnalysis COMMAND PulseForgeGpuValidationAnalysisTests)

	add_executable(PulseForgeSceneTests
		Tests/PulseForgeSceneTests.cpp
		Tests/SceneTests.cpp
		Tests/PropertyMetadataTests.cpp
		Tests/ScriptPropertyMetadataTests.cpp
	)
	target_include_directories(PulseForgeSceneTests PRIVATE Tests)
	target_link_libraries(PulseForgeSceneTests PRIVATE
		PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath PulseForgePhysics)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeSceneTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Scene COMMAND PulseForgeSceneTests)

	add_executable(PulseForgePropertyMetadataConsumer Tests/ModuleConsumers/PropertyMetadataConsumer.cpp)
	target_link_libraries(PulseForgePropertyMetadataConsumer PRIVATE PulseForgeScene)
	set_property(TARGET PulseForgePropertyMetadataConsumer PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
	add_test(NAME PulseForge.PropertyMetadataConsumer COMMAND PulseForgePropertyMetadataConsumer)

	add_executable(PulseForgeAssetTests
		Tests/PulseForgeAssetTests.cpp
		Tests/AssetsTests.cpp
		Tests/SceneAssetTests.cpp
		Tests/AssetRegistryTests.cpp
		Tests/AssetOperationsTests.cpp
		Tests/AssetDependencyCatalogTests.cpp
		Tests/AssetDependencyCorrectionTests.cpp
		Tests/AssetDependencyPublicationTests.cpp
	)
	target_include_directories(PulseForgeAssetTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_link_libraries(PulseForgeAssetTests PRIVATE
		PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAssetServices PulseForgeSceneSerialization PulseForgeAssetDependencyCatalog)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAssetTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Assets COMMAND PulseForgeAssetTests)
	add_test(NAME PulseForge.AssetDependencyCatalog COMMAND PulseForgeAssetTests --dependency-catalog-only)
	add_executable(PulseForgeAssetDependencyConsumer Tests/ModuleConsumers/AssetDependencyConsumer.cpp)
	target_link_libraries(PulseForgeAssetDependencyConsumer PRIVATE PulseForgeAssetDependencyCatalog)
	set_property(TARGET PulseForgeAssetDependencyConsumer PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
	add_test(NAME PulseForge.AssetDependencyConsumer COMMAND PulseForgeAssetDependencyConsumer)

	add_executable(PulseForgeAssetImportTests
		Tests/PulseForgeAssetImportTests.cpp
		Tests/GltfMeshImportTests.cpp
		Tests/GltfMeshImporterRegressionTests.cpp
		Tests/ImageAssetImportTests.cpp
		Tests/AssetImportProvenanceTests.cpp
	)
	target_include_directories(PulseForgeAssetImportTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_link_libraries(PulseForgeAssetImportTests PRIVATE
		PulseForgeAssetImporters PulseForgeAssetServices PulseForgeAssetDependencyCatalog)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAssetImportTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.AssetImports COMMAND PulseForgeAssetImportTests)

	add_executable(PulseForgeAssetImporterConsumer Tests/ModuleConsumers/AssetImporterConsumer.cpp)
	target_link_libraries(PulseForgeAssetImporterConsumer PRIVATE PulseForgeAssetImporters)
	add_test(NAME PulseForge.AssetImporterConsumer COMMAND PulseForgeAssetImporterConsumer)

	add_executable(PulseForgeSceneRendererConsumer Tests/ModuleConsumers/SceneRendererConsumer.cpp)
	target_link_libraries(PulseForgeSceneRendererConsumer PRIVATE PulseForgeSceneRenderer)
	add_test(NAME PulseForge.SceneRendererConsumer COMMAND PulseForgeSceneRendererConsumer)

	if(TARGET PulseForgeEditorCore)
		add_executable(PulseForgeEditorCoreConsumer Tests/ModuleConsumers/EditorCoreConsumer.cpp)
		target_link_libraries(PulseForgeEditorCoreConsumer PRIVATE PulseForgeEditorCore)
		if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
			add_dependencies(PulseForgeEditorCoreConsumer PulseForgePublicHeaderChecks)
		endif()
		add_test(NAME PulseForge.EditorCoreConsumer COMMAND PulseForgeEditorCoreConsumer)
	endif()

	add_executable(PulseForgeAudioRuntimeTests
		Tests/PulseForgeAudioRuntimeTests.cpp
		Tests/AudioRuntimeTests.cpp
	)
	target_include_directories(PulseForgeAudioRuntimeTests PRIVATE Tests PulseForge/Vendor/json/include)
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
		Tests/GameplayInputTests.cpp
		Tests/GameplayInputLifecycleTests.cpp
		Tests/GameplayInputStoreTests.cpp
		Tests/ScriptRuntimeTests.cpp
	)
	target_include_directories(PulseForgeScriptingRuntimeTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeScriptingRuntimeTests PRIVATE
		PULSEFORGE_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
	target_link_libraries(PulseForgeScriptingRuntimeTests PRIVATE PulseForgeRuntime PulseForgeAssetServices PulseForgeInput)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeScriptingRuntimeTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.ScriptingRuntime COMMAND PulseForgeScriptingRuntimeTests)

	add_executable(PulseForgePlayerLaunchConfigurationTests
		Tests/PlayerLaunchConfigurationTestMain.cpp
		Tests/PlayerLaunchConfigurationTests.cpp
		PulseForgeGame/Source/Private/Client/PlayerLaunchConfiguration.cpp
	)
	target_include_directories(PulseForgePlayerLaunchConfigurationTests PRIVATE Tests PulseForgeGame/Source/Public)
	target_link_libraries(PulseForgePlayerLaunchConfigurationTests PRIVATE PulseForgeAssetCatalog)
	add_test(NAME PulseForge.PlayerLaunchConfiguration COMMAND PulseForgePlayerLaunchConfigurationTests)

	if(PULSEFORGE_BUILD_EDITOR)
		add_executable(PulseForgeEditorPlayerProcessTests
			Tests/EditorPlayerProcessTestMain.cpp
			PulseForgeEditor/Source/Private/EditorPlayerProcess.cpp
			PulseForgeEditor/Source/Private/EditorPlayerExecutablePath.cpp
		)
		target_compile_definitions(PulseForgeEditorPlayerProcessTests PRIVATE ${PULSEFORGE_PLATFORM_COMPILE_DEFINITION})
		if(WIN32)
			target_sources(PulseForgeEditorPlayerProcessTests PRIVATE
				PulseForgeEditor/Source/Private/Platform/Windows/EditorPlayerProcess.cpp)
			target_link_libraries(PulseForgeEditorPlayerProcessTests PRIVATE kernel32)
		elseif(PULSEFORGE_PLATFORM_LINUX)
			target_sources(PulseForgeEditorPlayerProcessTests PRIVATE
				PulseForgeEditor/Source/Private/Platform/Linux/EditorPlayerProcess.cpp)
		elseif(PULSEFORGE_PLATFORM_MACOS)
			target_sources(PulseForgeEditorPlayerProcessTests PRIVATE
				PulseForgeEditor/Source/Private/Platform/MacOS/EditorPlayerProcess.cpp)
		endif()
		target_include_directories(PulseForgeEditorPlayerProcessTests PRIVATE
			Tests
			PulseForgeEditor/Source/Public)
		add_test(NAME PulseForge.EditorPlayerProcess COMMAND PulseForgeEditorPlayerProcessTests)
	endif()

	target_sources(PulseForgeScriptingRuntimeTests PRIVATE Tests/PlayerSessionTests.cpp)

	if(TARGET PulseForgeEditorCore)
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

		add_executable(PulseForgeEditorDocumentTransferFailureTests Tests/EditorDocumentTransferFailureTests.cpp)
		target_compile_definitions(PulseForgeEditorCore PRIVATE PULSEFORGE_EDITOR_DOCUMENT_MANAGER_TESTING)
		target_compile_definitions(PulseForgeEditorDocumentTransferFailureTests PRIVATE PULSEFORGE_EDITOR_DOCUMENT_MANAGER_TESTING)
		target_link_libraries(PulseForgeEditorDocumentTransferFailureTests PRIVATE PulseForgeEditorCore)
		add_test(NAME PulseForge.EditorDocumentTransferFailures COMMAND PulseForgeEditorDocumentTransferFailureTests)
	endif()

	if(PULSEFORGE_BUILD_EDITOR)
		set(PULSEFORGE_EDITOR_INTEGRATION_LIBRARIES
			PulseForgeCore PulseForgeWindow PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath
			PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAssetDependencyCatalog PulseForgeAudio PulseForgeAudioAssets
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
			Tests/EditorInspectorRegressionTests.cpp
			Tests/EditorMaterialProjectTests.cpp
			Tests/EditorProjectLoadingTests.cpp
			Tests/EditorAssetReferenceTests.cpp
			Tests/ProjectRuntimeTests.cpp
			Tests/EditorHistoryTests.cpp
			$<$<CONFIG:Debug>:Tests/EditorDebugAcceptanceCoordinatorTests.cpp>
		)
		target_include_directories(PulseForgeEditorTests PRIVATE
			Tests
			PulseForgeEditor/Source/Public
			PulseForge/Vendor/json/include
		)
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
			Tests/EditorPerformanceTraceTests.cpp
		)
		target_include_directories(PulseForgeEditorWorkspacePresentationTests PRIVATE
			Tests
			PulseForgeEditor/Source/Public
			PulseForge/Vendor/json/include
		)
		target_link_libraries(PulseForgeEditorWorkspacePresentationTests PRIVATE
			PulseForgeEditorCore PulseForgeEditorUI ${PULSEFORGE_EDITOR_INTEGRATION_LIBRARIES} PulseForgeImGui spdlog::spdlog)
		if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
			add_dependencies(PulseForgeEditorWorkspacePresentationTests PulseForgePublicHeaderChecks)
		endif()
		add_test(NAME PulseForge.EditorWorkspacePresentation COMMAND PulseForgeEditorWorkspacePresentationTests)
	endif()

	add_executable(PulseForgeBufferUpdateTests Tests/BufferUpdateValidationTests.cpp)
	target_link_libraries(PulseForgeBufferUpdateTests PRIVATE PulseForgeRenderer)
	add_test(NAME PulseForge.BufferUpdates COMMAND PulseForgeBufferUpdateTests)

	set(PULSEFORGE_TEST_EXECUTABLE_TARGETS
		PulseForgeRendererTests
		PulseForgeRenderMathTests
		PulseForgeGpuValidationAnalysisTests
		PulseForgeCoreTests
		PulseForgeCoreConsumer
		PulseForgeSceneTests
		PulseForgePropertyMetadataConsumer
		PulseForgeAssetTests
		PulseForgeAssetImportTests
		PulseForgeAssetImporterConsumer
		PulseForgeAssetDependencyConsumer
		PulseForgeSceneRendererConsumer
		PulseForgeAudioRuntimeTests
		PulseForgeScriptingRuntimeTests
		PulseForgePlayerLaunchConfigurationTests
		PulseForgeBufferUpdateTests
	)
	if(TARGET PulseForgeEditorPlayerProcessTests)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeEditorPlayerProcessTests)
	endif()
	foreach(EditorTestTarget IN ITEMS
		PulseForgeEditorCoreConsumer
		PulseForgeEditorCoreTests
		PulseForgeCommandHistoryFailureTests
		PulseForgeEditorDocumentTransferFailureTests)
		if(TARGET ${EditorTestTarget})
			list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS ${EditorTestTarget})
		endif()
	endforeach()
	if(PULSEFORGE_BUILD_EDITOR)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS
			PulseForgeEditorTests
			PulseForgeEditorWorkspacePresentationTests
		)
	endif()
	if(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_PLATFORM_MACOS)
		add_executable(PulseForgeNativeDialogValidation Tests/Platform/MacOSNativeDialogValidation.cpp)
		target_include_directories(PulseForgeNativeDialogValidation PRIVATE PulseForgeEditor/Source/Public)
		target_link_libraries(PulseForgeNativeDialogValidation PRIVATE PulseForgeEditorUI glfw)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeNativeDialogValidation)

		add_executable(PulseForgeMacOSVulkanLoaderLifecycleValidation
			Tests/Platform/MacOSVulkanLoaderLifecycleValidation.cpp)
		target_link_libraries(PulseForgeMacOSVulkanLoaderLifecycleValidation PRIVATE PulseForgeWindow glfw)
		add_test(NAME PulseForge.MacOSVulkanLoaderLifecycle
			COMMAND PulseForgeMacOSVulkanLoaderLifecycleValidation --vulkan-lifecycle)
		add_test(NAME PulseForge.MacOSVulkanLoaderRuntimeMismatch
			COMMAND PulseForgeMacOSVulkanLoaderLifecycleValidation --runtime-mismatch-policy)
		add_test(NAME PulseForge.MacOSOpenGLContextWithoutVulkanSdk
			COMMAND PulseForgeMacOSVulkanLoaderLifecycleValidation --opengl-context-without-vulkan-sdk)
		set_tests_properties(
			PulseForge.MacOSVulkanLoaderLifecycle
			PulseForge.MacOSVulkanLoaderRuntimeMismatch
			PROPERTIES RUN_SERIAL TRUE TIMEOUT 60)
		set_tests_properties(PulseForge.MacOSOpenGLContextWithoutVulkanSdk
			PROPERTIES RUN_SERIAL TRUE TIMEOUT 60 SKIP_RETURN_CODE 77)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeMacOSVulkanLoaderLifecycleValidation)
	endif()
	if(TARGET PulseForgeGpuValidation)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeGpuValidation)
	endif()
	if(TARGET PulseForgeMacOSRendererValidation)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeMacOSRendererValidation)
	endif()
	if(TARGET PulseForgeLinuxWindowLifecycleValidation)
		list(APPEND PULSEFORGE_TEST_EXECUTABLE_TARGETS PulseForgeLinuxWindowLifecycleValidation)
	endif()

	add_custom_target(PulseForgeTests)
	add_dependencies(PulseForgeTests ${PULSEFORGE_TEST_EXECUTABLE_TARGETS})
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeTests PulseForgePublicHeaderChecks)
	endif()
endif()
