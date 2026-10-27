function(pulseforge_stage_macos_vulkan_runtime Target ResourceDirectory Renderer)
	if(NOT PULSEFORGE_PLATFORM_MACOS OR NOT Renderer STREQUAL "Vulkan")
		return()
	endif()
	get_target_property(IS_MACOS_BUNDLE ${Target} MACOSX_BUNDLE)
	if(NOT IS_MACOS_BUNDLE)
		message(FATAL_ERROR "${Target} must be a macOS bundle to stage the Vulkan runtime")
	endif()
	add_custom_command(TARGET ${Target} POST_BUILD
		COMMAND "${CMAKE_COMMAND}"
			"-DPF_APP_BUNDLE=$<TARGET_BUNDLE_DIR:${Target}>"
			"-DPF_EXECUTABLE=$<TARGET_FILE:${Target}>"
			"-DPF_RESOURCE_DIR=${ResourceDirectory}"
			-P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulseForgeStageMacOSVulkanRuntime.cmake"
		VERBATIM
		COMMENT "Staging Vulkan loader and MoltenVK in ${Target} bundle")
endfunction()
