if(PULSEFORGE_BUILD_EDITOR)
	add_library(PulseForgeEditorUI STATIC
		PulseForgeEditor/Source/Private/EditorAssetDestinationDialog.cpp
		PulseForgeEditor/Source/Private/EditorAssetPreviewService.cpp
		PulseForgeEditor/Source/Private/EditorAssetPreviewStressHarness.cpp
		PulseForgeEditor/Source/Private/EditorAssetSelector.cpp
		PulseForgeEditor/Source/Private/EditorConsolePanel.cpp
		PulseForgeEditor/Source/Private/EditorConsoleSeverity.cpp
		PulseForgeEditor/Source/Private/EditorDocumentHostTestEditor.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserAssetView.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserNavigation.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserPanel.cpp
		PulseForgeEditor/Source/Private/EditorHierarchyPanel.cpp
		PulseForgeEditor/Source/Private/EditorInspectorPanel.cpp
		PulseForgeEditor/Source/Private/EditorMaterialDetailsPanel.cpp
		PulseForgeEditor/Source/Private/EditorStatusPanel.cpp
		PulseForgeEditor/Source/Private/EditorViewportSceneRenderView.cpp
		PulseForgeEditor/Source/Private/EditorViewportToolbar.cpp
		PulseForgeEditor/Source/Private/EditorImGuiRenderer.cpp
		PulseForgeEditor/Source/Private/EditorMainMenuPanel.cpp
		PulseForgeEditor/Source/Private/EditorLayout.cpp
		PulseForgeEditor/Source/Private/EditorStyle.cpp
		PulseForgeEditor/Source/Private/EditorWorkspacePresentation.cpp
		PulseForge/Vendor/imgui/misc/freetype/imgui_freetype.cpp
	)
	target_include_directories(PulseForgeEditorUI PUBLIC
		PulseForgeEditor/Source/Public
	)
	target_include_directories(PulseForgeEditorUI PRIVATE
		PulseForgeEditor/Source/Private
		PulseForge/Vendor/imgui/misc/freetype
	)
	target_include_directories(PulseForgeEditorUI SYSTEM PRIVATE
		PulseForge/Vendor/json/include
		PulseForge/Vendor/spdlog/include
	)
	target_compile_definitions(PulseForgeEditorUI PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorUI
		PUBLIC PulseForgeRenderer PulseForgeEditorCore
		PRIVATE PulseForgeCore PulseForgeApplication PulseForgeAssetImporters PulseForgeAssetServices
			PulseForgeImGui PulseForgeSceneRenderer freetype spdlog::spdlog shell32 ole32
	)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_compile_definitions(PulseForgeEditorUI PRIVATE PF_EDITOR_RENDERER_OPENGL)
	endif()
	set_source_files_properties(PulseForge/Vendor/imgui/misc/freetype/imgui_freetype.cpp PROPERTIES
		SKIP_PRECOMPILE_HEADERS ON)
	target_precompile_headers(PulseForgeEditorUI PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Source/Private/EditorUIPCH.h")

	add_executable(PulseForgeEditor WIN32
		PulseForgeEditor/Source/Private/EditorMain.cpp
		PulseForgeEditor/Source/Private/EditorApplication.cpp
	)
	get_target_property(PULSEFORGE_EDITOR_GUI_SUBSYSTEM PulseForgeEditor WIN32_EXECUTABLE)
	if(NOT PULSEFORGE_EDITOR_GUI_SUBSYSTEM)
		message(FATAL_ERROR "PulseForgeEditor must use the Windows GUI subsystem to avoid a separate console window.")
	endif()
	target_include_directories(PulseForgeEditor PRIVATE
		PulseForgeEditor/Source/Public
		PulseForge/Vendor/json/include
		PulseForge/Vendor/imgui/misc/freetype
	)
	target_compile_definitions(PulseForgeEditor PRIVATE PF_PLATFORM_WINDOWS)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_include_directories(PulseForgeEditor PRIVATE PulseForge/Vendor/Glad/include)
		target_compile_definitions(PulseForgeEditor PRIVATE PF_EDITOR_RENDERER_OPENGL)
	else()
		target_compile_definitions(PulseForgeEditor PRIVATE PF_EDITOR_SHADER_DIRECTORY="Shaders")
	endif()
	target_link_libraries(PulseForgeEditor PRIVATE
		PulseForgeEditorCore PulseForgeEditorUI PulseForge PulseForgeImGui nfd::nfd spdlog::spdlog shell32 ole32 dwmapi)
	add_custom_command(TARGET PulseForgeEditor POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeEditor>/EditorAssets"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Fonts"
			"$<TARGET_FILE_DIR:PulseForgeEditor>/EditorAssets/Fonts"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Icons"
			"$<TARGET_FILE_DIR:PulseForgeEditor>/EditorAssets/Icons"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/ThirdParty"
			"$<TARGET_FILE_DIR:PulseForgeEditor>/EditorAssets/ThirdParty"
		VERBATIM
		COMMENT "Copying bundled PulseForge editor fonts, icons, and notices"
	)
	set_property(TARGET PulseForgeEditor PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeEditor>")
endif()
