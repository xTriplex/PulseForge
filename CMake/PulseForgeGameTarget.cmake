add_executable(PulseForgeGame PulseForgeGame/Source/Private/Client/PulseForgeGame.cpp)
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
target_link_libraries(PulseForgeGame PRIVATE PulseForge)
