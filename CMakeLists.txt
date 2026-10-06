cmake_minimum_required(VERSION 3.24)

project(PulseForge LANGUAGES C CXX)

set(PULSEFORGE_SAMPLE_RENDERER "Vulkan" CACHE STRING "Renderer used by PulseForgeGame (Vulkan or OpenGL)")
set_property(CACHE PULSEFORGE_SAMPLE_RENDERER PROPERTY STRINGS Vulkan OpenGL)

if(NOT WIN32)
	message(FATAL_ERROR "PulseForge currently supports Windows only; desktop platform support is being implemented separately.")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

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

add_library(PulseForgeImGui STATIC
	PulseForge/Vendor/imgui/imgui.cpp
	PulseForge/Vendor/imgui/imgui_draw.cpp
	PulseForge/Vendor/imgui/imgui_tables.cpp
	PulseForge/Vendor/imgui/imgui_widgets.cpp
	PulseForge/Vendor/imgui/imgui_demo.cpp
	PulseForge/Vendor/imgui/backends/imgui_impl_glfw.cpp
	PulseForge/Vendor/imgui/backends/imgui_impl_opengl3.cpp
)
target_include_directories(PulseForgeImGui PUBLIC
	PulseForge/Vendor/imgui
	PulseForge/Vendor/imgui/backends
)
target_link_libraries(PulseForgeImGui PUBLIC glfw opengl32)
target_compile_options(PulseForgeImGui PRIVATE
	$<$<CXX_COMPILER_ID:MSVC>:/EHsc>
	$<$<AND:$<CXX_COMPILER_ID:MSVC>,$<CONFIG:Dist>>:/O2>
)

add_library(PulseForge STATIC
	PulseForge/Source/Private/Core/PulseForgePCH.cpp
	PulseForge/Source/Private/Audio/AudioEngine.cpp
	PulseForge/Source/Private/Audio/MiniAudio/MiniAudio.cpp
	PulseForge/Source/Private/Assets/AssetPathResolver.cpp
	PulseForge/Source/Private/Assets/AssetMetadata.cpp
	PulseForge/Source/Private/Assets/AssetOperations.cpp
	PulseForge/Source/Private/Assets/AssetReferenceValidator.cpp
	PulseForge/Source/Private/Assets/AssetRegistry.cpp
	PulseForge/Source/Private/Assets/AudioAssetCache.cpp
	PulseForge/Source/Private/Assets/MaterialAsset.cpp
	PulseForge/Source/Private/Assets/MaterialAssetCache.cpp
	PulseForge/Source/Private/Assets/MaterialAssetService.cpp
	PulseForge/Source/Private/Assets/Project.cpp
	PulseForge/Source/Private/Assets/GltfMeshImporter.cpp
	PulseForge/Source/Private/Assets/ImageAssetImporter.cpp
	PulseForge/Source/Private/Assets/MeshAssetCache.cpp
	PulseForge/Source/Private/Assets/PrefabAssetService.cpp
	PulseForge/Source/Private/Assets/SceneAssetService.cpp
	PulseForge/Source/Private/Assets/TextureAssetCache.cpp
	PulseForge/Source/Private/Assets/PrefabSerializer.cpp
	PulseForge/Source/Private/Core/Application.cpp
	PulseForge/Source/Private/Core/Core.cpp
	PulseForge/Source/Private/Core/EntryPoint.cpp
	PulseForge/Source/Private/Core/Input.cpp
	PulseForge/Source/Private/Core/Layer.cpp
	PulseForge/Source/Private/Core/LayerStack.cpp
	PulseForge/Source/Private/Core/Log.cpp
	PulseForge/Source/Private/ImGui/ImGuiLayer.cpp
	PulseForge/Source/Private/ImGui/UI.cpp
	PulseForge/Source/Private/Renderer/OpenGLRenderer.cpp
	PulseForge/Source/Private/Renderer/Buffer.cpp
	PulseForge/Source/Private/Renderer/Mesh.cpp
	PulseForge/Source/Private/Renderer/Texture.cpp
	PulseForge/Source/Private/Renderer/Binding.cpp
	PulseForge/Source/Private/Renderer/Graphics.cpp
	PulseForge/Source/Private/Renderer/RendererBackend.cpp
	PulseForge/Source/Private/Renderer/SceneRenderer.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanRenderer.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanSupport.cpp
	PulseForge/Source/Private/Scene/CameraComponent.cpp
	PulseForge/Source/Private/Scene/Components/RigidbodyComponent.cpp
	PulseForge/Source/Private/Scene/Components/BoxColliderComponent.cpp
	PulseForge/Source/Private/Scene/Scene.cpp
	PulseForge/Source/Private/Physics/PhysicsSceneRuntime.cpp
	PulseForge/Source/Private/Scene/SceneRenderSnapshot.cpp
	PulseForge/Source/Private/Scene/SceneSerializer.cpp
	PulseForge/Source/Private/Scene/TransformComponent.cpp
	PulseForge/Source/Private/Scene/UUID.cpp
	PulseForge/Source/Private/Utils/stb_image.cpp
	PulseForge/Source/Private/Window/Window.cpp
	PulseForge/Source/Private/Window/Platform/Windows/WindowsWindow.cpp
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
		PulseForge/Vendor/imgui
		PulseForge/Vendor/miniaudio
		PulseForge/Vendor/spdlog/include
		PulseForge/Vendor/stb
)
target_compile_definitions(PulseForge PRIVATE
	PF_PLATFORM_WINDOWS
	_CRT_SECURE_NO_WARNINGS
	GLFW_INCLUDE_NONE
	MA_NO_WIN32_LEAN_AND_MEAN
)
target_link_libraries(PulseForge PRIVATE
	glfw
	PulseForgeGlad
	PulseForgeImGui
	spdlog::spdlog
	opengl32
	nvrhi
	nvrhi_vk
	Vulkan-Headers
	Jolt
)
target_precompile_headers(PulseForge PRIVATE PulseForge/Source/Public/Core/PulseForgePCH.h)

