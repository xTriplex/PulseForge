add_executable(PulseForgeGame
	PulseForgeGame/Source/Private/Client/PulseForgeGame.cpp
	PulseForgeGame/Source/Private/Client/PlayerLaunchConfiguration.cpp
)
if(PULSEFORGE_PLATFORM_MACOS)
	set_target_properties(PulseForgeGame PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_GUI_IDENTIFIER org.pulseforge.player)
endif()
target_include_directories(PulseForgeGame PRIVATE PulseForgeGame/Source/Public)
target_compile_definitions(PulseForgeGame PRIVATE ${PULSEFORGE_PLATFORM_COMPILE_DEFINITION} "PF_SAMPLE_SHADER_DIRECTORY=\"Shaders\""
	"PF_BUILD_CONFIGURATION=\"$<CONFIG>\"")
if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "OpenGL")
	target_compile_definitions(PulseForgeGame PRIVATE PF_SAMPLE_RENDERER_OPENGL)
elseif(NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
	message(FATAL_ERROR "PULSEFORGE_SAMPLE_RENDERER must be Vulkan or OpenGL")
endif()
target_link_libraries(PulseForgeGame PRIVATE PulseForge PulseForgeCookedRuntimeProvider)

# A separately linked cooked Player intentionally has no PulseForge umbrella dependency: that umbrella includes
# source-project loaders, registry services and importers for the developer Player route above.
add_executable(PulseForgeCookedGame
	PulseForgeGame/Source/Private/Client/PulseForgeGame.cpp
	PulseForgeGame/Source/Private/Client/PlayerLaunchConfiguration.cpp
)
if(PULSEFORGE_PLATFORM_MACOS)
	set_target_properties(PulseForgeCookedGame PROPERTIES
		MACOSX_BUNDLE TRUE
		MACOSX_BUNDLE_GUI_IDENTIFIER org.pulseforge.cooked-player)
endif()
target_include_directories(PulseForgeCookedGame PRIVATE PulseForgeGame/Source/Public)
target_compile_definitions(PulseForgeCookedGame PRIVATE
	${PULSEFORGE_PLATFORM_COMPILE_DEFINITION} PF_COOKED_RUNTIME_ONLY "PF_SAMPLE_SHADER_DIRECTORY=\"Shaders\""
	"PF_BUILD_CONFIGURATION=\"$<CONFIG>\"")
target_link_libraries(PulseForgeCookedGame PRIVATE PulseForgeApplication PulseForgeCookedRuntime
	PulseForgeCookedSceneRenderer PulseForgeCookedRuntimeProvider)

include("${CMAKE_CURRENT_LIST_DIR}/PulseForgeRuntimeTargetClosure.cmake")

foreach(ForbiddenTarget IN ITEMS
		PulseForge PulseForgeAssetImporters PulseForgeAssetServices PulseForgeAssetDependencyCatalog
		PulseForgeAssetCpuProducts PulseForgeDerivedData)
	pulseforge_assert_no_runtime_target_dependency(PulseForgeCookedGame "${ForbiddenTarget}")
endforeach()
