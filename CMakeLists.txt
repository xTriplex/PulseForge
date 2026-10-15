cmake_minimum_required(VERSION 3.24)

project(PulseForge LANGUAGES C CXX)

set(PULSEFORGE_SAMPLE_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeGame (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_SAMPLE_RENDERER PROPERTY STRINGS Vulkan OpenGL)
option(PULSEFORGE_BUILD_EDITOR "Build the experimental PulseForge editor shell" ON)
option(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS "Build opt-in Vulkan GPU validation smoke tests" OFF)
option(PULSEFORGE_ENABLE_QUALITY_CHECKS "Enable explicitly requested source, architecture, formatting, and analysis checks" OFF)
option(PULSEFORGE_ENABLE_MSVC_ANALYSIS "Enable selected MSVC static analysis checks" OFF)
set(PULSEFORGE_EDITOR_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeEditor (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_EDITOR_RENDERER PROPERTY STRINGS Vulkan OpenGL)

if(PULSEFORGE_BUILD_EDITOR AND NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan" AND
	NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
	message(FATAL_ERROR "PULSEFORGE_EDITOR_RENDERER must be Vulkan or OpenGL")
endif()

if(NOT WIN32)
	message(FATAL_ERROR "PulseForge currently supports Windows only; desktop platform support is being implemented separately.")
endif()

if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS AND
	NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" AND
	(NOT PULSEFORGE_BUILD_EDITOR OR NOT PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
	message(FATAL_ERROR "PULSEFORGE_BUILD_GPU_VALIDATION_TESTS requires the Vulkan renderer and its pinned DXC shader path")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(PULSEFORGE_ENGINE_PCH "${CMAKE_CURRENT_SOURCE_DIR}/PulseForge/Source/Private/Core/PulseForgePCH.h")

if(CMAKE_CONFIGURATION_TYPES)
	list(APPEND CMAKE_CONFIGURATION_TYPES Dist)
	list(REMOVE_DUPLICATES CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_CONFIGURATION_TYPES "${CMAKE_CONFIGURATION_TYPES}" CACHE STRING "Available build configurations" FORCE)
endif()

set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(PulseForge/Vendor/Glfw)

# Build only Jolt's CPU physics library. PulseForge owns the engine-facing
# integration; Jolt remains a private implementation dependency.
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_DXC OFF CACHE BOOL "" FORCE)
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(DEBUG_RENDERER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DEBUG_AND_RELEASE OFF CACHE BOOL "" FORCE)
set(PROFILER_IN_DISTRIBUTION OFF CACHE BOOL "" FORCE)
set(ENABLE_OBJECT_STREAM OFF CACHE BOOL "" FORCE)
set(USE_AVX512 OFF CACHE BOOL "" FORCE)
set(USE_AVX2 OFF CACHE BOOL "" FORCE)
set(USE_AVX OFF CACHE BOOL "" FORCE)
set(USE_SSE4_2 OFF CACHE BOOL "" FORCE)
set(USE_SSE4_1 OFF CACHE BOOL "" FORCE)
set(USE_LZCNT OFF CACHE BOOL "" FORCE)
set(USE_TZCNT OFF CACHE BOOL "" FORCE)
set(USE_F16C OFF CACHE BOOL "" FORCE)
set(USE_FMADD OFF CACHE BOOL "" FORCE)
add_subdirectory(PulseForge/Vendor/JoltPhysics/Build "${CMAKE_BINARY_DIR}/_deps/JoltPhysics-build" EXCLUDE_FROM_ALL)

# NVRHI is an implementation dependency of the Vulkan renderer backend. Keep
# its upstream source revision pinned and build only the backend PulseForge
# currently uses.
include(FetchContent)
set(NVRHI_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(NVRHI_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NVRHI_INSTALL OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_VALIDATION ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_VULKAN ON CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX11 OFF CACHE BOOL "" FORCE)
set(NVRHI_WITH_DX12 OFF CACHE BOOL "" FORCE)
set(NVRHI_FETCH_VULKAN_HEADERS ON CACHE BOOL "" FORCE)
set(NVRHI_VULKAN_HEADERS_GIT_TAG v1.4.352 CACHE STRING "" FORCE)
FetchContent_Declare(PulseForgeNVRHI
	GIT_REPOSITORY https://github.com/NVIDIA-RTX/NVRHI.git
	GIT_TAG 6b96fb03e07539f08327aea76c56d55f1de9d906
)
FetchContent_MakeAvailable(PulseForgeNVRHI)

# Lua is embedded through PulseForge's narrow C API bindings. Pin the official
# release archive and verify it before compiling the VM as a private dependency.
FetchContent_Declare(PulseForgeLua
	URL https://www.lua.org/ftp/lua-5.5.1.tar.gz
	URL_HASH SHA256=1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce
	DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(PulseForgeLua)
set(PULSEFORGE_LUA_SOURCE_DIR "${pulseforgelua_SOURCE_DIR}/src")
add_library(PulseForgeLua STATIC
	${PULSEFORGE_LUA_SOURCE_DIR}/lapi.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lcode.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lctype.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ldebug.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ldo.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ldump.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lfunc.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lgc.c
	${PULSEFORGE_LUA_SOURCE_DIR}/llex.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lmem.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lobject.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lopcodes.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lparser.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lstate.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lstring.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ltable.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ltm.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lundump.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lvm.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lzio.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lauxlib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lbaselib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lmathlib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lstrlib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/ltablib.c
	${PULSEFORGE_LUA_SOURCE_DIR}/lutf8lib.c
)
target_include_directories(PulseForgeLua PUBLIC "${PULSEFORGE_LUA_SOURCE_DIR}")
set_target_properties(PulseForgeLua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)

set(SPDLOG_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(SPDLOG_BUILD_BENCH OFF CACHE BOOL "" FORCE)
add_subdirectory(PulseForge/Vendor/spdlog)
target_compile_options(spdlog PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
)

add_library(PulseForgeGlad STATIC PulseForge/Vendor/Glad/src/glad.c)
target_include_directories(PulseForgeGlad PUBLIC PulseForge/Vendor/Glad/include)
target_compile_options(PulseForgeGlad PRIVATE
	$<$<AND:$<C_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
)

if(PULSEFORGE_BUILD_EDITOR)
	set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
	set(NFD_BUILD_SDL2_TESTS OFF CACHE BOOL "" FORCE)
	set(NFD_BUILD_GLFW3_TESTS OFF CACHE BOOL "" FORCE)
	set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
	add_subdirectory(PulseForge/Vendor/nativefiledialog)

	# The current Font Awesome Free desktop font uses CFF outlines in an OTF.
	# Reuse the repository's pinned FreeType copy for the editor's font atlas;
	# compressed-font features are disabled because bundled assets are plain local TTF/OTF files.
	set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
	set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
	add_subdirectory(PulseForge/Vendor/freetype "${CMAKE_BINARY_DIR}/_deps/freetype-build" EXCLUDE_FROM_ALL)

	set(PULSEFORGE_IMGUI_SOURCES
		PulseForge/Vendor/imgui/imgui.cpp
		PulseForge/Vendor/imgui/imgui_draw.cpp
		PulseForge/Vendor/imgui/imgui_tables.cpp
		PulseForge/Vendor/imgui/imgui_widgets.cpp
		PulseForge/Vendor/imgui/imgui_demo.cpp
		PulseForge/Vendor/imgui/misc/cpp/imgui_stdlib.cpp
		PulseForge/Vendor/imgui/backends/imgui_impl_glfw.cpp
	)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		list(APPEND PULSEFORGE_IMGUI_SOURCES PulseForge/Vendor/imgui/backends/imgui_impl_opengl3.cpp)
	endif()
	add_library(PulseForgeImGui STATIC ${PULSEFORGE_IMGUI_SOURCES})
	target_include_directories(PulseForgeImGui PUBLIC
		PulseForge/Vendor/imgui
		PulseForge/Vendor/imgui/backends
	)
	target_link_libraries(PulseForgeImGui PUBLIC glfw)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_link_libraries(PulseForgeImGui PUBLIC opengl32)
	endif()
	target_compile_options(PulseForgeImGui PRIVATE
		$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
		$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
	)
endif()

add_library(PulseForgeCore STATIC
	PulseForge/Source/Private/Core/Log.cpp
	PulseForge/Source/Private/Core/Input.cpp
	PulseForge/Source/Private/Core/Layer.cpp
	PulseForge/Source/Private/Core/LayerStack.cpp
	PulseForge/Source/Private/Scene/UUID.cpp
)
target_include_directories(PulseForgeCore PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeCore PRIVATE PF_PLATFORM_WINDOWS)
target_link_libraries(PulseForgeCore PRIVATE spdlog::spdlog)
target_precompile_headers(PulseForgeCore PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeWindow STATIC
	PulseForge/Source/Private/Window/Window.cpp
	PulseForge/Source/Private/Window/Platform/Windows/WindowsWindow.cpp
)
target_include_directories(PulseForgeWindow
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/Glfw/include
)
target_compile_definitions(PulseForgeWindow PRIVATE PF_PLATFORM_WINDOWS GLFW_INCLUDE_NONE)
target_link_libraries(PulseForgeWindow PUBLIC PulseForgeCore PRIVATE glfw spdlog::spdlog)
add_library(PulseForgeScene STATIC
	PulseForge/Source/Private/Scene/CameraComponent.cpp
	PulseForge/Source/Private/Scene/Components/AudioSourceComponent.cpp
	PulseForge/Source/Private/Scene/Components/BoxColliderComponent.cpp
	PulseForge/Source/Private/Scene/Components/PointLightComponent.cpp
	PulseForge/Source/Private/Scene/Components/RigidbodyComponent.cpp
	PulseForge/Source/Private/Scene/Components/ScriptComponent.cpp
	PulseForge/Source/Private/Scene/Components/SpotLightComponent.cpp
	PulseForge/Source/Private/Scene/Scene.cpp
	PulseForge/Source/Private/Scene/SceneRenderSnapshot.cpp
	PulseForge/Source/Private/Scene/TransformComponent.cpp
)
target_include_directories(PulseForgeScene
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE PulseForge/Vendor/entt/src PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeScene PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeScene PUBLIC PulseForgeCore)
add_library(PulseForgeSceneSerialization STATIC PulseForge/Source/Private/Scene/SceneSerializer.cpp)
target_include_directories(PulseForgeSceneSerialization
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeSceneSerialization PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeSceneSerialization PUBLIC PulseForgeScene)
add_library(PulseForgeMeshData STATIC PulseForge/Source/Private/Renderer/MeshData.cpp)
target_include_directories(PulseForgeMeshData PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeMeshData PRIVATE PF_PLATFORM_WINDOWS)
add_library(PulseForgeRenderMath STATIC
	PulseForge/Source/Private/Renderer/DirectionalShadowMath.cpp
	PulseForge/Source/Private/Renderer/SpotlightShadowMath.cpp
	PulseForge/Source/Private/Renderer/PointLightShadowMath.cpp
	PulseForge/Source/Private/Renderer/ScreenSpaceAmbientOcclusion.cpp
)
target_include_directories(PulseForgeRenderMath PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeRenderMath PRIVATE PF_PLATFORM_WINDOWS)
target_link_libraries(PulseForgeRenderMath PUBLIC PulseForgeScene)
add_library(PulseForgeAssetCatalog STATIC
	PulseForge/Source/Private/Assets/AssetMetadata.cpp
	PulseForge/Source/Private/Assets/AssetOperations.cpp
	PulseForge/Source/Private/Assets/AssetPathResolver.cpp
	PulseForge/Source/Private/Assets/AssetReferenceValidator.cpp
	PulseForge/Source/Private/Assets/AssetRegistry.cpp
)
target_include_directories(PulseForgeAssetCatalog
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeAssetCatalog PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(PulseForgeAssetCatalog PUBLIC PulseForgeCore PulseForgeScene)
add_library(PulseForgeAudio STATIC
	PulseForge/Source/Private/Audio/AudioEngine.cpp
	PulseForge/Source/Private/Audio/MiniAudio/MiniAudio.cpp
)
target_include_directories(PulseForgeAudio
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE PulseForge/Vendor/miniaudio
)
target_compile_definitions(PulseForgeAudio PRIVATE PF_PLATFORM_WINDOWS MA_NO_WIN32_LEAN_AND_MEAN)
add_library(PulseForgeAudioAssets STATIC PulseForge/Source/Private/Assets/AudioAssetCache.cpp)
target_include_directories(PulseForgeAudioAssets PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeAudioAssets PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeAudioAssets PUBLIC PulseForgeAssetCatalog PulseForgeAudio)
add_library(PulseForgeAssetServices STATIC
	PulseForge/Source/Private/Assets/MaterialAsset.cpp
	PulseForge/Source/Private/Assets/MaterialAssetCache.cpp
	PulseForge/Source/Private/Assets/MaterialAssetService.cpp
	PulseForge/Source/Private/Assets/PrefabAssetService.cpp
	PulseForge/Source/Private/Assets/PrefabSerializer.cpp
	PulseForge/Source/Private/Assets/Project.cpp
	PulseForge/Source/Private/Assets/SceneAssetService.cpp
)
target_include_directories(PulseForgeAssetServices
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include PulseForge/Vendor/stb
)
target_compile_definitions(PulseForgeAssetServices PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeAssetServices
	PUBLIC PulseForgeAssetCatalog
	PRIVATE PulseForgeSceneSerialization
)
target_precompile_headers(PulseForgeAssetServices PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeAudioScene STATIC PulseForge/Source/Private/Audio/AudioSceneRuntime.cpp)
target_include_directories(PulseForgeAudioScene PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeAudioScene PRIVATE PF_PLATFORM_WINDOWS)
target_link_libraries(PulseForgeAudioScene
	PUBLIC PulseForgeAudio
	PRIVATE PulseForgeAudioAssets PulseForgeScene
)
add_library(PulseForgePhysics STATIC PulseForge/Source/Private/Physics/PhysicsSceneRuntime.cpp)
target_include_directories(PulseForgePhysics PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgePhysics PRIVATE PF_PLATFORM_WINDOWS NOMINMAX)
target_link_libraries(PulseForgePhysics PUBLIC PulseForgeScene PRIVATE Jolt spdlog::spdlog)
add_library(PulseForgeScripting STATIC PulseForge/Source/Private/Scripting/ScriptRuntime.cpp)
target_include_directories(PulseForgeScripting
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeScripting PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(PulseForgeScripting
	PUBLIC PulseForgeAssetCatalog
	PRIVATE PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics PulseForgeCore PulseForgeLua spdlog::spdlog
)
add_library(PulseForgeRuntime STATIC PulseForge/Source/Private/Runtime/SceneRuntime.cpp)
target_include_directories(PulseForgeRuntime PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeRuntime PRIVATE PF_PLATFORM_WINDOWS)
target_link_libraries(PulseForgeRuntime
	PUBLIC PulseForgeAudio PulseForgePhysics PulseForgeScripting
	PRIVATE PulseForgeAssetServices PulseForgeAudioAssets PulseForgeAudioScene PulseForgeScene
)
add_library(PulseForgeRenderer STATIC
	PulseForge/Source/Private/Renderer/OpenGLRenderer.cpp
	PulseForge/Source/Private/Renderer/Buffer.cpp
	PulseForge/Source/Private/Renderer/Mesh.cpp
	PulseForge/Source/Private/Renderer/RenderDevice.cpp
	PulseForge/Source/Private/Renderer/Texture.cpp
	PulseForge/Source/Private/Renderer/RenderTarget.cpp
	PulseForge/Source/Private/Renderer/Binding.cpp
	PulseForge/Source/Private/Renderer/Graphics.cpp
	PulseForge/Source/Private/Renderer/EnvironmentLighting.cpp
	PulseForge/Source/Private/Renderer/RendererBackend.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanRenderer.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanSupport.cpp
)
target_include_directories(PulseForgeRenderer
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE
		PulseForge/Source
		PulseForge/Source/Private
		PulseForge/Vendor/Glfw/include
		PulseForge/Vendor/Glad/include
		PulseForge/Vendor/stb
)
target_compile_definitions(PulseForgeRenderer PRIVATE
	PF_PLATFORM_WINDOWS
	_CRT_SECURE_NO_WARNINGS
	GLFW_INCLUDE_NONE
)
target_link_libraries(PulseForgeRenderer
	PUBLIC PulseForgeCore PulseForgeMeshData
	PRIVATE PulseForgeWindow glfw PulseForgeGlad opengl32 nvrhi nvrhi_vk Vulkan-Headers
)
target_precompile_headers(PulseForgeRenderer PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeAssetImporters STATIC
	PulseForge/Source/Private/Assets/GltfMeshImporter.cpp
	PulseForge/Source/Private/Assets/ImageAssetImporter.cpp
	PulseForge/Source/Private/Utils/stb_image.cpp
)
target_include_directories(PulseForgeAssetImporters
	PUBLIC PulseForge/Source/Public
)
target_include_directories(PulseForgeAssetImporters SYSTEM PRIVATE
	PulseForge/Vendor/json/include
	PulseForge/Vendor/stb
)
target_compile_definitions(PulseForgeAssetImporters PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeAssetImporters PUBLIC PulseForgeAssetCatalog PulseForgeMeshData)
target_precompile_headers(PulseForgeAssetImporters PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeApplication STATIC PulseForge/Source/Private/Core/Application.cpp)
target_include_directories(PulseForgeApplication
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Source PulseForge/Source/Private
)
target_compile_definitions(PulseForgeApplication PRIVATE PF_PLATFORM_WINDOWS)
target_link_libraries(PulseForgeApplication PUBLIC PulseForgeCore PulseForgeRenderer PulseForgeWindow)
add_library(PulseForgeSceneRenderer STATIC
	PulseForge/Source/Private/Renderer/AssetCache/MeshAssetCache.cpp
	PulseForge/Source/Private/Renderer/AssetCache/TextureAssetCache.cpp
	PulseForge/Source/Private/Renderer/EnvironmentLightingCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererAmbientOcclusionPass.cpp
	PulseForge/Source/Private/Renderer/SceneRenderer.cpp
	PulseForge/Source/Private/Renderer/SceneRendererToneMappingStage.cpp
)
target_include_directories(PulseForgeSceneRenderer
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE
		PulseForge/Source
		PulseForge/Source/Private
		PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeSceneRenderer PRIVATE PF_PLATFORM_WINDOWS _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeSceneRenderer
	PUBLIC PulseForgeRenderMath PulseForgeScene PulseForgeRenderer
	PRIVATE PulseForgeAssetImporters PulseForgeAssetServices
)
target_precompile_headers(PulseForgeSceneRenderer PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForge STATIC
	PulseForge/Source/Private/Core/Core.cpp
	PulseForge/Source/Private/Core/EntryPoint.cpp
)
target_include_directories(PulseForge
	PUBLIC PulseForge/Source/Public
		PulseForge/Vendor/glm
	PRIVATE
		PulseForge/Source
		PulseForge/Source/Private
		PulseForge/Vendor/entt/src
		PulseForge/Vendor/json/include
		PulseForge/Vendor/Glfw/include
		PulseForge/Vendor/Glad/include
		PulseForge/Vendor/stb
)
target_compile_definitions(PulseForge PRIVATE
	PF_PLATFORM_WINDOWS
	_CRT_SECURE_NO_WARNINGS
	GLFW_INCLUDE_NONE
	MA_NO_WIN32_LEAN_AND_MEAN
)
target_link_libraries(PulseForge
	PUBLIC PulseForgeApplication PulseForgeSceneRenderer PulseForgeSceneSerialization PulseForgeAssetImporters
		PulseForgeAssetServices
		PulseForgeRenderMath PulseForgeRuntime PulseForgeWindow
)

add_executable(PulseForgeGame PulseForgeGame/Source/Private/Client/PulseForgeGame.cpp)
target_include_directories(PulseForgeGame PRIVATE PulseForgeGame/Source/Private)
target_compile_definitions(PulseForgeGame PRIVATE PF_PLATFORM_WINDOWS "PF_SAMPLE_SHADER_DIRECTORY=\"Shaders\"")
if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "OpenGL")
	target_compile_definitions(PulseForgeGame PRIVATE PF_SAMPLE_RENDERER_OPENGL)
elseif(NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
	message(FATAL_ERROR "PULSEFORGE_SAMPLE_RENDERER must be Vulkan or OpenGL")
endif()
target_link_libraries(PulseForgeGame PRIVATE PulseForge)

include(CTest)
if(PULSEFORGE_BUILD_EDITOR OR BUILD_TESTING)
	set(PULSEFORGE_ENGINE_TEST_LIBRARIES
		PulseForgeCore PulseForgeWindow PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath
		PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAudio PulseForgeAudioAssets
		PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics PulseForgeScripting PulseForgeRuntime
		PulseForgeRenderer PulseForgeApplication PulseForgeSceneRenderer
	)

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
		PRIVATE PulseForgeAssetImporters PulseForgeSceneSerialization
	)
endif()

if(PULSEFORGE_BUILD_EDITOR)
	add_library(PulseForgeEditorUI STATIC
		PulseForgeEditor/Source/Private/EditorAssetDestinationDialog.cpp
		PulseForgeEditor/Source/Private/EditorAssetPreviewService.cpp
		PulseForgeEditor/Source/Private/EditorAssetPreviewStressHarness.cpp
		PulseForgeEditor/Source/Private/EditorAssetSelector.cpp
		PulseForgeEditor/Source/Private/EditorConsolePanel.cpp
		PulseForgeEditor/Source/Private/EditorConsoleSeverity.cpp
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
		PUBLIC PulseForgeRenderer
		PRIVATE PulseForgeApplication PulseForgeAssetImporters PulseForgeAssetServices PulseForgeEditorCore
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

if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan" OR
	(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan"))
	# Pin the compiler executable and its archive hash instead of relying on a
	# developer-installed DXC or whatever happens to be on PATH. DXC is a
	# build-time tool only; runtime builds consume the generated SPIR-V files.
	set(PULSEFORGE_DXC_VERSION "1.9.2602.24")
	set(PULSEFORGE_DXC_RELEASE_DATE "2026_05_27")
	set(PULSEFORGE_DXC_URL
		"https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${PULSEFORGE_DXC_VERSION}/dxc_${PULSEFORGE_DXC_RELEASE_DATE}.zip")
	set(PULSEFORGE_DXC_SHA512
		"cf331112f1753fca68525c85bb4964714d85dbf7cb5e41af4720bbdffefdfddad1878c7a1a74898e2105dff32957a526d188d9ee3f18f0df310ec8548827a374")
	set(PULSEFORGE_DXC_ROOT "${CMAKE_BINARY_DIR}/_tools/dxc-${PULSEFORGE_DXC_VERSION}")
	set(PULSEFORGE_DXC "${PULSEFORGE_DXC_ROOT}/bin/x64/dxc.exe")
	if(NOT EXISTS "${PULSEFORGE_DXC}")
		set(PULSEFORGE_DXC_DOWNLOAD_DIR "${CMAKE_BINARY_DIR}/_downloads")
		set(PULSEFORGE_DXC_ARCHIVE "${PULSEFORGE_DXC_DOWNLOAD_DIR}/dxc-${PULSEFORGE_DXC_VERSION}.zip")
		file(MAKE_DIRECTORY "${PULSEFORGE_DXC_DOWNLOAD_DIR}" "${PULSEFORGE_DXC_ROOT}")
		message(STATUS "Downloading pinned Microsoft DXC ${PULSEFORGE_DXC_VERSION} for HLSL-to-SPIR-V compilation")
		file(DOWNLOAD
			"${PULSEFORGE_DXC_URL}"
			"${PULSEFORGE_DXC_ARCHIVE}"
			EXPECTED_HASH "SHA512=${PULSEFORGE_DXC_SHA512}"
			TLS_VERIFY ON
			SHOW_PROGRESS
			STATUS PULSEFORGE_DXC_DOWNLOAD_STATUS
		)
		list(GET PULSEFORGE_DXC_DOWNLOAD_STATUS 0 PULSEFORGE_DXC_DOWNLOAD_CODE)
		if(NOT PULSEFORGE_DXC_DOWNLOAD_CODE EQUAL 0)
			list(GET PULSEFORGE_DXC_DOWNLOAD_STATUS 1 PULSEFORGE_DXC_DOWNLOAD_MESSAGE)
			message(FATAL_ERROR "Could not download pinned DXC ${PULSEFORGE_DXC_VERSION}: ${PULSEFORGE_DXC_DOWNLOAD_MESSAGE}")
		endif()
		file(ARCHIVE_EXTRACT INPUT "${PULSEFORGE_DXC_ARCHIVE}" DESTINATION "${PULSEFORGE_DXC_ROOT}")
	endif()
	if(NOT EXISTS "${PULSEFORGE_DXC}")
		message(FATAL_ERROR "Pinned DXC archive did not contain the expected Windows x64 compiler: ${PULSEFORGE_DXC}")
	endif()

	set(PULSEFORGE_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Runtime/Shaders")
	set(PULSEFORGE_TRIANGLE_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Triangle.vs.spv")
	set(PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Triangle.ps.spv")
	set(PULSEFORGE_TRIANGLE_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/Triangle.hlsl")
	set(PULSEFORGE_BACKGROUND_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/EnvironmentBackground.vs.spv")
	set(PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/EnvironmentBackground.ps.spv")
	set(PULSEFORGE_BACKGROUND_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/EnvironmentBackground.hlsl")
	set(PULSEFORGE_SHADOW_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/DirectionalShadow.vs.spv")
	set(PULSEFORGE_SHADOW_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/DirectionalShadow.ps.spv")
	set(PULSEFORGE_SHADOW_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/DirectionalShadow.hlsl")
	set(PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/PointShadow.vs.spv")
	set(PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/PointShadow.ps.spv")
	set(PULSEFORGE_POINT_SHADOW_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/PointShadow.hlsl")
	set(PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoNormalDepth.vs.spv")
	set(PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoNormalDepth.ps.spv")
	set(PULSEFORGE_SSAO_PREPASS_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/SsaoNormalDepth.hlsl")
	set(PULSEFORGE_SSAO_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Ssao.vs.spv")
	set(PULSEFORGE_SSAO_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Ssao.ps.spv")
	set(PULSEFORGE_SSAO_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/Ssao.hlsl")
	set(PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoBlur.vs.spv")
	set(PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/SsaoBlur.ps.spv")
	set(PULSEFORGE_SSAO_BLUR_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/SsaoBlur.hlsl")
	set(PULSEFORGE_TONEMAPPING_VERTEX_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Tonemapping.vs.spv")
	set(PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV "${PULSEFORGE_SHADER_OUTPUT_DIR}/Tonemapping.ps.spv")
	set(PULSEFORGE_TONEMAPPING_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Shaders/Tonemapping.hlsl")
	add_custom_command(
		OUTPUT "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}"
		DEPENDS "${PULSEFORGE_TRIANGLE_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge triangle vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 1 -fvk-t-shift 0 1 -fvk-s-shift 128 1 -fvk-b-shift 256 2 -fvk-t-shift 0 2 -fvk-s-shift 128 2 -fvk-t-shift 0 3 -fvk-s-shift 128 3 -fvk-t-shift 0 5 -fvk-s-shift 128 5 -O3 -WX
			-Fo "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}"
		DEPENDS "${PULSEFORGE_TRIANGLE_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge triangle fragment shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "${PULSEFORGE_BACKGROUND_HLSL}"
		DEPENDS "${PULSEFORGE_BACKGROUND_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge environment background vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "${PULSEFORGE_BACKGROUND_HLSL}"
		DEPENDS "${PULSEFORGE_BACKGROUND_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge environment background fragment shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_SHADOW_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 4 -O3 -WX
			-Fo "${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge directional-shadow vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "${PULSEFORGE_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge alpha-tested directional/spot shadow fragment shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 4 -O3 -WX
			-Fo "${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_POINT_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_POINT_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge point-shadow vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -fvk-b-shift 256 4 -O3 -WX
			-Fo "${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "${PULSEFORGE_POINT_SHADOW_HLSL}"
		DEPENDS "${PULSEFORGE_POINT_SHADOW_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge point-shadow fragment shader to SPIR-V"
	)
	foreach(PULSEFORGE_SSAO_SHADER IN ITEMS
		"${PULSEFORGE_SSAO_PREPASS_HLSL}|${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}|vs_6_0|VSMain|normal-depth vertex"
		"${PULSEFORGE_SSAO_PREPASS_HLSL}|${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}|ps_6_0|PSMain|normal-depth fragment"
		"${PULSEFORGE_SSAO_HLSL}|${PULSEFORGE_SSAO_VERTEX_SPIRV}|vs_6_0|VSMain|SSAO vertex"
		"${PULSEFORGE_SSAO_HLSL}|${PULSEFORGE_SSAO_FRAGMENT_SPIRV}|ps_6_0|PSMain|SSAO fragment"
		"${PULSEFORGE_SSAO_BLUR_HLSL}|${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}|vs_6_0|VSMain|SSAO blur vertex"
		"${PULSEFORGE_SSAO_BLUR_HLSL}|${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}|ps_6_0|PSMain|SSAO blur fragment")
		string(REPLACE "|" ";" PULSEFORGE_SSAO_FIELDS "${PULSEFORGE_SSAO_SHADER}")
		list(GET PULSEFORGE_SSAO_FIELDS 0 PULSEFORGE_SSAO_SOURCE)
		list(GET PULSEFORGE_SSAO_FIELDS 1 PULSEFORGE_SSAO_OUTPUT)
		list(GET PULSEFORGE_SSAO_FIELDS 2 PULSEFORGE_SSAO_PROFILE)
		list(GET PULSEFORGE_SSAO_FIELDS 3 PULSEFORGE_SSAO_ENTRY)
		list(GET PULSEFORGE_SSAO_FIELDS 4 PULSEFORGE_SSAO_LABEL)
		set(PULSEFORGE_SSAO_REGISTER_SHIFTS
			-fvk-b-shift 256 4 -fvk-t-shift 0 4 -fvk-s-shift 128 4)
		if("${PULSEFORGE_SSAO_OUTPUT}" STREQUAL "${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}")
			list(APPEND PULSEFORGE_SSAO_REGISTER_SHIFTS
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0)
		endif()
		add_custom_command(
			OUTPUT "${PULSEFORGE_SSAO_OUTPUT}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T "${PULSEFORGE_SSAO_PROFILE}" -E "${PULSEFORGE_SSAO_ENTRY}"
				-fspv-target-env=vulkan1.3 ${PULSEFORGE_SSAO_REGISTER_SHIFTS} -O3 -WX
				-Fo "${PULSEFORGE_SSAO_OUTPUT}" "${PULSEFORGE_SSAO_SOURCE}"
			DEPENDS "${PULSEFORGE_SSAO_SOURCE}"
			VERBATIM
			COMMENT "Compiling PulseForge ${PULSEFORGE_SSAO_LABEL} shader to SPIR-V"
		)
	endforeach()
	add_custom_command(
		OUTPUT "${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "${PULSEFORGE_TONEMAPPING_HLSL}"
		DEPENDS "${PULSEFORGE_TONEMAPPING_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge tone-mapping vertex shader to SPIR-V"
	)
	add_custom_command(
		OUTPUT "${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_SHADER_OUTPUT_DIR}"
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
			-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "${PULSEFORGE_TONEMAPPING_HLSL}"
		DEPENDS "${PULSEFORGE_TONEMAPPING_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge tone-mapping fragment shader to SPIR-V"
	)
	add_custom_target(PulseForgeSceneShaderAssets
		DEPENDS "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}"
			"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}"
			"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}"
			"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}" "${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SSAO_VERTEX_SPIRV}" "${PULSEFORGE_SSAO_FRAGMENT_SPIRV}"
			"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}" "${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}"
			"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}")
	if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS)
		set(PULSEFORGE_GPU_VALIDATION_SHADER_DIR "${CMAKE_CURRENT_BINARY_DIR}/GpuValidationShaders")
		set(PULSEFORGE_GPU_VALIDATION_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/Tests/GpuRendererValidation.hlsl")
		set(PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/GpuRendererValidation.vs.spv")
		set(PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/GpuRendererValidation.color.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/GpuRendererValidation.sample.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGui.vs.spv")
		set(PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGui.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGui.hlsl")
		set(PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGuiPresent.vs.spv")
		set(PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}/EditorImGuiPresent.ps.spv")
		set(PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGuiPresent.hlsl")
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge GPU-validation vertex shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSColor -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge GPU-validation color shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling production editor ImGui vertex shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling production editor ImGui fragment shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling editor UI presentation vertex shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling editor UI presentation fragment shader for GPU validation"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSSample -fspv-target-env=vulkan1.3
				-fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge GPU-validation sampling shader to SPIR-V"
		)
		add_custom_target(PulseForgeGpuValidationShaders
			DEPENDS "${PULSEFORGE_GPU_VALIDATION_VERTEX_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_COLOR_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_VERTEX_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_EDITOR_IMGUI_FRAGMENT_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_SAMPLE_SPIRV}" "${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_VERTEX_SPIRV}"
				"${PULSEFORGE_GPU_VALIDATION_UI_PRESENT_FRAGMENT_SPIRV}")
	endif()
	if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
		add_dependencies(PulseForgeGame PulseForgeSceneShaderAssets)
		add_custom_command(TARGET PulseForgeGame POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Triangle.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Triangle.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/EnvironmentBackground.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/EnvironmentBackground.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/DirectionalShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/DirectionalShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/PointShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/PointShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoNormalDepth.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoNormalDepth.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Ssao.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Ssao.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoBlur.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/SsaoBlur.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Tonemapping.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Tonemapping.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Models"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Models" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Models"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Textures"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Textures" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Textures"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Environments"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Environments" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Environments"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scenes"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scenes" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scenes"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Materials"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Materials" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Materials"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scripts"
			COMMAND "${CMAKE_COMMAND}" -E copy_directory
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scripts" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scripts"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/PulseForgeGame.pfproj" "$<TARGET_FILE_DIR:PulseForgeGame>/PulseForgeGame.pfproj"
			VERBATIM
		)
		set_property(TARGET PulseForgeGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeGame>")
	endif()

	if(PULSEFORGE_BUILD_EDITOR AND PULSEFORGE_EDITOR_RENDERER STREQUAL "Vulkan")
		set(PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/Editor/Shaders")
		set(PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGui.vs.spv")
		set(PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGui.ps.spv")
		set(PULSEFORGE_EDITOR_IMGUI_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGui.hlsl")
		set(PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGuiPresent.vs.spv")
		set(PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}/EditorImGuiPresent.ps.spv")
		set(PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL "${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeEditor/Assets/Shaders/EditorImGuiPresent.hlsl")
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui vertex shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui fragment shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T vs_6_0 -E VSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui UNORM presentation vertex shader to SPIR-V"
		)
		add_custom_command(
			OUTPUT "${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}"
			COMMAND "${CMAKE_COMMAND}" -E make_directory "${PULSEFORGE_EDITOR_SHADER_OUTPUT_DIR}"
			COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
				-Fo "${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_PRESENT_HLSL}"
			VERBATIM
			COMMENT "Compiling PulseForge editor ImGui UNORM presentation fragment shader to SPIR-V"
		)
		add_custom_target(PulseForgeEditorShaderAssets
			DEPENDS "${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}"
				"${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}")
		add_dependencies(PulseForgeEditor PulseForgeSceneShaderAssets PulseForgeEditorShaderAssets)
		add_custom_command(TARGET PulseForgeEditor POST_BUILD
			COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Triangle.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Triangle.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EnvironmentBackground.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_BACKGROUND_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EnvironmentBackground.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/DirectionalShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/DirectionalShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/PointShadow.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_POINT_SHADOW_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/PointShadow.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoNormalDepth.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_PREPASS_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoNormalDepth.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Ssao.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Ssao.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoBlur.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_SSAO_BLUR_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/SsaoBlur.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Tonemapping.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_TONEMAPPING_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/Tonemapping.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGui.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGui.ps.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_PRESENT_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGuiPresent.vs.spv"
			COMMAND "${CMAKE_COMMAND}" -E copy_if_different
				"${PULSEFORGE_EDITOR_IMGUI_PRESENT_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeEditor>/Shaders/EditorImGuiPresent.ps.spv"
			VERBATIM
		)
	endif()
endif()


if(BUILD_TESTING OR PULSEFORGE_BUILD_GPU_VALIDATION_TESTS)
	add_library(PulseForgeGpuValidationAnalysis STATIC Tests/GpuValidation/PointShadowImageAnalysis.cpp)
	target_include_directories(PulseForgeGpuValidationAnalysis PUBLIC Tests)
	target_compile_definitions(PulseForgeGpuValidationAnalysis PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeGpuValidationAnalysis PUBLIC PulseForgeRenderMath)
endif()

if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
	if(NOT BUILD_TESTING)
		message(FATAL_ERROR "PULSEFORGE_ENABLE_QUALITY_CHECKS requires BUILD_TESTING=ON")
	endif()
	if(NOT MSVC)
		message(FATAL_ERROR "PulseForge quality validation requires the supported MSVC /analyze workflow")
	endif()
	if(NOT PULSEFORGE_ENABLE_MSVC_ANALYSIS)
		message(FATAL_ERROR
			"PULSEFORGE_ENABLE_QUALITY_CHECKS requires PULSEFORGE_ENABLE_MSVC_ANALYSIS=ON; "
			"ordinary builds leave compiler analysis disabled by default.")
	endif()

	set(PULSEFORGE_QUALITY_PYTHON_EXECUTABLE "" CACHE FILEPATH
		"Optional explicit Python override with pinned PulseForge quality tools installed")
	set(QualityToolRoot "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality")
	set(QualityRequiredFiles
		"${QualityToolRoot}/BootstrapQualityTools.py"
		"${QualityToolRoot}/CheckSourceQuality.py"
		"${QualityToolRoot}/CheckCodeQualityTools.py"
		"${QualityToolRoot}/ExportConfiguredTargetGraph.cmake"
		"${QualityToolRoot}/SourceQualityRules.json"
		"${QualityToolRoot}/requirements.txt"
	)
	foreach(QualityFile IN LISTS QualityRequiredFiles)
		if(NOT EXISTS "${QualityFile}")
			message(FATAL_ERROR
				"Explicit quality validation is unavailable because required file '${QualityFile}' is missing. "
				"Restore Tools/Quality or disable PULSEFORGE_ENABLE_QUALITY_CHECKS.")
		endif()
	endforeach()

	set(QualityPythonDefault "${CMAKE_CURRENT_SOURCE_DIR}/Build/QualityTools/.venv/Scripts/python.exe")
	if(PULSEFORGE_QUALITY_PYTHON_EXECUTABLE STREQUAL "")
		set(QualityPythonSelected "${QualityPythonDefault}")
	else()
		set(QualityPythonSelected "${PULSEFORGE_QUALITY_PYTHON_EXECUTABLE}")
	endif()
	if(NOT EXISTS "${QualityPythonSelected}")
		message(FATAL_ERROR
			"Explicit quality validation requires Python at '${QualityPythonSelected}'. "
			"Bootstrap the shared pinned environment with: "
			"python Tools/Quality/BootstrapQualityTools.py --venv Build/QualityTools/.venv "
			"or set PULSEFORGE_QUALITY_PYTHON_EXECUTABLE to a compatible interpreter. "
			"An explicitly supplied invalid override is not replaced automatically.")
	endif()
	execute_process(
		COMMAND "${QualityPythonSelected}" -c
			"import importlib.metadata as m, sys; print(f'{sys.version_info.major}.{sys.version_info.minor} lizard {m.version(\"lizard\")}, pathspec {m.version(\"pathspec\")}, Pygments {m.version(\"Pygments\")}')"
		OUTPUT_VARIABLE QualityPythonVersion
		ERROR_VARIABLE QualityPythonError
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE QualityPythonResult
	)
	if(NOT "${QualityPythonResult}" STREQUAL "0" OR
		NOT QualityPythonVersion MATCHES "^3\\.(9|[1-9][0-9]) lizard 1\\.24\\.1, pathspec 1\\.1\\.1, Pygments 2\\.21\\.0$")
		string(STRIP "${QualityPythonError}" QualityPythonError)
		message(FATAL_ERROR
			"Quality Python at '${QualityPythonSelected}' is missing required packages or versions. "
			"Expected Python >=3.9 with Lizard 1.24.1, pathspec 1.1.1, and Pygments 2.21.0; "
			"received '${QualityPythonVersion}' (exit ${QualityPythonResult}): ${QualityPythonError}. "
			"Bootstrap Build/QualityTools/.venv or correct the explicit interpreter override.")
	endif()
	message(STATUS "PulseForge quality tools: Python ${QualityPythonVersion} (${QualityPythonSelected})")

	set(ClangFormatHints
		"$ENV{VSINSTALLDIR}/VC/Tools/Llvm/bin"
		"$ENV{VSINSTALLDIR}/VC/Tools/Llvm/x64/bin"
		"$ENV{VCToolsInstallDir}/../../Llvm/bin"
		"$ENV{VCToolsInstallDir}/../../Llvm/x64/bin"
		"${CMAKE_VS_INSTALL_PATH}/VC/Tools/Llvm/bin"
		"${CMAKE_VS_INSTALL_PATH}/VC/Tools/Llvm/x64/bin"
	)
	file(GLOB VisualStudioLlvmHints
		"$ENV{ProgramFiles}/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/bin"
		"$ENV{ProgramFiles}/Microsoft Visual Studio/2022/*/VC/Tools/Llvm/x64/bin"
	)
	list(APPEND ClangFormatHints ${VisualStudioLlvmHints})
	find_program(PULSEFORGE_CLANG_FORMAT_EXECUTABLE
		NAMES clang-format.exe clang-format
		HINTS ${ClangFormatHints}
		DOC "clang-format 19.1.5 used by PulseForge changed-code checks"
	)
	if(NOT PULSEFORGE_CLANG_FORMAT_EXECUTABLE)
		message(FATAL_ERROR
			"Required clang-format 19.1.5 was not found. Install the Visual Studio LLVM tools component "
			"or set PULSEFORGE_CLANG_FORMAT_EXECUTABLE to its executable path.")
	endif()
	execute_process(
		COMMAND "${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}" --version
		OUTPUT_VARIABLE ClangFormatVersion
		OUTPUT_STRIP_TRAILING_WHITESPACE
		RESULT_VARIABLE ClangFormatResult
	)
	if(NOT ClangFormatResult EQUAL 0 OR NOT ClangFormatVersion MATCHES "clang-format version 19\\.1\\.5")
		message(FATAL_ERROR
			"PulseForge formatting requires clang-format 19.1.5; found '${ClangFormatVersion}' at "
			"${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}")
	endif()
	message(STATUS "PulseForge changed-code formatter: ${ClangFormatVersion} (${PULSEFORGE_CLANG_FORMAT_EXECUTABLE})")
	message(STATUS "MSVC Code Analysis is enabled for importer, mesh-data, editor-core, editor-UI and scene-render owners")

	file(READ "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality/SourceQualityRules.json" PULSEFORGE_QUALITY_RULES_JSON)

	function(pf_add_public_header_check_target Target PublicRoot)
		file(GLOB_RECURSE PublicHeaders CONFIGURE_DEPENDS
			"${CMAKE_CURRENT_SOURCE_DIR}/${PublicRoot}/*.h"
			"${CMAKE_CURRENT_SOURCE_DIR}/${PublicRoot}/*.hpp"
		)
		string(JSON NonApiHeaderCount LENGTH "${PULSEFORGE_QUALITY_RULES_JSON}" non_api_public_headers)
		set(NonApiHeaders)
		if(NonApiHeaderCount GREATER 0)
			math(EXPR LastNonApiHeader "${NonApiHeaderCount} - 1")
			foreach(Index RANGE 0 ${LastNonApiHeader})
				string(JSON NonApiHeader GET "${PULSEFORGE_QUALITY_RULES_JSON}" non_api_public_headers ${Index})
				list(APPEND NonApiHeaders "${NonApiHeader}")
			endforeach()
		endif()
		set(OwnerTargets)
		foreach(Header IN LISTS PublicHeaders)
			file(RELATIVE_PATH HeaderPath "${CMAKE_CURRENT_SOURCE_DIR}" "${Header}")
			file(TO_CMAKE_PATH "${HeaderPath}" HeaderPath)
			file(RELATIVE_PATH IncludePath "${CMAKE_CURRENT_SOURCE_DIR}/${PublicRoot}" "${Header}")
			string(JSON HeaderOwner ERROR_VARIABLE OwnerError GET
				"${PULSEFORGE_QUALITY_RULES_JSON}" public_header_owners "${HeaderPath}")
			if(OwnerError)
				if(HeaderPath IN_LIST NonApiHeaders)
					continue()
				endif()
				message(FATAL_ERROR "Public header ${HeaderPath} has no owner-scoped compile target")
			endif()
			if(NOT TARGET "${HeaderOwner}")
				message(FATAL_ERROR "Public header ${HeaderPath} names unavailable owner target ${HeaderOwner}")
			endif()
			if(NOT HeaderOwner IN_LIST OwnerTargets)
				list(APPEND OwnerTargets "${HeaderOwner}")
			endif()
			string(MD5 HeaderID "${HeaderPath}")
			set(CheckSource "${CMAKE_CURRENT_BINARY_DIR}/PublicHeaderChecks/${Target}/${HeaderOwner}/${HeaderID}.cpp")
			get_filename_component(CheckDirectory "${CheckSource}" DIRECTORY)
			file(MAKE_DIRECTORY "${CheckDirectory}")
			set(CheckContent "#include \"${IncludePath}\"\nstatic_assert(true);\n")
			if(EXISTS "${CheckSource}")
				file(READ "${CheckSource}" ExistingCheckContent)
				if(NOT "${ExistingCheckContent}" STREQUAL "${CheckContent}")
					file(WRITE "${CheckSource}" "${CheckContent}")
				endif()
			else()
				file(WRITE "${CheckSource}" "${CheckContent}")
			endif()
			set(OwnerSources_${HeaderOwner} ${OwnerSources_${HeaderOwner}} "${CheckSource}")
		endforeach()
		set(CheckTargets)
		foreach(HeaderOwner IN LISTS OwnerTargets)
			set(CheckTarget "${Target}_${HeaderOwner}")
			add_library(${CheckTarget} OBJECT ${OwnerSources_${HeaderOwner}})
			target_compile_options(${CheckTarget} PRIVATE
				$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
				$<$<CXX_COMPILER_ID:MSVC>:/utf-8>
				$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
			)
			target_compile_definitions(${CheckTarget} PRIVATE
				PF_PLATFORM_WINDOWS
				$<$<CONFIG:Debug>:PF_DEBUG>
				$<$<CONFIG:Release>:PF_RELEASE>
				$<$<CONFIG:Dist>:PF_DIST>
			)
			target_link_libraries(${CheckTarget} PRIVATE "${HeaderOwner}")
			list(APPEND CheckTargets "${CheckTarget}")
		endforeach()
		add_custom_target(${Target} DEPENDS ${CheckTargets})
	endfunction()

	pf_add_public_header_check_target(PulseForgeEnginePublicHeaders PulseForge/Source/Public)
	pf_add_public_header_check_target(PulseForgeGamePublicHeaders PulseForgeGame/Source/Public)
	set(PULSEFORGE_PUBLIC_HEADER_CHECK_TARGETS
		PulseForgeEnginePublicHeaders
		PulseForgeGamePublicHeaders
	)
	if(PULSEFORGE_BUILD_EDITOR)
		pf_add_public_header_check_target(PulseForgeEditorPublicHeaders PulseForgeEditor/Source/Public)
		list(APPEND PULSEFORGE_PUBLIC_HEADER_CHECK_TARGETS PulseForgeEditorPublicHeaders)
	endif()
	add_custom_target(PulseForgePublicHeaderChecks DEPENDS ${PULSEFORGE_PUBLIC_HEADER_CHECK_TARGETS})

	set(PULSEFORGE_QUALITY_COMPARISON_BASE "" CACHE STRING
		"Git revision used to check all first-party source changed by this branch")
	if(PULSEFORGE_QUALITY_COMPARISON_BASE STREQUAL "")
		execute_process(
			COMMAND git merge-base origin/main HEAD
			WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
			OUTPUT_VARIABLE PULSEFORGE_QUALITY_COMPARISON_BASE
			OUTPUT_STRIP_TRAILING_WHITESPACE
			RESULT_VARIABLE QualityBaseResult
		)
		if(NOT QualityBaseResult EQUAL 0 OR PULSEFORGE_QUALITY_COMPARISON_BASE STREQUAL "")
			message(FATAL_ERROR
				"Source quality checks require origin/main or an explicit PULSEFORGE_QUALITY_COMPARISON_BASE revision")
		endif()
	endif()
	message(STATUS "PulseForge source quality comparison base: ${PULSEFORGE_QUALITY_COMPARISON_BASE}")
	add_test(NAME PulseForge.SourceQuality
		COMMAND "${QualityPythonSelected}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality/CheckSourceQuality.py"
			--root "${CMAKE_CURRENT_SOURCE_DIR}"
			--rules "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality/SourceQualityRules.json"
			--graph-manifest "${CMAKE_BINARY_DIR}/PulseForgeTargetGraph-$<CONFIG>.txt"
			--comparison-base "${PULSEFORGE_QUALITY_COMPARISON_BASE}")
	add_test(NAME PulseForge.SourceQualityRules
		COMMAND "${QualityPythonSelected}" -B -m unittest discover
			-s "${CMAKE_CURRENT_SOURCE_DIR}/Tests/Quality" -p test_*.py)
	add_test(NAME PulseForge.CodeQualityTools
		COMMAND "${QualityPythonSelected}" -B "${CMAKE_CURRENT_SOURCE_DIR}/Tools/Quality/CheckCodeQualityTools.py"
			--root "${CMAKE_CURRENT_SOURCE_DIR}"
			--comparison-base "${PULSEFORGE_QUALITY_COMPARISON_BASE}"
			--configuration "$<CONFIG>"
			--clang-format "${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}"
			--metrics-output "${CMAKE_BINARY_DIR}/QualityReports/FunctionMetrics-$<CONFIG>.json")
	set_tests_properties(PulseForge.SourceQuality PulseForge.SourceQualityRules PulseForge.CodeQualityTools PROPERTIES
		ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PULSEFORGE_QUALITY_CMAKE_COMMAND=${CMAKE_COMMAND};PULSEFORGE_QUALITY_GENERATOR=${CMAKE_GENERATOR};PULSEFORGE_QUALITY_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM};PULSEFORGE_QUALITY_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM};PULSEFORGE_CLANG_FORMAT_EXECUTABLE=${PULSEFORGE_CLANG_FORMAT_EXECUTABLE}")
endif()

if(BUILD_TESTING)
	if(NOT PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_test(NAME PulseForge.CMakeBuildMode
			COMMAND "${CMAKE_COMMAND}"
				"-DPULSEFORGE_BUILD_TREE=${CMAKE_BINARY_DIR}"
				-P "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CMake/CheckNormalBuildMode.cmake")
	endif()

	add_executable(PulseForgeRendererTests
		Tests/PulseForgeRendererTests.cpp
		Tests/RendererPolicyTests.cpp
	)
	target_include_directories(PulseForgeRendererTests PRIVATE Tests)
	target_compile_definitions(PulseForgeRendererTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeRendererTests PRIVATE PulseForgeRenderer)
	add_test(NAME PulseForge.RendererPolicy COMMAND PulseForgeRendererTests)

	add_executable(PulseForgeRenderMathTests
		Tests/PulseForgeRenderMathTests.cpp
		Tests/RenderMathTests.cpp
	)
	target_include_directories(PulseForgeRenderMathTests PRIVATE Tests)
	target_compile_definitions(PulseForgeRenderMathTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeRenderMathTests PRIVATE PulseForgeRenderMath)
	add_test(NAME PulseForge.RenderMath COMMAND PulseForgeRenderMathTests)

	add_executable(PulseForgeGpuValidationAnalysisTests
		Tests/PulseForgeGpuValidationAnalysisTests.cpp
		Tests/GpuValidation/PointShadowImageAnalysisTests.cpp
	)
	target_include_directories(PulseForgeGpuValidationAnalysisTests PRIVATE Tests)
	target_compile_definitions(PulseForgeGpuValidationAnalysisTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeGpuValidationAnalysisTests PRIVATE PulseForgeGpuValidationAnalysis)
	add_test(NAME PulseForge.GpuValidationAnalysis COMMAND PulseForgeGpuValidationAnalysisTests)

	add_executable(PulseForgeTests
		Tests/PulseForgeTests.cpp
		Tests/CoreTests.cpp
		Tests/SceneTests.cpp
		Tests/SceneAssetTests.cpp
		Tests/AssetsTests.cpp
		Tests/AssetRegistryTests.cpp
		Tests/AssetOperationsTests.cpp
		Tests/RendererTests.cpp
	)
	target_include_directories(PulseForgeTests PRIVATE
		Tests
		PulseForge/Vendor/json/include
	)
	target_compile_definitions(PulseForgeTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeTests PRIVATE ${PULSEFORGE_ENGINE_TEST_LIBRARIES})
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Engine COMMAND PulseForgeTests)

	add_executable(PulseForgeAssetImportTests
		Tests/PulseForgeAssetImportTests.cpp
		Tests/AssetImportTests.cpp
	)
	target_include_directories(PulseForgeAssetImportTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeAssetImportTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeAssetImportTests PRIVATE PulseForgeAssetImporters PulseForgeAssetServices)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAssetImportTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.AssetImports COMMAND PulseForgeAssetImportTests)

        add_executable(PulseForgeAssetImporterConsumer Tests/ModuleConsumers/AssetImporterConsumer.cpp)
        target_compile_definitions(PulseForgeAssetImporterConsumer PRIVATE PF_PLATFORM_WINDOWS)
        target_link_libraries(PulseForgeAssetImporterConsumer PRIVATE PulseForgeAssetImporters)
        add_test(NAME PulseForge.AssetImporterConsumer COMMAND PulseForgeAssetImporterConsumer)

	add_executable(PulseForgeSceneRendererConsumer Tests/ModuleConsumers/SceneRendererConsumer.cpp)
	target_compile_definitions(PulseForgeSceneRendererConsumer PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeSceneRendererConsumer PRIVATE PulseForgeSceneRenderer)
	add_test(NAME PulseForge.SceneRendererConsumer COMMAND PulseForgeSceneRendererConsumer)

	add_executable(PulseForgeAudioRuntimeTests
		Tests/PulseForgeAudioRuntimeTests.cpp
		Tests/AudioRuntimeTests.cpp
	)
	target_include_directories(PulseForgeAudioRuntimeTests PRIVATE Tests PulseForge/Vendor/json/include)
	target_compile_definitions(PulseForgeAudioRuntimeTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeAudioRuntimeTests PRIVATE
		PulseForgeAssetServices
		PulseForgeAudioAssets
		PulseForgeAudioScene
		PulseForgeScripting
	)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeAudioRuntimeTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.AudioRuntime COMMAND PulseForgeAudioRuntimeTests)

	add_executable(PulseForgeEditorCoreTests
		Tests/PulseForgeEditorCoreTests.cpp
		Tests/EditorSceneEntityCommandTests.cpp
		Tests/EditorSceneViewportInteractionTests.cpp
	)
	target_include_directories(PulseForgeEditorCoreTests PRIVATE Tests PulseForgeEditor/Source/Private)
	target_compile_definitions(PulseForgeEditorCoreTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorCoreTests PRIVATE PulseForgeEditorCore)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeEditorCoreTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.EditorCore COMMAND PulseForgeEditorCoreTests)

	if(PULSEFORGE_BUILD_EDITOR)
	add_executable(PulseForgeEditorTests
		Tests/PulseForgeEditorTests.cpp
		Tests/EditorTests.cpp
		Tests/EditorViewportTests.cpp
		Tests/EditorViewportSceneRenderViewTests.cpp
		Tests/EditorAssetPreviewStressHarnessTests.cpp
		Tests/EditorStateTests.cpp
		Tests/EditorPanelStateTests.cpp
		Tests/EditorSceneStateTests.cpp
		Tests/DocumentsTests.cpp
		Tests/EditorMaterialProjectTests.cpp
		Tests/EditorProjectLoadingTests.cpp
		Tests/EditorAssetReferenceTests.cpp
		Tests/ScriptRuntimeTests.cpp
		Tests/ProjectRuntimeTests.cpp
		Tests/EditorHistoryTests.cpp
	)
	target_include_directories(PulseForgeEditorTests PRIVATE
		Tests
		PulseForgeEditor/Source/Public
		PulseForgeEditor/Source/Private
		PulseForge/Vendor/json/include
	)
	target_compile_definitions(PulseForgeEditorTests PRIVATE PF_PLATFORM_WINDOWS)
	if(PULSEFORGE_EDITOR_RENDERER STREQUAL "OpenGL")
		target_compile_definitions(PulseForgeEditorTests PRIVATE PF_EDITOR_RENDERER_OPENGL)
	endif()
	target_link_libraries(PulseForgeEditorTests PRIVATE
		PulseForgeEditorCore PulseForgeEditorUI ${PULSEFORGE_ENGINE_TEST_LIBRARIES} PulseForgeImGui spdlog::spdlog)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeEditorTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.Editor COMMAND PulseForgeEditorTests)
	add_executable(PulseForgeEditorWorkspacePresentationTests
		Tests/EditorWorkspacePresentationTestMain.cpp
		Tests/EditorWorkspacePresentationTests.cpp
	)
	target_include_directories(PulseForgeEditorWorkspacePresentationTests PRIVATE
		Tests
		PulseForgeEditor/Source/Public
		PulseForgeEditor/Source/Private
		PulseForge/Vendor/json/include
	)
	target_compile_definitions(PulseForgeEditorWorkspacePresentationTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeEditorWorkspacePresentationTests PRIVATE
		PulseForgeEditorCore PulseForgeEditorUI ${PULSEFORGE_ENGINE_TEST_LIBRARIES} PulseForgeImGui spdlog::spdlog)
	if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
		add_dependencies(PulseForgeEditorWorkspacePresentationTests PulseForgePublicHeaderChecks)
	endif()
	add_test(NAME PulseForge.EditorWorkspacePresentation COMMAND PulseForgeEditorWorkspacePresentationTests)
	endif()

	add_executable(PulseForgeBufferUpdateTests Tests/BufferUpdateValidationTests.cpp)
	target_compile_definitions(PulseForgeBufferUpdateTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeBufferUpdateTests PRIVATE PulseForgeRenderer)
	add_test(NAME PulseForge.BufferUpdates COMMAND PulseForgeBufferUpdateTests)
endif()

if(PULSEFORGE_BUILD_GPU_VALIDATION_TESTS)
	add_executable(PulseForgeGpuValidation Tests/GpuRendererValidation.cpp)
	target_compile_definitions(PulseForgeGpuValidation PRIVATE
		PF_PLATFORM_WINDOWS
		PF_GPU_VALIDATION_SHADER_DIRECTORY="${PULSEFORGE_GPU_VALIDATION_SHADER_DIR}"
		PF_GPU_VALIDATION_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/PulseForgeGame.pfproj"
		PF_PBR_DEMO_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Samples/VisualValidation/PBRMaterials/PBRMaterials.pfproj"
		PF_TRANSPARENCY_DEMO_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Samples/VisualValidation/Transparency/Transparency.pfproj"
		PF_POINT_SHADOW_DEMO_PROJECT_FILE="${CMAKE_CURRENT_SOURCE_DIR}/Samples/VisualValidation/PointLightShadows/PointLightShadows.pfproj"
		PF_GPU_VALIDATION_SCENE_SHADER_DIRECTORY="${PULSEFORGE_SHADER_OUTPUT_DIR}")
	target_link_libraries(PulseForgeGpuValidation PRIVATE PulseForge PulseForgeGpuValidationAnalysis)
	add_dependencies(PulseForgeGpuValidation PulseForgeGpuValidationShaders PulseForgeSceneShaderAssets)
	set_property(TARGET PulseForgeGpuValidation PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeGpuValidation>")
	if(BUILD_TESTING)
		add_test(NAME PulseForge.GPU.RGBA16F COMMAND PulseForgeGpuValidation)
		add_test(NAME PulseForge.GPU.SceneHDR COMMAND PulseForgeGpuValidation --scene-hdr)
		add_test(NAME PulseForge.GPU.SceneToneMapping COMMAND PulseForgeGpuValidation --scene-tonemap)
		add_test(NAME PulseForge.GPU.EditorUIUnorm COMMAND PulseForgeGpuValidation --editor-ui-unorm)
		add_test(NAME PulseForge.GPU.LocalLights COMMAND PulseForgeGpuValidation --scene-local-lights)
		add_test(NAME PulseForge.GPU.SpotlightShadows COMMAND PulseForgeGpuValidation --scene-spot-shadows)
		add_test(NAME PulseForge.GPU.PointShadows COMMAND PulseForgeGpuValidation --scene-point-shadows)
		add_test(NAME PulseForge.GPU.PBRMaterials COMMAND PulseForgeGpuValidation --scene-pbr-materials)
		add_test(NAME PulseForge.GPU.Transparency COMMAND PulseForgeGpuValidation --scene-transparency)
		add_test(NAME PulseForge.GPU.PointShadowImages
			COMMAND PulseForgeGpuValidation --scene-point-shadow-images
				"${CMAKE_BINARY_DIR}/Diagnostics/PointShadowDirection")
		add_test(NAME PulseForge.GPU.ActiveFrameUploads COMMAND PulseForgeGpuValidation --active-frame-resource-upload)
		set_tests_properties(
			PulseForge.GPU.RGBA16F
			PulseForge.GPU.SceneHDR
			PulseForge.GPU.SceneToneMapping
			PulseForge.GPU.EditorUIUnorm
			PulseForge.GPU.LocalLights
			PulseForge.GPU.SpotlightShadows
			PulseForge.GPU.PointShadows
			PulseForge.GPU.PBRMaterials
			PulseForge.GPU.Transparency
			PulseForge.GPU.PointShadowImages
			PulseForge.GPU.ActiveFrameUploads
			PROPERTIES RUN_SERIAL TRUE TIMEOUT 180 LABELS "gpu;renderer")
	endif()
endif()

if(BUILD_TESTING)
	if(NOT TARGET PulseForgeEditorCoreTests)
		message(FATAL_ERROR "BUILD_TESTING requires the independent PulseForgeEditorCoreTests target")
	endif()
	if(PULSEFORGE_BUILD_EDITOR)
		if(NOT TARGET PulseForgeEditorUI OR NOT TARGET PulseForgeEditorTests)
			message(FATAL_ERROR "Editor-enabled testing requires PulseForgeEditorUI and PulseForgeEditorTests")
		endif()
	elseif(TARGET PulseForgeEditorUI OR TARGET PulseForgeEditorTests OR TARGET PulseForgeImGui)
		message(FATAL_ERROR "Editor-disabled configuration must not create editor UI or its tests")
	endif()
endif()

foreach(Target PulseForgeCore PulseForgeWindow PulseForgeScene PulseForgeSceneSerialization PulseForgeRenderMath
		PulseForgeAssetCatalog PulseForgeAssetImporters PulseForgeAudio PulseForgeAudioAssets PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics
		PulseForgeScripting PulseForgeRuntime PulseForgeRenderer PulseForgeMeshData
		PulseForgeApplication PulseForgeSceneRenderer
		PulseForge PulseForgeGame PulseForgeEditor PulseForgeEditorCore PulseForgeEditorUI PulseForgeRendererTests PulseForgeRenderMathTests PulseForgeGpuValidationAnalysis PulseForgeGpuValidationAnalysisTests
                                PulseForgeTests PulseForgeAssetImportTests PulseForgeAssetImporterConsumer PulseForgeAudioRuntimeTests
				PulseForgeSceneRendererConsumer
                                PulseForgeEditorTests PulseForgeEditorWorkspacePresentationTests PulseForgeEditorCoreTests
				PulseForgeBufferUpdateTests PulseForgeGpuValidation)
	if(TARGET ${Target})
		target_compile_options(${Target} PRIVATE
			$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
			$<$<CXX_COMPILER_ID:MSVC>:/utf-8>
			$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
		)
		target_compile_definitions(${Target} PRIVATE
			$<$<CONFIG:Debug>:PF_DEBUG>
			$<$<CONFIG:Release>:PF_RELEASE>
			$<$<CONFIG:Dist>:PF_DIST>
		)
		if(PULSEFORGE_ENABLE_MSVC_ANALYSIS AND
			"${Target}" MATCHES "^(PulseForgeAssetImporters|PulseForgeMeshData|PulseForgeEditorCore|PulseForgeEditorUI|PulseForgeSceneRenderer)$")
			target_compile_options(${Target} PRIVATE /analyze /analyze:external- /external:W0)
		endif()
	endif()
endforeach()

if(PULSEFORGE_ENABLE_QUALITY_CHECKS)
	include(Tools/Quality/ExportConfiguredTargetGraph.cmake)
endif()
