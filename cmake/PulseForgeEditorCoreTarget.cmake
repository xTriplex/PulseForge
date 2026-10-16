if(PULSEFORGE_BUILD_EDITOR OR BUILD_TESTING)
	add_library(PulseForgeEditorCore STATIC
		PulseForgeEditor/Source/Private/EditorEntityClipboard.cpp
		PulseForgeEditor/Source/Private/EditorAssetWorkflowState.cpp
		PulseForgeEditor/Source/Private/EditorConsoleHistory.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserState.cpp
		PulseForgeEditor/Source/Private/EditorMaterialDraftStore.cpp
		PulseForgeEditor/Source/Private/EditorMaterialDraftController.cpp
		PulseForgeEditor/Source/Private/EditorProjectOpening.cpp
		PulseForgeEditor/Source/Private/EditorSceneWorkspaceState.cpp
		PulseForgeEditor/Source/Private/EditorSceneHistoryState.cpp
		PulseForgeEditor/Source/Private/EditorSceneTransitionQueue.cpp
		PulseForgeEditor/Source/Private/EditorSceneEditController.cpp
		PulseForgeEditor/Source/Private/EditorSceneEntityCommands.cpp
		PulseForgeEditor/Source/Private/EditorSceneViewportInteraction.cpp
		PulseForgeEditor/Source/Private/EditorSceneVisibilityCommands.cpp
		PulseForgeEditor/Source/Private/EditorSceneHierarchyOperations.cpp
		PulseForgeEditor/Source/Private/EditorViewportToolState.cpp
		PulseForgeEditor/Source/Private/ViewportGizmoGeometry.cpp
		PulseForgeEditor/Source/Private/EditorViewportGizmoInteraction.cpp
		PulseForgeEditor/Source/Private/EditorDocumentManager.cpp
		PulseForgeEditor/Source/Private/EditorDocumentWorkspace.cpp
		PulseForgeEditor/Source/Private/EditorDocumentWorkspaceStore.cpp
		PulseForgeEditor/Source/Private/EditorMeshPickingCache.cpp
		PulseForgeEditor/Source/Private/EditorViewportCamera.cpp
	)
	target_include_directories(PulseForgeEditorCore
		PUBLIC PulseForgeEditor/Source/Public
		PRIVATE PulseForge/Source/Public PulseForgeEditor/Source/Private PulseForge/Vendor/json/include
	)
	target_compile_definitions(PulseForgeEditorCore PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorCore
		PUBLIC PulseForgeAssetServices PulseForgeScene
		PRIVATE PulseForgeCore PulseForgeAssetImporters PulseForgeSceneSerialization
	)
endif()
