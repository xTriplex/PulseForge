if(BUILD_TESTING)
	if(NOT TARGET PulseForgeEditorCoreTests)
		message(FATAL_ERROR "BUILD_TESTING requires the independent PulseForgeEditorCoreTests target")
	endif()
	if(PULSEFORGE_BUILD_EDITOR)
		if(NOT TARGET PulseForgeEditorUI OR NOT TARGET PulseForgeEditorTests)
			message(FATAL_ERROR "Editor-enabled testing requires PulseForgeEditorUI and PulseForgeEditorTests")
		endif()
	elseif(TARGET PulseForgeEditorUI OR TARGET PulseForgeEditorTests OR TARGET PulseForgeImGui)
		message(FATAL_ERROR "Editor-disabled configuration must not create editor UI or its tests")
	endif()
endif()

foreach(Target PulseForgeCore PulseForgeWindow PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath
		PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAudio PulseForgeAudioAssets PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics
		PulseForgeScripting PulseForgeRuntime PulseForgeRenderer PulseForgeMeshData
		PulseForgeApplication PulseForgeSceneRenderer
		PulseForge PulseForgeGame PulseForgeEditor PulseForgeEditorCore PulseForgeEditorUI PulseForgeRendererTests PulseForgeRenderMathTests PulseForgeGpuValidationAnalysis PulseForgeGpuValidationAnalysisTests
		PulseForgeCoreTests PulseForgeSceneTests PulseForgeAssetTests PulseForgeAssetImportTests PulseForgeAssetImporterConsumer PulseForgeAudioRuntimeTests
		PulseForgeSceneRendererConsumer PulseForgeEditorTests PulseForgeEditorWorkspacePresentationTests PulseForgeEditorCoreTests
		PulseForgeBufferUpdateTests PulseForgeGpuValidation)
	if(TARGET ${Target})
		target_compile_options(${Target} PRIVATE
			$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
			$<$<CXX_COMPILER_ID:MSVC>:/utf-8>
			$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
		)
		target_compile_definitions(${Target} PRIVATE
			$<$<CONFIG:Debug>:PF_DEBUG>
			$<$<CONFIG:Release>:PF_RELEASE>
			$<$<CONFIG:Dist>:PF_DIST>
		)
		if(PULSEFORGE_ENABLE_MSVC_ANALYSIS AND
			"${Target}" MATCHES "^(PulseForgeAssetImporters|PulseForgeMeshData|PulseForgeEditorCore|PulseForgeEditorUI|PulseForgeSceneRenderer)$")
			target_compile_options(${Target} PRIVATE /analyze /analyze:external- /external:W0)
		endif()
	endif()
endforeach()