add_executable(PulseForgeGame PulseForgeGame/Source/Private/Client/PulseForgeGame.cpp)
target_include_directories(PulseForgeGame PRIVATE PulseForgeGame/Source/Public)
target_compile_definitions(PulseForgeGame PRIVATE PF_PLATFORM_WINDOWS "PF_SAMPLE_SHADER_DIRECTORY=\"Shaders\"")
if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "OpenGL")
	target_compile_definitions(PulseForgeGame PRIVATE PF_SAMPLE_RENDERER_OPENGL)
elseif(NOT PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
	message(FATAL_ERROR "PULSEFORGE_SAMPLE_RENDERER must be Vulkan or OpenGL")
endif()
target_link_libraries(PulseForgeGame PRIVATE PulseForge)

if(PULSEFORGE_SAMPLE_RENDERER STREQUAL "Vulkan")
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
		COMMAND "${PULSEFORGE_DXC}" -spirv -T ps_6_0 -E PSMain -fspv-target-env=vulkan1.3 -fvk-b-shift 256 0 -fvk-t-shift 0 0 -fvk-s-shift 128 0 -O3 -WX
			-Fo "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "${PULSEFORGE_TRIANGLE_HLSL}"
		DEPENDS "${PULSEFORGE_TRIANGLE_HLSL}"
		VERBATIM
		COMMENT "Compiling PulseForge triangle fragment shader to SPIR-V"
	)
	add_custom_target(PulseForgeShaderAssets
		DEPENDS "${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}")
	add_dependencies(PulseForgeGame PulseForgeShaderAssets)
	add_custom_command(TARGET PulseForgeGame POST_BUILD
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${PULSEFORGE_TRIANGLE_VERTEX_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Triangle.vs.spv"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${PULSEFORGE_TRIANGLE_FRAGMENT_SPIRV}" "$<TARGET_FILE_DIR:PulseForgeGame>/Shaders/Triangle.ps.spv"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Models"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Models" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Models"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Textures"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Textures" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Textures"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scenes"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Scenes" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Scenes"
		COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Materials"
		COMMAND "${CMAKE_COMMAND}" -E copy_directory
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/Assets/Materials" "$<TARGET_FILE_DIR:PulseForgeGame>/Assets/Materials"
		COMMAND "${CMAKE_COMMAND}" -E copy_if_different
			"${CMAKE_CURRENT_SOURCE_DIR}/PulseForgeGame/PulseForgeGame.pfproj" "$<TARGET_FILE_DIR:PulseForgeGame>/PulseForgeGame.pfproj"
		VERBATIM
	)
	set_property(TARGET PulseForgeGame PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "$<TARGET_FILE_DIR:PulseForgeGame>")
endif()

include(CTest)
if(BUILD_TESTING)
	add_executable(PulseForgeTests Tests/PulseForgeTests.cpp)
	target_compile_definitions(PulseForgeTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeTests PRIVATE PulseForge)
	add_test(NAME PulseForge.Core COMMAND PulseForgeTests)

	add_executable(PulseForgeBufferUpdateTests Tests/BufferUpdateValidationTests.cpp)
	target_compile_definitions(PulseForgeBufferUpdateTests PRIVATE PF_PLATFORM_WINDOWS)
	target_link_libraries(PulseForgeBufferUpdateTests PRIVATE PulseForge)
	add_test(NAME PulseForge.BufferUpdates COMMAND PulseForgeBufferUpdateTests)
endif()

foreach(Target PulseForge PulseForgeGame PulseForgeTests PulseForgeBufferUpdateTests)
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
	endif()
endforeach()
