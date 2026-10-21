if(PULSEFORGE_BUILD_EDITOR OR (BUILD_TESTING AND NOT PULSEFORGE_PLATFORM_LINUX))
	add_library(PulseForgeEditorCore STATIC
		PulseForgeEditor/Source/Private/EditorEntityClipboard.cpp
		PulseForgeEditor/Source/Private/EditorAssetWorkflowState.cpp
		PulseForgeEditor/Source/Private/EditorConsoleHistory.cpp
		PulseForgeEditor/Source/Private/EditorConsoleSeverity.cpp
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
		PulseForgeEditor/Source/Private/EditorDocumentTabInteraction.cpp
		PulseForgeEditor/Source/Private/EditorDocumentCloseRequest.cpp
		PulseForgeEditor/Source/Private/EditorDocumentWorkspace.cpp
		PulseForgeEditor/Source/Private/EditorDocumentWorkspaceStore.cpp
		PulseForgeEditor/Source/Private/EditorMeshPickingCache.cpp
		PulseForgeEditor/Source/Private/EditorViewportCamera.cpp
		PulseForgeEditor/Source/Private/ViewportMath.cpp
		PulseForgeEditor/Source/Private/EditorCommandLine.cpp
		$<$<CONFIG:Debug>:PulseForgeEditor/Source/Private/EditorDebugAcceptanceCoordinators.cpp>
	)
	if(WIN32)
		target_sources(PulseForgeEditorCore PRIVATE
			PulseForgeEditor/Source/Private/Platform/Windows/EditorPlatformFileSystem.cpp)
	elseif(PULSEFORGE_PLATFORM_LINUX)
		target_sources(PulseForgeEditorCore PRIVATE
			PulseForgeEditor/Source/Private/Platform/Linux/EditorPlatformFileSystem.cpp)
	elseif(PULSEFORGE_PLATFORM_MACOS)
		target_sources(PulseForgeEditorCore PRIVATE
			PulseForgeEditor/Source/Private/Platform/MacOS/EditorPlatformFileSystem.cpp)
	else()
		message(FATAL_ERROR "No EditorPlatformFileSystem implementation is available for this target platform.")
	endif()
	target_include_directories(PulseForgeEditorCore
		PUBLIC PulseForgeEditor/Source/Public
		PRIVATE PulseForge/Source/Public PulseForge/Vendor/json/include
	)
	target_link_libraries(PulseForgeEditorCore
		PUBLIC PulseForgeAssetServices PulseForgeScene
		PRIVATE PulseForgeCore PulseForgeAssetImporters PulseForgeSceneSerialization
	)
	target_precompile_headers(PulseForgeEditorCore PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Source/Public/Editor/EditorCorePCH.h")
endif()
