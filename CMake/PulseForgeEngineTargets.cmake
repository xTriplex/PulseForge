add_library(PulseForgeInput STATIC
	PulseForge/Source/Private/Input/GameplayInput.cpp
	PulseForge/Source/Private/Input/GamepadStateAdapter.cpp
	PulseForge/Source/Private/Input/GameplayInputStore.cpp
)
target_include_directories(PulseForgeInput
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include
)
target_link_libraries(PulseForgeInput PUBLIC PulseForgeCore)

add_library(PulseForgeWindow STATIC
	PulseForge/Source/Private/Window/Window.cpp
	PulseForge/Source/Private/Window/Platform/GLFW/GLFWWindow.cpp
)
target_include_directories(PulseForgeWindow
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/Glfw/include
)
target_compile_definitions(PulseForgeWindow PRIVATE ${PULSEFORGE_PLATFORM_COMPILE_DEFINITION} GLFW_INCLUDE_NONE)
target_link_libraries(PulseForgeWindow PUBLIC PulseForgeCore PulseForgeInput PRIVATE glfw spdlog::spdlog)
if(PULSEFORGE_PLATFORM_MACOS)
	target_link_libraries(PulseForgeWindow PRIVATE Vulkan-Headers)
endif()
add_library(PulseForgeScene STATIC
	PulseForge/Source/Private/Scene/CameraComponent.cpp
	PulseForge/Source/Private/Scene/Components/AudioSourceComponent.cpp
	PulseForge/Source/Private/Scene/Components/BoxColliderComponent.cpp
	PulseForge/Source/Private/Scene/Components/PointLightComponent.cpp
	PulseForge/Source/Private/Scene/Components/RigidbodyComponent.cpp
	PulseForge/Source/Private/Scene/Components/ScriptComponent.cpp
	PulseForge/Source/Private/Scene/Components/SpotLightComponent.cpp
	PulseForge/Source/Private/Scene/PropertyMetadata.cpp
	PulseForge/Source/Private/Scene/Scene.cpp
	PulseForge/Source/Private/Scene/SceneRenderSnapshot.cpp
	PulseForge/Source/Private/Scene/TransformComponent.cpp
)
target_include_directories(PulseForgeScene
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE PulseForge/Vendor/entt/src PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeScene PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeScene PUBLIC PulseForgeCore)
add_library(PulseForgeSceneSerialization STATIC
	PulseForge/Source/Private/Scene/SceneSerializer.cpp
	PulseForge/Source/Private/Assets/PrefabSerializer.cpp
)
target_include_directories(PulseForgeSceneSerialization
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeSceneSerialization PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeSceneSerialization PUBLIC PulseForgeScene)
add_library(PulseForgeMeshData STATIC PulseForge/Source/Private/Renderer/MeshData.cpp)
target_include_directories(PulseForgeMeshData PUBLIC PulseForge/Source/Public)
add_library(PulseForgeImageData INTERFACE)
target_include_directories(PulseForgeImageData INTERFACE PulseForge/Source/Public)
add_library(PulseForgeCookedProductCodecs STATIC PulseForge/Source/Private/Assets/CookedProductCodec.cpp)
target_include_directories(PulseForgeCookedProductCodecs PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeCookedProductCodecs PUBLIC PulseForgeCore PulseForgeMeshData PulseForgeImageData)
set_property(TARGET PulseForgeCookedProductCodecs PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
add_library(PulseForgeRenderMath STATIC
	PulseForge/Source/Private/Renderer/DirectionalShadowMath.cpp
	PulseForge/Source/Private/Renderer/SpotlightShadowMath.cpp
	PulseForge/Source/Private/Renderer/PointLightShadowMath.cpp
	PulseForge/Source/Private/Renderer/ScreenSpaceAmbientOcclusion.cpp
)
target_include_directories(PulseForgeRenderMath PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeRenderMath PUBLIC PulseForgeScene)
add_library(PulseForgeAssetCatalog STATIC
	PulseForge/Source/Private/Assets/AssetImportProvenance.cpp
	PulseForge/Source/Private/Assets/AssetMetadata.cpp
	PulseForge/Source/Private/Assets/AssetOperations.cpp
	PulseForge/Source/Private/Assets/AssetPathResolver.cpp
	PulseForge/Source/Private/Assets/GltfRelocation.cpp
	PulseForge/Source/Private/Assets/AssetReferenceValidator.cpp
	PulseForge/Source/Private/Assets/AssetRegistry.cpp
	PulseForge/Source/Private/Assets/GltfPrimitiveInspector.cpp
	PulseForge/Source/Private/Assets/GltfSourceDocument.cpp
)
target_include_directories(PulseForgeAssetCatalog
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include PulseForge/Vendor/tracy/server
)
target_compile_definitions(PulseForgeAssetCatalog PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(PulseForgeAssetCatalog PUBLIC PulseForgeCore PulseForgeScene)
add_library(PulseForgeCookedAssetCatalog STATIC PulseForge/Source/Private/Assets/CookedAssetCatalog.cpp)
target_include_directories(PulseForgeCookedAssetCatalog
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE PulseForge/Vendor/json/include PulseForge/Vendor/tracy/server
)
target_link_libraries(PulseForgeCookedAssetCatalog PUBLIC PulseForgeScene)
set_property(TARGET PulseForgeCookedAssetCatalog PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
add_library(PulseForgeCookedRuntimeProvider STATIC PulseForge/Source/Private/Runtime/CookedContentProvider.cpp)
target_include_directories(PulseForgeCookedRuntimeProvider PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeCookedRuntimeProvider
	PUBLIC PulseForgeCookedAssetCatalog PulseForgeSceneSerialization PulseForgeInput PulseForgeCookedProductCodecs)
set_property(TARGET PulseForgeCookedRuntimeProvider PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
add_library(PulseForgeAudio STATIC
	PulseForge/Source/Private/Audio/AudioEngine.cpp
	PulseForge/Source/Private/Audio/MiniAudio/MiniAudio.cpp
)
target_include_directories(PulseForgeAudio
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE PulseForge/Vendor/miniaudio
)
target_compile_definitions(PulseForgeAudio PRIVATE MA_NO_WIN32_LEAN_AND_MEAN)
add_library(PulseForgeAudioAssets STATIC PulseForge/Source/Private/Assets/AudioAssetCache.cpp)
target_include_directories(PulseForgeAudioAssets PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeAudioAssets PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeAudioAssets PUBLIC PulseForgeAssetCatalog PulseForgeAudio)
add_library(PulseForgeAssetServices STATIC
	PulseForge/Source/Private/Assets/MaterialAsset.cpp
	PulseForge/Source/Private/Assets/MaterialAssetCache.cpp
	PulseForge/Source/Private/Assets/MaterialAssetService.cpp
	PulseForge/Source/Private/Assets/PrefabAssetService.cpp
	PulseForge/Source/Private/Assets/Project.cpp
	PulseForge/Source/Private/Assets/SceneAssetService.cpp
)
target_include_directories(PulseForgeAssetServices
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include PulseForge/Vendor/stb
)
target_compile_definitions(PulseForgeAssetServices PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeAssetServices
	PUBLIC PulseForgeAssetCatalog
	PRIVATE PulseForgeSceneSerialization
)
target_precompile_headers(PulseForgeAssetServices PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeAudioScene STATIC PulseForge/Source/Private/Audio/AudioSceneRuntime.cpp)
target_include_directories(PulseForgeAudioScene PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeAudioScene
	PUBLIC PulseForgeAudio
	PRIVATE PulseForgeAudioAssets PulseForgeScene
)
add_library(PulseForgePhysics STATIC PulseForge/Source/Private/Physics/PhysicsSceneRuntime.cpp)
target_include_directories(PulseForgePhysics PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgePhysics PRIVATE NOMINMAX)
target_link_libraries(PulseForgePhysics PUBLIC PulseForgeScene PRIVATE Jolt spdlog::spdlog)
add_library(PulseForgeScripting STATIC
	PulseForge/Source/Private/Scripting/ScriptEnvironment.cpp
	PulseForge/Source/Private/Scripting/ScriptRuntime.cpp
)
target_include_directories(PulseForgeScripting
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeScripting PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(PulseForgeScripting
	PUBLIC PulseForgeAssetCatalog
	PRIVATE PulseForgeAssetServices PulseForgeAudioScene PulseForgePhysics PulseForgeInput PulseForgeCore PulseForgeLua
		PulseForgeCookedRuntimeProvider spdlog::spdlog
)
add_library(PulseForgeRuntime STATIC
	PulseForge/Source/Private/Runtime/SceneRuntime.cpp
	PulseForge/Source/Private/Runtime/PlayerSession.cpp
)
target_include_directories(PulseForgeRuntime PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeRuntime
	PUBLIC PulseForgeAudio PulseForgePhysics PulseForgeScripting
	PRIVATE PulseForgeAssetServices PulseForgeAudioAssets PulseForgeAudioScene PulseForgeCookedRuntimeProvider PulseForgeScene
		PulseForgeInput
)

# The cooked player compiles the same runtime owners with their source-project branches removed.
# This keeps the shipped cooked route out of the authoring registry and importer target closure.
add_library(PulseForgeCookedAudioAssets STATIC PulseForge/Source/Private/Assets/AudioAssetCache.cpp)
target_include_directories(PulseForgeCookedAudioAssets PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeCookedAudioAssets PRIVATE PF_COOKED_RUNTIME_ONLY)
target_link_libraries(PulseForgeCookedAudioAssets PUBLIC PulseForgeAudio)
set_property(TARGET PulseForgeCookedAudioAssets PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

add_library(PulseForgeCookedAudioScene STATIC PulseForge/Source/Private/Audio/AudioSceneRuntime.cpp)
target_include_directories(PulseForgeCookedAudioScene PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeCookedAudioScene PUBLIC PulseForgeAudio PulseForgeScene
	PRIVATE PulseForgeCookedAudioAssets)
set_property(TARGET PulseForgeCookedAudioScene PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

add_library(PulseForgeCookedScripting STATIC
	PulseForge/Source/Private/Scripting/ScriptEnvironment.cpp
	PulseForge/Source/Private/Scripting/ScriptRuntime.cpp)
target_include_directories(PulseForgeCookedScripting PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include)
target_compile_definitions(PulseForgeCookedScripting PRIVATE PF_COOKED_RUNTIME_ONLY _CRT_SECURE_NO_WARNINGS NOMINMAX)
target_link_libraries(PulseForgeCookedScripting
	PUBLIC PulseForgeScene
	PRIVATE PulseForgeCookedRuntimeProvider PulseForgeCookedAudioScene PulseForgePhysics PulseForgeInput PulseForgeCore
		PulseForgeLua spdlog::spdlog)
set_property(TARGET PulseForgeCookedScripting PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

add_library(PulseForgeCookedRuntime STATIC
	PulseForge/Source/Private/Runtime/SceneRuntime.cpp
	PulseForge/Source/Private/Runtime/PlayerSession.cpp)
target_include_directories(PulseForgeCookedRuntime PUBLIC PulseForge/Source/Public)
target_compile_definitions(PulseForgeCookedRuntime PUBLIC PF_COOKED_RUNTIME_ONLY)
target_link_libraries(PulseForgeCookedRuntime
	PUBLIC PulseForgeAudio PulseForgePhysics PulseForgeCookedScripting PulseForgeCookedRuntimeProvider PulseForgeScene
	PRIVATE PulseForgeCookedAudioAssets PulseForgeCookedAudioScene PulseForgeInput)
set_property(TARGET PulseForgeCookedRuntime PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
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
	PulseForge/Source/Private/Renderer/Vulkan/VulkanResourceObjects.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanPresentationResources.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanRenderer.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanRendererShutdown.cpp
	PulseForge/Source/Private/Renderer/Vulkan/VulkanSupport.cpp
)
target_include_directories(PulseForgeRenderer
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE
		PulseForge/Vendor/Glfw/include
		PulseForge/Vendor/Glad/include
		PulseForge/Vendor/stb
)
target_compile_definitions(PulseForgeRenderer PRIVATE
	_CRT_SECURE_NO_WARNINGS
	GLFW_INCLUDE_NONE
)
target_link_libraries(PulseForgeRenderer
	PUBLIC PulseForgeCore PulseForgeMeshData PulseForgeImageData
	PRIVATE PulseForgeWindow glfw PulseForgeGlad OpenGL::GL nvrhi_vk nvrhi Vulkan-Headers
)
target_precompile_headers(PulseForgeRenderer PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeAssetImporters STATIC
	PulseForge/Source/Private/Assets/GltfMeshImportCoordinator.cpp
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
target_compile_definitions(PulseForgeAssetImporters PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeAssetImporters PUBLIC
	PulseForgeAssetCatalog PulseForgeMeshData PulseForgeImageData PulseForgeCookedProductCodecs)
target_precompile_headers(PulseForgeAssetImporters PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeAssetDependencyCatalog STATIC
	PulseForge/Source/Private/Assets/AssetEditTransaction.cpp
	PulseForge/Source/Private/Assets/AssetEditRecoveryJournal.cpp
	PulseForge/Source/Private/Assets/AssetDependencyCatalog.cpp
	PulseForge/Source/Private/Assets/AssetDependencyExtractor.cpp
)
target_include_directories(PulseForgeAssetDependencyCatalog
	PUBLIC PulseForge/Source/Public
	PRIVATE PulseForge/Vendor/json/include PulseForge/Vendor/tracy/server
)
target_link_libraries(PulseForgeAssetDependencyCatalog
	PUBLIC PulseForgeAssetCatalog
	PRIVATE PulseForgeAssetImporters PulseForgeAssetServices PulseForgeSceneSerialization
)
target_precompile_headers(PulseForgeAssetDependencyCatalog PRIVATE "${PULSEFORGE_ENGINE_PCH}")
add_library(PulseForgeAssetPreparation STATIC PulseForge/Source/Private/Assets/AssetPreparationService.cpp)
target_include_directories(PulseForgeAssetPreparation PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeAssetPreparation PUBLIC PulseForgeCore)
add_library(PulseForgeDerivedData STATIC PulseForge/Source/Private/Assets/DerivedDataArtifactStore.cpp)
target_include_directories(PulseForgeDerivedData PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeDerivedData PUBLIC PulseForgeAssetCatalog)
add_library(PulseForgeAssetCpuProducts STATIC PulseForge/Source/Private/Assets/AssetCpuProducts.cpp)
target_include_directories(PulseForgeAssetCpuProducts PUBLIC PulseForge/Source/Public)
target_link_libraries(PulseForgeAssetCpuProducts
	PUBLIC PulseForgeAssetPreparation PulseForgeDerivedData PulseForgeAssetImporters PulseForgeAssetDependencyCatalog
		PulseForgeCookedProductCodecs)
add_library(PulseForgeApplication STATIC PulseForge/Source/Private/Core/Application.cpp)
target_include_directories(PulseForgeApplication
	PUBLIC PulseForge/Source/Public
)
target_link_libraries(PulseForgeApplication PUBLIC PulseForgeCore PulseForgeInput PulseForgeRenderer PulseForgeWindow)
add_library(PulseForgeSceneRenderer STATIC
	PulseForge/Source/Private/Renderer/AssetCache/MeshAssetCache.cpp
	PulseForge/Source/Private/Renderer/AssetCache/TextureAssetCache.cpp
	PulseForge/Source/Private/Renderer/EnvironmentLightingCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererAmbientOcclusionPass.cpp
	PulseForge/Source/Private/Renderer/SceneRendererHdrTargetCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererLocalLightingState.cpp
	PulseForge/Source/Private/Renderer/SceneRendererMaterialBindingCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererPipelineCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererShadowPass.cpp
	PulseForge/Source/Private/Renderer/SceneRendererShadowTargetCache.cpp
	PulseForge/Source/Private/Renderer/SceneRenderer.cpp
	PulseForge/Source/Private/Renderer/SceneRendererToneMappingStage.cpp
)
target_include_directories(PulseForgeSceneRenderer
	PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm
	PRIVATE PulseForge/Vendor/json/include
)
target_compile_definitions(PulseForgeSceneRenderer PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeSceneRenderer
	PUBLIC PulseForgeRenderMath PulseForgeScene PulseForgeRenderer PulseForgeAssetPreparation PulseForgeDerivedData
	PRIVATE PulseForgeAssetCpuProducts PulseForgeAssetImporters PulseForgeAssetServices PulseForgeCookedProductCodecs
		PulseForgeCookedRuntimeProvider
)
target_precompile_headers(PulseForgeSceneRenderer PRIVATE "${PULSEFORGE_ENGINE_PCH}")

# This renderer variant consumes only cooked CPU products. Source import, registry and artifact-store owners are
# compiled out of its cache adapters so a cooked Player does not inherit the authoring pipeline through linkage.
add_library(PulseForgeCookedSceneRenderer STATIC
	PulseForge/Source/Private/Renderer/AssetCache/MeshAssetCache.cpp
	PulseForge/Source/Private/Renderer/AssetCache/TextureAssetCache.cpp
	PulseForge/Source/Private/Renderer/CookedEnvironmentLightingCache.cpp
	PulseForge/Source/Private/Assets/MaterialAssetCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererAmbientOcclusionPass.cpp
	PulseForge/Source/Private/Renderer/SceneRendererHdrTargetCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererLocalLightingState.cpp
	PulseForge/Source/Private/Renderer/SceneRendererMaterialBindingCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererPipelineCache.cpp
	PulseForge/Source/Private/Renderer/SceneRendererShadowPass.cpp
	PulseForge/Source/Private/Renderer/SceneRendererShadowTargetCache.cpp
	PulseForge/Source/Private/Renderer/SceneRenderer.cpp
	PulseForge/Source/Private/Renderer/SceneRendererToneMappingStage.cpp
)
target_include_directories(PulseForgeCookedSceneRenderer PUBLIC PulseForge/Source/Public PulseForge/Vendor/glm)
target_compile_definitions(PulseForgeCookedSceneRenderer PUBLIC PF_COOKED_RUNTIME_ONLY _CRT_SECURE_NO_WARNINGS)
target_link_libraries(PulseForgeCookedSceneRenderer
	PUBLIC PulseForgeRenderMath PulseForgeScene PulseForgeRenderer PulseForgeAssetPreparation
	PRIVATE PulseForgeCookedRuntimeProvider PulseForgeCookedProductCodecs)
set_property(TARGET PulseForgeCookedSceneRenderer PROPERTY DISABLE_PRECOMPILE_HEADERS ON)

add_library(PulseForgeAssetCookService STATIC PulseForge/Source/Private/Assets/AssetCookService.cpp)
target_include_directories(PulseForgeAssetCookService PUBLIC PulseForge/Source/Public PRIVATE PulseForge/Vendor/json/include)
target_link_libraries(PulseForgeAssetCookService
	PUBLIC PulseForgeCookedAssetCatalog
	PRIVATE PulseForgeAssetCpuProducts PulseForgeAssetDependencyCatalog PulseForgeAssetImporters
		PulseForgeAssetServices PulseForgeSceneSerialization PulseForgeInput PulseForgeSceneRenderer)
set_property(TARGET PulseForgeAssetCookService PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
add_executable(PulseForgeCook PulseForge/Source/Private/Assets/AssetCookCommand.cpp)
target_include_directories(PulseForgeCook PRIVATE PulseForge/Source/Public)
target_compile_definitions(PulseForgeCook PRIVATE "PF_BUILD_CONFIGURATION=\"$<CONFIG>\"")
target_link_libraries(PulseForgeCook PRIVATE PulseForgeAssetCookService PulseForgeAssetServices PulseForgeAssetDependencyCatalog)
set_property(TARGET PulseForgeCook PROPERTY DISABLE_PRECOMPILE_HEADERS ON)
add_library(PulseForge STATIC
	PulseForge/Source/Private/Core/Core.cpp
	PulseForge/Source/Private/Core/EntryPoint.cpp
)
target_include_directories(PulseForge
	PUBLIC PulseForge/Source/Public
		PulseForge/Vendor/glm
	PRIVATE
		PulseForge/Vendor/entt/src
		PulseForge/Vendor/json/include
		PulseForge/Vendor/Glfw/include
		PulseForge/Vendor/Glad/include
		PulseForge/Vendor/stb
)
target_compile_definitions(PulseForge PRIVATE
	${PULSEFORGE_PLATFORM_COMPILE_DEFINITION}
	_CRT_SECURE_NO_WARNINGS
	GLFW_INCLUDE_NONE
	MA_NO_WIN32_LEAN_AND_MEAN
)
target_link_libraries(PulseForge
	PUBLIC PulseForgeApplication PulseForgeSceneRenderer PulseForgeSceneSerialization PulseForgeAssetImporters
		PulseForgeAssetServices
		PulseForgeRenderMath PulseForgeRuntime PulseForgeWindow
)
