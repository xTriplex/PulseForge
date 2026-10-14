#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>
#include <vector>

#include "Assets/AssetMetadata.h"
#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetReferenceValidator.h"
#include "Assets/AssetRegistry.h"
#include "Assets/AudioAssetCache.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/MaterialAsset.h"
#include "Assets/MaterialAssetCache.h"
#include "Assets/MaterialAssetService.h"
#include "Assets/Project.h"
#include "Core/Log.h"
#include "Assets/PrefabAssetService.h"
#include "Assets/PrefabSerializer.h"
#include "Assets/SceneAssetService.h"
#include "Core/Input.h"
#include "Core/LayerStack.h"
#include "Core/Timestep.h"
#include "Editor/EditorLayout.h"
#include "Editor/EditorLayoutSettingsPolicy.h"
#include "Editor/EditorMaterialEditorPolicy.h"
#include "Editor/EditorGlfwViewportPolicy.h"
#include "Editor/EditorPlatformViewportPolicy.h"
#include "Editor/EditorMaterialPropertyLayoutPolicy.h"
#include "Editor/EditorCommandHistory.h"
#include "Editor/EditorCommandLine.h"
#include "Editor/EditorDeferredOperationQueue.h"
#include "Editor/EditorContentBrowserPolicy.h"
#include "Editor/EditorAssetDestinationPolicy.h"
#include "Editor/EditorAssetSelectorPolicy.h"
#include "Editor/EditorAssetPreviewPolicy.h"
#include "Editor/EditorEntityIconPolicy.h"
#include "Editor/EditorHierarchyRowPolicy.h"
#include "Editor/EditorInteractionPolicy.h"
#include "Editor/EditorProjectOpening.h"
#include "Editor/EditorDocumentManager.h"
#include "Editor/EditorDocumentHostPolicy.h"
#include "Editor/EditorDocumentEditorRegistry.h"
#include "Editor/EditorDocumentCloseBatch.h"
#include "Editor/EditorDocumentTabPolicy.h"
#include "Editor/EditorDocumentWorkspace.h"
#include "Editor/EditorNativeWindowBoundsPolicy.h"
#include "Editor/EditorSnapshotHistory.h"
#include "Editor/EditorSelection.h"
#include "Editor/EditorVisibility.h"
#include "Editor/ConsoleFormatting.h"
#include "Editor/EditorStyle.h"
#include "Editor/EditorWorkspaceView.h"
#include "Editor/ViewportMouseCapture.h"
#include "Editor/EditorViewportToolbarPolicy.h"
#include "Editor/ImGuiRendererMath.h"
#include "Editor/ViewportMath.h"
#include "Audio/AudioEngine.h"
#include "Audio/AudioSceneRuntime.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Runtime/SceneRuntime.h"
#include "Scripting/ScriptRuntime.h"
#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/Mesh.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/DirectionalShadowMath.h"
#include "Renderer/SpotlightShadowMath.h"
#include "Renderer/PointLightShadowMath.h"
#include "Renderer/EnvironmentLighting.h"
#include "Renderer/ScreenSpaceAmbientOcclusion.h"
#include "Renderer/ToneMapping.h"
#include "Renderer/LocalLighting.h"
#include <glm/geometric.hpp>
#include "Renderer/Vulkan/VulkanSupport.h"

#include <nlohmann/json.hpp>
#include "Scene/Scene.h"
#include "Scene/SceneRenderSnapshot.h"
#include "Scene/SceneSerializer.h"
#include "Scene/UUID.h"
#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include <array>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/quaternion.hpp>
#include <imgui.h>

	class TestRunner
	{
	public:
		void Check(bool Passed, std::string_view Expression, const char* File, int Line)
		{
			++m_AssertionCount;
			if (Passed)
				return;

			++m_FailureCount;
			std::cerr << File << ':' << Line << ": check failed: " << Expression << '\n';
		}

		int Finish() const
		{
			if (m_FailureCount == 0)
				std::cout << "Passed " << m_AssertionCount << " checks.\n";
			else
				std::cerr << m_FailureCount << " of " << m_AssertionCount << " checks failed.\n";

			return m_FailureCount == 0 ? 0 : 1;
		}

	private:
		int m_AssertionCount = 0;
		int m_FailureCount = 0;
	};

#define PF_CHECK(Runner, Expression) \
	(Runner).Check(static_cast<bool>(Expression), #Expression, __FILE__, __LINE__)

void RunCoreTests(TestRunner& Tests);
void RunSceneTests(TestRunner& Tests);
void RunEditorTests(TestRunner& Tests);
void RunAssetsTests(TestRunner& Tests);
void RunDocumentsTests(TestRunner& Tests);
void RunProjectRuntimeTests(TestRunner& Tests);
void RunImportsTests(TestRunner& Tests);
void RunRendererTests(TestRunner& Tests);
void RunEditorHistoryTests(TestRunner& Tests);
