if(PULSEFORGE_BUILD_EDITOR)
	set(PULSEFORGE_BUILD_IDENTIFIER "source-archive" CACHE STRING "Build identifier written to diagnostics captures")
	if(PULSEFORGE_BUILD_IDENTIFIER STREQUAL "source-archive")
		find_package(Git QUIET)
		if(GIT_FOUND)
			execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=12 HEAD
				WORKING_DIRECTORY "${PROJECT_SOURCE_DIR}" OUTPUT_VARIABLE PULSEFORGE_DETECTED_BUILD_ID
				OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
			if(PULSEFORGE_DETECTED_BUILD_ID)
				set(PULSEFORGE_BUILD_IDENTIFIER "${PULSEFORGE_DETECTED_BUILD_ID}")
			endif()
		endif()
	endif()
	add_library(PulseForgeEditorUI STATIC
		PulseForgeEditor/Source/Private/EditorAssetDestinationDialog.cpp
		PulseForgeEditor/Source/Private/EditorAssetPreviewService.cpp
		PulseForgeEditor/Source/Private/EditorAssetPreviewStressHarness.cpp
		PulseForgeEditor/Source/Private/EditorAssetSelector.cpp
		PulseForgeEditor/Source/Private/EditorConsolePanel.cpp
		PulseForgeEditor/Source/Private/EditorDocumentHostTestEditor.cpp
		PulseForgeEditor/Source/Private/EditorPerformancePanel.cpp
		PulseForgeEditor/Source/Private/EditorPerformanceCaptureService.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserAssetView.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserNavigation.cpp
		PulseForgeEditor/Source/Private/EditorContentBrowserPanel.cpp
		PulseForgeEditor/Source/Private/EditorHierarchyPanel.cpp
		PulseForgeEditor/Source/Private/EditorInputMappingPanel.cpp
		PulseForgeEditor/Source/Private/EditorInspectorPanel.cpp
		PulseForgeEditor/Source/Private/EditorMaterialDetailsPanel.cpp
		PulseForgeEditor/Source/Private/EditorStatusPanel.cpp
		PulseForgeEditor/Source/Private/EditorViewportMouseCapture.cpp
		PulseForgeEditor/Source/Private/EditorViewportScrollInput.cpp
		PulseForgeEditor/Source/Private/EditorViewportSceneRenderView.cpp
		PulseForgeEditor/Source/Private/EditorViewportToolbar.cpp
		PulseForgeEditor/Source/Private/EditorImGuiRenderer.cpp
		PulseForgeEditor/Source/Private/EditorMainMenuPanel.cpp
		PulseForgeEditor/Source/Private/EditorLayout.cpp
		PulseForgeEditor/Source/Private/EditorDockingPolicy.cpp
		PulseForgeEditor/Source/Private/EditorStyle.cpp
		PulseForgeEditor/Source/Private/EditorWorkspacePresentation.cpp
		PulseForgeEditor/Source/Private/EditorPlatformServices.cpp
		PulseForgeEditor/Source/Private/EditorPlayerProcess.cpp
		PulseForge/Vendor/imgui/misc/freetype/imgui_freetype.cpp
	)
	if(WIN32)
		target_sources(PulseForgeEditorUI PRIVATE
			PulseForgeEditor/Source/Private/Platform/Windows/EditorPlatformServices.cpp
			PulseForgeEditor/Source/Private/Platform/Windows/EditorPlayerProcess.cpp)
	elseif(PULSEFORGE_PLATFORM_LINUX)
		target_sources(PulseForgeEditorUI PRIVATE
			PulseForgeEditor/Source/Private/Platform/Linux/EditorPlatformServices.cpp
			PulseForgeEditor/Source/Private/Platform/Linux/EditorPlayerProcess.cpp)
	elseif(PULSEFORGE_PLATFORM_MACOS)
		target_sources(PulseForgeEditorUI PRIVATE
			PulseForgeEditor/Source/Private/Platform/MacOS/EditorPlatformServices.cpp
			PulseForgeEditor/Source/Private/Platform/MacOS/EditorPlayerProcess.cpp)
	else()
		message(FATAL_ERROR "No EditorPlatformServices implementation is available for this target platform.")
	endif()
	target_include_directories(PulseForgeEditorUI PUBLIC
		PulseForgeEditor/Source/Public
	)
	target_include_directories(PulseForgeEditorUI PRIVATE
		PulseForge/Vendor/imgui/misc/freetype
	)
	target_include_directories(PulseForgeEditorUI SYSTEM PRIVATE
		PulseForge/Vendor/json/include
		PulseForge/Vendor/spdlog/include
	)
	target_link_libraries(PulseForgeEditorUI
		PUBLIC PulseForgeRenderer PulseForgeEditorCore
		PRIVATE PulseForgeCore PulseForgeInput PulseForgeApplication PulseForgeAssetImporters PulseForgeAssetServices
			PulseForgeAssetDependencyCatalog
			PulseForgeImGui PulseForgeSceneRenderer freetype spdlog::spdlog
			nfd::nfd
	)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_compile_definitions(PulseForgeEditorUI PRIVATE PF_EDITOR_RENDERER_OPENGL)
	endif()
	set_source_files_properties(PulseForge/Vendor/imgui/misc/freetype/imgui_freetype.cpp PROPERTIES
		SKIP_PRECOMPILE_HEADERS ON)
	target_precompile_headers(PulseForgeEditorUI PRIVATE
		"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Source/Public/Editor/EditorUIPCH.h")

	if(WIN32)
		add_executable(PulseForgeEditor WIN32
			PulseForgeEditor/Source/Private/EditorMain.cpp
			PulseForgeEditor/Source/Private/EditorApplication.cpp
			PulseForgeEditor/Source/Private/EditorPlayerExecutablePath.cpp
		)
	else()
		add_executable(PulseForgeEditor
			PulseForgeEditor/Source/Private/EditorMain.cpp
			PulseForgeEditor/Source/Private/EditorApplication.cpp
			PulseForgeEditor/Source/Private/EditorPlayerExecutablePath.cpp
		)
	endif()
	target_compile_definitions(PulseForgeEditor PRIVATE
		${PULSEFORGE_PLATFORM_COMPILE_DEFINITION}
		PF_BUILD_IDENTIFIER="${PULSEFORGE_BUILD_IDENTIFIER}")
	get_target_property(PULSEFORGE_EDITOR_COMPILE_DEFINITIONS PulseForgeEditor COMPILE_DEFINITIONS)
	if(NOT "${PULSEFORGE_PLATFORM_COMPILE_DEFINITION}" IN_LIST PULSEFORGE_EDITOR_COMPILE_DEFINITIONS)
		message(FATAL_ERROR "PulseForgeEditor must compile with ${PULSEFORGE_PLATFORM_COMPILE_DEFINITION}.")
	endif()
	if(PULSEFORGE_PLATFORM_MACOS)
		set_target_properties(PulseForgeEditor PROPERTIES
			MACOSX_BUNDLE TRUE
			MACOSX_BUNDLE_GUI_IDENTIFIER org.pulseforge.editor)
		target_sources(PulseForgeEditor PRIVATE
			$<$<CONFIG:Debug>:PulseForgeEditor/Source/Private/Platform/MacOS/MacOSDocumentCloseEventObserver.m>)
		target_link_libraries(PulseForgeEditor PRIVATE "-framework AppKit")
	endif()
	if(WIN32)
		get_target_property(PULSEFORGE_EDITOR_GUI_SUBSYSTEM PulseForgeEditor WIN32_EXECUTABLE)
		if(NOT PULSEFORGE_EDITOR_GUI_SUBSYSTEM)
			message(FATAL_ERROR "PulseForgeEditor must use the Windows GUI subsystem to avoid a separate console window.")
		endif()
	endif()
	target_include_directories(PulseForgeEditor PRIVATE
		PulseForgeEditor/Source/Public
		PulseForge/Vendor/json/include
		PulseForge/Vendor/imgui/misc/freetype
	)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_include_directories(PulseForgeEditor PRIVATE PulseForge/Vendor/Glad/include)
		target_compile_definitions(PulseForgeEditor PRIVATE PF_EDITOR_RENDERER_OPENGL)
	else()
		target_compile_definitions(PulseForgeEditor PRIVATE PF_EDITOR_SHADER_DIRECTORY="Shaders")
	endif()
	target_link_libraries(PulseForgeEditor PRIVATE
		PulseForgeEditorCore PulseForgeEditorUI PulseForge PulseForgeInput PulseForgeImGui spdlog::spdlog)
	if(WIN32)
		target_link_libraries(PulseForgeEditor PRIVATE shell32 ole32 dwmapi)
	endif()
	pulseforge_runtime_resource_directory(PulseForgeEditor PULSEFORGE_EDITOR_RESOURCE_DIRECTORY)
	pulseforge_attach_asset_deployment(PulseForgeEditor "${PULSEFORGE_EDITOR_RESOURCE_DIRECTORY}/EditorAssets"
		DIRECTORIES
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Fonts" "Fonts"
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Icons" "Icons"
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/ThirdParty" "ThirdParty"
	)
	pulseforge_apply_visual_studio_debugger_working_directory(PulseForgeEditor)
endif()

if(PULSEFORGE_ENABLE_MACOS_VULKAN_PROOF)
	# Compile the production editor ImGui renderer independently of the unfinished
	# macOS editor shell so the G04 proof exercises its real engine integration.
	add_library(PulseForgeEditorImGuiProofRenderer STATIC
		PulseForgeEditor/Source/Private/EditorImGuiRenderer.cpp
	)
	target_include_directories(PulseForgeEditorImGuiProofRenderer
		PUBLIC PulseForgeEditor/Source/Public
		PRIVATE PulseForge/Vendor/imgui
	)
	target_link_libraries(PulseForgeEditorImGuiProofRenderer
		PUBLIC PulseForge PulseForgeImGui
	)
endif()
