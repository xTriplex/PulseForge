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
target_compile_definitions(PulseForgeGame PRIVATE ${PULSEFORGE_PLATFORM_COMPILE_DEFINITION} "PF_SAMPLE_SHADER_DIRECTORY=\"Shaders\"")
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
	${PULSEFORGE_PLATFORM_COMPILE_DEFINITION} PF_COOKED_RUNTIME_ONLY "PF_SAMPLE_SHADER_DIRECTORY=\"Shaders\"")
target_link_libraries(PulseForgeCookedGame PRIVATE PulseForgeApplication PulseForgeCookedRuntime
	PulseForgeCookedSceneRenderer PulseForgeCookedRuntimeProvider)

function(pulseforge_assert_no_runtime_target_dependency RootTarget ForbiddenTarget)
	set(PendingTargets "${RootTarget}")
	set(VisitedTargets)
	while(PendingTargets)
		list(POP_FRONT PendingTargets CurrentTarget)
		if(CurrentTarget IN_LIST VisitedTargets)
			continue()
		endif()
		list(APPEND VisitedTargets "${CurrentTarget}")
		get_target_property(LinkedItems "${CurrentTarget}" LINK_LIBRARIES)
		foreach(LinkedItem IN LISTS LinkedItems)
			string(REGEX MATCH "PulseForge[A-Za-z0-9_]+" DependencyTarget "${LinkedItem}")
			if(NOT DependencyTarget)
				continue()
			endif()
			if("${DependencyTarget}" STREQUAL "${ForbiddenTarget}")
				message(FATAL_ERROR "${RootTarget} cooked runtime closure must not link ${ForbiddenTarget}")
			endif()
			if(TARGET "${DependencyTarget}")
				list(APPEND PendingTargets "${DependencyTarget}")
			endif()
		endforeach()
	endwhile()
endfunction()

foreach(ForbiddenTarget IN ITEMS
		PulseForge PulseForgeAssetImporters PulseForgeAssetServices PulseForgeAssetDependencyCatalog
		PulseForgeAssetCpuProducts PulseForgeDerivedData)
	pulseforge_assert_no_runtime_target_dependency(PulseForgeCookedGame "${ForbiddenTarget}")
endforeach()
