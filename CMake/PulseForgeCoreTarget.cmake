if(PULSEFORGE_PLATFORM_WINDOWS)
	set(PULSEFORGE_CORE_PLATFORM_SOURCE PulseForge/Source/Private/Core/Platform/Windows/PlatformServices.cpp)
elseif(PULSEFORGE_PLATFORM_LINUX)
	set(PULSEFORGE_CORE_PLATFORM_SOURCE PulseForge/Source/Private/Core/Platform/Linux/PlatformServices.cpp)
elseif(PULSEFORGE_PLATFORM_MACOS)
	set(PULSEFORGE_CORE_PLATFORM_SOURCE PulseForge/Source/Private/Core/Platform/MacOS/PlatformServices.cpp)
endif()

add_library(PulseForgeCore STATIC
	PulseForge/Source/Private/Core/BoundedInput.cpp
	PulseForge/Source/Private/Core/Diagnostics.cpp
	PulseForge/Source/Private/Core/Log.cpp
	PulseForge/Source/Private/Core/Input.cpp
	PulseForge/Source/Private/Core/Layer.cpp
	PulseForge/Source/Private/Core/LayerStack.cpp
	PulseForge/Source/Private/Scene/UUID.cpp
	${PULSEFORGE_CORE_PLATFORM_SOURCE}
)
target_include_directories(PulseForgeCore PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeCore PRIVATE spdlog::spdlog)

if(PULSEFORGE_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeCore PRIVATE shell32 ole32)
endif()
target_precompile_headers(PulseForgeCore PRIVATE "${PULSEFORGE_ENGINE_PCH}")
