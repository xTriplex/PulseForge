if(NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" OR NOT TARGET PulseForgeCookedGame)
	return()
endif()

if(PULSEFORGE_PLATFORM_WINDOWS)
	if(CMAKE_GENERATOR_PLATFORM AND NOT CMAKE_GENERATOR_PLATFORM MATCHES "^(x64|X64)$")
		message(FATAL_ERROR "Player export templates currently support Windows x64 only")
	endif()
	set(PF_TEMPLATE_PLATFORM "Windows")
	set(PF_TEMPLATE_ARCH "x64")
	set(PF_TEMPLATE_ROOT "$<TARGET_FILE_DIR:PulseForgeCookedGame>")
elseif(PULSEFORGE_PLATFORM_LINUX)
	if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
		message(FATAL_ERROR "Player export templates currently support Linux x64 only")
	endif()
	set(PF_TEMPLATE_PLATFORM "Linux")
	set(PF_TEMPLATE_ARCH "x64")
	set(PF_TEMPLATE_ROOT "$<TARGET_FILE_DIR:PulseForgeCookedGame>")
elseif(PULSEFORGE_PLATFORM_MACOS)
	set(PF_TEMPLATE_PLATFORM "macOS")
	if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
		set(PF_TEMPLATE_ARCH "arm64")
	else()
		message(FATAL_ERROR "The macOS player template currently supports Apple Silicon arm64 only")
	endif()
	set(PF_TEMPLATE_ROOT "$<TARGET_BUNDLE_DIR:PulseForgeCookedGame>")
else()
	return()
endif()

if(NOT PULSEFORGE_COOKED_GAME_RESOURCE_DIRECTORY)
	message(FATAL_ERROR "Cooked player resource directory was not configured")
endif()

add_custom_target(PulseForgeCookedPlayerTemplate
	COMMAND "${CMAKE_COMMAND}"
		"-DPF_TEMPLATE_ROOT=${PF_TEMPLATE_ROOT}"
		"-DPF_PLAYER_FILE=$<TARGET_FILE:PulseForgeCookedGame>"
		"-DPF_RESOURCE_DIR=${PULSEFORGE_COOKED_GAME_RESOURCE_DIRECTORY}"
		"-DPF_SOURCE_ROOT=${PROJECT_SOURCE_DIR}"
		"-DPF_LUA_SOURCE_DIR=${PULSEFORGE_LUA_SOURCE_DIR}"
		"-DPF_NVRHI_SOURCE_DIR=${pulseforgenvrhi_SOURCE_DIR}"
		"-DPF_CONFIGURATION=$<CONFIG>"
		"-DPF_PLATFORM=${PF_TEMPLATE_PLATFORM}"
		"-DPF_ARCHITECTURE=${PF_TEMPLATE_ARCH}"
		"-DPF_DESCRIPTOR=$<TARGET_FILE_DIR:PulseForgeCookedGame>/PulseForgePlayerTemplate.json"
		-P "${CMAKE_CURRENT_LIST_DIR}/PulseForgeWritePlayerTemplate.cmake"
	DEPENDS PulseForgeCookedGame
	VERBATIM
	COMMENT "Refreshing compatible PulseForge cooked-player template metadata")
