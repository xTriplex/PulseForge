if(BUILD_TESTING)
	if(NOT TARGET PulseForgeTests)
		message(FATAL_ERROR "BUILD_TESTING requires the PulseForgeTests aggregate target")
	endif()
	get_target_property(PULSEFORGE_TEST_AGGREGATE_DEPENDENCIES PulseForgeTests MANUALLY_ADDED_DEPENDENCIES)
	if(PULSEFORGE_BUILD_CORE_ONLY)
		foreach(TestTarget PulseForgeCoreTests PulseForgeCoreConsumer)
			if(NOT TARGET ${TestTarget})
				message(FATAL_ERROR "Core-only testing requires ${TestTarget}")
			endif()
			if(NOT TestTarget IN_LIST PULSEFORGE_TEST_AGGREGATE_DEPENDENCIES)
				message(FATAL_ERROR "PulseForgeTests does not depend on core test executable ${TestTarget}")
			endif()
		endforeach()
	else()
		if(TARGET PulseForgeEditorCore)
			foreach(TestTarget PulseForgeEditorCoreConsumer PulseForgeEditorCoreTests
					PulseForgeCommandHistoryFailureTests PulseForgeEditorDocumentTransferFailureTests)
				if(NOT TARGET ${TestTarget})
					message(FATAL_ERROR
						"PulseForgeEditorCore exists but its registered test executable '${TestTarget}' is missing")
				endif()
			endforeach()
		elseif(NOT PULSEFORGE_PLATFORM_LINUX)
			message(FATAL_ERROR
				"Non-Linux BUILD_TESTING requires the independent PulseForgeEditorCore test target")
		endif()
		foreach(TestTarget IN LISTS PULSEFORGE_TEST_EXECUTABLE_TARGETS)
			if(NOT TARGET ${TestTarget})
				message(FATAL_ERROR "PulseForgeTests names an unavailable test executable: ${TestTarget}")
			endif()
			if(NOT TestTarget IN_LIST PULSEFORGE_TEST_AGGREGATE_DEPENDENCIES)
				message(FATAL_ERROR "PulseForgeTests does not depend on registered test executable ${TestTarget}")
			endif()
		endforeach()
		if(PULSEFORGE_BUILD_EDITOR)
			if(NOT TARGET PulseForgeEditorUI OR NOT TARGET PulseForgeEditorTests)
				message(FATAL_ERROR "Editor-enabled testing requires PulseForgeEditorUI and PulseForgeEditorTests")
			endif()
		elseif(TARGET PulseForgeEditorUI OR TARGET PulseForgeEditorTests OR
			(TARGET PulseForgeImGui AND NOT PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF))
			message(FATAL_ERROR "Editor-disabled configuration must not create editor UI or its tests")
		endif()
		if(PULSEFORGE_PLATFORM_LINUX AND NOT PULSEFORGE_BUILD_EDITOR)
			foreach(UnsupportedEditorTarget PulseForgeEditorCore PulseForgeEditorCoreConsumer PulseForgeEditorCoreTests
					PulseForgeCommandHistoryFailureTests PulseForgeEditorDocumentTransferFailureTests
					PulseForgeEditorWorkspacePresentationTests PulseForgeEditorTests)
				if(TARGET ${UnsupportedEditorTarget})
					message(FATAL_ERROR
						"Linux runtime testing does not support '${UnsupportedEditorTarget}'; disable editor-only targets")
				endif()
			endforeach()
		endif()
	endif()
endif()

foreach(Target PulseForgeCore PulseForgeInput PulseForgeWindow PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath
		PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAssetDependencyCatalog PulseForgeAssetPreparation PulseForgeDerivedData PulseForgeAssetCpuProducts PulseForgeAudio PulseForgeAudioAssets PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics
		PulseForgeCookedProductCodecs PulseForgeCookedAssetCatalog PulseForgeCookedRuntimeProvider PulseForgeCookedAudioAssets
		PulseForgeCookedAudioScene PulseForgeCookedScripting PulseForgeCookedRuntime PulseForgeCookedSceneRenderer
		PulseForgeAssetCookService PulseForgeCook PulseForgeBuildProfiles PulseForgeAssetExportService PulseForgeExport
		PulseForgeCookedGame
		PulseForgeScripting PulseForgeRuntime PulseForgeRenderer PulseForgeMeshData
		PulseForgeApplication PulseForgeSceneRenderer
		PulseForge PulseForgeGame PulseForgeEditor PulseForgeEditorCore PulseForgeEditorUI PulseForgeRendererTests PulseForgeRenderMathTests PulseForgeGpuValidationAnalysis PulseForgeGpuValidationAnalysisTests
		PulseForgeCoreTests PulseForgeCoreConsumer PulseForgeSceneTests PulseForgeAssetTests PulseForgeAssetImportTests PulseForgeAssetImporterConsumer PulseForgeAssetDependencyConsumer PulseForgeDerivedDataTests PulseForgeDerivedDataConsumer PulseForgeAudioRuntimeTests
		PulseForgeSceneRendererConsumer PulseForgeEditorCoreConsumer PulseForgeScriptingRuntimeTests PulseForgeEditorTests PulseForgeEditorWorkspacePresentationTests PulseForgeEditorCoreTests
		PulseForgeBufferUpdateTests PulseForgeCommandHistoryFailureTests PulseForgeEditorDocumentTransferFailureTests PulseForgeGpuValidation
		PulseForgeLinuxWindowLifecycleValidation PulseForgeEditorImGuiProofRenderer PulseForgeMacOSRendererValidation
		PulseForgeMacOSVulkanLoaderLifecycleValidation)
	if(TARGET ${Target})
		target_compile_options(${Target} PRIVATE
			$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
			$<$<CXX_COMPILER_ID:MSVC>:/utf-8>
		)
		pulseforge_apply_configuration_identity(${Target})
		if(PULSEFORGE_ENABLE_MSVC_ANALYSIS AND
			"${Target}" MATCHES "^(PulseForgeAssetImporters|PulseForgeAssetDependencyCatalog|PulseForgeMeshData|PulseForgeEditorCore|PulseForgeEditorUI|PulseForgeSceneRenderer)$")
			target_compile_options(${Target} PRIVATE /analyze /analyze:external- /external:W0)
		endif()
	endif()
endforeach()
