#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <numbers>
#include <optional>
#include <string_view>

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

namespace
{
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

	struct LayerLifecycle
	{
		int AttachCount = 0;
		int DetachCount = 0;
		int DestroyCount = 0;
	};

	class TrackingLayer final : public PulseForge::Layer
	{
	public:
		explicit TrackingLayer(LayerLifecycle& Lifecycle)
			: Layer("TrackingLayer"), m_Lifecycle(Lifecycle)
		{
		}

		~TrackingLayer() override { ++m_Lifecycle.DestroyCount; }
		void OnAttach() override { ++m_Lifecycle.AttachCount; }
		void OnDetach() override { ++m_Lifecycle.DetachCount; }

	private:
		LayerLifecycle& m_Lifecycle;
	};

	void TestLayerStackOwnership(TestRunner& Tests)
	{
		LayerLifecycle RegularLifecycle;
		LayerLifecycle OverlayLifecycle;
		{
			PulseForge::LayerStack Stack;
			PulseForge::Layer& Regular = Stack.PushLayer(std::make_unique<TrackingLayer>(RegularLifecycle));
			PulseForge::Layer& Overlay = Stack.PushOverlay(std::make_unique<TrackingLayer>(OverlayLifecycle));

			PF_CHECK(Tests, RegularLifecycle.AttachCount == 1);
			PF_CHECK(Tests, OverlayLifecycle.AttachCount == 1);
			PF_CHECK(Tests, !Stack.PopOverlay(Regular));
			PF_CHECK(Tests, !Stack.PopLayer(Overlay));
			PF_CHECK(Tests, Stack.PopLayer(Regular));
			PF_CHECK(Tests, RegularLifecycle.DetachCount == 1);
			PF_CHECK(Tests, RegularLifecycle.DestroyCount == 1);
			PF_CHECK(Tests, Stack.PopOverlay(Overlay));
			PF_CHECK(Tests, OverlayLifecycle.DetachCount == 1);
			PF_CHECK(Tests, OverlayLifecycle.DestroyCount == 1);
		}
		PF_CHECK(Tests, RegularLifecycle.DetachCount == 1);
		PF_CHECK(Tests, OverlayLifecycle.DetachCount == 1);
	}

	void TestLayerStackClearDetachesInReverseOrder(TestRunner& Tests)
	{
		std::vector<int> DetachOrder;
		class OrderedLayer final : public PulseForge::Layer
		{
		public:
			OrderedLayer(int Id, std::vector<int>& Order)
				: Layer("OrderedLayer"), m_Id(Id), m_Order(Order) {}
			void OnDetach() override { m_Order.push_back(m_Id); }
		private:
			int m_Id;
			std::vector<int>& m_Order;
		};

		{
			PulseForge::LayerStack Stack;
			Stack.PushLayer(std::make_unique<OrderedLayer>(1, DetachOrder));
			Stack.PushLayer(std::make_unique<OrderedLayer>(2, DetachOrder));
			Stack.PushOverlay(std::make_unique<OrderedLayer>(3, DetachOrder));
			Stack.Clear();
			PF_CHECK(Tests, DetachOrder == std::vector<int>({ 3, 2, 1 }));
		}
		PF_CHECK(Tests, DetachOrder == std::vector<int>({ 3, 2, 1 }));
	}

	void TestLayerAndOverlayIterationOrder(TestRunner& Tests)
	{
		std::vector<int> UpdateOrder;
		class VisitingLayer final : public PulseForge::Layer
		{
		public:
			VisitingLayer(int Id, std::vector<int>& Order)
				: Layer("VisitingLayer"), m_Id(Id), m_Order(Order) {}
			void OnUpdate(PulseForge::Timestep) override { m_Order.push_back(m_Id); }
		private:
			int m_Id;
			std::vector<int>& m_Order;
		};

		PulseForge::LayerStack Stack;
		Stack.PushLayer(std::make_unique<VisitingLayer>(1, UpdateOrder));
		Stack.PushLayer(std::make_unique<VisitingLayer>(2, UpdateOrder));
		Stack.PushOverlay(std::make_unique<VisitingLayer>(3, UpdateOrder));
		Stack.PushOverlay(std::make_unique<VisitingLayer>(4, UpdateOrder));
		for (const auto& Layer : Stack)
			Layer->OnUpdate(PulseForge::Timestep{});

		PF_CHECK(Tests, UpdateOrder == std::vector<int>({ 1, 2, 3, 4 }));
	}

	void TestEventDispatchAndHandledState(TestRunner& Tests)
	{
		PulseForge::KeyPressedEvent KeyEvent(65, 0);
		PulseForge::EventDispatcher Dispatcher(KeyEvent);
		int CallbackCount = 0;
		const bool WasDispatched = Dispatcher.Dispatch<PulseForge::KeyPressedEvent>(
			[&CallbackCount](PulseForge::KeyPressedEvent&) { ++CallbackCount; return true; });
		PF_CHECK(Tests, WasDispatched);
		PF_CHECK(Tests, CallbackCount == 1);
		PF_CHECK(Tests, KeyEvent.bHandled);

		PulseForge::MouseMovedEvent MouseEvent(3.0f, 4.0f);
		PulseForge::EventDispatcher MismatchedDispatcher(MouseEvent);
		const bool MismatchedDispatch = MismatchedDispatcher.Dispatch<PulseForge::KeyPressedEvent>(
			[&CallbackCount](PulseForge::KeyPressedEvent&) { ++CallbackCount; return true; });
		PF_CHECK(Tests, !MismatchedDispatch);
		PF_CHECK(Tests, CallbackCount == 1);
		PF_CHECK(Tests, !MouseEvent.bHandled);

		KeyEvent.bHandled = true;
		PulseForge::EventDispatcher AlreadyHandledDispatcher(KeyEvent);
		AlreadyHandledDispatcher.Dispatch<PulseForge::KeyPressedEvent>(
			[](PulseForge::KeyPressedEvent&) { return false; });
		PF_CHECK(Tests, KeyEvent.bHandled);
	}

	void TestLayerAttachFailureRollsBackOwnership(TestRunner& Tests)
	{
		class FailingLayer final : public PulseForge::Layer
		{
		public:
			FailingLayer(int& DetachCount, int& DestroyCount)
				: Layer("FailingLayer"), m_DetachCount(DetachCount), m_DestroyCount(DestroyCount) {}
			~FailingLayer() override { ++m_DestroyCount; }
			void OnAttach() override { throw std::runtime_error("expected attach failure"); }
			void OnDetach() override { ++m_DetachCount; }
		private:
			int& m_DetachCount;
			int& m_DestroyCount;
		};

		PulseForge::LayerStack Stack;
		int DetachCount = 0;
		int DestroyCount = 0;
		bool Threw = false;
		try
		{
			Stack.PushLayer(std::make_unique<FailingLayer>(DetachCount, DestroyCount));
		}
		catch (const std::runtime_error&)
		{
			Threw = true;
		}
		PF_CHECK(Tests, Threw);
		PF_CHECK(Tests, Stack.begin() == Stack.end());
		PF_CHECK(Tests, DetachCount == 1);
		PF_CHECK(Tests, DestroyCount == 1);
	}

	void TestLayerPopDetachesAndDestroysWhenDetachThrows(TestRunner& Tests)
	{
		class ThrowingDetachLayer final : public PulseForge::Layer
		{
		public:
			ThrowingDetachLayer(std::string Name, int& DetachCount, int& DestroyCount)
				: Layer(Name), m_DetachCount(DetachCount), m_DestroyCount(DestroyCount) {}
			~ThrowingDetachLayer() override { ++m_DestroyCount; }
			void OnDetach() override
			{
				++m_DetachCount;
				throw std::runtime_error("expected pop detach failure");
			}
		private:
			int& m_DetachCount;
			int& m_DestroyCount;
		};

		int LayerDetachCount = 0;
		int LayerDestroyCount = 0;
		int OverlayDetachCount = 0;
		int OverlayDestroyCount = 0;
		{
			PulseForge::LayerStack Stack;
			PulseForge::Layer& Layer = Stack.PushLayer(std::make_unique<ThrowingDetachLayer>(
				"ThrowingLayer", LayerDetachCount, LayerDestroyCount));
			PulseForge::Layer& Overlay = Stack.PushOverlay(std::make_unique<ThrowingDetachLayer>(
				"ThrowingOverlay", OverlayDetachCount, OverlayDestroyCount));

			PF_CHECK(Tests, Stack.PopLayer(Layer));
			PF_CHECK(Tests, LayerDetachCount == 1);
			PF_CHECK(Tests, LayerDestroyCount == 1);
			PF_CHECK(Tests, Stack.PopOverlay(Overlay));
			PF_CHECK(Tests, OverlayDetachCount == 1);
			PF_CHECK(Tests, OverlayDestroyCount == 1);
			Stack.Clear();
		}
		PF_CHECK(Tests, LayerDetachCount == 1);
		PF_CHECK(Tests, LayerDestroyCount == 1);
		PF_CHECK(Tests, OverlayDetachCount == 1);
		PF_CHECK(Tests, OverlayDestroyCount == 1);
	}

	void TestLayerClearSurvivesDetachFailure(TestRunner& Tests)
	{
		class DetachingLayer final : public PulseForge::Layer
		{
		public:
			DetachingLayer(int& DetachCount, int& DestroyCount, bool ShouldThrow)
				: Layer("DetachingLayer"), m_DetachCount(DetachCount),
				  m_DestroyCount(DestroyCount), m_ShouldThrow(ShouldThrow) {}
			~DetachingLayer() override { ++m_DestroyCount; }
			void OnDetach() override
			{
				++m_DetachCount;
				if (m_ShouldThrow)
					throw std::runtime_error("expected detach failure");
			}
		private:
			int& m_DetachCount;
			int& m_DestroyCount;
			bool m_ShouldThrow;
		};

		int DetachCount = 0;
		int LaterDetachCount = 0;
		int DestroyCount = 0;
		int LaterDestroyCount = 0;
		PulseForge::LayerStack Stack;
		Stack.PushLayer(std::make_unique<DetachingLayer>(LaterDetachCount, LaterDestroyCount, false));
		Stack.PushOverlay(std::make_unique<DetachingLayer>(DetachCount, DestroyCount, true));
		Stack.Clear();
		PF_CHECK(Tests, DetachCount == 1);
		PF_CHECK(Tests, LaterDetachCount == 1);
		PF_CHECK(Tests, DestroyCount == 1);
		PF_CHECK(Tests, LaterDestroyCount == 1);
		PF_CHECK(Tests, Stack.begin() == Stack.end());
	}

	void TestInputState(TestRunner& Tests)
	{
		PulseForge::Input Input;
		PulseForge::KeyPressedEvent KeyDown(65, 0);
		Input.OnEvent(KeyDown);
		PF_CHECK(Tests, Input.IsKeyPressed(65));

		PulseForge::MouseButtonPressedEvent MouseDown(1);
		Input.OnEvent(MouseDown);
		PF_CHECK(Tests, Input.IsMouseButtonPressed(1));

		PulseForge::MouseMovedEvent MouseMove(24.5f, 81.0f);
		Input.OnEvent(MouseMove);
		const auto Position = Input.GetMousePosition();
		PF_CHECK(Tests, Position.X == 24.5);
		PF_CHECK(Tests, Position.Y == 81.0);

		PulseForge::WindowLostFocusEvent LostFocus;
		Input.OnEvent(LostFocus);
		PF_CHECK(Tests, !Input.IsKeyPressed(65));
		PF_CHECK(Tests, !Input.IsMouseButtonPressed(1));

		PulseForge::KeyPressedEvent KeyDownAgain(65, 1);
		Input.OnEvent(KeyDownAgain);
		PulseForge::KeyReleasedEvent KeyUp(65);
		Input.OnEvent(KeyUp);
		PF_CHECK(Tests, !Input.IsKeyPressed(65));
	}

	void TestTimestepConversions(TestRunner& Tests)
	{
		const PulseForge::Timestep DeltaTime(std::chrono::milliseconds(250));
		PF_CHECK(Tests, DeltaTime.GetSeconds() == 0.25);
		PF_CHECK(Tests, DeltaTime.GetMilliseconds() == 250.0);
		PF_CHECK(Tests, PulseForge::Timestep(1.5).GetSeconds() == 1.5);
	}

	void TestBufferDescriptionValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BufferDesc ValidDescription{ 48, BufferUsage::Vertex, "Test vertex buffer" };
		PF_CHECK(Tests, ValidateBufferDescription(ValidDescription, 48).has_value());
		PF_CHECK(Tests, ValidateBufferDescription(ValidDescription, 0).has_value());

		auto DynamicVertexDescription = ValidDescription;
		DynamicVertexDescription.IsDynamic = true;
		PF_CHECK(Tests, ValidateBufferDescription(DynamicVertexDescription, 48).has_value());

		auto ZeroSize = ValidDescription;
		ZeroSize.ByteSize = 0;
		auto ZeroSizeResult = ValidateBufferDescription(ZeroSize, 0);
		PF_CHECK(Tests, !ZeroSizeResult.has_value());
		PF_CHECK(Tests, ZeroSizeResult.error().Code == BufferCreateErrorCode::InvalidByteSize);

		auto TooMuchData = ValidateBufferDescription(ValidDescription, 49);
		PF_CHECK(Tests, !TooMuchData.has_value());
		PF_CHECK(Tests, TooMuchData.error().Code == BufferCreateErrorCode::InitialDataTooLarge);

		auto UnsupportedUsage = ValidDescription;
		UnsupportedUsage.Usage = static_cast<BufferUsage>(0xff);
		auto UnsupportedUsageResult = ValidateBufferDescription(UnsupportedUsage, 0);
		PF_CHECK(Tests, !UnsupportedUsageResult.has_value());
		PF_CHECK(Tests, UnsupportedUsageResult.error().Code == BufferCreateErrorCode::UnsupportedUsage);

		BufferDesc ConstantDescription{ 16, BufferUsage::Constant, "Test constant buffer" };
		PF_CHECK(Tests, ValidateBufferDescription(ConstantDescription, 16).has_value());
		ConstantDescription.IsDynamic = true;
		PF_CHECK(Tests, !ValidateBufferDescription(ConstantDescription, 0).has_value());
		ConstantDescription.IsDynamic = false;
		ConstantDescription.ByteSize = 17;
		PF_CHECK(Tests, !ValidateBufferDescription(ConstantDescription, 0).has_value());
	}

	class TestShader final : public PulseForge::Shader
	{
	public:
		explicit TestShader(PulseForge::ShaderStage Stage)
		{
			m_Description.Stage = Stage;
			m_Description.EntryPoint = Stage == PulseForge::ShaderStage::Vertex ? "VSMain" : "PSMain";
		}

		const PulseForge::ShaderDesc& GetDescription() const noexcept override
		{
			return m_Description;
		}

	private:
		PulseForge::ShaderDesc m_Description;
	};

	class TestBuffer final : public PulseForge::Buffer
	{
	public:
		explicit TestBuffer(PulseForge::BufferDesc Description)
			: m_Description(std::move(Description))
		{
		}

		const PulseForge::BufferDesc& GetDescription() const noexcept override
		{
			return m_Description;
		}

	private:
		PulseForge::BufferDesc m_Description;
	};

	class TestTexture final : public PulseForge::Texture
	{
	public:
		explicit TestTexture(PulseForge::TextureDesc Description)
			: m_Description(std::move(Description))
		{
		}

		const PulseForge::TextureDesc& GetDescription() const noexcept override
		{
			return m_Description;
		}

	private:
		PulseForge::TextureDesc m_Description;
	};

	class TestSampler final : public PulseForge::Sampler
	{
	public:
		explicit TestSampler(PulseForge::SamplerDesc Description = {})
			: m_Description(std::move(Description))
		{
		}

		const PulseForge::SamplerDesc& GetDescription() const noexcept override
		{
			return m_Description;
		}

	private:
		PulseForge::SamplerDesc m_Description;
	};

	class TestBindingLayout final : public PulseForge::BindingLayout
	{
	public:
		explicit TestBindingLayout(PulseForge::BindingLayoutDesc Description)
			: m_Description(std::move(Description))
		{
		}

		const PulseForge::BindingLayoutDesc& GetDescription() const noexcept override
		{
			return m_Description;
		}

	private:
		PulseForge::BindingLayoutDesc m_Description;
	};

	class TestBindingSet final : public PulseForge::BindingSet
	{
	public:
		explicit TestBindingSet(PulseForge::BindingLayoutHandle Layout)
			: m_Layout(std::move(Layout))
		{
		}

		const PulseForge::BindingLayout& GetLayout() const noexcept override
		{
			return *m_Layout;
		}

	private:
		PulseForge::BindingLayoutHandle m_Layout;
	};

	PulseForge::GraphicsPipelineDesc MakeTestPipelineDescription()
	{
		using namespace PulseForge;
		GraphicsPipelineDesc Description;
		Description.VertexShader = std::make_shared<TestShader>(ShaderStage::Vertex);
		Description.FragmentShader = std::make_shared<TestShader>(ShaderStage::Fragment);
		Description.VertexLayout.Stride = sizeof(float) * 6;
		Description.VertexLayout.Attributes = {
			{ VertexSemantic::Position, VertexFormat::Float3, 0 },
			{ VertexSemantic::Color, VertexFormat::Float3, sizeof(float) * 3 }
		};
		return Description;
	}

	void TestShaderDescriptionAndSpirVValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		ShaderDesc Description;
		Description.Stage = ShaderStage::Vertex;
		Description.EntryPoint = "VSMain";
		std::array<std::byte, 20> ValidBytecode{};
		ValidBytecode[0] = std::byte{ 0x03 };
		ValidBytecode[1] = std::byte{ 0x02 };
		ValidBytecode[2] = std::byte{ 0x23 };
		ValidBytecode[3] = std::byte{ 0x07 };

		PF_CHECK(Tests, ValidateShaderBytecode(Description, ValidBytecode).has_value());
		PF_CHECK(Tests, !ValidateShaderBytecode(Description, {}).has_value());

		std::array<std::byte, 21> MisalignedBytecode{};
		PF_CHECK(Tests, !ValidateShaderBytecode(Description, MisalignedBytecode).has_value());

		auto InvalidMagic = ValidBytecode;
		InvalidMagic[0] = std::byte{ 0 };
		PF_CHECK(Tests, !ValidateShaderBytecode(Description, InvalidMagic).has_value());

		auto InvalidStage = Description;
		InvalidStage.Stage = static_cast<ShaderStage>(0xff);
		PF_CHECK(Tests, !ValidateShaderBytecode(InvalidStage, ValidBytecode).has_value());

		auto MissingEntryPoint = Description;
		MissingEntryPoint.EntryPoint.clear();
		PF_CHECK(Tests, !ValidateShaderBytecode(MissingEntryPoint, ValidBytecode).has_value());
	}

	void TestVertexLayoutValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		VertexLayoutDesc Layout;
		Layout.Stride = sizeof(float) * 6;
		Layout.Attributes = {
			{ VertexSemantic::Position, VertexFormat::Float3, 0 },
			{ VertexSemantic::Color, VertexFormat::Float3, sizeof(float) * 3 }
		};
		PF_CHECK(Tests, ValidateVertexLayout(Layout).has_value());
		auto NormalLayout = Layout;
		NormalLayout.Stride = sizeof(float) * 9;
		NormalLayout.Attributes.push_back({ VertexSemantic::Normal, VertexFormat::Float3, sizeof(float) * 6 });
		PF_CHECK(Tests, ValidateVertexLayout(NormalLayout).has_value());

		auto ZeroStride = Layout;
		ZeroStride.Stride = 0;
		PF_CHECK(Tests, !ValidateVertexLayout(ZeroStride).has_value());

		auto NoAttributes = Layout;
		NoAttributes.Attributes.clear();
		PF_CHECK(Tests, !ValidateVertexLayout(NoAttributes).has_value());

		auto DuplicateSemantic = Layout;
		DuplicateSemantic.Attributes[1].Semantic = VertexSemantic::Position;
		PF_CHECK(Tests, !ValidateVertexLayout(DuplicateSemantic).has_value());

		auto AttributeOutsideStride = Layout;
		AttributeOutsideStride.Attributes[1].Offset = Layout.Stride - sizeof(float) * 2;
		PF_CHECK(Tests, !ValidateVertexLayout(AttributeOutsideStride).has_value());

		auto InvalidFormat = Layout;
		InvalidFormat.Attributes[0].Format = static_cast<VertexFormat>(0xff);
		PF_CHECK(Tests, !ValidateVertexLayout(InvalidFormat).has_value());

		auto InvalidSemantic = Layout;
		InvalidSemantic.Attributes[0].Semantic = static_cast<VertexSemantic>(0xff);
		PF_CHECK(Tests, !ValidateVertexLayout(InvalidSemantic).has_value());
	}

	void TestMeshDescriptionAndDrawValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::array<float, 9> Vertices = {
			0.0f, 0.5f, 0.0f,
			0.5f, -0.5f, 0.0f,
			-0.5f, -0.5f, 0.0f
		};
		const std::array<uint32_t, 3> Indices = { 0, 1, 2 };
		MeshDesc Description;
		Description.VertexLayout.Stride = sizeof(float) * 3;
		Description.VertexLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 } };
		Description.VertexData = std::as_bytes(std::span(Vertices));
		Description.Indices = Indices;
		Description.DebugName = "Test indexed mesh";

		const MeshValidationResult ValidMesh = ValidateMeshDescription(Description);
		PF_CHECK(Tests, ValidMesh.has_value());
		PF_CHECK(Tests, ValidMesh && ValidMesh->VertexCount == 3 && ValidMesh->IndexCount == 3);
		const uint32_t ValidatedIndexCount = ValidMesh ? ValidMesh->IndexCount : 0;

		auto EmptyVertices = Description;
		EmptyVertices.VertexData = {};
		const auto EmptyVertexResult = ValidateMeshDescription(EmptyVertices);
		PF_CHECK(Tests, !EmptyVertexResult.has_value());
		PF_CHECK(Tests, EmptyVertexResult.error().Code == MeshErrorCode::InvalidVertexData);

		auto InvalidLayout = Description;
		InvalidLayout.VertexLayout.Stride = 0;
		PF_CHECK(Tests, !ValidateMeshDescription(InvalidLayout).has_value());

		std::array<std::byte, sizeof(Vertices) + 1> TrailingVertexByte{};
		auto MisalignedVertexData = Description;
		MisalignedVertexData.VertexData = TrailingVertexByte;
		PF_CHECK(Tests, !ValidateMeshDescription(MisalignedVertexData).has_value());

		const std::array<uint32_t, 3> OutOfRangeIndices = { 0, 1, 3 };
		auto InvalidIndices = Description;
		InvalidIndices.Indices = OutOfRangeIndices;
		const auto InvalidIndexResult = ValidateMeshDescription(InvalidIndices);
		PF_CHECK(Tests, !InvalidIndexResult.has_value());
		PF_CHECK(Tests, InvalidIndexResult.error().Code == MeshErrorCode::InvalidIndexData);

		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		Pipeline.VertexLayout = Description.VertexLayout;
		const DrawIndexedArguments IndexedTriangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateIndexedDrawArguments(
			IndexedTriangle,
			Pipeline,
			Description.VertexLayout,
			ValidatedIndexCount).has_value());

		auto IndexRangePastEnd = IndexedTriangle;
		IndexRangePastEnd.FirstIndex = 2;
		PF_CHECK(Tests, !ValidateIndexedDrawArguments(
			IndexRangePastEnd,
			Pipeline,
			Description.VertexLayout,
			ValidatedIndexCount).has_value());
		PF_CHECK(Tests, !ValidateIndexedDrawArguments(
			IndexedTriangle,
			Pipeline,
			Description.VertexLayout,
			0).has_value());

		auto ZeroIndexedInstances = IndexedTriangle;
		ZeroIndexedInstances.InstanceCount = 0;
		PF_CHECK(Tests, !ValidateIndexedDrawArguments(
			ZeroIndexedInstances,
			Pipeline,
			Description.VertexLayout,
			ValidatedIndexCount).has_value());

		auto MismatchedPipeline = Pipeline;
		MismatchedPipeline.VertexLayout.Attributes[0].Offset = sizeof(float);
		PF_CHECK(Tests, !ValidateIndexedDrawArguments(
			IndexedTriangle,
			MismatchedPipeline,
			Description.VertexLayout,
			ValidatedIndexCount).has_value());

		const BufferDesc VertexBuffer{ Description.VertexData.size(), BufferUsage::Vertex, "Test mesh vertices" };
		const DrawArguments Triangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateMeshDrawArguments(Triangle, Pipeline, Description.VertexLayout, VertexBuffer).has_value());

		VertexLayoutDesc FullMeshLayout;
		FullMeshLayout.Stride = sizeof(float) * 6;
		FullMeshLayout.Attributes = {
			{ VertexSemantic::Position, VertexFormat::Float3, 0 },
			{ VertexSemantic::Color, VertexFormat::Float3, sizeof(float) * 3 }
		};
		VertexLayoutDesc PositionOnlyPipelineLayout;
		PositionOnlyPipelineLayout.Stride = FullMeshLayout.Stride;
		PositionOnlyPipelineLayout.Attributes = { { VertexSemantic::Position, VertexFormat::Float3, 0 } };
		auto PositionOnlyPipeline = Pipeline;
		PositionOnlyPipeline.VertexLayout = PositionOnlyPipelineLayout;
		PF_CHECK(Tests, ValidateIndexedDrawArguments(
			IndexedTriangle, PositionOnlyPipeline, FullMeshLayout, ValidatedIndexCount).has_value());
		PositionOnlyPipeline.VertexLayout.Attributes[0].Format = VertexFormat::Float2;
		PF_CHECK(Tests, !ValidateIndexedDrawArguments(
			IndexedTriangle, PositionOnlyPipeline, FullMeshLayout, ValidatedIndexCount).has_value());

		auto NonIndexedDescription = Description;
		NonIndexedDescription.Indices = {};
		const auto NonIndexedMesh = ValidateMeshDescription(NonIndexedDescription);
		PF_CHECK(Tests, NonIndexedMesh && NonIndexedMesh->VertexCount == 3 && NonIndexedMesh->IndexCount == 0);
	}

	void TestUUIDBehavior(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto Parsed = UUID::Parse("00112233-4455-6677-8899-aabbccddeeff");
		PF_CHECK(Tests, Parsed.has_value());
		PF_CHECK(Tests, Parsed && Parsed->ToString() == "00112233-4455-6677-8899-aabbccddeeff");
		PF_CHECK(Tests, Parsed && Parsed->GetHigh() == 0x0011223344556677ull);
		PF_CHECK(Tests, Parsed && Parsed->GetLow() == 0x8899aabbccddeeffull);
		PF_CHECK(Tests, !UUID::Parse("00112233-4455-6677-8899-aabbccddeefg").has_value());
		PF_CHECK(Tests, !UUID::Parse("00112233445566778899aabbccddeeff").has_value());

		const auto GeneratedFirst = UUID::Generate();
		const auto GeneratedSecond = UUID::Generate();
		PF_CHECK(Tests, GeneratedFirst.has_value() && GeneratedSecond.has_value());
		if (GeneratedFirst && GeneratedSecond)
		{
			PF_CHECK(Tests, !GeneratedFirst->IsNil());
			PF_CHECK(Tests, *GeneratedFirst != *GeneratedSecond);
			const auto ParsedGenerated = UUID::Parse(GeneratedFirst->ToString());
			PF_CHECK(Tests, ParsedGenerated && *ParsedGenerated == *GeneratedFirst);
			PF_CHECK(Tests, ((GeneratedFirst->GetHigh() >> 12) & 0xf) == 4);
			PF_CHECK(Tests, (GeneratedFirst->GetLow() >> 62) == 2);
		}
	}

	void TestSceneEntityAndHierarchy(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene TestScene;
		const UUID ParentId{ 0x1000000000000000ull, 1 };
		const UUID ChildId{ 0x2000000000000000ull, 2 };
		const UUID GrandchildId{ 0x3000000000000000ull, 3 };
		auto ParentResult = TestScene.CreateEntityWithUUID(ParentId, "Root");
		auto ChildResult = TestScene.CreateEntityWithUUID(ChildId, "Child");
		auto GrandchildResult = TestScene.CreateEntityWithUUID(GrandchildId, "Grandchild");
		PF_CHECK(Tests, ParentResult.has_value() && ChildResult.has_value() && GrandchildResult.has_value());
		if (!ParentResult || !ChildResult || !GrandchildResult)
			return;

		Entity Parent = *ParentResult;
		Entity Child = *ChildResult;
		Entity Grandchild = *GrandchildResult;
		PF_CHECK(Tests, Parent.IsValid() && Child.IsValid() && Grandchild.IsValid());
		PF_CHECK(Tests, TestScene.GetEntityCount() == 3);
		PF_CHECK(Tests, TestScene.FindEntity(ChildId) == Child);
		PF_CHECK(Tests, !TestScene.FindEntity(UUID{ 0x4000000000000000ull, 4 }).has_value());
		PF_CHECK(Tests, !TestScene.CreateEntityWithUUID(ChildId, "Duplicate UUID").has_value());
		PF_CHECK(Tests, !TestScene.CreateEntityWithUUID(UUID{}, "Nil UUID").has_value());

		auto ChildTag = Child.GetTag();
		PF_CHECK(Tests, ChildTag && ChildTag->Name == "Child");
		if (ChildTag)
		{
			ChildTag->Name = "Renamed child";
			PF_CHECK(Tests, Child.SetTag(*ChildTag).has_value());
		}

		TransformComponent ParentTransform;
		ParentTransform.Translation = { 10.0f, 0.0f, 0.0f };
		TransformComponent ChildTransform;
		ChildTransform.Translation = { 2.0f, 0.0f, 0.0f };
		TransformComponent GrandchildTransform;
		GrandchildTransform.Translation = { 1.0f, 0.0f, 0.0f };
		PF_CHECK(Tests, Parent.SetTransform(ParentTransform).has_value());
		PF_CHECK(Tests, Child.SetTransform(ChildTransform).has_value());
		PF_CHECK(Tests, Grandchild.SetTransform(GrandchildTransform).has_value());
		auto InvalidTransform = ChildTransform;
		InvalidTransform.Translation.x = std::numeric_limits<float>::infinity();
		const auto InvalidTransformResult = Child.SetTransform(InvalidTransform);
		PF_CHECK(Tests, !InvalidTransformResult.has_value());
		PF_CHECK(Tests, !InvalidTransformResult && InvalidTransformResult.error().Code == SceneErrorCode::InvalidTransform);
		InvalidTransform = ChildTransform;
		InvalidTransform.Rotation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
		PF_CHECK(Tests, !Child.SetTransform(InvalidTransform).has_value());
		InvalidTransform = ChildTransform;
		InvalidTransform.Rotation = glm::quat(2.0f, 0.0f, 0.0f, 0.0f);
		PF_CHECK(Tests, Child.SetTransform(InvalidTransform).has_value());
		const auto NormalizedTransform = Child.GetTransform();
		PF_CHECK(Tests, NormalizedTransform && glm::abs(glm::length(NormalizedTransform->Rotation) - 1.0f) < 0.0001f);
		PF_CHECK(Tests, Child.SetTransform(ChildTransform).has_value());
		PF_CHECK(Tests, Child.SetParent(Parent).has_value());
		PF_CHECK(Tests, Grandchild.SetParent(Child).has_value());

		const auto WorldMatrix = Grandchild.GetWorldMatrix();
		PF_CHECK(Tests, WorldMatrix.has_value());
		if (WorldMatrix)
		{
			const glm::vec4 WorldOrigin = *WorldMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			PF_CHECK(Tests, glm::abs(WorldOrigin.x - 13.0f) < 0.0001f);
		}

		const auto ParentChildren = Parent.GetChildren();
		const auto ChildParent = Child.GetParent();
		PF_CHECK(Tests, ParentChildren && ParentChildren->size() == 1 && ParentChildren->front() == Child);
		PF_CHECK(Tests, ChildParent && ChildParent->has_value() && **ChildParent == Parent);
		const auto CycleResult = Parent.SetParent(Grandchild);
		PF_CHECK(Tests, !CycleResult.has_value());
		PF_CHECK(Tests, !CycleResult && CycleResult.error().Code == SceneErrorCode::ParentCycle);

		PF_CHECK(Tests, Grandchild.SetParent(Parent).has_value());
		const auto ChildChildrenAfterReparent = Child.GetChildren();
		const auto ParentChildrenAfterReparent = Parent.GetChildren();
		PF_CHECK(Tests, ChildChildrenAfterReparent && ChildChildrenAfterReparent->empty());
		PF_CHECK(Tests, ParentChildrenAfterReparent && ParentChildrenAfterReparent->size() == 2);
		CameraComponent DuplicateCamera;
		DuplicateCamera.VerticalFieldOfViewRadians = 1.0f;
		DuplicateCamera.IsPrimary = true;
		PF_CHECK(Tests, Child.SetCamera(DuplicateCamera).has_value());
		PF_CHECK(Tests, Child.SetAudioListener(AudioListenerComponent{ true }).has_value());
		const AssetID DuplicateMeshAsset{ 0x4000000000000000ull, 4 };
		PF_CHECK(Tests, Child.SetMeshRenderer(MeshRendererComponent{ DuplicateMeshAsset }).has_value());
		const auto InvalidMeshRenderer = Child.SetMeshRenderer(MeshRendererComponent{ UUID{} });
		PF_CHECK(Tests, !InvalidMeshRenderer.has_value());
		PF_CHECK(Tests, !InvalidMeshRenderer && InvalidMeshRenderer.error().Code == SceneErrorCode::InvalidAssetReference);

		auto DuplicateResult = TestScene.DuplicateEntity(Child);
		PF_CHECK(Tests, DuplicateResult.has_value());
		if (DuplicateResult)
		{
			const Entity Duplicate = *DuplicateResult;
			const auto DuplicateTag = Duplicate.GetTag();
			const auto DuplicateTransform = Duplicate.GetTransform();
			const auto DuplicatedCamera = Duplicate.GetCamera();
			const auto DuplicatedListener = Duplicate.GetAudioListener();
			const auto OriginalCamera = Child.GetCamera();
			const auto OriginalListener = Child.GetAudioListener();
			const auto DuplicatedMeshRenderer = Duplicate.GetMeshRenderer();
			const auto DuplicateParent = Duplicate.GetParent();
			PF_CHECK(Tests, Duplicate.GetUUID() != Child.GetUUID());
			PF_CHECK(Tests, DuplicateTag && DuplicateTag->Name == "Renamed child Copy");
			PF_CHECK(Tests, DuplicateTransform && glm::all(glm::equal(DuplicateTransform->Translation, ChildTransform.Translation)));
			PF_CHECK(Tests, DuplicatedCamera && DuplicatedCamera->has_value() &&
				DuplicatedCamera->value().VerticalFieldOfViewRadians == DuplicateCamera.VerticalFieldOfViewRadians &&
				!DuplicatedCamera->value().IsPrimary);
			PF_CHECK(Tests, OriginalCamera && OriginalCamera->has_value() && OriginalCamera->value().IsPrimary);
			PF_CHECK(Tests, DuplicatedListener && DuplicatedListener->has_value() && !DuplicatedListener->value().IsPrimary);
			PF_CHECK(Tests, OriginalListener && OriginalListener->has_value() && OriginalListener->value().IsPrimary);
			PF_CHECK(Tests, DuplicatedMeshRenderer && DuplicatedMeshRenderer->has_value() &&
				DuplicatedMeshRenderer->value().MeshAsset == DuplicateMeshAsset);
			PF_CHECK(Tests, DuplicateParent && DuplicateParent->has_value() && **DuplicateParent == Parent);
			PF_CHECK(Tests, Duplicate.RemoveMeshRenderer().has_value());
			const auto RemovedMeshRenderer = Duplicate.GetMeshRenderer();
			PF_CHECK(Tests, RemovedMeshRenderer && !RemovedMeshRenderer->has_value());
			const auto MissingMeshRendererRemoval = Duplicate.RemoveMeshRenderer();
			PF_CHECK(Tests, !MissingMeshRendererRemoval.has_value());
			PF_CHECK(Tests, !MissingMeshRendererRemoval &&
				MissingMeshRendererRemoval.error().Code == SceneErrorCode::MissingComponent);
		}

		Scene NonPrimaryScene;
		auto NonPrimarySource = NonPrimaryScene.CreateEntity("Non-primary camera");
		PF_CHECK(Tests, NonPrimarySource.has_value());
		if (NonPrimarySource)
		{
			CameraComponent NonPrimaryCamera;
			PF_CHECK(Tests, NonPrimarySource->SetCamera(NonPrimaryCamera).has_value());
			PF_CHECK(Tests, NonPrimarySource->SetAudioListener(AudioListenerComponent{ false }).has_value());
			auto NonPrimaryDuplicate = NonPrimaryScene.DuplicateEntity(*NonPrimarySource);
			PF_CHECK(Tests, NonPrimaryDuplicate.has_value());
			if (NonPrimaryDuplicate)
			{
				const auto CopiedCamera = NonPrimaryDuplicate->GetCamera();
				const auto CopiedListener = NonPrimaryDuplicate->GetAudioListener();
				PF_CHECK(Tests, CopiedCamera && CopiedCamera->has_value() && !CopiedCamera->value().IsPrimary);
				PF_CHECK(Tests, CopiedListener && CopiedListener->has_value() && !CopiedListener->value().IsPrimary);
			}
		}

		Scene OtherScene;
		auto OtherEntityResult = OtherScene.CreateEntity("Other scene");
		PF_CHECK(Tests, OtherEntityResult.has_value());
		if (OtherEntityResult)
		{
			PF_CHECK(Tests, !Child.SetParent(*OtherEntityResult).has_value());
			PF_CHECK(Tests, !OtherScene.DestroyEntity(Child).has_value());
		}

		PF_CHECK(Tests, TestScene.DestroyEntity(Parent).has_value());
		PF_CHECK(Tests, !Parent.IsValid());
		PF_CHECK(Tests, Child.IsValid() && Grandchild.IsValid());
		const auto DetachedGrandchild = Grandchild.GetParent();
		PF_CHECK(Tests, DetachedGrandchild && !DetachedGrandchild->has_value());
		PF_CHECK(Tests, TestScene.GetEntityCount() == 3);
		PF_CHECK(Tests, !TestScene.DestroyEntity(Parent).has_value());

		const auto OrderedEntities = TestScene.GetEntities();
		PF_CHECK(Tests, std::is_sorted(OrderedEntities.begin(), OrderedEntities.end(), [](const Entity& First, const Entity& Second)
		{
			return First.GetUUID() < Second.GetUUID();
		}));
	}

	void TestPhysicsComponentsAndPersistence(TestRunner& Tests)
	{
		using namespace PulseForge;
		RigidbodyComponent Rigidbody;
		PF_CHECK(Tests, Rigidbody.Validate().has_value());
		Rigidbody.Mass = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());
		Rigidbody.Mass = 1.0f;
		Rigidbody.Friction = -0.1f;
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());
		Rigidbody.Friction = 0.2f;
		Rigidbody.Restitution = 1.1f;
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());
		Rigidbody.Restitution = 0.0f;
		Rigidbody.MotionType = static_cast<RigidbodyMotionType>(0xff);
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());

		BoxColliderComponent Collider;
		PF_CHECK(Tests, Collider.Validate().has_value());
		Collider.HalfExtents.y = 0.0f;
		PF_CHECK(Tests, !Collider.Validate().has_value());
		Collider.HalfExtents = { 0.75f, 0.5f, 0.25f };

		Scene Source;
		const auto Body = Source.CreateEntity("Physics body");
		PF_CHECK(Tests, Body.has_value());
		if (!Body)
			return;

		Rigidbody = { RigidbodyMotionType::Dynamic, 3.0f, 0.6f, 0.15f, false };
		PF_CHECK(Tests, Body->SetRigidbody(Rigidbody).has_value());
		PF_CHECK(Tests, Body->SetBoxCollider(Collider).has_value());
		Rigidbody.Mass = std::numeric_limits<float>::quiet_NaN();
		const auto InvalidRigidbodySet = Body->SetRigidbody(Rigidbody);
		PF_CHECK(Tests, !InvalidRigidbodySet && InvalidRigidbodySet.error().Code == SceneErrorCode::InvalidPhysicsComponent);
		Collider.HalfExtents.y = 0.0f;
		const auto InvalidColliderSet = Body->SetBoxCollider(Collider);
		PF_CHECK(Tests, !InvalidColliderSet && InvalidColliderSet.error().Code == SceneErrorCode::InvalidPhysicsComponent);
		Collider.HalfExtents.y = 0.5f;

		const auto Duplicated = Source.DuplicateEntity(*Body);
		PF_CHECK(Tests, Duplicated.has_value());
		if (Duplicated)
		{
			const auto DuplicateRigidbody = Duplicated->GetRigidbody();
			const auto DuplicateCollider = Duplicated->GetBoxCollider();
			PF_CHECK(Tests, DuplicateRigidbody && DuplicateRigidbody->has_value() &&
				DuplicateRigidbody->value().Mass == 3.0f);
			PF_CHECK(Tests, DuplicateCollider && DuplicateCollider->has_value() &&
				DuplicateCollider->value().HalfExtents == Collider.HalfExtents);
		}

		const auto Serialized = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		PF_CHECK(Tests, Serialized->find("\"version\": 7") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"motionType\": \"dynamic\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"halfExtents\": [\n          0.75,") != std::string::npos);

		Scene Destination;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, Destination).has_value());
		const auto LoadedBody = Destination.FindEntity(Body->GetUUID());
		PF_CHECK(Tests, LoadedBody.has_value());
		if (LoadedBody)
		{
			const auto LoadedRigidbody = LoadedBody->GetRigidbody();
			const auto LoadedCollider = LoadedBody->GetBoxCollider();
			PF_CHECK(Tests, LoadedRigidbody && LoadedRigidbody->has_value() &&
				LoadedRigidbody->value().Mass == 3.0f && LoadedRigidbody->value().Friction == 0.6f &&
				LoadedRigidbody->value().Restitution == 0.15f && !LoadedRigidbody->value().AllowSleeping);
			PF_CHECK(Tests, LoadedCollider && LoadedCollider->has_value() &&
				LoadedCollider->value().HalfExtents == Collider.HalfExtents);
		}

		const auto PrefabData = PrefabSerializer::Serialize(Source, *Body);
		PF_CHECK(Tests, PrefabData.has_value());
		if (PrefabData)
		{
			PF_CHECK(Tests, PrefabData->find("\"version\": 3") != std::string::npos);
			Scene PrefabDestination;
			const auto Instance = PrefabSerializer::Instantiate(*PrefabData, PrefabDestination);
			PF_CHECK(Tests, Instance.has_value());
			if (Instance)
			{
				const auto InstanceRigidbody = Instance->GetRigidbody();
				const auto InstanceCollider = Instance->GetBoxCollider();
				PF_CHECK(Tests, InstanceRigidbody && InstanceRigidbody->has_value() &&
					InstanceRigidbody->value().Mass == 3.0f);
				PF_CHECK(Tests, InstanceCollider && InstanceCollider->has_value() &&
					InstanceCollider->value().HalfExtents == Collider.HalfExtents);
			}
		}
	}

	void TestPhysicsSceneRuntime(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const auto Floor = Source.CreateEntity("Floor");
		const auto FallingBody = Source.CreateEntity("Falling body");
		PF_CHECK(Tests, Floor.has_value() && FallingBody.has_value());
		if (!Floor || !FallingBody)
			return;

		TransformComponent FloorTransform;
		FloorTransform.Translation.y = -0.5f;
		PF_CHECK(Tests, Floor->SetTransform(FloorTransform).has_value());
		PF_CHECK(Tests, Floor->SetRigidbody({ RigidbodyMotionType::Static }).has_value());
		PF_CHECK(Tests, Floor->SetBoxCollider({ { 5.0f, 0.5f, 5.0f } }).has_value());

		TransformComponent FallingTransform;
		FallingTransform.Translation.y = 2.0f;
		PF_CHECK(Tests, FallingBody->SetTransform(FallingTransform).has_value());
		PF_CHECK(Tests, FallingBody->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
		PF_CHECK(Tests, FallingBody->SetBoxCollider({ { 0.5f, 0.5f, 0.5f } }).has_value());

		PhysicsSceneRuntimeDesc Description;
		Description.MaxSubsteps = 4;
		PhysicsSceneRuntime Runtime(Description);
		const auto BeforeStart = Runtime.Advance(Source, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, !BeforeStart && BeforeStart.error().Code == PhysicsSceneRuntimeErrorCode::NotRunning);
		const auto ForceBeforeStart = Runtime.ApplyForce(FallingBody->GetUUID(), { 0.0f, 10.0f, 0.0f });
		PF_CHECK(Tests, !ForceBeforeStart && ForceBeforeStart.error().Code == PhysicsSceneRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, Runtime.Start(Source).has_value());
		PF_CHECK(Tests, Runtime.IsRunning());
		const auto DuplicateStart = Runtime.Start(Source);
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == PhysicsSceneRuntimeErrorCode::AlreadyRunning);

		Scene OtherScene;
		const auto DifferentScene = Runtime.Advance(OtherScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, !DifferentScene && DifferentScene.error().Code == PhysicsSceneRuntimeErrorCode::DifferentScene);
		PhysicsSceneRuntime IndependentRuntime;
		PF_CHECK(Tests, IndependentRuntime.Start(OtherScene).has_value());
		const auto NegativeDelta = Runtime.Advance(Source, Timestep(-0.1));
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == PhysicsSceneRuntimeErrorCode::InvalidDeltaTime);
		const auto NonFiniteDelta = Runtime.Advance(Source, Timestep(std::numeric_limits<double>::quiet_NaN()));
		PF_CHECK(Tests, !NonFiniteDelta && NonFiniteDelta.error().Code == PhysicsSceneRuntimeErrorCode::InvalidDeltaTime);

		constexpr double FixedStep = 1.0 / 60.0;
		const auto PartialStep = Runtime.Advance(Source, Timestep(FixedStep * 0.5));
		PF_CHECK(Tests, PartialStep && *PartialStep == 0);
		const auto FirstStep = Runtime.Advance(Source, Timestep(FixedStep * 0.5));
		PF_CHECK(Tests, FirstStep && *FirstStep == 1);
		const auto FirstUpdatedTransform = FallingBody->GetTransform();
		PF_CHECK(Tests, FirstUpdatedTransform && FirstUpdatedTransform->Translation.y < 2.0f);

		uint32_t TotalSteps = 1;
		for (uint32_t Index = 0; Index < 179; ++Index)
		{
			const auto Step = Runtime.Advance(Source, Timestep(FixedStep));
			if (!Step)
			{
				PF_CHECK(Tests, false);
				break;
			}
			TotalSteps += *Step;
		}
		PF_CHECK(Tests, TotalSteps == 180);
		const auto SettledTransform = FallingBody->GetTransform();
		PF_CHECK(Tests, SettledTransform && SettledTransform->Translation.y > 0.4f && SettledTransform->Translation.y < 0.7f);
		PF_CHECK(Tests, Runtime.ApplyForce(FallingBody->GetUUID(), { 0.0f, 100.0f, 0.0f }).has_value());
		const auto StaticForce = Runtime.ApplyForce(Floor->GetUUID(), { 0.0f, 100.0f, 0.0f });
		PF_CHECK(Tests, !StaticForce && StaticForce.error().Code == PhysicsSceneRuntimeErrorCode::StaticBody);
		const auto MissingBodyForce = Runtime.ApplyForce(UUID{ 0x5300000000000000ull, 3 }, { 0.0f, 1.0f, 0.0f });
		PF_CHECK(Tests, !MissingBodyForce && MissingBodyForce.error().Code == PhysicsSceneRuntimeErrorCode::MissingBody);
		const auto InvalidForce = Runtime.ApplyForce(
			FallingBody->GetUUID(),
			{ std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f });
		PF_CHECK(Tests, !InvalidForce && InvalidForce.error().Code == PhysicsSceneRuntimeErrorCode::InvalidForce);
		PF_CHECK(Tests, Runtime.Advance(Source, Timestep(FixedStep)).has_value());
		const auto ForcedTransform = FallingBody->GetTransform();
		PF_CHECK(Tests, ForcedTransform && SettledTransform && ForcedTransform->Translation.y > SettledTransform->Translation.y);

		const auto CappedCatchup = Runtime.Advance(Source, Timestep(0.25));
		PF_CHECK(Tests, CappedCatchup && *CappedCatchup == Description.MaxSubsteps);

		const auto RuntimeAddedBody = Source.CreateEntity("Runtime-added body");
		PF_CHECK(Tests, RuntimeAddedBody.has_value());
		if (RuntimeAddedBody)
		{
			TransformComponent RuntimeAddedTransform;
			RuntimeAddedTransform.Translation.y = 4.0f;
			PF_CHECK(Tests, RuntimeAddedBody->SetTransform(RuntimeAddedTransform).has_value());
			PF_CHECK(Tests, RuntimeAddedBody->SetRigidbody(
				{ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
			PF_CHECK(Tests, RuntimeAddedBody->SetBoxCollider({ { 0.5f, 0.5f, 0.5f } }).has_value());

			const auto ReconciledWithoutStep = Runtime.Advance(Source, Timestep(0.0));
			PF_CHECK(Tests, ReconciledWithoutStep && *ReconciledWithoutStep == 0);
			PF_CHECK(Tests, Runtime.ApplyForce(RuntimeAddedBody->GetUUID(), { 0.0f, 120.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Runtime.Advance(Source, Timestep(FixedStep)).has_value());
			const auto RuntimeAddedTransformAfterStep = RuntimeAddedBody->GetTransform();
			PF_CHECK(Tests, RuntimeAddedTransformAfterStep && RuntimeAddedTransformAfterStep->Translation.y > 4.0f);

			PF_CHECK(Tests, RuntimeAddedBody->RemoveRigidbody().has_value());
			const auto RejectedPartialPair = Runtime.Advance(Source, Timestep(0.0));
			PF_CHECK(Tests, !RejectedPartialPair &&
				RejectedPartialPair.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			const auto RemovedPartialBodyForce = Runtime.ApplyForce(RuntimeAddedBody->GetUUID(), { 0.0f, 1.0f, 0.0f });
			PF_CHECK(Tests, !RemovedPartialBodyForce &&
				RemovedPartialBodyForce.error().Code == PhysicsSceneRuntimeErrorCode::MissingBody);
			PF_CHECK(Tests, RuntimeAddedBody->RemoveBoxCollider().has_value());
			PF_CHECK(Tests, Runtime.Advance(Source, Timestep(0.0)).has_value());
		}

		const UUID RemovedEntityIdentifier = FallingBody->GetUUID();
		PF_CHECK(Tests, Source.DestroyEntity(*FallingBody).has_value());
		PF_CHECK(Tests, Runtime.Advance(Source, Timestep(0.0)).has_value());
		const auto RemovedEntityForce = Runtime.ApplyForce(RemovedEntityIdentifier, { 0.0f, 1.0f, 0.0f });
		PF_CHECK(Tests, !RemovedEntityForce && RemovedEntityForce.error().Code == PhysicsSceneRuntimeErrorCode::MissingBody);
		Runtime.Stop();
		PF_CHECK(Tests, !Runtime.IsRunning());
		const auto IndependentStep = IndependentRuntime.Advance(OtherScene, Timestep(FixedStep));
		PF_CHECK(Tests, IndependentStep && *IndependentStep == 1);
		IndependentRuntime.Stop();

		Scene InvalidScene;
		const auto Parent = InvalidScene.CreateEntity("Physics parent");
		const auto Child = InvalidScene.CreateEntity("Parented physics body");
		PF_CHECK(Tests, Parent.has_value() && Child.has_value());
		if (Parent && Child)
		{
			PF_CHECK(Tests, Child->SetParent(*Parent).has_value());
			PF_CHECK(Tests, Child->SetRigidbody({}).has_value());
			PF_CHECK(Tests, Child->SetBoxCollider({}).has_value());
			PhysicsSceneRuntime InvalidRuntime;
			const auto RejectedHierarchy = InvalidRuntime.Start(InvalidScene);
			PF_CHECK(Tests, !RejectedHierarchy &&
				RejectedHierarchy.error().Code == PhysicsSceneRuntimeErrorCode::UnsupportedHierarchy);
			PF_CHECK(Tests, !InvalidRuntime.IsRunning());
		}

		Scene IncompleteScene;
		const auto IncompleteBody = IncompleteScene.CreateEntity("Incomplete physics body");
		PF_CHECK(Tests, IncompleteBody.has_value());
		if (IncompleteBody)
		{
			PF_CHECK(Tests, IncompleteBody->SetRigidbody({}).has_value());
			PhysicsSceneRuntime IncompleteRuntime;
			const auto RejectedIncompletePair = IncompleteRuntime.Start(IncompleteScene);
			PF_CHECK(Tests, !RejectedIncompletePair &&
				RejectedIncompletePair.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			PF_CHECK(Tests, !IncompleteRuntime.IsRunning());
		}

		PhysicsSceneRuntime InvalidSettingsRuntime(
			PhysicsSceneRuntimeDesc{ 0.0, 0, 0.0, glm::vec3(0.0f) });
		const auto RejectedSettings = InvalidSettingsRuntime.Start(Source);
		PF_CHECK(Tests, !RejectedSettings && RejectedSettings.error().Code == PhysicsSceneRuntimeErrorCode::InvalidSettings);
		PhysicsSceneRuntime UnrepresentableStepRuntime(PhysicsSceneRuntimeDesc{
			1.0e100, 1, 1.0e101, glm::vec3(0.0f) });
		const auto RejectedStep = UnrepresentableStepRuntime.Start(Source);
		PF_CHECK(Tests, !RejectedStep && RejectedStep.error().Code == PhysicsSceneRuntimeErrorCode::InvalidSettings);
		PhysicsSceneRuntime UnderflowingStepRuntime(PhysicsSceneRuntimeDesc{
			std::numeric_limits<double>::denorm_min(), 1, 1.0, glm::vec3(0.0f) });
		const auto RejectedUnderflowingStep = UnderflowingStepRuntime.Start(Source);
		PF_CHECK(Tests, !RejectedUnderflowingStep &&
			RejectedUnderflowingStep.error().Code == PhysicsSceneRuntimeErrorCode::InvalidSettings);

		Scene ScaledExtentsOverflowScene;
		const auto OverflowingBody = ScaledExtentsOverflowScene.CreateEntity("Overflowing collider");
		PF_CHECK(Tests, OverflowingBody.has_value());
		if (OverflowingBody)
		{
			TransformComponent OverflowingTransform;
			OverflowingTransform.Scale = glm::vec3(std::numeric_limits<float>::max());
			PF_CHECK(Tests, OverflowingBody->SetTransform(OverflowingTransform).has_value());
			PF_CHECK(Tests, OverflowingBody->SetRigidbody({}).has_value());
			PF_CHECK(Tests, OverflowingBody->SetBoxCollider({ glm::vec3(std::numeric_limits<float>::max()) }).has_value());
			PhysicsSceneRuntime OverflowRuntime;
			const auto RejectedOverflow = OverflowRuntime.Start(ScaledExtentsOverflowScene);
			PF_CHECK(Tests, !RejectedOverflow &&
				RejectedOverflow.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			PF_CHECK(Tests, !OverflowRuntime.IsRunning());
		}
	}

	void TestEntityHandleIdentityAndLifetime(TestRunner& Tests)
	{
		using namespace PulseForge;
		Entity StaleHandle;
		{
			Scene TemporaryScene;
			auto Created = TemporaryScene.CreateEntity("Temporary");
			PF_CHECK(Tests, Created.has_value());
			if (Created)
				StaleHandle = *Created;
			PF_CHECK(Tests, StaleHandle.IsValid());
		}
		PF_CHECK(Tests, !StaleHandle.IsValid());
		const auto DestroyedSceneTag = StaleHandle.GetTag();
		PF_CHECK(Tests, !DestroyedSceneTag && DestroyedSceneTag.error().Code == SceneErrorCode::InvalidEntity);

		Scene ReuseScene;
		const UUID ReusedIdentifier{ 0x6f00000000000000ull, 42 };
		auto Original = ReuseScene.CreateEntityWithUUID(ReusedIdentifier, "Original incarnation");
		PF_CHECK(Tests, Original.has_value());
		if (!Original)
			return;

		const Entity OldHandle = *Original;
		PF_CHECK(Tests, OldHandle.IsValid());
		PF_CHECK(Tests, ReuseScene.DestroyEntity(OldHandle).has_value());
		PF_CHECK(Tests, !OldHandle.IsValid());
		PF_CHECK(Tests, !ReuseScene.FindEntity(ReusedIdentifier).has_value());
		const auto DestroyedTag = OldHandle.GetTag();
		PF_CHECK(Tests, !DestroyedTag && DestroyedTag.error().Code == SceneErrorCode::InvalidEntity);
		const auto DestroyedTransform = OldHandle.GetTransform();
		PF_CHECK(Tests, !DestroyedTransform && DestroyedTransform.error().Code == SceneErrorCode::InvalidEntity);
		const auto StaleTagSet = OldHandle.SetTag(TagComponent{ "Must not reach replacement" });
		PF_CHECK(Tests, !StaleTagSet && StaleTagSet.error().Code == SceneErrorCode::InvalidEntity);
		const auto StaleTransformSet = OldHandle.SetTransform(TransformComponent{});
		PF_CHECK(Tests, !StaleTransformSet && StaleTransformSet.error().Code == SceneErrorCode::InvalidEntity);
		const auto StaleDestroy = ReuseScene.DestroyEntity(OldHandle);
		PF_CHECK(Tests, !StaleDestroy && StaleDestroy.error().Code == SceneErrorCode::InvalidEntity);

		auto Replacement = ReuseScene.CreateEntityWithUUID(ReusedIdentifier, "Replacement incarnation");
		PF_CHECK(Tests, Replacement.has_value());
		if (!Replacement)
			return;
		const Entity NewHandle = *Replacement;
		PF_CHECK(Tests, NewHandle.IsValid());
		PF_CHECK(Tests, OldHandle.GetUUID() == NewHandle.GetUUID());
		PF_CHECK(Tests, OldHandle != NewHandle);
		PF_CHECK(Tests, !OldHandle.IsValid());
		PF_CHECK(Tests, ReuseScene.FindEntity(ReusedIdentifier) == NewHandle);
		const auto ReplacementTag = NewHandle.GetTag();
		PF_CHECK(Tests, ReplacementTag && ReplacementTag->Name == "Replacement incarnation");
		const auto StillStaleTag = OldHandle.GetTag();
		PF_CHECK(Tests, !StillStaleTag && StillStaleTag.error().Code == SceneErrorCode::InvalidEntity);
		PF_CHECK(Tests, NewHandle.SetTag(TagComponent{ "Replacement works" }).has_value());
		const auto UpdatedReplacementTag = NewHandle.GetTag();
		PF_CHECK(Tests, UpdatedReplacementTag && UpdatedReplacementTag->Name == "Replacement works");

		PF_CHECK(Tests, ReuseScene.DestroyEntity(NewHandle).has_value());
		bool ReusePreservedStaleHandle = true;
		// Exercise beyond EnTT's current packed-entity generation cycle for one repeatedly reused slot.
		for (uint32_t Reuse = 0; Reuse < 5000; ++Reuse)
		{
			auto RecycledEntity = ReuseScene.CreateEntityWithUUID(ReusedIdentifier, "Recycled incarnation");
			if (!RecycledEntity)
			{
				ReusePreservedStaleHandle = false;
				break;
			}
			if (!RecycledEntity->IsValid() || OldHandle.IsValid() || !ReuseScene.DestroyEntity(*RecycledEntity))
			{
				ReusePreservedStaleHandle = false;
				break;
			}
		}
		PF_CHECK(Tests, ReusePreservedStaleHandle);
	}

	void TestSceneRenderSnapshot(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene TestScene;
		const UUID CameraID{ 0x1000000000000000ull, 1 };
		const UUID ParentID{ 0x2000000000000000ull, 2 };
		const UUID FirstMeshID{ 0x3000000000000000ull, 3 };
		const UUID SharedMeshID{ 0x3800000000000000ull, 38 };
		const UUID SecondMeshID{ 0x4000000000000000ull, 4 };
		const UUID LightID{ 0x7000000000000000ull, 7 };
		const AssetID FirstMeshAssetID{ 0x5000000000000000ull, 5 };
		const AssetID SecondMeshAssetID{ 0x6000000000000000ull, 6 };
		const AssetID MaterialAssetID{ 0x6100000000000000ull, 61 };
		const AssetID EnvironmentAssetID{ 0x6200000000000000ull, 62 };
		auto Camera = TestScene.CreateEntityWithUUID(CameraID, "Camera");
		auto Parent = TestScene.CreateEntityWithUUID(ParentID, "Parent");
		auto FirstMesh = TestScene.CreateEntityWithUUID(FirstMeshID, "First mesh");
		auto SharedMesh = TestScene.CreateEntityWithUUID(SharedMeshID, "Shared mesh instance");
		auto SecondMesh = TestScene.CreateEntityWithUUID(SecondMeshID, "Second mesh");
		auto Light = TestScene.CreateEntityWithUUID(LightID, "Sun");
		PF_CHECK(Tests, Camera && Parent && FirstMesh && SharedMesh && SecondMesh && Light);
		if (!Camera || !Parent || !FirstMesh || !SharedMesh || !SecondMesh || !Light)
			return;

		TransformComponent CameraTransform;
		CameraTransform.Translation = { 0.0f, 0.0f, 5.0f };
		CameraComponent PrimaryCameraComponent;
		PrimaryCameraComponent.IsPrimary = true;
		PF_CHECK(Tests, Camera->SetTransform(CameraTransform).has_value());
		PF_CHECK(Tests, Camera->SetCamera(PrimaryCameraComponent).has_value());
		TransformComponent LightTransform;
		LightTransform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		PF_CHECK(Tests, Light->SetTransform(LightTransform).has_value());
		PF_CHECK(Tests, Light->SetDirectionalLight(DirectionalLightComponent{ glm::vec3(0.5f, 0.6f, 0.7f), 2.0f }).has_value());
		PF_CHECK(Tests, Light->SetPointLight(PointLightComponent{ glm::vec3(1.0f, 0.5f, 0.25f), 4.0f, 12.0f }).has_value());
		SpotLightComponent SnapshotSpotLight{ glm::vec3(0.2f, 0.4f, 1.0f), 8.0f, 20.0f, 10.0f, 25.0f };
		SnapshotSpotLight.CastShadows = true;
		SnapshotSpotLight.ShadowBias = 0.004f;
		PF_CHECK(Tests, Light->SetSpotLight(SnapshotSpotLight).has_value());
		PointLightComponent InvalidPointLight;
		InvalidPointLight.Intensity = -1.0f;
		const auto InvalidPointLightSet = Light->SetPointLight(InvalidPointLight);
		PF_CHECK(Tests, !InvalidPointLightSet && InvalidPointLightSet.error().Code == SceneErrorCode::InvalidPointLight);
		SpotLightComponent InvalidSpotLight;
		InvalidSpotLight.OuterConeAngleDegrees = InvalidSpotLight.InnerConeAngleDegrees;
		const auto InvalidSpotLightSet = Light->SetSpotLight(InvalidSpotLight);
		PF_CHECK(Tests, !InvalidSpotLightSet && InvalidSpotLightSet.error().Code == SceneErrorCode::InvalidSpotLight);
		const auto ExistingPointLight = Light->GetPointLight();
		const auto ExistingSpotLight = Light->GetSpotLight();
		PF_CHECK(Tests, ExistingPointLight && ExistingPointLight->has_value() && ExistingPointLight->value().Intensity == 4.0f);
		PF_CHECK(Tests, ExistingSpotLight && ExistingSpotLight->has_value() && ExistingSpotLight->value().Intensity == 8.0f);
		auto DuplicateLightEntity = TestScene.DuplicateEntity(*Light);
		PF_CHECK(Tests, DuplicateLightEntity.has_value());
		if (DuplicateLightEntity)
		{
			const auto CopiedLight = DuplicateLightEntity->GetDirectionalLight();
			PF_CHECK(Tests, CopiedLight && CopiedLight->has_value() && CopiedLight->value().Intensity == 2.0f);
			const auto CopiedPoint = DuplicateLightEntity->GetPointLight();
			const auto CopiedSpot = DuplicateLightEntity->GetSpotLight();
			PF_CHECK(Tests, CopiedPoint && CopiedPoint->has_value() && CopiedPoint->value().Range == 12.0f);
			PF_CHECK(Tests, CopiedSpot && CopiedSpot->has_value() && CopiedSpot->value().OuterConeAngleDegrees == 25.0f);
			(void)TestScene.DestroyEntity(*DuplicateLightEntity);
		}
		DirectionalLightComponent InvalidLight;
		InvalidLight.Intensity = -1.0f;
		const auto InvalidLightSet = Light->SetDirectionalLight(InvalidLight);
		PF_CHECK(Tests, !InvalidLightSet && InvalidLightSet.error().Code == SceneErrorCode::InvalidDirectionalLight);

		TransformComponent ParentTransform;
		ParentTransform.Translation = { 2.0f, 1.0f, 0.0f };
		TransformComponent FirstMeshTransform;
		FirstMeshTransform.Translation = { -1.0f, 0.0f, -2.0f };
		TransformComponent SharedMeshTransform;
		SharedMeshTransform.Translation = { 3.0f, 0.0f, -1.0f };
		TransformComponent SecondMeshTransform;
		SecondMeshTransform.Translation = { 0.0f, -1.0f, -4.0f };
		PF_CHECK(Tests, Parent->SetTransform(ParentTransform).has_value());
		PF_CHECK(Tests, Light->SetParent(*Parent).has_value());
		PF_CHECK(Tests, Light->SetEnvironmentLight(EnvironmentLightComponent{ EnvironmentAssetID, 1.5f }).has_value());
		PF_CHECK(Tests, FirstMesh->SetTransform(FirstMeshTransform).has_value());
		PF_CHECK(Tests, SharedMesh->SetTransform(SharedMeshTransform).has_value());
		PF_CHECK(Tests, SecondMesh->SetTransform(SecondMeshTransform).has_value());
		PF_CHECK(Tests, FirstMesh->SetParent(*Parent).has_value());
		PF_CHECK(Tests, FirstMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID }).has_value());
		PF_CHECK(Tests, SharedMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID, MaterialAssetID }).has_value());
		PF_CHECK(Tests, SecondMesh->SetMeshRenderer(MeshRendererComponent{ SecondMeshAssetID }).has_value());

		const auto Snapshot = SceneRenderSnapshotBuilder::Build(TestScene, 16.0f / 9.0f);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot)
			return;
		PF_CHECK(Tests, Snapshot->CameraEntity == CameraID);
		PF_CHECK(Tests, glm::all(glm::equal(Snapshot->CameraWorldPosition, glm::vec3(0.0f, 0.0f, 5.0f))));
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->Entity == LightID);
		PF_CHECK(Tests, Snapshot->DirectionalLight && glm::abs(Snapshot->DirectionalLight->RayDirection.x + 1.0f) < 0.0001f);
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->Intensity == 2.0f);
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->CastShadows);
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->ShadowDistance == 100.0f);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && Snapshot->EnvironmentLight->Entity == LightID);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && Snapshot->EnvironmentLight->HdrImage == EnvironmentAssetID);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && Snapshot->EnvironmentLight->Intensity == 1.5f);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && glm::abs((glm::mat3(
			Snapshot->EnvironmentLight->WorldRotation) * glm::vec3(0.0f, 0.0f, -1.0f)).x + 1.0f) < 0.0001f);
		PF_CHECK(Tests, Snapshot->PointLights.size() == 1 && Snapshot->PointLights[0].Entity == LightID);
		PF_CHECK(Tests, Snapshot->PointLights.size() == 1 && glm::all(glm::equal(
			Snapshot->PointLights[0].WorldPosition, glm::vec3(2.0f, 1.0f, 0.0f))));
		PF_CHECK(Tests, Snapshot->SpotLights.size() == 1 && Snapshot->SpotLights[0].Entity == LightID);
		PF_CHECK(Tests, Snapshot->SpotLights.size() == 1 && glm::abs(Snapshot->SpotLights[0].WorldDirection.x + 1.0f) < 0.0001f);
		PF_CHECK(Tests, Snapshot->SpotLights.size() == 1 && Snapshot->SpotLights[0].CastShadows &&
			Snapshot->SpotLights[0].ShadowBias == 0.004f && Snapshot->SpotLights[0].ShadowSoftness == 1.5f);
		PF_CHECK(Tests, Snapshot->Meshes.size() == 3);
		PF_CHECK(Tests, Snapshot->Meshes[0].Entity == FirstMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[1].Entity == SharedMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[2].Entity == SecondMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[0].MeshAsset == FirstMeshAssetID);
		PF_CHECK(Tests, Snapshot->Meshes[1].MeshAsset == FirstMeshAssetID);
		PF_CHECK(Tests, Snapshot->Meshes[2].MeshAsset == SecondMeshAssetID);
		PF_CHECK(Tests, !Snapshot->Meshes[0].MaterialAsset.has_value());
		PF_CHECK(Tests, Snapshot->Meshes[1].MaterialAsset == MaterialAssetID);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].x - 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].y - 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].z + 2.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[1].WorldTransform[3].x - 3.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[1].WorldTransform[3].z + 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[2].WorldTransform[3].y + 1.0f) < 0.0001f);
		const glm::vec4 CameraPositionClip = Snapshot->ViewProjection * glm::vec4(CameraTransform.Translation, 1.0f);
		PF_CHECK(Tests, glm::abs(CameraPositionClip.x) < 0.0001f && glm::abs(CameraPositionClip.y) < 0.0001f);
		auto DuplicatedCamera = TestScene.DuplicateEntity(*Camera);
		PF_CHECK(Tests, DuplicatedCamera.has_value());
		if (DuplicatedCamera)
		{
			const auto DuplicateCameraComponent = DuplicatedCamera->GetCamera();
			PF_CHECK(Tests, DuplicateCameraComponent && DuplicateCameraComponent->has_value() &&
				!DuplicateCameraComponent->value().IsPrimary);
			const auto SnapshotAfterCameraDuplication = SceneRenderSnapshotBuilder::Build(TestScene, 16.0f / 9.0f);
			PF_CHECK(Tests, SnapshotAfterCameraDuplication && SnapshotAfterCameraDuplication->CameraEntity == CameraID);
		}
		auto SecondLight = TestScene.CreateEntity("Second sun");
		PF_CHECK(Tests, SecondLight.has_value());
		if (SecondLight)
		{
			PF_CHECK(Tests, SecondLight->SetDirectionalLight(DirectionalLightComponent{}).has_value());
			const auto MultipleLights = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 16.0f / 9.0f);
			PF_CHECK(Tests, !MultipleLights && MultipleLights.error().Code == SceneRenderSnapshotErrorCode::MultipleDirectionalLights);
			(void)TestScene.DestroyEntity(*SecondLight);
		}
		auto SecondEnvironment = TestScene.CreateEntity("Second environment");
		PF_CHECK(Tests, SecondEnvironment.has_value());
		if (SecondEnvironment)
		{
			PF_CHECK(Tests, SecondEnvironment->SetEnvironmentLight(EnvironmentLightComponent{ EnvironmentAssetID, 0.5f }).has_value());
			const auto MultipleEnvironments = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 16.0f / 9.0f);
			PF_CHECK(Tests, !MultipleEnvironments &&
				MultipleEnvironments.error().Code == SceneRenderSnapshotErrorCode::MultipleEnvironmentLights);
			(void)TestScene.DestroyEntity(*SecondEnvironment);
		}
		TransformComponent SingularMeshTransform;
		SingularMeshTransform.Scale.x = 0.0f;
		PF_CHECK(Tests, FirstMesh->SetTransform(SingularMeshTransform).has_value());
		const auto SingularMeshWorld = FirstMesh->GetWorldMatrix();
		PF_CHECK(Tests, SingularMeshWorld.has_value());
		PF_CHECK(Tests, SingularMeshWorld && !BuildNormalTransform(*SingularMeshWorld));
		PF_CHECK(Tests, FirstMesh->SetTransform(FirstMeshTransform).has_value());
		const auto NonUniformNormal = BuildNormalTransform(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f)));
		PF_CHECK(Tests, NonUniformNormal.has_value());
		if (NonUniformNormal)
			PF_CHECK(Tests, glm::abs((*NonUniformNormal)[0][0] - 0.5f) < 1.0e-6f &&
				glm::abs((*NonUniformNormal)[1][1] - 1.0f) < 1.0e-6f &&
				glm::abs((*NonUniformNormal)[2][2] - 2.0f) < 1.0e-6f);
		PF_CHECK(Tests, !BuildNormalTransform(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f))));

		const auto NilCamera = SceneRenderSnapshotBuilder::Build(TestScene, UUID{}, 1.0f);
		PF_CHECK(Tests, !NilCamera && NilCamera.error().Code == SceneRenderSnapshotErrorCode::InvalidCameraEntity);
		const auto MissingCamera = SceneRenderSnapshotBuilder::Build(TestScene, FirstMeshID, 1.0f);
		PF_CHECK(Tests, !MissingCamera && MissingCamera.error().Code == SceneRenderSnapshotErrorCode::MissingCameraComponent);
		const auto InvalidAspect = SceneRenderSnapshotBuilder::Build(TestScene, 0.0f);
		PF_CHECK(Tests, !InvalidAspect && InvalidAspect.error().Code == SceneRenderSnapshotErrorCode::InvalidCamera);
		CameraComponent NonPrimaryCameraComponent;
		PF_CHECK(Tests, Camera->SetCamera(NonPrimaryCameraComponent).has_value());
		const auto NoPrimaryCamera = SceneRenderSnapshotBuilder::Build(TestScene, 1.0f);
		PF_CHECK(Tests, !NoPrimaryCamera &&
			NoPrimaryCamera.error().Code == SceneRenderSnapshotErrorCode::MissingPrimaryCamera);
		PF_CHECK(Tests, Camera->SetCamera(PrimaryCameraComponent).has_value());
		auto AdditionalCamera = TestScene.CreateEntityWithUUID(UUID{ 0x1800000000000000ull, 18 }, "Second camera");
		PF_CHECK(Tests, AdditionalCamera.has_value());
		if (AdditionalCamera)
		{
			PF_CHECK(Tests, AdditionalCamera->SetCamera(PrimaryCameraComponent).has_value());
			const auto MultiplePrimaryCameras = SceneRenderSnapshotBuilder::Build(TestScene, 1.0f);
			PF_CHECK(Tests, !MultiplePrimaryCameras &&
				MultiplePrimaryCameras.error().Code == SceneRenderSnapshotErrorCode::MultiplePrimaryCameras);
		}

		TransformComponent SingularCameraTransform = CameraTransform;
		SingularCameraTransform.Scale.x = 0.0f;
		PF_CHECK(Tests, Camera->SetTransform(SingularCameraTransform).has_value());
		const auto SingularCamera = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 1.0f);
		PF_CHECK(Tests, !SingularCamera && SingularCamera.error().Code == SceneRenderSnapshotErrorCode::InvalidCameraTransform);

		Scene CameraLessScene;
		auto CameraLessMesh = CameraLessScene.CreateEntity("Editor view mesh");
		PF_CHECK(Tests, CameraLessMesh.has_value());
		if (CameraLessMesh)
			PF_CHECK(Tests, CameraLessMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID }).has_value());

		const glm::mat4 TransientViewProjection(1.0f);
		const auto TransientView = SceneRenderSnapshotBuilder::BuildForView(
			CameraLessScene, TransientViewProjection, glm::vec3(3.0f, 4.0f, 5.0f));
		PF_CHECK(Tests, TransientView.has_value());
		PF_CHECK(Tests, TransientView && !TransientView->HasCameraFrustum);
		if (TransientView)
		{
			PF_CHECK(Tests, !TransientView->CameraEntity.has_value());
			PF_CHECK(Tests, glm::all(glm::equal(TransientView->CameraWorldPosition, glm::vec3(3.0f, 4.0f, 5.0f))));
			PF_CHECK(Tests, TransientView->Meshes.size() == 1);
			PF_CHECK(Tests, !TransientView->DirectionalLight.has_value());
			PF_CHECK(Tests, TransientView->ViewProjection[0][0] == 1.0f);
		}

		CameraComponent CascadeCamera;
		CascadeCamera.NearClipPlane = 0.1f;
		CascadeCamera.FarClipPlane = 100.0f;
		const auto CascadeProjection = CascadeCamera.GetProjectionMatrix(16.0f / 9.0f);
		PF_CHECK(Tests, CascadeProjection.has_value());
		if (CascadeProjection)
		{
			const auto ExplicitTransientView = SceneRenderSnapshotBuilder::BuildForView(
				CameraLessScene, glm::mat4(1.0f), *CascadeProjection,
				glm::vec3(3.0f, 4.0f, 5.0f), 0.1f, 100.0f);
			PF_CHECK(Tests, ExplicitTransientView && ExplicitTransientView->HasCameraFrustum);
			const auto Cascades = BuildDirectionalShadowCascades(
				glm::mat4(1.0f), *CascadeProjection, 0.1f, 100.0f, 60.0f, glm::vec3(0.0f, 1.0f, 0.0f));
			PF_CHECK(Tests, Cascades.has_value());
			if (Cascades)
			{
				PF_CHECK(Tests, Cascades->ShadowDistance == 60.0f);
				PF_CHECK(Tests, Cascades->Cascades.front().NearDistance == 0.1f);
				PF_CHECK(Tests, Cascades->Cascades.back().FarDistance == 60.0f);
				for (size_t Index = 0; Index < Cascades->Cascades.size(); ++Index)
				{
					PF_CHECK(Tests, Cascades->Cascades[Index].FarDistance > Cascades->Cascades[Index].NearDistance);
					if (Index > 0)
						PF_CHECK(Tests, Cascades->Cascades[Index].NearDistance == Cascades->Cascades[Index - 1].FarDistance);
					for (int Column = 0; Column < 4; ++Column)
						for (int Row = 0; Row < 4; ++Row)
							PF_CHECK(Tests, std::isfinite(Cascades->Cascades[Index].ViewProjection[Column][Row]));

					for (const float Distance : { Cascades->Cascades[Index].NearDistance, Cascades->Cascades[Index].FarDistance })
					{
						for (const float Y : { -1.0f, 1.0f })
						{
							for (const float X : { -1.0f, 1.0f })
							{
								glm::vec4 ViewCorner = glm::inverse(*CascadeProjection) * glm::vec4(X, Y, 1.0f, 1.0f);
								ViewCorner /= ViewCorner.w;
								ViewCorner = glm::vec4(glm::vec3(ViewCorner) * (Distance / -ViewCorner.z), 1.0f);
								const glm::vec4 ShadowClip = Cascades->Cascades[Index].ViewProjection * ViewCorner;
								const glm::vec3 ShadowNdc = glm::vec3(ShadowClip) / ShadowClip.w;
								PF_CHECK(Tests, ShadowNdc.x >= -1.001f && ShadowNdc.x <= 1.001f);
								PF_CHECK(Tests, ShadowNdc.y >= -1.001f && ShadowNdc.y <= 1.001f);
								PF_CHECK(Tests, ShadowNdc.z >= 0.0f && ShadowNdc.z <= 1.0f);
							}
						}
					}
				}
			}
			const glm::mat4 SlightCameraTranslation = glm::translate(glm::mat4(1.0f), glm::vec3(0.001f, 0.0f, 0.0f));
			const auto StableCascades = BuildDirectionalShadowCascades(
				SlightCameraTranslation, *CascadeProjection, 0.1f, 100.0f, 60.0f, glm::vec3(0.0f, 1.0f, 0.0f));
			PF_CHECK(Tests, StableCascades.has_value());
			if (Cascades && StableCascades)
				for (int Column = 0; Column < 4; ++Column)
					for (int Row = 0; Row < 4; ++Row)
						PF_CHECK(Tests, glm::abs(Cascades->Cascades[0].ViewProjection[Column][Row] -
							StableCascades->Cascades[0].ViewProjection[Column][Row]) < 0.001f);
			PF_CHECK(Tests, !BuildDirectionalShadowCascades(glm::mat4(1.0f), *CascadeProjection,
				0.1f, 100.0f, 10.0f, glm::vec3(0.0f)).has_value());
			PF_CHECK(Tests, !BuildDirectionalShadowCascades(glm::mat4(1.0f), *CascadeProjection,
				0.1f, 100.0f, 0.05f, glm::vec3(0.0f, 1.0f, 0.0f)).has_value());
		}

		glm::mat4 InvalidTransientView(1.0f);
		InvalidTransientView[2][1] = std::numeric_limits<float>::quiet_NaN();
		const auto InvalidView = SceneRenderSnapshotBuilder::BuildForView(
			CameraLessScene, InvalidTransientView, glm::vec3(0.0f));
		PF_CHECK(Tests, !InvalidView &&
			InvalidView.error().Code == SceneRenderSnapshotErrorCode::InvalidViewProjection);
	}

	void TestEditorViewportMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		PF_CHECK(Tests, GetViewportToolForShortcut('q') == ViewportTool::Select);
		PF_CHECK(Tests, GetViewportToolForShortcut('Q') == ViewportTool::Select);
		PF_CHECK(Tests, GetViewportToolForShortcut('w') == ViewportTool::Move);
		PF_CHECK(Tests, GetViewportToolForShortcut('W') == ViewportTool::Move);
		PF_CHECK(Tests, GetViewportToolForShortcut('e') == ViewportTool::Rotate);
		PF_CHECK(Tests, GetViewportToolForShortcut('E') == ViewportTool::Rotate);
		PF_CHECK(Tests, GetViewportToolForShortcut('r') == ViewportTool::Scale);
		PF_CHECK(Tests, GetViewportToolForShortcut('R') == ViewportTool::Scale);
		PF_CHECK(Tests, !GetViewportToolForShortcut('x'));
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('1') == ViewportTool::Select);
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('2') == ViewportTool::Move);
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('3') == ViewportTool::Rotate);
		PF_CHECK(Tests, GetFocusedViewportToolForShortcut('4') == ViewportTool::Scale);
		PF_CHECK(Tests, !GetFocusedViewportToolForShortcut('w'));
		const ViewportShortcutContext Ready{ .SceneAvailable = true };
		PF_CHECK(Tests, CanHandleViewportToolShortcuts(Ready));
		const ViewportShortcutContext Focused{ .SceneAvailable = true, .ViewportKeyboardFocused = true };
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts(Focused));
		PF_CHECK(Tests, CanHandleFocusedViewportToolShortcuts(Focused));
		PF_CHECK(Tests, ResolveViewportKeyboardFocus(false,
			ViewportKeyboardFocusEvent::RenderedImageClicked));
		PF_CHECK(Tests, ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::NoChange));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::OtherEditorPanelClicked));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::ApplicationFocusLost));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::ViewportUnavailable));
		PF_CHECK(Tests, !ResolveViewportKeyboardFocus(true,
			ViewportKeyboardFocusEvent::PlayModeStarted));
		PF_CHECK(Tests, !CanHandleFocusedViewportToolShortcuts({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanHandleFocusedViewportToolShortcuts({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ModifierHeld = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .RuntimeActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .CameraNavigationActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .RightMouseHeld = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .ActiveImGuiItem = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .PopupOpen = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({ .SceneAvailable = true, .ModifierHeld = true }));
		PF_CHECK(Tests, !CanHandleViewportToolShortcuts({}));

		const ViewportCameraNavigationContext KeyboardNavigation{
			.SceneAvailable = true,
			.ViewportKeyboardFocused = true,
			.ApplicationFocused = true
		};
		PF_CHECK(Tests, CanNavigateViewportCamera(KeyboardNavigation));
		PF_CHECK(Tests, !CanNavigateViewportCamera({ .SceneAvailable = true, .ApplicationFocused = true }));
		PF_CHECK(Tests, CanNavigateViewportCamera({
			.SceneAvailable = true, .RightMouseNavigationActive = true, .ApplicationFocused = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .RuntimeActive = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .ActiveImGuiItem = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .PopupOpen = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .ModifierHeld = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .GizmoInteractionActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = true, .CameraGestureActive = true }));
		PF_CHECK(Tests, !CanNavigateViewportCamera({
			.SceneAvailable = true, .ViewportKeyboardFocused = true, .ApplicationFocused = false }));

		const glm::vec3 CameraForward{ 0.0f, 0.0f, -1.0f };
		const glm::vec3 CameraRight{ 1.0f, 0.0f, 0.0f };
		const auto ForwardStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true }, 5.0f, 0.5);
		const auto BackwardStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Backward = true }, 5.0f, 0.1);
		const auto RightStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Right = true }, 5.0f, 0.1);
		const auto LeftStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Left = true }, 5.0f, 0.1);
		const auto UpStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Up = true }, 5.0f, 0.1);
		const auto DownStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Down = true }, 5.0f, 0.1);
		const auto DiagonalStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true, .Right = true }, 5.0f, 0.1);
		const auto FastStep = CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true, .Fast = true }, 5.0f, 0.1);
		PF_CHECK(Tests, glm::length(ForwardStep - glm::vec3(0.0f, 0.0f, -0.5f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(BackwardStep - glm::vec3(0.0f, 0.0f, 0.5f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(RightStep - glm::vec3(0.5f, 0.0f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(LeftStep - glm::vec3(-0.5f, 0.0f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(UpStep - glm::vec3(0.0f, 0.5f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(DownStep - glm::vec3(0.0f, -0.5f, 0.0f)) < 1.0e-5f);
		PF_CHECK(Tests, std::abs(glm::length(DiagonalStep) - glm::length(RightStep)) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(FastStep - ForwardStep * 4.0f) < 1.0e-5f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true }, 5.0f, 0.0)) == 0.0f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, {}, 5.0f, 0.1)) == 0.0f);
		PF_CHECK(Tests, glm::length(CalculateViewportCameraMoveOffset(
			CameraForward, CameraRight, { .Forward = true }, 5.0f, 2.0) - ForwardStep) < 1.0e-5f);

		const float WheelDollyDelta = GetViewportWheelDollyDelta(1.0f);
		const auto ZoomWithoutPivot = CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, WheelDollyDelta);
		const auto ZoomOutWithoutPivot = CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, GetViewportWheelDollyDelta(-1.0f));
		const auto ZoomWithPivot = CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, glm::vec3(0.0f, 0.0f, -10.0f), 5.0f, WheelDollyDelta);
		PF_CHECK(Tests, ZoomWithoutPivot && ZoomWithoutPivot->Position.z < 0.0f);
		PF_CHECK(Tests, ZoomWithoutPivot && glm::length(ZoomWithoutPivot->FocusPivot - glm::vec3(0, 0, -10)) < 1.0e-5f);
		PF_CHECK(Tests, ZoomOutWithoutPivot && ZoomOutWithoutPivot->Position.z > 0.0f);
		PF_CHECK(Tests, ZoomWithPivot && ZoomWithPivot->Position.z < 0.0f);
		PF_CHECK(Tests, ZoomWithPivot && glm::length(ZoomWithPivot->FocusPivot - glm::vec3(0, 0, -10)) < 1.0e-5f);
		glm::vec3 RepeatedZoomPosition{ 0.0f };
		std::optional<glm::vec3> RepeatedZoomPivot;
		for (size_t Step = 0; Step < 100; ++Step)
		{
			const auto Updated = CalculateViewportCameraDolly(
				RepeatedZoomPosition, CameraForward, RepeatedZoomPivot, 5.0f, WheelDollyDelta);
			if (!Updated)
				break;
			RepeatedZoomPosition = Updated->Position;
			RepeatedZoomPivot = Updated->FocusPivot;
		}
		PF_CHECK(Tests, PulseForgeEditor::Detail::IsFinite(RepeatedZoomPosition) && RepeatedZoomPivot &&
			glm::length(RepeatedZoomPosition - *RepeatedZoomPivot) >= 0.149f);
		PF_CHECK(Tests, GetViewportWheelDollyDelta(100.0f) == 6.0f);
		PF_CHECK(Tests, GetViewportWheelDollyDelta(std::numeric_limits<float>::infinity()) == 0.0f);
		PF_CHECK(Tests, !CalculateViewportCameraDolly(
			glm::vec3(0.0f), CameraForward, std::nullopt, 5.0f, std::numeric_limits<float>::quiet_NaN()));

		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true }) == ViewportWheelAction::Zoom);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RightMouseHeld = true }) ==
			ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({
			.PointerOverViewportImage = true, .RightMouseNavigationActive = true, .RightMouseHeld = true }) ==
			ViewportWheelAction::AdjustFlySpeed);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .RuntimeActive = true }) ==
			ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({ .PointerOverViewportImage = true, .PopupOpen = true }) ==
			ViewportWheelAction::None);
		PF_CHECK(Tests, ResolveViewportWheelAction({}) == ViewportWheelAction::ScrollPanel);
		PF_CHECK(Tests, FindEditorIcon(EditorIcon::Translate)->Codepoint !=
			FindEditorIcon(EditorIcon::Scale)->Codepoint);

		const ViewportImageRect ImageRect{ { 100.0f, 50.0f }, { 300.0f, 150.0f } };
		const auto TopLeft = NdcToViewportImage({ -1.0f, 1.0f }, ImageRect);
		const auto BottomRight = NdcToViewportImage({ 1.0f, -1.0f }, ImageRect);
		const auto ImageCenter = NdcToViewportImage({ 0.0f, 0.0f }, ImageRect);
		PF_CHECK(Tests, TopLeft && glm::length(*TopLeft - ImageRect.Minimum) < 1.0e-5f);
		PF_CHECK(Tests, BottomRight && glm::length(*BottomRight - ImageRect.Maximum) < 1.0e-5f);
		PF_CHECK(Tests, ImageCenter && glm::length(*ImageCenter - glm::vec2(200.0f, 100.0f)) < 1.0e-5f);
		const auto TopLeftNdc = ViewportImageToNdc(ImageRect.Minimum, ImageRect);
		const auto BottomRightNdc = ViewportImageToNdc(ImageRect.Maximum, ImageRect);
		PF_CHECK(Tests, TopLeftNdc && glm::length(*TopLeftNdc - glm::vec2(-1.0f, 1.0f)) < 1.0e-5f);
		PF_CHECK(Tests, BottomRightNdc && glm::length(*BottomRightNdc - glm::vec2(1.0f, -1.0f)) < 1.0e-5f);
		PF_CHECK(Tests, !NdcToViewportImage({ 0.0f, 0.0f }, {}));
		PF_CHECK(Tests, !ViewportImageToNdc({ 0.0f, 0.0f }, {}));
		const auto KnownProjectedPoint = ProjectWorldToViewportImage(
			{ 0.25f, -0.5f, 0.5f }, glm::mat4(1.0f), ImageRect);
		PF_CHECK(Tests, KnownProjectedPoint &&
			glm::length(*KnownProjectedPoint - glm::vec2(225.0f, 125.0f)) < 1.0e-5f);

		CameraComponent Camera;
		const auto Projection = Camera.GetProjectionMatrix(2.0f);
		PF_CHECK(Tests, Projection.has_value());
		if (!Projection)
			return;
		const glm::mat4 View = glm::lookAtRH(
			glm::vec3(0.0f, 0.0f, 5.0f),
			glm::vec3(0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f));
		const glm::mat4 ViewProjection = *Projection * View;
		const auto ProjectedAbove = ProjectWorldToViewportImage({ 0.0f, 1.0f, 0.0f }, ViewProjection, ImageRect);
		const auto ProjectedBelow = ProjectWorldToViewportImage({ 0.0f, -1.0f, 0.0f }, ViewProjection, ImageRect);
		PF_CHECK(Tests, ProjectedAbove && ProjectedAbove->y < 100.0f);
		PF_CHECK(Tests, ProjectedBelow && ProjectedBelow->y > 100.0f);
		const ViewportImageRect DpiScaledRect{ { 200.0f, 100.0f }, { 600.0f, 300.0f } };
		const auto LogicalCenterNdc = ViewportImageToNdc({ 200.0f, 100.0f }, ImageRect);
		const auto DpiCenterNdc = ViewportImageToNdc({ 400.0f, 200.0f }, DpiScaledRect);
		PF_CHECK(Tests, LogicalCenterNdc && DpiCenterNdc &&
			glm::length(*LogicalCenterNdc - *DpiCenterNdc) < 1.0e-5f);

		const auto CenterRay = MakeViewportRay({ 200.0f, 100.0f }, ImageRect, ViewProjection);
		PF_CHECK(Tests, CenterRay.has_value());
		if (CenterRay)
		{
			PF_CHECK(Tests, glm::length(CenterRay->Direction - glm::vec3(0.0f, 0.0f, -1.0f)) < 1.0e-4f);
			PF_CHECK(Tests, CenterRay->Origin.z < 5.0f && CenterRay->Origin.z > 4.8f);
		}
		const auto RightRay = MakeViewportRay({ 250.0f, 100.0f }, ImageRect, ViewProjection);
		PF_CHECK(Tests, RightRay && RightRay->Direction.x > 0.0f);
		const auto TopImageRay = MakeViewportRay({ 200.0f, 75.0f }, ImageRect, ViewProjection);
		PF_CHECK(Tests, TopImageRay && TopImageRay->Direction.y > 0.0f);
		PF_CHECK(Tests, !MakeViewportRay({ 300.0f, 100.0f }, ImageRect, ViewProjection));
		PF_CHECK(Tests, !MakeViewportRay({ 99.0f, 100.0f }, ImageRect, ViewProjection));
		PF_CHECK(Tests, !MakeViewportRay({ 200.0f, 100.0f }, {}, ViewProjection));
		PF_CHECK(Tests, !MakeViewportRay({ 200.0f, 100.0f }, ImageRect, glm::mat4(0.0f)));
		const glm::vec2 Outside{ 350.0f, 100.0f };
		PF_CHECK(Tests, MakeViewportRay(Outside, ImageRect, ViewProjection, true).has_value());

		const std::array<ViewportImageRect, 2> ResizedImageRects{
			ViewportImageRect{ { 13.0f, 27.0f }, { 613.0f, 327.0f } },
			ViewportImageRect{ { 41.0f, 19.0f }, { 341.0f, 619.0f } }
		};
		for (const ViewportImageRect& TestRect : ResizedImageRects)
		{
			const float AspectRatio = (TestRect.Maximum.x - TestRect.Minimum.x) /
				(TestRect.Maximum.y - TestRect.Minimum.y);
			const auto TestProjection = Camera.GetProjectionMatrix(AspectRatio);
			PF_CHECK(Tests, TestProjection.has_value());
			if (!TestProjection)
				continue;

			const std::array<glm::vec3, 3> CameraPositions{
				glm::vec3(0.0f, 0.0f, 5.0f),
				glm::vec3(2.0f, 1.0f, 5.0f),
				glm::vec3(-3.0f, 2.0f, 4.0f)
			};
			const std::array<glm::vec3, 3> CameraTargets{
				glm::vec3(0.4f, -0.2f, 0.0f),
				glm::vec3(-0.5f, 0.3f, -0.5f),
				glm::vec3(0.8f, -0.4f, -1.0f)
			};
			for (size_t CameraIndex = 0; CameraIndex < CameraPositions.size(); ++CameraIndex)
			{
				const glm::mat4 CameraView = glm::lookAtRH(
					CameraPositions[CameraIndex],
					CameraTargets[CameraIndex],
					glm::vec3(0.0f, 1.0f, 0.0f));
				const glm::mat4 TestViewProjection = *TestProjection * CameraView;
				const glm::vec3 WorldPoint = CameraTargets[CameraIndex] + glm::vec3(0.15f, 0.1f, 0.0f);
				const auto Projected = ProjectWorldToViewportImage(WorldPoint, TestViewProjection, TestRect);
				PF_CHECK(Tests, Projected.has_value());
				if (!Projected)
					continue;

				const auto ThroughPoint = MakeViewportRay(*Projected, TestRect, TestViewProjection);
				PF_CHECK(Tests, ThroughPoint.has_value());
				if (ThroughPoint)
				{
					const glm::vec3 ToPoint = WorldPoint - ThroughPoint->Origin;
					const float AlongRay = glm::dot(ToPoint, ThroughPoint->Direction);
					const glm::vec3 ClosestPoint = ThroughPoint->Origin + ThroughPoint->Direction * AlongRay;
					PF_CHECK(Tests, AlongRay > 0.0f && AlongRay <= ThroughPoint->MaxDistance + 1.0e-3f);
					PF_CHECK(Tests, glm::length(ClosestPoint - WorldPoint) < 1.0e-3f);
				}
			}
		}

		const std::array<glm::vec3, 3> Triangle{
			glm::vec3(-1.0f, -1.0f, 0.0f), glm::vec3(1.0f, -1.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f) };
		const std::array<uint32_t, 3> Indices{ 0, 1, 2 };
		const UUID FarEntity{ 1, 1 };
		const UUID NearEntity{ 2, 2 };
		const glm::mat4 FarTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, -2.0f });
		const glm::mat4 NearTransform = glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, 1.0f });
		const std::array<PickableMesh, 2> Overlapping{
			PickableMesh{ FarEntity, FarTransform, Triangle, Indices },
			PickableMesh{ NearEntity, NearTransform, Triangle, Indices } };
		const Ray WorldRay{ { 0.0f, 0.0f, 5.0f }, { 0.0f, 0.0f, -1.0f } };
		const auto Nearest = RaycastMeshes(WorldRay, Overlapping);
		PF_CHECK(Tests, Nearest && Nearest->Entity == NearEntity);
		PF_CHECK(Tests, Nearest && std::abs(Nearest->Distance - 4.0f) < 1.0e-4f);
		const std::array<PickableMesh, 1> NoHit{
			PickableMesh{ FarEntity, glm::translate(glm::mat4(1.0f), { 5.0f, 0.0f, 0.0f }), Triangle, Indices } };
		PF_CHECK(Tests, !RaycastMeshes(WorldRay, NoHit));

		const glm::mat4 RotatedNonUniform =
			glm::rotate(glm::scale(glm::mat4(1.0f), { 2.0f, 0.5f, 1.0f }), glm::radians(35.0f), { 0.0f, 0.0f, 1.0f });
		const std::array<PickableMesh, 1> TransformedMesh{
			PickableMesh{ NearEntity, RotatedNonUniform, Triangle, Indices } };
		const auto TransformedHit = RaycastMeshes(WorldRay, TransformedMesh);
		PF_CHECK(Tests, TransformedHit && TransformedHit->Entity == NearEntity);
		PF_CHECK(Tests, std::abs(glm::length(WorldRay.Direction) - 1.0f) < 1.0e-5f);
		const std::array<PickableMesh, 1> BeyondFarPlane{
			PickableMesh{ NearEntity, glm::translate(glm::mat4(1.0f), { 0.0f, 0.0f, -2000.0f }), Triangle, Indices } };
		PF_CHECK(Tests, !RaycastMeshes(*CenterRay, BeyondFarPlane));
		const std::array<uint32_t, 3> InvalidIndices{ 0, 1, 9 };
		const std::array<PickableMesh, 1> InvalidGeometry{
			PickableMesh{ NearEntity, glm::mat4(1.0f), Triangle, InvalidIndices } };
		PF_CHECK(Tests, !RaycastMeshes(WorldRay, InvalidGeometry));

		const std::array<glm::vec3, 4> FloorVertices{
			glm::vec3(-2.0f, 4.0f, -2.0f), glm::vec3(2.0f, 4.0f, -2.0f),
			glm::vec3(2.0f, 4.0f, 2.0f), glm::vec3(-2.0f, 4.0f, 2.0f) };
		const std::array<uint32_t, 6> FloorIndices{ 0, 1, 2, 0, 2, 3 };
		const std::array<glm::vec3, 4> LowerFloorVertices{
			glm::vec3(-2.0f, 2.0f, -2.0f), glm::vec3(2.0f, 2.0f, -2.0f),
			glm::vec3(2.0f, 2.0f, 2.0f), glm::vec3(-2.0f, 2.0f, 2.0f) };
		const UUID FloorEntity{ 3, 3 };
		const std::array<PickableMesh, 2> StackedFloors{
			PickableMesh{ FarEntity, glm::mat4(1.0f), LowerFloorVertices, FloorIndices },
			PickableMesh{ FloorEntity, glm::mat4(1.0f), FloorVertices, FloorIndices } };
		const auto NearestFloor = RaycastSupportingSurface(
			Ray{ { 0.0f, 5.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, StackedFloors);
		PF_CHECK(Tests, NearestFloor && NearestFloor->Entity == FloorEntity);
		PF_CHECK(Tests, NearestFloor && std::abs(NearestFloor->Position.y - 4.0f) < 1.0e-4f &&
			std::abs(NearestFloor->Distance - 1.0f) < 1.0e-4f && NearestFloor->Normal.y > 0.999f);
		const std::array<glm::vec3, 3> WallVertices{
			glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 2.0f, -1.0f), glm::vec3(0.0f, 0.0f, 1.0f) };
		const std::array<uint32_t, 3> WallIndices{ 0, 1, 2 };
		const std::array<PickableMesh, 1> VerticalWall{
			PickableMesh{ FloorEntity, glm::mat4(1.0f), WallVertices, WallIndices } };
		PF_CHECK(Tests, !RaycastSupportingSurface(
			Ray{ { 0.0f, 3.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, VerticalWall));
		const std::array<glm::vec3, 3> SlopeVertices{
			glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(2.0f, 1.0f, -1.0f), glm::vec3(2.0f, 1.0f, 1.0f) };
		const std::array<uint32_t, 3> SlopeIndices{ 0, 1, 2 };
		const std::array<PickableMesh, 1> SlopeMesh{
			PickableMesh{ FloorEntity, glm::mat4(1.0f), SlopeVertices, SlopeIndices } };
		const auto SlopeHit = RaycastSupportingSurface(
			Ray{ { 1.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, SlopeMesh);
		PF_CHECK(Tests, SlopeHit && std::abs(SlopeHit->Position.y - 0.5f) < 1.0e-4f && SlopeHit->Normal.y > 0.89f);
		PF_CHECK(Tests, !RaycastSupportingSurface(
			Ray{ { 1.0f, 2.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } }, SlopeMesh, 0.95f));
		PF_CHECK(Tests, SnapValueToIncrement(1.26f, 0.5f) == 1.5f);
		PF_CHECK(Tests, SnapValueToIncrement(-1.26f, 0.5f) == -1.5f);
		PF_CHECK(Tests, SnapValueToIncrement(2.0f, 0.0f) == std::nullopt);
		PF_CHECK(Tests, SnapValueToIncrement(std::numeric_limits<float>::infinity(), 1.0f) == std::nullopt);

		Scene HierarchyScene;
		auto Parent = HierarchyScene.CreateEntity("Picking parent");
		auto Child = HierarchyScene.CreateEntity("Picking child");
		PF_CHECK(Tests, Parent && Child);
		if (Parent && Child)
		{
			TransformComponent ParentTransform;
			ParentTransform.Translation = { 0.5f, 0.25f, 0.0f };
			ParentTransform.Rotation = glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			ParentTransform.Scale = { 1.5f, 0.75f, 1.0f };
			TransformComponent ChildTransform;
			ChildTransform.Rotation = glm::angleAxis(glm::radians(-15.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			ChildTransform.Scale = { 0.8f, 1.2f, 1.0f };
			PF_CHECK(Tests, Parent->SetTransform(ParentTransform).has_value());
			PF_CHECK(Tests, Child->SetTransform(ChildTransform).has_value());
			PF_CHECK(Tests, Child->SetParent(*Parent).has_value());
			const auto ChildWorld = Child->GetWorldMatrix();
			PF_CHECK(Tests, ChildWorld.has_value());
			if (ChildWorld)
			{
				const glm::vec3 Center = glm::vec3((*ChildWorld)[3]);
				const glm::vec3 Normal = glm::normalize(glm::transpose(glm::inverse(glm::mat3(*ChildWorld))) *
					glm::vec3(0.0f, 0.0f, 1.0f));
				const Ray ChildRay{ Center + Normal * 4.0f, -Normal };
				const std::array<PickableMesh, 1> ParentedMesh{
					PickableMesh{ Child->GetUUID(), *ChildWorld, Triangle, Indices } };
				const auto ChildHit = RaycastMeshes(ChildRay, ParentedMesh);
				PF_CHECK(Tests, ChildHit && ChildHit->Entity == Child->GetUUID());
				PF_CHECK(Tests, ChildHit && std::abs(ChildHit->Distance - 4.0f) < 1.0e-3f);
			}
		}

		const glm::mat4 ParentWorld = glm::translate(glm::mat4(1.0f), { 3.0f, -1.0f, 2.0f }) *
			glm::rotate(glm::mat4(1.0f), glm::radians(25.0f), { 0.0f, 0.0f, 1.0f }) *
			glm::scale(glm::mat4(1.0f), { 2.0f, 3.0f, 0.5f });
		const glm::quat LocalRotation = glm::angleAxis(glm::radians(40.0f), glm::normalize(glm::vec3(1.0f, 1.0f, 0.0f)));
		const auto ParentAxis = GetWorldGizmoAxis(ParentWorld, LocalRotation, 1);
		const auto WorldAxis = GetGizmoAxis(ParentWorld, LocalRotation, 0, false);
		const auto LocalDelta = WorldDeltaToLocalAxisDelta(2.5f, ParentWorld, LocalRotation, 1);
		PF_CHECK(Tests, ParentAxis && LocalDelta && WorldAxis && ParentAxis->WorldUnitsPerLocalUnit > 0.0f);
		PF_CHECK(Tests, WorldAxis && glm::length(WorldAxis->Direction - glm::vec3(1.0f, 0.0f, 0.0f)) < 1.0e-6f &&
			std::abs(WorldAxis->WorldUnitsPerLocalUnit - 1.0f) < 1.0e-6f);
		if (ParentAxis && LocalDelta)
		{
			const glm::vec3 LocalMovement = LocalRotation * glm::vec3(0.0f, 1.0f, 0.0f) * *LocalDelta;
			const glm::vec3 WorldMovement = glm::mat3(ParentWorld) * LocalMovement;
			PF_CHECK(Tests, glm::length(WorldMovement - ParentAxis->Direction * 2.5f) < 1.0e-4f);
		}
		PF_CHECK(Tests, !GetWorldGizmoAxis(ParentWorld, LocalRotation, 3));
		PF_CHECK(Tests, !WorldDeltaToLocalAxisDelta(std::numeric_limits<float>::quiet_NaN(), ParentWorld, LocalRotation, 1));

		TransformComponent Initial;
		Initial.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
		const auto Translated = ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Translate, 0, 2.0f);
		PF_CHECK(Tests, Translated && glm::length(Translated->Translation - glm::vec3(0.0f, 2.0f, 0.0f)) < 1.0e-4f);
		const auto Rotated = ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Rotate, 2, glm::half_pi<float>());
		PF_CHECK(Tests, Rotated && std::abs(glm::length(Rotated->Rotation) - 1.0f) < 1.0e-5f);
		const auto Scaled = ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Scale, 1, 0.5f);
		PF_CHECK(Tests, Scaled && std::abs(Scaled->Scale.y - 1.5f) < 1.0e-5f);
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Scale, 0, -1.0f));
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Rotate, 3, 0.5f));
		PF_CHECK(Tests, !ApplyLocalGizmoDelta(Initial, TransformGizmoOperation::Translate, 0,
			std::numeric_limits<float>::infinity()));
	}

	void TestEditorWorkspacePanelState(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		PF_CHECK(Tests, CurrentDefaultEditorLayoutVersion == 4);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("0") == 0);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("1") == 1);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("01") == 1);
		PF_CHECK(Tests, ParseDocumentOpeningPreference("2") == 2);
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("3"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("4294967296"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference("remembered"));
		PF_CHECK(Tests, !ParseDocumentOpeningPreference(" 1"));
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, true, 2) == 2);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, false, 2) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(false, true, 1) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, ResolveDocumentOpeningPreference(true, true, std::nullopt) == DefaultDocumentOpeningPreference);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::ContentBrowser) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Console) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Scene) == EditorDefaultDockRegion::Bottom);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::Inspector) == EditorDefaultDockRegion::Right);
		PF_CHECK(Tests, GetDefaultDockRegion(EditorPanel::MaterialDetails) == EditorDefaultDockRegion::Right);
		PF_CHECK(Tests, EditorPanelDescriptors.size() == static_cast<size_t>(EditorPanel::Count));
		constexpr std::array<std::string_view, static_cast<size_t>(EditorPanel::Count)> StablePanelNames = {
			"Scene Viewport", "Hierarchy", "Inspector", "Content Browser", "Console", "Scene", "Material Editor"
		};

		EditorPanelVisibility Panels;
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::SceneViewport));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Hierarchy));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Inspector));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::ContentBrowser));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Console));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::MaterialDetails));

		for (size_t Index = 0; Index < EditorPanelDescriptors.size(); ++Index)
		{
			const EditorPanelDescriptor& Descriptor = EditorPanelDescriptors[Index];
			PF_CHECK(Tests, Descriptor.WindowName == StablePanelNames[Index]);
			PF_CHECK(Tests, !Descriptor.SettingsKey.empty());
			PF_CHECK(Tests, !Descriptor.WindowName.empty());
			if (Descriptor.Panel == EditorPanel::MaterialDetails)
				PF_CHECK(Tests, Descriptor.IsUtilityPanel && !Descriptor.DefaultVisible &&
					Descriptor.SettingsKey == "MaterialEditor");
			for (size_t OtherIndex = Index + 1; OtherIndex < EditorPanelDescriptors.size(); ++OtherIndex)
			{
				PF_CHECK(Tests, Descriptor.SettingsKey != EditorPanelDescriptors[OtherIndex].SettingsKey);
				PF_CHECK(Tests, Descriptor.WindowName != EditorPanelDescriptors[OtherIndex].WindowName);
			}
		}

		PF_CHECK(Tests, Panels.SetVisible(EditorPanel::Scene, true));
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, Panels.SetVisible(EditorPanel::Console, false));
		PF_CHECK(Tests, !Panels.SetVisible(EditorPanel::Console, false));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Console));
		Panels.ResetToDefaults();
		PF_CHECK(Tests, Panels.IsVisible(EditorPanel::Console));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Scene));
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::MaterialDetails));
		PF_CHECK(Tests, Panels.GetVisibility(EditorPanel::Scene) != nullptr);
		PF_CHECK(Tests, Panels.GetVisibility(EditorPanel::Count) == nullptr);
		PF_CHECK(Tests, !Panels.IsVisible(EditorPanel::Count));
		PF_CHECK(Tests, !Panels.SetVisible(EditorPanel::Count, true));
		PF_CHECK(Tests, !ShouldApplyFirstOpenPanelDocking(true, true));
		PF_CHECK(Tests, !ShouldApplyFirstOpenPanelDocking(false, false));
		PF_CHECK(Tests, ShouldApplyFirstOpenPanelDocking(false, true));
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".material", true, false) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".material", true, true) == AssetBrowserOpenAction::ShowMaterialDetails);
		// Utility-panel opening is intentionally independent of document-host preferences.
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".scene", true, true) == AssetBrowserOpenAction::OpenScene);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".png", true, true) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, ResolveAssetBrowserOpenAction(".scene", false, true) == AssetBrowserOpenAction::None);
		PF_CHECK(Tests, !IsRegisteredAssetDocumentEditor(EditorDocumentKind::Material));
		PF_CHECK(Tests, !IsRegisteredAssetDocumentEditor(EditorDocumentKind::Texture));
		const std::string DraftKey = MakeMaterialDraftKey("C:/Project", "material-uuid");
		PF_CHECK(Tests, DraftKey == MakeMaterialDraftKey("C:/Project", "material-uuid"));
		PF_CHECK(Tests, DraftKey != MakeMaterialDraftKey("D:/Project", "material-uuid"));
		PF_CHECK(Tests, DraftKey != MakeMaterialDraftKey("C:/Project", "other-material-uuid"));
	}

	void TestEditorAssetDestinationPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const std::filesystem::path Source = "Assets/Materials/Amber.material";
		const auto Extensionless = ResolveAssetDestinationFilename("Amber Copy", Source);
		PF_CHECK(Tests, Extensionless && *Extensionless == "Amber Copy.material");
		const auto MatchingExtension = ResolveAssetDestinationFilename("Amber-Renamed.MATERIAL", Source);
		PF_CHECK(Tests, MatchingExtension && *MatchingExtension == "Amber-Renamed.MATERIAL");
		const auto EmptyName = ResolveAssetDestinationFilename("", Source);
		PF_CHECK(Tests, !EmptyName && EmptyName.error() == AssetDestinationNameError::InvalidFilename);
		const auto RelativePath = ResolveAssetDestinationFilename("nested/Amber.material", Source);
		PF_CHECK(Tests, !RelativePath && RelativePath.error() == AssetDestinationNameError::InvalidFilename);
		const auto InvalidCharacter = ResolveAssetDestinationFilename("Amber?.material", Source);
		PF_CHECK(Tests, !InvalidCharacter && InvalidCharacter.error() == AssetDestinationNameError::InvalidFilename);
		const auto TrailingSpace = ResolveAssetDestinationFilename("Amber .material ", Source);
		PF_CHECK(Tests, !TrailingSpace && TrailingSpace.error() == AssetDestinationNameError::InvalidFilename);
		const auto MismatchedExtension = ResolveAssetDestinationFilename("Amber.png", Source);
		PF_CHECK(Tests, !MismatchedExtension && MismatchedExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		const auto ReservedExtension = ResolveAssetDestinationFilename("Amber.meta", Source);
		PF_CHECK(Tests, !ReservedExtension &&
			ReservedExtension.error() == AssetDestinationNameError::ReservedSidecarExtension);
		const auto DestinationBreadcrumbs = BuildAssetDestinationBreadcrumbs("Assets/Materials/Props");
		PF_CHECK(Tests, DestinationBreadcrumbs.size() == 3);
		PF_CHECK(Tests, DestinationBreadcrumbs[0] == std::filesystem::path("Assets"));
		PF_CHECK(Tests, DestinationBreadcrumbs[1] == std::filesystem::path("Assets/Materials"));
		PF_CHECK(Tests, DestinationBreadcrumbs[2] == std::filesystem::path("Assets/Materials/Props"));
		const auto RootBreadcrumb = BuildAssetDestinationBreadcrumbs("Assets");
		PF_CHECK(Tests, RootBreadcrumb.size() == 1 && RootBreadcrumb.front() == std::filesystem::path("Assets"));
		const auto NewSceneName = ResolveAssetDestinationFilename(
			"Overview", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, NewSceneName && *NewSceneName == "Overview.scene");
		const auto SavedSceneName = ResolveAssetDestinationFilename(
			"Overview Copy.scene", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, SavedSceneName && *SavedSceneName == "Overview Copy.scene");
		const auto NewPrefabName = ResolveAssetDestinationFilename(
			"Environment", std::filesystem::path("Assets/Prefabs/NewPrefab.prefab"));
		PF_CHECK(Tests, NewPrefabName && *NewPrefabName == "Environment.prefab");
		const auto WrongSceneExtension = ResolveAssetDestinationFilename(
			"Overview.prefab", std::filesystem::path("Assets/Scenes/NewScene.scene"));
		PF_CHECK(Tests, !WrongSceneExtension &&
			WrongSceneExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		const auto WrongPrefabExtension = ResolveAssetDestinationFilename(
			"Environment.scene", std::filesystem::path("Assets/Prefabs/NewPrefab.prefab"));
		PF_CHECK(Tests, !WrongPrefabExtension &&
			WrongPrefabExtension.error() == AssetDestinationNameError::ExtensionMismatch);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, false, false, false, false, false) ==
			AssetDestinationConflict::None);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, true, true, false) ==
			AssetDestinationConflict::SamePath);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, false, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(true, false, true, false, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, false, true, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, true, true, false, false, true) ==
			AssetDestinationConflict::CaseOnlyRename);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, true, true, true, false, false, false) ==
			AssetDestinationConflict::ExistingDestination);
		PF_CHECK(Tests, ResolveAssetDestinationConflict(false, false, false, false, false, true) ==
			AssetDestinationConflict::InspectionFailed);
	}

	void TestEditorAssetSelectorPolicy(TestRunner& Tests)
	{
		using PulseForgeEditor::AssetSelectorMatchesSearch;
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", ""));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "stone"));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "TEXTURES"));
		PF_CHECK(Tests, AssetSelectorMatchesSearch("Assets/Materials/青い.material", "青い"));
		PF_CHECK(Tests, !AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "roughness"));
		PF_CHECK(Tests, !AssetSelectorMatchesSearch("Assets/Textures/Stone_BaseColor.png", "Assets/Models"));
	}

	void TestEditorLayoutOpeningPreferencePersistence(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		PF_CHECK(Tests, ShouldBuildDefaultDockspace(false));
		PF_CHECK(Tests, !ShouldBuildDefaultDockspace(true));

		const auto TestID = PulseForge::UUID::Generate();
		PF_CHECK(Tests, TestID.has_value());
		if (!TestID)
			return;

		std::error_code Error;
		const std::filesystem::path SettingsDirectory = std::filesystem::temp_directory_path() /
			("PulseForgeEditorLayoutPreference_" + TestID->ToString());
		const bool DirectoryCreated = std::filesystem::create_directory(SettingsDirectory, Error);
		PF_CHECK(Tests, DirectoryCreated && !Error);
		if (!DirectoryCreated || Error)
			return;
		struct ScopedDirectoryCleanup
		{
			std::filesystem::path Path;
			~ScopedDirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ SettingsDirectory };

		const wchar_t* ExistingSettingsDirectory = _wgetenv(L"PULSEFORGE_EDITOR_SETTINGS_DIR");
		const std::optional<std::wstring> PreviousSettingsDirectory = ExistingSettingsDirectory
			? std::optional<std::wstring>(ExistingSettingsDirectory) : std::nullopt;
		const std::wstring SettingsDirectoryValue = SettingsDirectory.native();
		const errno_t EnvironmentResult = _wputenv_s(
			L"PULSEFORGE_EDITOR_SETTINGS_DIR", SettingsDirectoryValue.c_str());
		PF_CHECK(Tests, EnvironmentResult == 0);
		if (EnvironmentResult != 0)
			return;
		struct ScopedSettingsEnvironment
		{
			std::optional<std::wstring> PreviousValue;
			~ScopedSettingsEnvironment()
			{
				(void)_wputenv_s(L"PULSEFORGE_EDITOR_SETTINGS_DIR",
					PreviousValue ? PreviousValue->c_str() : L"");
			}
		} EnvironmentCleanup{ PreviousSettingsDirectory };

		struct ScopedImGuiContext
		{
			ImGuiContext* Previous = ImGui::GetCurrentContext();
			ImGuiContext* Context = ImGui::CreateContext();
			~ScopedImGuiContext()
			{
				if (Context)
					ImGui::DestroyContext(Context);
				ImGui::SetCurrentContext(Previous);
			}
		};

		const std::filesystem::path SettingsFile = SettingsDirectory / L"imgui.ini";
		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (!Initialized)
				return;
			PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == DefaultDocumentOpeningPreference);
			PF_CHECK(Tests, Layout.SetDocumentOpeningPreference(2));
			const auto Saved = Layout.SaveNow();
			PF_CHECK(Tests, Saved.has_value());
		}

		std::ifstream SavedSettingsInput(SettingsFile, std::ios::binary);
		const std::string SavedSettings(
			(std::istreambuf_iterator<char>(SavedSettingsInput)), std::istreambuf_iterator<char>());
		PF_CHECK(Tests, SavedSettings.find("DocumentOpeningPreference=2") != std::string::npos);

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (Initialized)
				PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == 2);
		}

		{
			std::ofstream IncompleteSettingsOutput(SettingsFile, std::ios::binary | std::ios::trunc);
			IncompleteSettingsOutput << "[PulseForgeWorkspace][State]\n"
				"StateFormat=1\nDocumentOpeningPreference=1\nHierarchy=0\n";
			PF_CHECK(Tests, static_cast<bool>(IncompleteSettingsOutput));
		}

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (!Initialized)
				return;
			PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == 1);
			PF_CHECK(Tests, !Layout.IsPanelVisible(EditorPanel::Hierarchy));
		}

		{
			std::ofstream InvalidSettingsOutput(SettingsFile, std::ios::binary | std::ios::trunc);
			InvalidSettingsOutput << "[PulseForgeWorkspace][State]\n"
				"StateFormat=99\nDocumentOpeningPreference=1\nHierarchy=0\n";
			PF_CHECK(Tests, static_cast<bool>(InvalidSettingsOutput));
		}

		{
			ScopedImGuiContext Context;
			PF_CHECK(Tests, Context.Context != nullptr);
			if (!Context.Context)
				return;

			EditorLayout Layout;
			const auto Initialized = Layout.Initialize();
			PF_CHECK(Tests, Initialized.has_value());
			if (Initialized)
			{
				PF_CHECK(Tests, Layout.GetDocumentOpeningPreference() == DefaultDocumentOpeningPreference);
				PF_CHECK(Tests, Layout.IsPanelVisible(EditorPanel::Hierarchy));
			}
		}
	}

	void TestEditorWorkspaceViewHelpers(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", "cube"));
		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", "SAMPLE"));
		PF_CHECK(Tests, ContainsCaseInsensitive("Sample Cube", ""));
		PF_CHECK(Tests, !ContainsCaseInsensitive("Sample Cube", "sphere"));
		PF_CHECK(Tests, !ContainsCaseInsensitive("abc", "abcd"));

		PF_CHECK(Tests, IsManagedAssetInFolder("Assets/Readme.txt", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Props/Cube.glb", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Props/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, IsManagedAssetInFolder("Assets/Models/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/Models/Cube.glb", "Assets/Materials"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/ModelsExtra/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Outside/Cube.glb", "Assets"));
		PF_CHECK(Tests, !IsManagedAssetInFolder("Assets/../Outside/Cube.glb", "Assets"));
		PF_CHECK(Tests, IsImmediateChildFolder("Assets/Models", "Assets"));
		PF_CHECK(Tests, IsImmediateChildFolder("Assets/Models/Props", "Assets/Models"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/Models/Props", "Assets"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/ModelsExtra", "Assets/Models"));
		PF_CHECK(Tests, !IsImmediateChildFolder("Assets/../Outside", "Assets"));
		PF_CHECK(Tests, IsManagedAssetInFolderSubtree("Assets/Models/Cube.glb", "Assets"));
		PF_CHECK(Tests, IsManagedAssetInFolderSubtree("Assets/Models/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, !IsManagedAssetInFolderSubtree("Assets/ModelsExtra/Cube.glb", "Assets/Models"));
		PF_CHECK(Tests, MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "cube"));
		PF_CHECK(Tests, MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "models/cube"));
		PF_CHECK(Tests, !MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets/Materials", "cube"));
		PF_CHECK(Tests, !MatchesManagedAssetFolderSearch("Assets/Models/Cube.glb", "Assets", "missing"));

		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Scenes/Main.scene") == EditorIcon::Scene);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Models/Cube.GLB") == EditorIcon::Cube);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Materials/Default.material") == EditorIcon::Material);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Scripts/Move.lua") == EditorIcon::Script);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Textures/Icon.png") == EditorIcon::Image);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Audio/Click.wav") == EditorIcon::AudioSource);
		PF_CHECK(Tests, GetAssetBrowserIcon("Assets/Other/Readme.txt") == EditorIcon::File);

		PF_CHECK(Tests, IsFolderAncestorOrSelf("Assets/Models/Props", "Assets/Models"));
		PF_CHECK(Tests, IsFolderAncestorOrSelf("Assets/Models", "Assets/Models"));
		PF_CHECK(Tests, !IsFolderAncestorOrSelf("Assets/ModelsExtra", "Assets/Models"));

		const auto FrontFacingAxes = ProjectViewportOrientationAxes({ 0.0f, 0.0f, -1.0f });
		PF_CHECK(Tests, FrontFacingAxes.has_value());
		if (FrontFacingAxes)
		{
			PF_CHECK(Tests, glm::length((*FrontFacingAxes)[0].ScreenDirection - glm::vec2(1.0f, 0.0f)) < 1.0e-5f);
			PF_CHECK(Tests, glm::length((*FrontFacingAxes)[1].ScreenDirection - glm::vec2(0.0f, -1.0f)) < 1.0e-5f);
			PF_CHECK(Tests, std::abs((*FrontFacingAxes)[2].Depth + 1.0f) < 1.0e-5f);
		}
		const auto RotatedAxes = ProjectViewportOrientationAxes(glm::normalize(glm::vec3(1.0f, 1.0f, -1.0f)));
		PF_CHECK(Tests, RotatedAxes.has_value());
		if (RotatedAxes)
			PF_CHECK(Tests, glm::length((*RotatedAxes)[2].ScreenDirection) > 0.1f);
		PF_CHECK(Tests, !ProjectViewportOrientationAxes(glm::vec3(0.0f)));
	}

	void TestViewportMouseCaptureState(TestRunner& Tests)
	{
		PulseForgeEditor::Detail::ViewportMouseCaptureState Capture;
		PF_CHECK(Tests, !Capture.OwnsMouse());
		PF_CHECK(Tests, !Capture.Begin(true, false, true, false, false)); // RMB outside the viewport image.
		PF_CHECK(Tests, !Capture.Begin(true, true, false, false, false)); // Unfocused window.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, true, false)); // Gizmo drag owns the pointer.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, false, true)); // Runtime camera is active.
		PF_CHECK(Tests, Capture.Begin(true, true, true, false, false));
		PF_CHECK(Tests, Capture.OwnsMouse()); // Capture owns mouse routing until release/cancellation.
		PF_CHECK(Tests, !Capture.Begin(true, true, true, false, false)); // No duplicate capture transition.
		Capture.End(); // Same transition is used for RMB release, focus loss, and layout/project changes.
		PF_CHECK(Tests, !Capture.OwnsMouse());
		PF_CHECK(Tests, Capture.Begin(true, true, true, false, false));
		Capture.End();
		PF_CHECK(Tests, !Capture.OwnsMouse());
	}

	void TestEditorImGuiRendererMath(TestRunner& Tests)
	{
		using namespace PulseForgeEditor::ImGuiRendererMath;
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Position) == 0);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Color) == 1);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::TexCoord) == 2);
		PF_CHECK(Tests, static_cast<uint8_t>(PulseForge::VertexSemantic::Normal) == 3);

		const auto ClippedToOldSwapchain = PulseForge::IntersectScissorRect({ 0, 0, 2560, 1361 }, 1280, 720);
		PF_CHECK(Tests, ClippedToOldSwapchain && ClippedToOldSwapchain->X == 0 && ClippedToOldSwapchain->Y == 0 &&
			ClippedToOldSwapchain->Width == 1280 && ClippedToOldSwapchain->Height == 720);
		const auto PartiallyVisible = PulseForge::IntersectScissorRect({ 1000, 600, 500, 300 }, 1280, 720);
		PF_CHECK(Tests, PartiallyVisible && PartiallyVisible->X == 1000 && PartiallyVisible->Y == 600 &&
			PartiallyVisible->Width == 280 && PartiallyVisible->Height == 120);
		PF_CHECK(Tests, !PulseForge::IntersectScissorRect({ 1280, 0, 1, 1 }, 1280, 720));
		PF_CHECK(Tests, !PulseForge::IntersectScissorRect({ 0, 0, 1, 1 }, 0, 720));

		const glm::vec2 DisplayPosition{ 20.0f, 30.0f };
		const glm::vec2 DisplaySize{ 100.0f, 50.0f };
		const auto TopLeft = PositionToVulkanNdc(DisplayPosition, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, TopLeft && glm::length(*TopLeft - glm::vec2(-1.0f, 1.0f)) < 1.0e-6f);
		const auto Center = PositionToVulkanNdc(DisplayPosition + DisplaySize * 0.5f, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, Center && glm::length(*Center) < 1.0e-6f);
		const auto BottomRight = PositionToVulkanNdc(DisplayPosition + DisplaySize, DisplayPosition, DisplaySize);
		PF_CHECK(Tests, BottomRight && glm::length(*BottomRight - glm::vec2(1.0f, -1.0f)) < 1.0e-6f);
		PF_CHECK(Tests, !PositionToVulkanNdc({ 0.0f, 0.0f }, DisplayPosition, { 0.0f, 50.0f }));
		PF_CHECK(Tests, !PositionToVulkanNdc({ std::numeric_limits<float>::infinity(), 0.0f }, DisplayPosition, DisplaySize));

		const auto Extent = CalculateFramebufferExtent(DisplaySize, { 1.5f, 2.0f });
		PF_CHECK(Tests, Extent && Extent->Width == 150 && Extent->Height == 100);
		const auto FractionalExtent = CalculateFramebufferExtent({ 10.1f, 20.2f }, { 1.5f, 1.25f });
		PF_CHECK(Tests, FractionalExtent && FractionalExtent->Width == 16 && FractionalExtent->Height == 26);
		PF_CHECK(Tests, !CalculateFramebufferExtent({ 0.0f, 1.0f }, { 1.0f, 1.0f }));
		PF_CHECK(Tests, !CalculateFramebufferExtent(DisplaySize, { 1.0f, 0.0f }));
		PF_CHECK(Tests, !CalculateFramebufferExtent({ std::numeric_limits<float>::infinity(), 1.0f }, { 1.0f, 1.0f }));
		const auto LargeExtent = CalculateFramebufferExtent({ 100000.0f, 50000.0f }, { 1.0f, 1.0f });
		PF_CHECK(Tests, LargeExtent && LargeExtent->Width == 100000 && LargeExtent->Height == 50000);

		const FramebufferExtent ScissorExtent{ 200, 100 };
		const glm::vec2 Scale{ 2.0f, 2.0f };
		const auto Full = ClipRectToScissor(
			DisplayPosition,
			DisplayPosition + DisplaySize,
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, Full && *Full && (*Full)->X == 0 && (*Full)->Y == 0 &&
			(*Full)->Width == 200 && (*Full)->Height == 100);

		const auto Fractional = ClipRectToScissor(
			DisplayPosition + glm::vec2(1.2f, 2.4f),
			DisplayPosition + glm::vec2(5.1f, 6.2f),
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, Fractional && *Fractional && (*Fractional)->X == 2 && (*Fractional)->Y == 4 &&
			(*Fractional)->Width == 9 && (*Fractional)->Height == 9);

		const auto PartlyOutside = ClipRectToScissor(
			DisplayPosition + glm::vec2(-10.0f, -5.0f),
			DisplayPosition + glm::vec2(5.0f, 8.0f),
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, PartlyOutside && *PartlyOutside && (*PartlyOutside)->X == 0 && (*PartlyOutside)->Y == 0 &&
			(*PartlyOutside)->Width == 10 && (*PartlyOutside)->Height == 16);

		const auto FullyOutside = ClipRectToScissor(
			DisplayPosition + glm::vec2(150.0f, 10.0f),
			DisplayPosition + glm::vec2(160.0f, 20.0f),
			DisplayPosition,
			Scale,
			ScissorExtent);
		PF_CHECK(Tests, FullyOutside && !*FullyOutside);
		PF_CHECK(Tests, !ClipRectToScissor({ 0.0f, 0.0f }, { 1.0f, 1.0f }, {}, { 1.0f, 1.0f }, {}));
	}

	void TestEditorPlatformViewportSubmissionOrder(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		std::vector<int> Events;
		for (int ViewportID : { 1, 3 })
		{
			RenderPlatformViewportSequentially(
				[&] { Events.push_back(ViewportID * 10 + 1); },
				[&] { Events.push_back(ViewportID * 10 + 2); },
				[&] { Events.push_back(ViewportID * 10 + 3); },
				[&] { Events.push_back(ViewportID * 10 + 4); });
		}
		const std::vector<int> Expected{ 11, 12, 13, 14, 31, 32, 33, 34 };
		PF_CHECK(Tests, Events == Expected);
	}

	void TestEditorFontAndIconDescriptors(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		int GlfwWindowMarker = 0;
		int NativeWindowMarker = 0;
		const EditorGlfwViewportHandles ViewportHandles{
			.GlfwWindow = &GlfwWindowMarker,
			.NativeWindow = &NativeWindowMarker
		};
		PF_CHECK(Tests, GetGlfwContextWindow(ViewportHandles) == &GlfwWindowMarker);
		PF_CHECK(Tests, GetGlfwContextWindow(ViewportHandles) != &NativeWindowMarker);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(590.0f) == EditorViewportToolbarLayout::Full);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(589.0f) == EditorViewportToolbarLayout::Compact);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(300.0f) == EditorViewportToolbarLayout::Compact);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(299.0f) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(-1.0f) == EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorViewportToolbarLayout(std::numeric_limits<float>::quiet_NaN()) ==
			EditorViewportToolbarLayout::Overflow);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(100.0f) == EditorStyleMetrics::PropertyLabelWidthMinimum);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(300.0f) == 90.0f);
		PF_CHECK(Tests, GetEditorPropertyLabelWidth(1000.0f) == EditorStyleMetrics::PropertyLabelWidthMaximum);
		PF_CHECK(Tests, GetMaterialPropertyLayout(260.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(360.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(500.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(559.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(560.0f) == MaterialPropertyLayout::TwoColumn);
		PF_CHECK(Tests, GetMaterialPropertyLayout(700.0f) == MaterialPropertyLayout::TwoColumn);
		PF_CHECK(Tests, GetMaterialPropertyLabelWidth(700.0f) == 264.0f);
		PF_CHECK(Tests, GetMaterialPropertyLabelWidth(1000.0f) == 264.0f);
		PF_CHECK(Tests, GetMaterialPropertyLayout(-1.0f) == MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(std::numeric_limits<float>::quiet_NaN()) ==
			MaterialPropertyLayout::Stacked);
		PF_CHECK(Tests, GetMaterialPropertyLayout(std::numeric_limits<float>::infinity()) ==
			MaterialPropertyLayout::Stacked);
		const MaterialValuePairLayout NarrowMaterialValuePair = GetMaterialValuePairLayout(260.0f, 8.0f);
		PF_CHECK(Tests, NarrowMaterialValuePair.Stacked && NarrowMaterialValuePair.FirstWidth == 260.0f &&
			NarrowMaterialValuePair.SecondWidth == 260.0f);
		const MaterialValuePairLayout RegularMaterialValuePair = GetMaterialValuePairLayout(360.0f, 8.0f);
		PF_CHECK(Tests, !RegularMaterialValuePair.Stacked && RegularMaterialValuePair.FirstWidth == 176.0f &&
			RegularMaterialValuePair.SecondWidth == 176.0f &&
			RegularMaterialValuePair.FirstWidth + RegularMaterialValuePair.SecondWidth + 8.0f <= 360.0f);
		const MaterialValuePairLayout BrowserWidthMaterialValuePair = GetMaterialValuePairLayout(500.0f, 8.0f);
		PF_CHECK(Tests, !BrowserWidthMaterialValuePair.Stacked && BrowserWidthMaterialValuePair.FirstWidth == 246.0f &&
			BrowserWidthMaterialValuePair.SecondWidth == 246.0f &&
			BrowserWidthMaterialValuePair.FirstWidth + BrowserWidthMaterialValuePair.SecondWidth + 8.0f <= 500.0f);
		const MaterialValuePairLayout WideMaterialValuePair = GetMaterialValuePairLayout(700.0f, 8.0f);
		PF_CHECK(Tests, !WideMaterialValuePair.Stacked && WideMaterialValuePair.FirstWidth == 346.0f &&
			WideMaterialValuePair.SecondWidth == 346.0f);
		const MaterialValuePairLayout InvalidMaterialValuePair = GetMaterialValuePairLayout(
			std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity());
		PF_CHECK(Tests, InvalidMaterialValuePair.Stacked && InvalidMaterialValuePair.FirstWidth == 0.0f &&
			InvalidMaterialValuePair.SecondWidth == 0.0f);
		for (const MaterialValuePairLayout Layout : { NarrowMaterialValuePair, RegularMaterialValuePair,
			BrowserWidthMaterialValuePair, WideMaterialValuePair, InvalidMaterialValuePair })
		{
			PF_CHECK(Tests, std::isfinite(Layout.FirstWidth) && Layout.FirstWidth >= 0.0f);
			PF_CHECK(Tests, std::isfinite(Layout.SecondWidth) && Layout.SecondWidth >= 0.0f);
		}
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarHeight == 36.0f);
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarPaddingX == 7.0f);
		PF_CHECK(Tests, EditorStyleMetrics::PanelToolbarPaddingY == 4.0f);
		PF_CHECK(Tests, UseCompactVector3PropertyInput(229.0f));
		PF_CHECK(Tests, !UseCompactVector3PropertyInput(230.0f));
		PF_CHECK(Tests, UseCompactVector3PropertyInput(std::numeric_limits<float>::quiet_NaN()));
		const ContentBrowserVerticalLayout NoSelection = GetContentBrowserVerticalLayout(320.0f, false, true);
		PF_CHECK(Tests, NoSelection.AssetItemsHeight == 320.0f && NoSelection.DetailsHeight == 0.0f);
		const ContentBrowserVerticalLayout CompactSelection = GetContentBrowserVerticalLayout(120.0f, true, true);
		PF_CHECK(Tests, CompactSelection.SelectionSummaryHeight ==
			ContentBrowserLayoutMetrics::CompactSelectionSummaryHeight);
		PF_CHECK(Tests, CompactSelection.AssetItemsHeight == 92.0f && CompactSelection.DetailsHeight == 0.0f);
		const ContentBrowserVerticalLayout ExpandedCompactSelection =
			GetContentBrowserVerticalLayout(120.0f, true, true, true);
		PF_CHECK(Tests, ExpandedCompactSelection.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, ExpandedCompactSelection.DetailsHeight == 0.0f && ExpandedCompactSelection.AssetItemsHeight == 92.0f);
		const ContentBrowserVerticalLayout RegularAssetDetails = GetContentBrowserVerticalLayout(320.0f, true, false);
		PF_CHECK(Tests, RegularAssetDetails.DetailsHeight == ContentBrowserLayoutMetrics::RegularDetailsMaximumHeight);
		PF_CHECK(Tests, RegularAssetDetails.AssetItemsHeight >= ContentBrowserLayoutMetrics::MinimumAssetItemsHeight);
		const ContentBrowserVerticalLayout ShortMaterialSummary = GetContentBrowserVerticalLayout(180.0f, true, true);
		PF_CHECK(Tests, ShortMaterialSummary.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, ShortMaterialSummary.DetailsHeight == 0.0f && ShortMaterialSummary.AssetItemsHeight == 152.0f);
		const ContentBrowserVerticalLayout TallMaterialSummary = GetContentBrowserVerticalLayout(840.0f, true, true);
		PF_CHECK(Tests, TallMaterialSummary.SelectionSummaryHeight == 28.0f);
		PF_CHECK(Tests, TallMaterialSummary.DetailsHeight == 0.0f && TallMaterialSummary.AssetItemsHeight == 812.0f);
		const ContentBrowserVerticalLayout InvalidContentHeight = GetContentBrowserVerticalLayout(
			std::numeric_limits<float>::quiet_NaN(), true, true);
		PF_CHECK(Tests, InvalidContentHeight.AssetItemsHeight == 0.0f && InvalidContentHeight.SelectionSummaryHeight == 0.0f);
		PF_CHECK(Tests, GetEditorEntityIcon({}) == EditorIcon::Entity);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasCamera = true, .HasMesh = true }) == EditorIcon::Camera);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasMesh = true, .HasPointLight = true }) == EditorIcon::Cube);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasSpotLight = true, .HasPointLight = true }) == EditorIcon::SpotLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasPointLight = true, .HasDirectionalLight = true }) == EditorIcon::PointLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasDirectionalLight = true }) == EditorIcon::DirectionalLight);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasEnvironmentLight = true }) == EditorIcon::Environment);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasAudioSource = true, .HasAudioListener = true }) == EditorIcon::AudioSource);
		PF_CHECK(Tests, GetEditorEntityIcon({ .HasAudioListener = true }) == EditorIcon::AudioListener);
		PF_CHECK(Tests, EditorFontDescriptors.size() == 3);
		PF_CHECK(Tests, EditorFontDescriptors[0].Role == EditorFontRole::Interface);
		PF_CHECK(Tests, EditorFontDescriptors[1].Role == EditorFontRole::Emphasis);
		PF_CHECK(Tests, EditorFontDescriptors[2].Role == EditorFontRole::Monospace);
		PF_CHECK(Tests, EditorFontDescriptors[0].RelativePath == "Fonts/Inter-Regular.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[1].RelativePath == "Fonts/Inter-SemiBold.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[2].RelativePath == "Fonts/JetBrainsMono-Regular.ttf");
		PF_CHECK(Tests, EditorFontDescriptors[0].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorFontDescriptors[1].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorFontDescriptors[2].PixelSize == 16.0f);
		PF_CHECK(Tests, EditorIconFontPixelSize == EditorFontDescriptors[0].PixelSize);
		PF_CHECK(Tests, IsValidEditorTextGlyphRanges());
		PF_CHECK(Tests, EditorTextGlyphRanges.back() == 0);
		for (size_t Index = 0; Index + 1 < EditorTextGlyphRanges.size() - 1; Index += 2)
		{
			PF_CHECK(Tests, EditorTextGlyphRanges[Index] <= EditorTextGlyphRanges[Index + 1]);
			if (Index > 0)
				PF_CHECK(Tests, EditorTextGlyphRanges[Index - 1] < EditorTextGlyphRanges[Index]);
		}
		for (const uint32_t Codepoint : {
			0x011e, 0x011f, 0x0130, 0x0131, 0x015e, 0x015f, 0x010c, 0x0141, 0x017d,
			0x0150, 0x00c5, 0x00e9, 0x00df, 0x20ac, 0x03a9, 0x0416, 0x1e0a, 0x2014 })
			PF_CHECK(Tests, EditorTextGlyphRangeContains(Codepoint));
		PF_CHECK(Tests, !EditorTextGlyphRangeContains(0x4e2d));
		PF_CHECK(Tests, EditorFontRoleHasIcons(EditorFontRole::Interface));
		PF_CHECK(Tests, EditorFontRoleHasIcons(EditorFontRole::Emphasis));
		PF_CHECK(Tests, !EditorFontRoleHasIcons(EditorFontRole::Monospace));

		const std::string_view TwoByteText = "\xc4\x9eX";
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 1) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 2) == 2);
		PF_CHECK(Tests, Utf8SafePrefixLength(TwoByteText, 3) == 3);
		const std::string_view ThreeByteText = "\xe2\x82\xacX";
		PF_CHECK(Tests, Utf8SafePrefixLength(ThreeByteText, 2) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(ThreeByteText, 3) == 3);
		const std::string_view FourByteText = "\xf0\x9f\x98\x80X";
		PF_CHECK(Tests, Utf8SafePrefixLength(FourByteText, 3) == 0);
		PF_CHECK(Tests, Utf8SafePrefixLength(FourByteText, 4) == 4);

		for (const EditorColorValue& Color : EditorPalette)
		{
			PF_CHECK(Tests, Color.Red >= 0.0f && Color.Red <= 1.0f);
			PF_CHECK(Tests, Color.Green >= 0.0f && Color.Green <= 1.0f);
			PF_CHECK(Tests, Color.Blue >= 0.0f && Color.Blue <= 1.0f);
			PF_CHECK(Tests, Color.Alpha >= 0.0f && Color.Alpha <= 1.0f);
		}
		constexpr std::array<EditorColorToken, 4> GizmoColorTokens = {
			EditorColorToken::GizmoAxisX,
			EditorColorToken::GizmoAxisY,
			EditorColorToken::GizmoAxisZ,
			EditorColorToken::GizmoHighlight
		};
		for (const EditorColorToken Token : GizmoColorTokens)
		{
			const EditorColorValue Color = GetEditorColorValue(Token);
			const EditorColorValue SrgbOutput = ConvertEditorColorForOutput(
				Color, PulseForge::OutputColorEncoding::SrgbAttachment);
			const EditorColorValue UnormOutput = ConvertEditorColorForOutput(
				Color, PulseForge::OutputColorEncoding::UnormAttachment);
			PF_CHECK(Tests, SrgbOutput.Alpha == Color.Alpha);
			PF_CHECK(Tests, UnormOutput.Red == Color.Red && UnormOutput.Green == Color.Green &&
				UnormOutput.Blue == Color.Blue && UnormOutput.Alpha == Color.Alpha);
		}

		const EditorColorValue DisplayColor{ 0.5f, 0.04045f, 1.0f, 0.37f };
		const EditorColorValue LinearColor = ConvertEditorColorForOutput(
			DisplayColor, PulseForge::OutputColorEncoding::SrgbAttachment);
		PF_CHECK(Tests, std::abs(LinearColor.Red - 0.21404114f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(LinearColor.Green - 0.003130805f) < 1.0e-7f);
		PF_CHECK(Tests, LinearColor.Blue == 1.0f);
		PF_CHECK(Tests, LinearColor.Alpha == DisplayColor.Alpha);
		const EditorColorValue UnormColor = ConvertEditorColorForOutput(
			DisplayColor, PulseForge::OutputColorEncoding::UnormAttachment);
		PF_CHECK(Tests, UnormColor.Red == DisplayColor.Red && UnormColor.Green == DisplayColor.Green &&
			UnormColor.Blue == DisplayColor.Blue && UnormColor.Alpha == DisplayColor.Alpha);
		PF_CHECK(Tests, SelectEditorUiCompositionPath(PulseForge::OutputColorEncoding::SrgbAttachment) ==
			EditorUiCompositionPath::DirectSrgbSwapchain);
		PF_CHECK(Tests, SelectEditorUiCompositionPath(PulseForge::OutputColorEncoding::UnormAttachment) ==
			EditorUiCompositionPath::SrgbIntermediateToUnormSwapchain);
		PF_CHECK(Tests, !SelectEditorUiCompositionPath(static_cast<PulseForge::OutputColorEncoding>(255)));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(false, 1280, 720, 1280, 720));
		PF_CHECK(Tests, IsEditorUiCompositionTargetReusable(true, 1280, 720, 1280, 720));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(true, 1280, 720, 1280, 721));
		PF_CHECK(Tests, !IsEditorUiCompositionTargetReusable(true, 1280, 720, 0, 720));
		PF_CHECK(Tests, LinearToSrgbForEditorUi(0.0f) == 0.0f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(1.0f) == 1.0f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(0.5f) - 0.7353569f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(0.0031308f) - 0.0404499f) < 1.0e-6f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(-1.0f) == 0.0f);
		PF_CHECK(Tests, LinearToSrgbForEditorUi(2.0f) == 1.0f);

		const float WhiteOverBlackLinear = BlendSourceOverLinear(1.0f, 0.0f, 0.5f);
		PF_CHECK(Tests, WhiteOverBlackLinear == 0.5f);
		PF_CHECK(Tests, std::abs(LinearToSrgbForEditorUi(WhiteOverBlackLinear) - 0.7353569f) < 1.0e-6f);
		PF_CHECK(Tests, std::abs(0.5f - LinearToSrgbForEditorUi(WhiteOverBlackLinear)) > 0.2f);
		PF_CHECK(Tests, BlendSourceOverLinear(1.0f, 0.25f, 0.0f) == 0.25f);
		PF_CHECK(Tests, BlendSourceOverLinear(0.25f, 0.75f, 1.0f) == 0.25f);
		PF_CHECK(Tests, std::abs(BlendSourceOverLinear(0.8f, 0.2f, 0.25f) - 0.35f) < 1.0e-6f);
		const float FirstLayer = BlendSourceOverLinear(0.8f, 0.1f, 0.5f);
		const float OrderedLayers = BlendSourceOverLinear(0.2f, FirstLayer, 0.25f);
		PF_CHECK(Tests, std::abs(OrderedLayers - 0.3875f) < 1.0e-6f);

		std::array<bool, static_cast<size_t>(EditorIcon::Count)> SeenIcons{};
		for (size_t Index = 0; Index < EditorIconDescriptors.size(); ++Index)
		{
			const EditorIconDescriptor& Descriptor = EditorIconDescriptors[Index];
			const size_t IconIndex = static_cast<size_t>(Descriptor.Icon);
			PF_CHECK(Tests, IconIndex < SeenIcons.size());
			if (IconIndex < SeenIcons.size())
			{
				PF_CHECK(Tests, !SeenIcons[IconIndex]);
				SeenIcons[IconIndex] = true;
			}
			PF_CHECK(Tests, !Descriptor.Name.empty());
			PF_CHECK(Tests, Descriptor.Codepoint >= 0xe000 && Descriptor.Codepoint <= 0xf8ff);
			PF_CHECK(Tests, EditorIconGlyphRanges[Index * 2] == Descriptor.Codepoint);
			PF_CHECK(Tests, EditorIconGlyphRanges[Index * 2 + 1] == Descriptor.Codepoint);
			if (Index > 0)
				PF_CHECK(Tests, EditorIconDescriptors[Index - 1].Codepoint < Descriptor.Codepoint);
			PF_CHECK(Tests, FindEditorIcon(Descriptor.Icon) == &Descriptor);
		}
		PF_CHECK(Tests, std::all_of(SeenIcons.begin(), SeenIcons.end(), [](bool Seen) { return Seen; }));
		PF_CHECK(Tests, EditorIconGlyphRanges.back() == 0);
		PF_CHECK(Tests, FindEditorIcon(EditorIcon::Count) == nullptr);
		const EditorIconDescriptor* SelectIcon = FindEditorIcon(EditorIcon::Select);
		PF_CHECK(Tests, SelectIcon && SelectIcon->Codepoint == 0xf245);
	}

	void TestSceneSerializationRoundTrip(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const UUID RootId{ 0x2000000000000000ull, 2 };
		const UUID ChildId{ 0x1000000000000000ull, 1 };
		const AssetID MeshAssetIdentifier{ 0x5000000000000000ull, 5 };
		const AssetID MaterialAssetIdentifier{ 0x5100000000000000ull, 51 };
		const AssetID AudioAssetIdentifier{ 0x5200000000000000ull, 52 };
		const AssetID ScriptAssetIdentifier{ 0x5300000000000000ull, 53 };
		auto ChildResult = Source.CreateEntityWithUUID(ChildId, "Child \"one\"");
		auto RootResult = Source.CreateEntityWithUUID(RootId, "Root");
		PF_CHECK(Tests, ChildResult.has_value() && RootResult.has_value());
		if (!ChildResult || !RootResult)
			return;

		Entity Child = *ChildResult;
		Entity Root = *RootResult;
		TransformComponent RootTransform;
		RootTransform.Translation = { 3.0f, 0.0f, 0.0f };
		TransformComponent ChildTransform;
		ChildTransform.Translation = { 4.0f, 2.0f, 1.0f };
		ChildTransform.Rotation = glm::quat(0.9238795f, 0.0f, 0.3826834f, 0.0f);
		ChildTransform.Scale = { 2.0f, 2.0f, 2.0f };
		PF_CHECK(Tests, Root.SetTransform(RootTransform).has_value());
		PF_CHECK(Tests, Child.SetTransform(ChildTransform).has_value());
		PF_CHECK(Tests, Child.SetParent(Root).has_value());
		CameraComponent SourceCamera;
		SourceCamera.VerticalFieldOfViewRadians = 0.9f;
		SourceCamera.NearClipPlane = 0.25f;
		SourceCamera.FarClipPlane = 80.0f;
		SourceCamera.IsPrimary = true;
		PF_CHECK(Tests, SourceCamera.Validate().has_value());
		const auto SourceProjection = SourceCamera.GetProjectionMatrix(16.0f / 9.0f);
		PF_CHECK(Tests, SourceProjection.has_value());
		PF_CHECK(Tests, SourceProjection && (*SourceProjection)[1][1] > 0.0f);
		const auto InvalidAspectProjection = SourceCamera.GetProjectionMatrix(0.0f);
		PF_CHECK(Tests, !InvalidAspectProjection.has_value());
		const auto GLMAssertAspectProjection = SourceCamera.GetProjectionMatrix(std::numeric_limits<float>::epsilon());
		PF_CHECK(Tests, !GLMAssertAspectProjection.has_value());
		CameraComponent OverflowingProjectionCamera = SourceCamera;
		OverflowingProjectionCamera.NearClipPlane = 2.0f;
		OverflowingProjectionCamera.FarClipPlane = std::numeric_limits<float>::max();
		const auto OverflowingProjection = OverflowingProjectionCamera.GetProjectionMatrix(16.0f / 9.0f);
		PF_CHECK(Tests, !OverflowingProjection.has_value());
		PF_CHECK(Tests, !OverflowingProjection && OverflowingProjection.error().Code == CameraErrorCode::NonFiniteProjection);
		CameraComponent InvalidCamera = SourceCamera;
		InvalidCamera.VerticalFieldOfViewRadians = 0.0f;
		const auto InvalidCameraSet = Child.SetCamera(InvalidCamera);
		PF_CHECK(Tests, !InvalidCameraSet.has_value());
		PF_CHECK(Tests, !InvalidCameraSet && InvalidCameraSet.error().Code == SceneErrorCode::InvalidCamera);
		const auto CameraBeforeSet = Child.GetCamera();
		PF_CHECK(Tests, CameraBeforeSet && !CameraBeforeSet->has_value());
		PF_CHECK(Tests, Child.SetCamera(SourceCamera).has_value());
		DirectionalLightComponent SourceLight{ glm::vec3(0.75f, 0.8f, 1.0f), 3.0f };
		SourceLight.CastShadows = false;
		SourceLight.ShadowDistance = 42.0f;
		SourceLight.ShadowBias = 2.0f;
		SourceLight.ShadowNormalBias = 0.04f;
		SourceLight.ShadowSoftness = 3.0f;
		PF_CHECK(Tests, SourceLight.Validate().has_value());
		PF_CHECK(Tests, Child.SetDirectionalLight(SourceLight).has_value());
		PointLightComponent SourcePointLight{ glm::vec3(0.2f, 0.7f, 0.4f), 5.5f, 18.0f };
		SourcePointLight.CastShadows = true;
		SourcePointLight.ShadowBias = 0.003f;
		SourcePointLight.ShadowNormalBias = 0.08f;
		SourcePointLight.ShadowSoftness = 2.25f;
		SpotLightComponent SourceSpotLight{ glm::vec3(0.5f, 0.6f, 0.9f), 12.0f, 24.0f, 12.0f, 30.0f };
		SourceSpotLight.CastShadows = true;
		SourceSpotLight.ShadowBias = 0.0025f;
		SourceSpotLight.ShadowNormalBias = 0.05f;
		SourceSpotLight.ShadowSoftness = 2.0f;
		PF_CHECK(Tests, Child.SetPointLight(SourcePointLight).has_value());
		PF_CHECK(Tests, Child.SetSpotLight(SourceSpotLight).has_value());
		const EnvironmentLightComponent SourceEnvironment{ AssetID{ 0x7400000000000000ull, 74 }, 1.25f };
		PF_CHECK(Tests, SourceEnvironment.Validate().has_value());
		const EnvironmentLightComponent MissingEnvironmentAsset{ AssetID{}, 1.0f };
		const EnvironmentLightComponent NegativeEnvironmentIntensity{ SourceEnvironment.HdrImage, -1.0f };
		const EnvironmentLightComponent InfiniteEnvironmentIntensity{
			SourceEnvironment.HdrImage, std::numeric_limits<float>::infinity() };
		PF_CHECK(Tests, !MissingEnvironmentAsset.Validate().has_value());
		PF_CHECK(Tests, !NegativeEnvironmentIntensity.Validate().has_value());
		PF_CHECK(Tests, !InfiniteEnvironmentIntensity.Validate().has_value());
		PF_CHECK(Tests, Child.SetEnvironmentLight(SourceEnvironment).has_value());
		PF_CHECK(Tests, Child.SetMeshRenderer(MeshRendererComponent{ MeshAssetIdentifier, MaterialAssetIdentifier }).has_value());
		PF_CHECK(Tests, !Child.SetMeshRenderer(MeshRendererComponent{ MeshAssetIdentifier, AssetID{} }).has_value());
		const AudioSourceComponent SourceAudio{ AudioAssetIdentifier, 0.35f, true, false, true };
		PF_CHECK(Tests, Child.SetAudioSource(SourceAudio).has_value());
		PF_CHECK(Tests, Child.SetScript(ScriptComponent{ ScriptAssetIdentifier, false }).has_value());
		const auto InvalidScriptSet = Child.SetScript(ScriptComponent{});
		PF_CHECK(Tests, !InvalidScriptSet && InvalidScriptSet.error().Code == SceneErrorCode::InvalidScriptComponent);
		PF_CHECK(Tests, Root.SetAudioListener(AudioListenerComponent{ true }).has_value());
		AudioSourceComponent InvalidAudio = SourceAudio;
		InvalidAudio.AudioAsset = AssetID{};
		const auto InvalidAudioSet = Child.SetAudioSource(InvalidAudio);
		PF_CHECK(Tests, !InvalidAudioSet && InvalidAudioSet.error().Code == SceneErrorCode::InvalidAudioComponent);
		const auto RootCamera = Root.GetCamera();
		PF_CHECK(Tests, RootCamera.has_value() && !RootCamera->has_value());

		const auto Serialized = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		PF_CHECK(Tests, Serialized->find("\"format\": \"PulseForgeScene\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"version\": 7") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"primary\": true") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"directionalLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"castShadows\": false") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"shadowDistance\": 42.0") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"environmentLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"pointLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"shadowSoftness\": 2.25") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"spotLight\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"meshAsset\": \"" + MeshAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"materialAsset\": \"" + MaterialAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"asset\": \"" + AudioAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"playOnStart\": false") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"audioListener\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"script\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"asset\": \"" + ScriptAssetIdentifier.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("Assets/") == std::string::npos);
		const size_t ChildEntityPosition = Serialized->find("\"uuid\": \"" + ChildId.ToString() + "\"");
		const size_t RootEntityPosition = Serialized->find("\"uuid\": \"" + RootId.ToString() + "\"");
		PF_CHECK(Tests, ChildEntityPosition != std::string::npos && RootEntityPosition != std::string::npos &&
			ChildEntityPosition < RootEntityPosition);

		auto LegacyDocument = nlohmann::json::parse(*Serialized);
		bool FoundLegacyLight = false;
		for (auto& EntityRecord : LegacyDocument["entities"])
		{
			if (EntityRecord.value("uuid", std::string{}) != ChildId.ToString())
				continue;
			FoundLegacyLight = EntityRecord.contains("directionalLight");
			if (!FoundLegacyLight)
				break;
			for (const std::string_view Key : { "castShadows", "shadowDistance", "shadowBias", "shadowNormalBias", "shadowSoftness" })
				EntityRecord["directionalLight"].erase(std::string(Key));
			break;
		}
		PF_CHECK(Tests, FoundLegacyLight);
		const std::string LegacySerialized = LegacyDocument.dump(2) + "\n";
		Scene LegacyDestination;
		const auto LegacyLoaded = SceneSerializer::Deserialize(LegacySerialized, LegacyDestination);
		PF_CHECK(Tests, LegacyLoaded.has_value());
		if (LegacyLoaded)
		{
			const auto LegacyEntity = LegacyDestination.FindEntity(ChildId);
			PF_CHECK(Tests, LegacyEntity.has_value());
			if (LegacyEntity)
			{
				const auto LegacyLight = LegacyEntity->GetDirectionalLight();
				PF_CHECK(Tests, LegacyLight && LegacyLight->has_value() && LegacyLight->value().CastShadows);
				PF_CHECK(Tests, LegacyLight && LegacyLight->has_value() && LegacyLight->value().ShadowDistance == 100.0f);
				PF_CHECK(Tests, LegacyLight && LegacyLight->has_value() && LegacyLight->value().ShadowBias == 1.0f &&
					LegacyLight->value().ShadowNormalBias == 0.025f && LegacyLight->value().ShadowSoftness == 1.5f);
			}
		}

		Scene Destination;
		auto PreviousResult = Destination.CreateEntity("Previous scene");
		PF_CHECK(Tests, PreviousResult.has_value());
		if (!PreviousResult)
			return;
		Entity PreviousEntity = *PreviousResult;
		const auto Loaded = SceneSerializer::Deserialize(*Serialized, Destination);
		PF_CHECK(Tests, Loaded.has_value());
		PF_CHECK(Tests, !PreviousEntity.IsValid());
		PF_CHECK(Tests, Destination.GetEntityCount() == 2);

		const auto LoadedRoot = Destination.FindEntity(RootId);
		const auto LoadedChild = Destination.FindEntity(ChildId);
		PF_CHECK(Tests, LoadedRoot.has_value() && LoadedChild.has_value());
		if (!LoadedRoot || !LoadedChild)
			return;

		const auto LoadedTag = LoadedChild->GetTag();
		const auto LoadedTransform = LoadedChild->GetTransform();
		const auto LoadedCamera = LoadedChild->GetCamera();
		const auto LoadedLight = LoadedChild->GetDirectionalLight();
		const auto LoadedEnvironment = LoadedChild->GetEnvironmentLight();
		const auto LoadedPoint = LoadedChild->GetPointLight();
		const auto LoadedSpot = LoadedChild->GetSpotLight();
		const auto LoadedMeshRenderer = LoadedChild->GetMeshRenderer();
		const auto LoadedAudioSource = LoadedChild->GetAudioSource();
		const auto LoadedAudioListener = LoadedRoot->GetAudioListener();
		const auto LoadedScript = LoadedChild->GetScript();
		const auto LoadedParent = LoadedChild->GetParent();
		PF_CHECK(Tests, LoadedTag && LoadedTag->Name == "Child \"one\"");
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Translation, ChildTransform.Translation)));
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Scale, ChildTransform.Scale)));
		PF_CHECK(Tests, LoadedTransform && glm::abs(glm::length(LoadedTransform->Rotation) - 1.0f) < 0.0001f);
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value());
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value() &&
			glm::abs(LoadedCamera->value().VerticalFieldOfViewRadians - SourceCamera.VerticalFieldOfViewRadians) < 0.0001f);
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value() && LoadedCamera->value().IsPrimary);
		PF_CHECK(Tests, LoadedLight && LoadedLight->has_value() &&
			LoadedLight->value().Intensity == SourceLight.Intensity &&
			glm::all(glm::equal(LoadedLight->value().Color, SourceLight.Color)));
		PF_CHECK(Tests, LoadedLight && LoadedLight->has_value() &&
			!LoadedLight->value().CastShadows && LoadedLight->value().ShadowDistance == 42.0f &&
			LoadedLight->value().ShadowBias == 2.0f && LoadedLight->value().ShadowNormalBias == 0.04f &&
			LoadedLight->value().ShadowSoftness == 3.0f);
		PF_CHECK(Tests, LoadedEnvironment && LoadedEnvironment->has_value() &&
			LoadedEnvironment->value().HdrImage == SourceEnvironment.HdrImage &&
			LoadedEnvironment->value().Intensity == SourceEnvironment.Intensity);
		PF_CHECK(Tests, LoadedPoint && LoadedPoint->has_value() &&
			LoadedPoint->value().Intensity == SourcePointLight.Intensity && LoadedPoint->value().Range == SourcePointLight.Range &&
			glm::all(glm::equal(LoadedPoint->value().Color, SourcePointLight.Color)) &&
			LoadedPoint->value().CastShadows && LoadedPoint->value().ShadowBias == SourcePointLight.ShadowBias &&
			LoadedPoint->value().ShadowNormalBias == SourcePointLight.ShadowNormalBias &&
			LoadedPoint->value().ShadowSoftness == SourcePointLight.ShadowSoftness);
		PF_CHECK(Tests, LoadedSpot && LoadedSpot->has_value() &&
			LoadedSpot->value().InnerConeAngleDegrees == SourceSpotLight.InnerConeAngleDegrees &&
			LoadedSpot->value().OuterConeAngleDegrees == SourceSpotLight.OuterConeAngleDegrees &&
			LoadedSpot->value().Range == SourceSpotLight.Range && LoadedSpot->value().CastShadows &&
			LoadedSpot->value().ShadowBias == SourceSpotLight.ShadowBias &&
			LoadedSpot->value().ShadowNormalBias == SourceSpotLight.ShadowNormalBias &&
			LoadedSpot->value().ShadowSoftness == SourceSpotLight.ShadowSoftness);
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MeshAsset == MeshAssetIdentifier);
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MaterialAsset == MaterialAssetIdentifier);
		PF_CHECK(Tests, LoadedAudioSource && LoadedAudioSource->has_value() &&
			LoadedAudioSource->value().AudioAsset == AudioAssetIdentifier);
		PF_CHECK(Tests, LoadedAudioSource && LoadedAudioSource->has_value() &&
			LoadedAudioSource->value().Volume == SourceAudio.Volume && LoadedAudioSource->value().Looping &&
			!LoadedAudioSource->value().PlayOnStart && LoadedAudioSource->value().Spatialized);
		PF_CHECK(Tests, LoadedAudioListener && LoadedAudioListener->has_value() && LoadedAudioListener->value().IsPrimary);
		PF_CHECK(Tests, LoadedScript && LoadedScript->has_value() &&
			LoadedScript->value().ScriptAsset == ScriptAssetIdentifier && !LoadedScript->value().Enabled);
		PF_CHECK(Tests, LoadedParent && LoadedParent->has_value() && **LoadedParent == *LoadedRoot);
		auto LegacySpotDocument = nlohmann::json::parse(*Serialized);
		for (auto& EntityRecord : LegacySpotDocument["entities"])
			if (EntityRecord.contains("spotLight"))
			{
				EntityRecord["spotLight"].erase("castShadows");
				EntityRecord["spotLight"].erase("shadowBias");
				EntityRecord["spotLight"].erase("shadowNormalBias");
				EntityRecord["spotLight"].erase("shadowSoftness");
			}
		Scene LegacySpotDestination;
		const auto LegacySpotLoad = SceneSerializer::Deserialize(LegacySpotDocument.dump(), LegacySpotDestination);
		PF_CHECK(Tests, LegacySpotLoad.has_value());
		if (LegacySpotLoad)
		{
			const auto LegacySpotEntity = LegacySpotDestination.FindEntity(ChildId);
			PF_CHECK(Tests, LegacySpotEntity.has_value());
			if (LegacySpotEntity)
			{
				const auto LegacySpot = LegacySpotEntity->GetSpotLight();
				PF_CHECK(Tests, LegacySpot && LegacySpot->has_value() && !LegacySpot->value().CastShadows &&
					LegacySpot->value().ShadowBias == SpotLightComponent{}.ShadowBias);
			}
		}
		auto InvalidSpotDocument = nlohmann::json::parse(*Serialized);
		for (auto& EntityRecord : InvalidSpotDocument["entities"])
			if (EntityRecord.value("uuid", std::string{}) == ChildId.ToString())
				EntityRecord["spotLight"]["innerConeAngleDegrees"] =
					EntityRecord["spotLight"]["outerConeAngleDegrees"];
		const auto InvalidSpotLoad = SceneSerializer::Deserialize(InvalidSpotDocument.dump(), Destination);
		PF_CHECK(Tests, !InvalidSpotLoad);
		PF_CHECK(Tests, Destination.FindEntity(ChildId).has_value() && Destination.GetEntityCount() == 2);
		const auto SerializedAgain = SceneSerializer::Serialize(Destination);
		PF_CHECK(Tests, SerializedAgain && *SerializedAgain == *Serialized);
		if (LoadedCamera && LoadedCamera->has_value())
		{
			PF_CHECK(Tests, LoadedChild->RemoveCamera().has_value());
			const auto RemovedCamera = LoadedChild->GetCamera();
			PF_CHECK(Tests, RemovedCamera && !RemovedCamera->has_value());
			const auto MissingCameraRemoval = LoadedChild->RemoveCamera();
			PF_CHECK(Tests, !MissingCameraRemoval.has_value());
			PF_CHECK(Tests, !MissingCameraRemoval && MissingCameraRemoval.error().Code == SceneErrorCode::MissingComponent);
		}

		const auto DestinationRootBeforeFailure = Destination.FindEntity(RootId);
		std::string UnsupportedVersion = *Serialized;
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 7");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 7").size(), "\"version\": 99");
		const auto UnsupportedVersionResult = SceneSerializer::Deserialize(UnsupportedVersion, Destination);
		PF_CHECK(Tests, !UnsupportedVersionResult.has_value());
		PF_CHECK(Tests, !UnsupportedVersionResult && UnsupportedVersionResult.error().Code == SceneSerializationErrorCode::UnsupportedVersion);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidAudioVolume = *Serialized;
		const size_t AudioSourcePosition = InvalidAudioVolume.find("\"audioSource\"");
		const size_t AudioVolumePosition = InvalidAudioVolume.find("\"volume\": ", AudioSourcePosition);
		PF_CHECK(Tests, AudioSourcePosition != std::string::npos && AudioVolumePosition != std::string::npos);
		if (AudioVolumePosition != std::string::npos)
		{
			const size_t AudioVolumeValueStart = AudioVolumePosition + std::string("\"volume\": ").size();
			const size_t AudioVolumeValueEnd = InvalidAudioVolume.find_first_of(",\n", AudioVolumeValueStart);
			PF_CHECK(Tests, AudioVolumeValueEnd != std::string::npos);
			if (AudioVolumeValueEnd != std::string::npos)
				InvalidAudioVolume.replace(AudioVolumeValueStart, AudioVolumeValueEnd - AudioVolumeValueStart, "2.0");
		}
		const auto InvalidAudioVolumeResult = SceneSerializer::Deserialize(InvalidAudioVolume, Destination);
		PF_CHECK(Tests, !InvalidAudioVolumeResult &&
			InvalidAudioVolumeResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string LegacyVersionThree = *Serialized;
		const size_t PrimaryFieldPosition = LegacyVersionThree.find("\"primary\": true");
		PF_CHECK(Tests, PrimaryFieldPosition != std::string::npos);
		if (PrimaryFieldPosition != std::string::npos)
		{
			const size_t PrimaryCommaPosition = LegacyVersionThree.rfind(',', PrimaryFieldPosition);
			const size_t PrimaryLineEnd = LegacyVersionThree.find('\n', PrimaryFieldPosition);
			PF_CHECK(Tests, PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos);
			if (PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos)
				LegacyVersionThree.erase(PrimaryCommaPosition, PrimaryLineEnd - PrimaryCommaPosition);
		}
		const size_t LegacyVersionThreePosition = LegacyVersionThree.find("\"version\": 7");
		PF_CHECK(Tests, LegacyVersionThreePosition != std::string::npos);
		if (LegacyVersionThreePosition != std::string::npos)
			LegacyVersionThree.replace(LegacyVersionThreePosition, std::string("\"version\": 7").size(), "\"version\": 3");
		Scene LegacyVersionThreeDestination;
		const auto LegacyVersionThreeLoad = SceneSerializer::Deserialize(LegacyVersionThree, LegacyVersionThreeDestination);
		PF_CHECK(Tests, LegacyVersionThreeLoad.has_value());
		const auto LegacyCamera = LegacyVersionThreeDestination.FindEntity(ChildId);
		PF_CHECK(Tests, LegacyCamera.has_value());
		if (LegacyCamera)
		{
			const auto LegacyCameraComponent = LegacyCamera->GetCamera();
			PF_CHECK(Tests, LegacyCameraComponent && LegacyCameraComponent->has_value() &&
				!LegacyCameraComponent->value().IsPrimary);
		}

		Scene LegacySource;
		const auto LegacyEntity = LegacySource.CreateEntityWithUUID(UUID{ 0x6000000000000000ull, 6 }, "Version one");
		PF_CHECK(Tests, LegacyEntity.has_value());
		const auto LegacySerializedVersionTwo = SceneSerializer::Serialize(LegacySource);
		PF_CHECK(Tests, LegacySerializedVersionTwo.has_value());
		if (LegacySerializedVersionTwo)
		{
			std::string LegacyVersionOne = *LegacySerializedVersionTwo;
			const size_t LegacyVersionPosition = LegacyVersionOne.find("\"version\": 7");
			PF_CHECK(Tests, LegacyVersionPosition != std::string::npos);
			if (LegacyVersionPosition != std::string::npos)
				LegacyVersionOne.replace(LegacyVersionPosition, std::string("\"version\": 7").size(), "\"version\": 1");
			Scene LegacyDestination;
			const auto LegacyLoad = SceneSerializer::Deserialize(LegacyVersionOne, LegacyDestination);
			PF_CHECK(Tests, LegacyLoad.has_value());
			PF_CHECK(Tests, LegacyDestination.GetEntityCount() == 1);
			const auto LegacyLoadedEntity = LegacyDestination.FindEntity(UUID{ 0x6000000000000000ull, 6 });
			PF_CHECK(Tests, LegacyLoadedEntity.has_value());
			if (LegacyLoadedEntity)
			{
				const auto LegacyMeshRenderer = LegacyLoadedEntity->GetMeshRenderer();
				PF_CHECK(Tests, LegacyMeshRenderer && !LegacyMeshRenderer->has_value());
			}
		}

		std::string LegacyVersionTwo = LegacyVersionThree;
		const std::string MaterialField = "\"materialAsset\": \"" + MaterialAssetIdentifier.ToString() + "\"";
		const size_t MaterialFieldPosition = LegacyVersionTwo.find(MaterialField);
		PF_CHECK(Tests, MaterialFieldPosition != std::string::npos);
		if (MaterialFieldPosition != std::string::npos)
		{
			const size_t CommaPosition = LegacyVersionTwo.rfind(',', MaterialFieldPosition);
			const size_t LineEnd = LegacyVersionTwo.find('\n', MaterialFieldPosition);
			PF_CHECK(Tests, CommaPosition != std::string::npos && LineEnd != std::string::npos);
			if (CommaPosition != std::string::npos && LineEnd != std::string::npos)
				LegacyVersionTwo.erase(CommaPosition, LineEnd - CommaPosition);
		}
		const size_t VersionTwoPosition = LegacyVersionTwo.find("\"version\": 3");
		PF_CHECK(Tests, VersionTwoPosition != std::string::npos);
		if (VersionTwoPosition != std::string::npos)
			LegacyVersionTwo.replace(VersionTwoPosition, std::string("\"version\": 3").size(), "\"version\": 2");
		Scene LegacyVersionTwoDestination;
		const auto LegacyVersionTwoLoad = SceneSerializer::Deserialize(LegacyVersionTwo, LegacyVersionTwoDestination);
		PF_CHECK(Tests, LegacyVersionTwoLoad.has_value());
		const auto LegacyVersionTwoChild = LegacyVersionTwoDestination.FindEntity(ChildId);
		PF_CHECK(Tests, LegacyVersionTwoChild.has_value());
		if (LegacyVersionTwoChild)
		{
			const auto LegacyMeshRenderer = LegacyVersionTwoChild->GetMeshRenderer();
			PF_CHECK(Tests, LegacyMeshRenderer && LegacyMeshRenderer->has_value());
			PF_CHECK(Tests, LegacyMeshRenderer && LegacyMeshRenderer->has_value() &&
				!LegacyMeshRenderer->value().MaterialAsset.has_value());
		}

		std::string InvalidMeshAsset = *Serialized;
		const size_t MeshAssetPosition = InvalidMeshAsset.find(MeshAssetIdentifier.ToString());
		PF_CHECK(Tests, MeshAssetPosition != std::string::npos);
		if (MeshAssetPosition != std::string::npos)
			InvalidMeshAsset.replace(MeshAssetPosition, MeshAssetIdentifier.ToString().size(), "not-a-uuid");
		const auto InvalidMeshAssetLoad = SceneSerializer::Deserialize(InvalidMeshAsset, Destination);
		PF_CHECK(Tests, !InvalidMeshAssetLoad.has_value());
		PF_CHECK(Tests, !InvalidMeshAssetLoad &&
			InvalidMeshAssetLoad.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidMaterialAsset = *Serialized;
		const size_t InvalidMaterialPosition = InvalidMaterialAsset.find(MaterialAssetIdentifier.ToString());
		PF_CHECK(Tests, InvalidMaterialPosition != std::string::npos);
		if (InvalidMaterialPosition != std::string::npos)
			InvalidMaterialAsset.replace(InvalidMaterialPosition, MaterialAssetIdentifier.ToString().size(), "not-a-uuid");
		const auto InvalidMaterialLoad = SceneSerializer::Deserialize(InvalidMaterialAsset, Destination);
		PF_CHECK(Tests, !InvalidMaterialLoad && InvalidMaterialLoad.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidCameraDocument = *Serialized;
		const size_t CameraFieldPosition = InvalidCameraDocument.find("\"verticalFovRadians\": ");
		const size_t CameraValuePosition = InvalidCameraDocument.find_first_of("0123456789", CameraFieldPosition);
		const size_t CameraValueEnd = InvalidCameraDocument.find_first_of(",\r\n", CameraValuePosition);
		PF_CHECK(Tests, CameraFieldPosition != std::string::npos && CameraValuePosition != std::string::npos &&
			CameraValueEnd != std::string::npos);
		if (CameraValuePosition != std::string::npos && CameraValueEnd != std::string::npos)
			InvalidCameraDocument.replace(CameraValuePosition, CameraValueEnd - CameraValuePosition, "0.0");
		const auto InvalidCameraResult = SceneSerializer::Deserialize(InvalidCameraDocument, Destination);
		PF_CHECK(Tests, !InvalidCameraResult.has_value());
		PF_CHECK(Tests, !InvalidCameraResult && InvalidCameraResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidPrimaryCameraDocument = *Serialized;
		const size_t PrimaryValuePosition = InvalidPrimaryCameraDocument.find("\"primary\": true");
		PF_CHECK(Tests, PrimaryValuePosition != std::string::npos);
		if (PrimaryValuePosition != std::string::npos)
			InvalidPrimaryCameraDocument.replace(PrimaryValuePosition, std::string("\"primary\": true").size(), "\"primary\": \"yes\"");
		const auto InvalidPrimaryCameraResult = SceneSerializer::Deserialize(InvalidPrimaryCameraDocument, Destination);
		PF_CHECK(Tests, !InvalidPrimaryCameraResult &&
			InvalidPrimaryCameraResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string UnsupportedFormat = *Serialized;
		const size_t FormatPosition = UnsupportedFormat.find("PulseForgeScene");
		PF_CHECK(Tests, FormatPosition != std::string::npos);
		if (FormatPosition != std::string::npos)
			UnsupportedFormat.replace(FormatPosition, std::string("PulseForgeScene").size(), "OtherSceneFormat");
		const auto UnsupportedFormatResult = SceneSerializer::Deserialize(UnsupportedFormat, Destination);
		PF_CHECK(Tests, !UnsupportedFormatResult.has_value());
		PF_CHECK(Tests, !UnsupportedFormatResult && UnsupportedFormatResult.error().Code == SceneSerializationErrorCode::UnsupportedFormat);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string InvalidTransform = *Serialized;
		const size_t TranslationPosition = InvalidTransform.find("\"translation\": [");
		const size_t FirstTranslation = InvalidTransform.find("4.0", TranslationPosition);
		PF_CHECK(Tests, TranslationPosition != std::string::npos && FirstTranslation != std::string::npos);
		if (FirstTranslation != std::string::npos)
			InvalidTransform.replace(FirstTranslation, 3, "\"bad\"");
		const auto InvalidTransformResult = SceneSerializer::Deserialize(InvalidTransform, Destination);
		PF_CHECK(Tests, !InvalidTransformResult.has_value());
		PF_CHECK(Tests, !InvalidTransformResult && InvalidTransformResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string MissingParent = *Serialized;
		const size_t ChildPosition = MissingParent.find(ChildId.ToString());
		const size_t ParentPosition = MissingParent.find(RootId.ToString(), ChildPosition);
		PF_CHECK(Tests, ChildPosition != std::string::npos && ParentPosition != std::string::npos);
		if (ParentPosition != std::string::npos)
			MissingParent.replace(ParentPosition, RootId.ToString().size(), UUID{ 0x3000000000000000ull, 3 }.ToString());
		const auto MissingParentResult = SceneSerializer::Deserialize(MissingParent, Destination);
		PF_CHECK(Tests, !MissingParentResult.has_value());
		PF_CHECK(Tests, !MissingParentResult && MissingParentResult.error().Code == SceneSerializationErrorCode::MissingParent);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string HierarchyCycle = *Serialized;
		const size_t DetachedParentPosition = HierarchyCycle.find("\"parent\": null");
		PF_CHECK(Tests, DetachedParentPosition != std::string::npos);
		if (DetachedParentPosition != std::string::npos)
		{
			HierarchyCycle.replace(
				DetachedParentPosition,
				std::string("\"parent\": null").size(),
				"\"parent\": \"" + ChildId.ToString() + "\"");
		}
		const auto HierarchyCycleResult = SceneSerializer::Deserialize(HierarchyCycle, Destination);
		PF_CHECK(Tests, !HierarchyCycleResult.has_value());
		PF_CHECK(Tests, !HierarchyCycleResult && HierarchyCycleResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		std::string DuplicateUUID = *Serialized;
		const std::string RootRecordPrefix = "\"uuid\": \"";
		const size_t RootRecordPosition = DuplicateUUID.find(RootRecordPrefix + RootId.ToString() + "\"");
		PF_CHECK(Tests, RootRecordPosition != std::string::npos);
		if (RootRecordPosition != std::string::npos)
			DuplicateUUID.replace(RootRecordPosition + RootRecordPrefix.size(), RootId.ToString().size(), ChildId.ToString());
		const auto DuplicateUUIDResult = SceneSerializer::Deserialize(DuplicateUUID, Destination);
		PF_CHECK(Tests, !DuplicateUUIDResult.has_value());
		PF_CHECK(Tests, !DuplicateUUIDResult && DuplicateUUIDResult.error().Code == SceneSerializationErrorCode::InvalidEntityData);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		const auto InvalidJsonResult = SceneSerializer::Deserialize("{invalid json", Destination);
		PF_CHECK(Tests, !InvalidJsonResult.has_value());
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		Scene EmptyScene;
		const auto EmptySerialized = SceneSerializer::Serialize(EmptyScene);
		PF_CHECK(Tests, EmptySerialized.has_value());
		if (EmptySerialized)
		{
			PF_CHECK(Tests, SceneSerializer::Deserialize(*EmptySerialized, Destination).has_value());
			PF_CHECK(Tests, Destination.GetEntityCount() == 0);
			PF_CHECK(Tests, !LoadedRoot->IsValid());
		}

		std::error_code TemporaryDirectoryError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(TemporaryDirectoryError);
		PF_CHECK(Tests, !TemporaryDirectoryError);
		if (TemporaryDirectoryError)
			return;
		const auto FileIdentifier = UUID::Generate();
		PF_CHECK(Tests, FileIdentifier.has_value());
		if (!FileIdentifier)
			return;

		const std::filesystem::path ScenePath = TemporaryDirectory / ("PulseForgeScene-" + FileIdentifier->ToString() + ".json");
		struct SceneFileCleanup
		{
			std::filesystem::path Path;
			~SceneFileCleanup()
			{
				std::error_code Error;
				std::filesystem::remove(Path, Error);
			}
		} Cleanup{ ScenePath };

		const auto SaveResult = SceneSerializer::SaveToFile(Source, ScenePath);
		PF_CHECK(Tests, SaveResult.has_value());
		if (!SaveResult)
			return;

		Scene FileLoadedScene;
		const auto LoadResult = SceneSerializer::LoadFromFile(ScenePath, FileLoadedScene);
		PF_CHECK(Tests, LoadResult.has_value());
		PF_CHECK(Tests, FileLoadedScene.GetEntityCount() == 2);
		const auto FileLoadedChild = FileLoadedScene.FindEntity(ChildId);
		PF_CHECK(Tests, FileLoadedChild.has_value());
		if (FileLoadedChild)
		{
			const auto FileLoadedParent = FileLoadedChild->GetParent();
			PF_CHECK(Tests, FileLoadedParent.has_value() && FileLoadedParent->has_value());
		}

		const auto OverwriteResult = SceneSerializer::SaveToFile(EmptyScene, ScenePath);
		PF_CHECK(Tests, OverwriteResult.has_value());
		if (OverwriteResult)
		{
			PF_CHECK(Tests, SceneSerializer::LoadFromFile(ScenePath, FileLoadedScene).has_value());
			PF_CHECK(Tests, FileLoadedScene.GetEntityCount() == 0);
			PF_CHECK(Tests, FileLoadedChild && !FileLoadedChild->IsValid());
		}

		std::filesystem::path MissingScenePath = ScenePath;
		MissingScenePath += ".missing";
		const auto EmptyPathResult = SceneSerializer::SaveToFile(Source, {});
		PF_CHECK(Tests, !EmptyPathResult.has_value());
		const auto MissingFileResult = SceneSerializer::LoadFromFile(MissingScenePath, Source);
		PF_CHECK(Tests, !MissingFileResult.has_value());
		PF_CHECK(Tests, !MissingFileResult && MissingFileResult.error().Code == SceneSerializationErrorCode::FileOpenFailed);
		PF_CHECK(Tests, Source.GetEntityCount() == 2);
	}

	void TestSceneAssetsByUUID(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeSceneAssets-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path SceneDirectory = ProjectRoot / "Assets" / "Scenes";
		std::filesystem::create_directories(SceneDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto CameraID = UUID::Parse("1b6cafe2-0f30-44ea-b841-10cf66f3e517");
		const auto MeshEntityID = UUID::Parse("2c76c6f7-e8f5-4936-9a24-61dbe7527e08");
		const auto MeshAssetID = UUID::Parse("3d56dd08-fd22-4a47-9ddf-09f392f1a619");
		PF_CHECK(Tests, CameraID && MeshEntityID && MeshAssetID);
		if (!CameraID || !MeshEntityID || !MeshAssetID)
			return;

		Scene Source;
		const auto Camera = Source.CreateEntityWithUUID(*CameraID, "Camera");
		const auto MeshEntity = Source.CreateEntityWithUUID(*MeshEntityID, "Mesh");
		PF_CHECK(Tests, Camera && MeshEntity);
		if (!Camera || !MeshEntity)
			return;
		PF_CHECK(Tests, Camera->SetCamera(CameraComponent{}).has_value());
		PF_CHECK(Tests, MeshEntity->SetMeshRenderer(MeshRendererComponent{ *MeshAssetID }).has_value());

		const std::filesystem::path SourcePath = SceneDirectory / "validation.scene";
		PF_CHECK(Tests, SceneSerializer::SaveToFile(Source, SourcePath).has_value());
		const auto CreatedMetadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, CreatedMetadata.has_value());
		if (!CreatedMetadata)
			return;
		const AssetID SceneID = CreatedMetadata->ID;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto CreatedScene = SceneAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Scenes/generated.scene",
			Source);
		PF_CHECK(Tests, CreatedScene.has_value());
		PF_CHECK(Tests, CreatedScene && !CreatedScene->ID.IsNil() &&
			CreatedScene->ID != SceneID &&
			CreatedScene->ProjectRelativePath == "Assets/Scenes/generated.scene");
		if (CreatedScene)
		{
			const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(
				ProjectRoot / "Assets/Scenes/generated.scene.meta");
			PF_CHECK(Tests, CreatedMetadata && CreatedMetadata->ID == CreatedScene->ID);
			PF_CHECK(Tests, Registry.Find(CreatedScene->ID).has_value());

			Scene GeneratedScene;
			PF_CHECK(Tests, SceneAssetService::Load(CreatedScene->ID, ProjectRoot, Registry, GeneratedScene).has_value());
			PF_CHECK(Tests, GeneratedScene.GetEntityCount() == Source.GetEntityCount());
			PF_CHECK(Tests, GeneratedScene.FindEntity(*MeshEntityID).has_value());

			const auto DuplicateCreation = SceneAssetService::Create(
				Registry,
				ProjectRoot,
				"Assets/Scenes/generated.scene",
				Source);
			PF_CHECK(Tests, !DuplicateCreation &&
				DuplicateCreation.error().Code == SceneAssetErrorCode::AssetOperationFailed);
			PF_CHECK(Tests, Registry.Find(CreatedScene->ID).has_value());
		}
		const auto InvalidSceneCreation = SceneAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Scenes/generated.json",
			Source);
		PF_CHECK(Tests, !InvalidSceneCreation &&
			InvalidSceneCreation.error().Code == SceneAssetErrorCode::UnsupportedAssetType);

		const auto ResolvedSource = AssetPathResolver::ResolveManagedSourcePath(
			ProjectRoot,
			std::filesystem::path("Assets/Scenes/validation.scene"));
		PF_CHECK(Tests, ResolvedSource && *ResolvedSource == std::filesystem::absolute(SourcePath).lexically_normal());
		PF_CHECK(Tests, !AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, "Assets/../outside.scene"));
		PF_CHECK(Tests, !AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, SourcePath));

		Scene Loaded;
		const auto PreviousEntity = Loaded.CreateEntity("Previous scene contents");
		const auto LoadedScene = SceneAssetService::Load(SceneID, ProjectRoot, Registry, Loaded);
		PF_CHECK(Tests, LoadedScene.has_value());
		PF_CHECK(Tests, PreviousEntity && !PreviousEntity->IsValid());
		PF_CHECK(Tests, Loaded.GetEntityCount() == 2);
		const auto LoadedMesh = Loaded.FindEntity(*MeshEntityID);
		PF_CHECK(Tests, LoadedMesh.has_value());
		if (!LoadedMesh)
			return;
		const auto LoadedMeshRenderer = LoadedMesh->GetMeshRenderer();
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value());
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MeshAsset == *MeshAssetID);

		const auto MoveResult = AssetOperations::Move(
			Registry,
			ProjectRoot,
			SceneID,
			"Assets/Scenes/renamed.scene");
		PF_CHECK(Tests, MoveResult && MoveResult->ID == SceneID);
		const auto MovedRecord = Registry.Find(SceneID);
		PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == "Assets/Scenes/renamed.scene");
		PF_CHECK(Tests, SceneAssetService::Load(SceneID, ProjectRoot, Registry, Loaded).has_value());

		const auto UpdatedMesh = Loaded.FindEntity(*MeshEntityID);
		PF_CHECK(Tests, UpdatedMesh.has_value());
		if (!UpdatedMesh)
			return;
		auto UpdatedTag = UpdatedMesh->GetTag();
		PF_CHECK(Tests, UpdatedTag.has_value());
		if (!UpdatedTag)
			return;
		UpdatedTag->Name = "Saved through stable scene UUID";
		PF_CHECK(Tests, UpdatedMesh->SetTag(*UpdatedTag).has_value());
		PF_CHECK(Tests, SceneAssetService::Save(SceneID, ProjectRoot, Registry, Loaded).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(ProjectRoot / "Assets" / "Scenes" / "renamed.scene"));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == SceneID);
		Scene SavedAgain;
		PF_CHECK(Tests, SceneAssetService::Load(SceneID, ProjectRoot, Registry, SavedAgain).has_value());
		const auto SavedEntity = SavedAgain.FindEntity(*MeshEntityID);
		PF_CHECK(Tests, SavedEntity && SavedEntity->GetTag() && SavedEntity->GetTag()->Name == "Saved through stable scene UUID");

		const std::filesystem::path OtherAssetPath = ProjectRoot / "Assets" / "Scenes" / "notes.txt";
		{
			std::ofstream Output(OtherAssetPath, std::ios::binary | std::ios::trunc);
			Output << "not a scene";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OtherMetadata = AssetMetadataSerializer::CreateForNewAsset(OtherAssetPath);
		PF_CHECK(Tests, OtherMetadata.has_value());

		const std::filesystem::path InvalidScenePath = ProjectRoot / "Assets" / "Scenes" / "broken.scene";
		{
			std::ofstream Output(InvalidScenePath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto InvalidSceneMetadata = AssetMetadataSerializer::CreateForNewAsset(InvalidScenePath);
		PF_CHECK(Tests, InvalidSceneMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (OtherMetadata)
		{
			const auto WrongType = SceneAssetService::Load(OtherMetadata->ID, ProjectRoot, Registry, SavedAgain);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == SceneAssetErrorCode::UnsupportedAssetType);
		}
		if (InvalidSceneMetadata)
		{
			const auto ExistingSavedEntity = SavedAgain.FindEntity(*MeshEntityID);
			const auto BrokenLoad = SceneAssetService::Load(InvalidSceneMetadata->ID, ProjectRoot, Registry, SavedAgain);
			PF_CHECK(Tests, !BrokenLoad && BrokenLoad.error().Code == SceneAssetErrorCode::SerializationFailed);
			PF_CHECK(Tests, ExistingSavedEntity && ExistingSavedEntity->IsValid());
		}

		const auto MissingID = UUID::Generate();
		PF_CHECK(Tests, MissingID.has_value());
		if (MissingID)
		{
			const auto Missing = SceneAssetService::Load(*MissingID, ProjectRoot, Registry, SavedAgain);
			PF_CHECK(Tests, !Missing && Missing.error().Code == SceneAssetErrorCode::AssetNotFound);
		}
	}

	void TestAssetMetadataAndRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAssetRegistry-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path ShaderDirectory = ProjectRoot / "Assets" / "Shaders";
		const std::filesystem::path ModelDirectory = ProjectRoot / "Assets" / "Models";
		std::filesystem::create_directories(ShaderDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::create_directories(ModelDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto WriteFile = [](const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output)
				return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		};
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::nullopt;
			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::nullopt;
			return Data;
		};
		const auto HasIssue = [](const AssetRegistryError& Error, AssetRegistryIssueCode Code)
		{
			return std::find_if(Error.Issues.begin(), Error.Issues.end(), [Code](const AssetRegistryIssue& Issue)
			{
				return Issue.Code == Code;
			}) != Error.Issues.end();
		};

		const std::filesystem::path FirstSource = ShaderDirectory / "first.hlsl";
		const std::filesystem::path SecondSource = ShaderDirectory / "second.hlsl";
		const std::filesystem::path DuplicateSource = ModelDirectory / "duplicate.hlsl";
		PF_CHECK(Tests, WriteFile(FirstSource, "first shader source"));
		PF_CHECK(Tests, WriteFile(SecondSource, "second shader source"));
		std::filesystem::copy_file(FirstSource, DuplicateSource, std::filesystem::copy_options::none, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto FirstMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstSource);
		const auto SecondMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		const auto DuplicateMetadata = AssetMetadataSerializer::CreateForNewAsset(DuplicateSource);
		PF_CHECK(Tests, FirstMetadata.has_value());
		PF_CHECK(Tests, SecondMetadata.has_value());
		PF_CHECK(Tests, DuplicateMetadata.has_value());
		if (!FirstMetadata || !SecondMetadata || !DuplicateMetadata)
			return;

		PF_CHECK(Tests, FirstMetadata->Version == AssetMetadataSerializer::CurrentVersion);
		PF_CHECK(Tests, FirstMetadata->ID != SecondMetadata->ID);
		PF_CHECK(Tests, FirstMetadata->ID != DuplicateMetadata->ID);
		PF_CHECK(Tests, AssetMetadataSerializer::GetSidecarPath(FirstSource).filename() == "first.hlsl.meta");
		const auto LoadedFirstMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(FirstSource));
		PF_CHECK(Tests, LoadedFirstMetadata.has_value());
		PF_CHECK(Tests, LoadedFirstMetadata && LoadedFirstMetadata->ID == FirstMetadata->ID);
		const auto RecreateExistingMetadata = AssetMetadataSerializer::CreateForNewAsset(FirstSource);
		PF_CHECK(Tests, !RecreateExistingMetadata.has_value());
		PF_CHECK(Tests, !RecreateExistingMetadata &&
			RecreateExistingMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		const auto ExistingIdentity = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(FirstSource));
		PF_CHECK(Tests, ExistingIdentity && ExistingIdentity->ID == FirstMetadata->ID);

		AssetRegistry Registry;
		const auto InitialBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, InitialBuild.has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 3);
		const std::vector<AssetRecord> RegisteredAssets = Registry.GetAssets();
		PF_CHECK(Tests, RegisteredAssets.size() == 3);
		if (RegisteredAssets.size() != 3)
			return;
		PF_CHECK(Tests, RegisteredAssets[0].ProjectRelativePath == std::filesystem::path("Assets/Models/duplicate.hlsl"));
		PF_CHECK(Tests, RegisteredAssets[1].ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		PF_CHECK(Tests, RegisteredAssets[2].ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
		const auto FirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, FirstRecord.has_value());
		if (!InitialBuild || !FirstRecord)
			return;
		PF_CHECK(Tests, FirstRecord && FirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		PF_CHECK(Tests, !Registry.Find(UUID{}).has_value());

		AssetRegistry ReconstructedRegistry;
		PF_CHECK(Tests, ReconstructedRegistry.Rebuild(ProjectRoot).has_value());
		const auto ReconstructedFirst = ReconstructedRegistry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, ReconstructedFirst && ReconstructedFirst->ProjectRelativePath == FirstRecord->ProjectRelativePath);

		const std::filesystem::path RenamedSource = ModelDirectory / "renamed.hlsl";
		const std::filesystem::path FirstSidecar = AssetMetadataSerializer::GetSidecarPath(FirstSource);
		const std::filesystem::path RenamedSidecar = AssetMetadataSerializer::GetSidecarPath(RenamedSource);
		std::filesystem::rename(FirstSource, RenamedSource, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		std::filesystem::rename(FirstSidecar, RenamedSidecar, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		const auto StaleFirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, StaleFirstRecord &&
			StaleFirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Shaders/first.hlsl"));
		const auto RebuiltAfterMove = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, RebuiltAfterMove.has_value());
		const auto MovedRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
		const std::vector<AssetRecord> AssetsAfterMove = Registry.GetAssets();
		const auto MovedRecordInListing = std::find_if(AssetsAfterMove.begin(), AssetsAfterMove.end(),
			[&FirstMetadata](const AssetRecord& Asset) { return Asset.ID == FirstMetadata->ID; });
		PF_CHECK(Tests, MovedRecordInListing != AssetsAfterMove.end() &&
			MovedRecordInListing->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));

		const std::filesystem::path SecondSidecar = AssetMetadataSerializer::GetSidecarPath(SecondSource);
		const auto OriginalSecondSidecar = ReadFile(SecondSidecar);
		PF_CHECK(Tests, OriginalSecondSidecar.has_value());
		if (!OriginalSecondSidecar || !MovedRecord)
			return;
		PF_CHECK(Tests, OriginalSecondSidecar->find("\"importSettings\": {}") != std::string::npos);

		std::filesystem::remove(SecondSidecar, FileError);
		PF_CHECK(Tests, !FileError);
		const auto MissingMetadataBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !MissingMetadataBuild.has_value());
		PF_CHECK(Tests, !MissingMetadataBuild.has_value() &&
			HasIssue(MissingMetadataBuild.error(), AssetRegistryIssueCode::MissingMetadata));
		if (!MissingMetadataBuild)
		{
			const auto MissingIssue = std::find_if(
				MissingMetadataBuild.error().Issues.begin(),
				MissingMetadataBuild.error().Issues.end(),
				[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::MissingMetadata; });
			PF_CHECK(Tests, MissingIssue != MissingMetadataBuild.error().Issues.end());
			if (MissingIssue != MissingMetadataBuild.error().Issues.end())
			{
				PF_CHECK(Tests, MissingIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
				PF_CHECK(Tests, !MissingIssue->Message.empty());
			}
		}
		PF_CHECK(Tests, !std::filesystem::exists(SecondSidecar));
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		PF_CHECK(Tests, WriteFile(SecondSidecar, "{ malformed metadata"));
		const auto CorruptMetadataLoad = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !CorruptMetadataLoad.has_value());
		const auto CorruptSidecarBeforeCreate = ReadFile(SecondSidecar);
		const auto RecreateCorruptMetadata = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		PF_CHECK(Tests, !RecreateCorruptMetadata.has_value());
		PF_CHECK(Tests, !RecreateCorruptMetadata &&
			RecreateCorruptMetadata.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		PF_CHECK(Tests, CorruptSidecarBeforeCreate && ReadFile(SecondSidecar) == CorruptSidecarBeforeCreate);
		const auto CorruptMetadataBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !CorruptMetadataBuild.has_value());
		PF_CHECK(Tests, !CorruptMetadataBuild.has_value() &&
			HasIssue(CorruptMetadataBuild.error(), AssetRegistryIssueCode::InvalidMetadata));
		if (!CorruptMetadataBuild)
		{
			const auto InvalidIssue = std::find_if(
				CorruptMetadataBuild.error().Issues.begin(),
				CorruptMetadataBuild.error().Issues.end(),
				[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::InvalidMetadata; });
			PF_CHECK(Tests, InvalidIssue != CorruptMetadataBuild.error().Issues.end());
			if (InvalidIssue != CorruptMetadataBuild.error().Issues.end())
				PF_CHECK(Tests, InvalidIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
		}
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		const std::string UnsupportedVersion = "{\"format\":\"PulseForgeAssetMeta\",\"version\":2,\"uuid\":\"" +
			SecondMetadata->ID.ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, WriteFile(SecondSidecar, UnsupportedVersion));
		const auto UnsupportedMetadataVersion = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !UnsupportedMetadataVersion.has_value());
		PF_CHECK(Tests, !UnsupportedMetadataVersion &&
			UnsupportedMetadataVersion.error().Code == AssetMetadataErrorCode::UnsupportedVersion);
		std::string InvalidImportSettings = *OriginalSecondSidecar;
		const size_t ImportSettingsObject = InvalidImportSettings.find("\"importSettings\": {}");
		PF_CHECK(Tests, ImportSettingsObject != std::string::npos);
		if (ImportSettingsObject != std::string::npos)
			InvalidImportSettings.replace(ImportSettingsObject, std::string("\"importSettings\": {}").size(), "\"importSettings\": []");
		PF_CHECK(Tests, WriteFile(SecondSidecar, InvalidImportSettings));
		const auto InvalidImportSettingsLoad = AssetMetadataSerializer::LoadFromFile(SecondSidecar);
		PF_CHECK(Tests, !InvalidImportSettingsLoad.has_value());
		PF_CHECK(Tests, !InvalidImportSettingsLoad &&
			InvalidImportSettingsLoad.error().Code == AssetMetadataErrorCode::InvalidDocument);
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));

		std::filesystem::copy_file(
			RenamedSidecar,
			SecondSidecar,
			std::filesystem::copy_options::overwrite_existing,
			FileError);
		PF_CHECK(Tests, !FileError);
		const auto DuplicateIdentityBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !DuplicateIdentityBuild.has_value());
		PF_CHECK(Tests, !DuplicateIdentityBuild.has_value() &&
			HasIssue(DuplicateIdentityBuild.error(), AssetRegistryIssueCode::DuplicateAssetID));
		if (!DuplicateIdentityBuild)
		{
			const auto DuplicateIssue = std::find_if(
				DuplicateIdentityBuild.error().Issues.begin(),
				DuplicateIdentityBuild.error().Issues.end(),
				[](const AssetRegistryIssue& Issue) { return Issue.Code == AssetRegistryIssueCode::DuplicateAssetID; });
			PF_CHECK(Tests, DuplicateIssue != DuplicateIdentityBuild.error().Issues.end());
			if (DuplicateIssue != DuplicateIdentityBuild.error().Issues.end())
			{
				PF_CHECK(Tests, DuplicateIssue->ProjectRelativePath == std::filesystem::path("Assets/Shaders/second.hlsl"));
				PF_CHECK(Tests, DuplicateIssue->RelatedProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
				PF_CHECK(Tests, DuplicateIssue->Message.find(FirstMetadata->ID.ToString()) != std::string::npos);
			}
		}
		PF_CHECK(Tests, Registry.Find(SecondMetadata->ID).has_value());
		const auto PreservedFirstRecord = Registry.Find(FirstMetadata->ID);
		PF_CHECK(Tests, PreservedFirstRecord &&
			PreservedFirstRecord->ProjectRelativePath == std::filesystem::path("Assets/Models/renamed.hlsl"));
		const auto DuplicateCreation = AssetMetadataSerializer::CreateForNewAsset(SecondSource);
		PF_CHECK(Tests, !DuplicateCreation.has_value());
		PF_CHECK(Tests, !DuplicateCreation && DuplicateCreation.error().Code == AssetMetadataErrorCode::MetadataAlreadyExists);
		PF_CHECK(Tests, WriteFile(SecondSidecar, *OriginalSecondSidecar));
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());

		std::filesystem::remove(DuplicateSource, FileError);
		PF_CHECK(Tests, !FileError);
		const auto OrphanedSidecarBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !OrphanedSidecarBuild.has_value());
		PF_CHECK(Tests, !OrphanedSidecarBuild.has_value() &&
			HasIssue(OrphanedSidecarBuild.error(), AssetRegistryIssueCode::MissingSourceAsset));
		PF_CHECK(Tests, Registry.Find(DuplicateMetadata->ID).has_value());

		const std::filesystem::path ReservedSuffixSource = ShaderDirectory / "reserved-suffix";
		PF_CHECK(Tests, WriteFile(ReservedSuffixSource, "regular source asset"));
		const auto ReservedSuffixMetadata = AssetMetadataSerializer::CreateForNewAsset(ReservedSuffixSource);
		PF_CHECK(Tests, ReservedSuffixMetadata.has_value());
		const auto NestedSidecarID = UUID::Generate();
		PF_CHECK(Tests, NestedSidecarID.has_value());
		if (!ReservedSuffixMetadata || !NestedSidecarID)
			return;

		const std::filesystem::path NestedSidecarPath =
			AssetMetadataSerializer::GetSidecarPath(AssetMetadataSerializer::GetSidecarPath(ReservedSuffixSource));
		const std::string NestedSidecar = "{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
			NestedSidecarID->ToString() + "\",\"importSettings\":{}}";
		PF_CHECK(Tests, WriteFile(NestedSidecarPath, NestedSidecar));
		const auto ReservedSuffixBuild = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, !ReservedSuffixBuild.has_value());
		PF_CHECK(Tests, !ReservedSuffixBuild.has_value() &&
			HasIssue(ReservedSuffixBuild.error(), AssetRegistryIssueCode::UnsupportedEntry));
	}

	void TestAssetOperations(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAssetOperations-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path AssetDirectory = ProjectRoot / "Assets" / "Models";
		std::filesystem::create_directories(AssetDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto WriteFile = [](const std::filesystem::path& Path, std::string_view Data)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output)
				return false;
			Output.write(Data.data(), static_cast<std::streamsize>(Data.size()));
			Output.close();
			return static_cast<bool>(Output);
		};
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::nullopt;
			std::string Data{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::nullopt;
			return Data;
		};

		const std::filesystem::path Source = AssetDirectory / "ship.gltf";
		PF_CHECK(Tests, WriteFile(Source, "managed model source"));
		const auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(Source);
		PF_CHECK(Tests, SourceMetadata.has_value());
		if (!SourceMetadata)
			return;
		const std::filesystem::path SourceSidecar = AssetMetadataSerializer::GetSidecarPath(Source);
		const std::string MetadataWithImportSettings =
			"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
			SourceMetadata->ID.ToString() + "\",\"importSettings\":{\"scale\":2.5}}";
		PF_CHECK(Tests, WriteFile(SourceSidecar, MetadataWithImportSettings));

		AssetRegistry InitialRegistry;
		PF_CHECK(Tests, InitialRegistry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);

		const auto UnknownAsset = UUID::Generate();
		PF_CHECK(Tests, UnknownAsset.has_value());
		if (!UnknownAsset)
			return;
		const auto UnknownMove = AssetOperations::Move(
			InitialRegistry,
			ProjectRoot,
			*UnknownAsset,
			"Assets/Models/not-found-move.gltf");
		PF_CHECK(Tests, !UnknownMove && UnknownMove.error().Code == AssetOperationErrorCode::AssetNotFound);
		PF_CHECK(Tests, !UnknownMove && UnknownMove.error().Message.find(UnknownAsset->ToString()) != std::string::npos);
		const auto UnknownDuplicate = AssetOperations::Duplicate(
			InitialRegistry,
			ProjectRoot,
			*UnknownAsset,
			"Assets/Models/not-found-duplicate.gltf");
		PF_CHECK(Tests, !UnknownDuplicate && UnknownDuplicate.error().Code == AssetOperationErrorCode::AssetNotFound);
		const auto UnknownDelete = AssetOperations::Delete(InitialRegistry, ProjectRoot, *UnknownAsset);
		PF_CHECK(Tests, !UnknownDelete && UnknownDelete.error().Code == AssetOperationErrorCode::AssetNotFound);
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);
		PF_CHECK(Tests, std::filesystem::exists(Source) && std::filesystem::exists(SourceSidecar));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/not-found-move.gltf"));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/not-found-duplicate.gltf"));

		const std::filesystem::path MovedPath = "Assets/Models/ship-renamed.gltf";
		const auto MoveResult = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, MovedPath);
		PF_CHECK(Tests, MoveResult.has_value());
		PF_CHECK(Tests, MoveResult && MoveResult->ID == SourceMetadata->ID);
		PF_CHECK(Tests, MoveResult && MoveResult->ProjectRelativePath == MovedPath);
		const std::filesystem::path MovedSource = ProjectRoot / MovedPath;
		PF_CHECK(Tests, !std::filesystem::exists(Source));
		PF_CHECK(Tests, !std::filesystem::exists(SourceSidecar));
		PF_CHECK(Tests, ReadFile(MovedSource) == std::optional<std::string>("managed model source"));
		const std::filesystem::path MovedSidecarPath = AssetMetadataSerializer::GetSidecarPath(MovedSource);
		PF_CHECK(Tests, ReadFile(MovedSidecarPath) == std::optional<std::string>(MetadataWithImportSettings));
		const auto MovedMetadata = AssetMetadataSerializer::LoadFromFile(MovedSidecarPath);
		PF_CHECK(Tests, MovedMetadata && MovedMetadata->ID == SourceMetadata->ID);

		const auto MovedRecord = InitialRegistry.Find(SourceMetadata->ID);
		PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == MovedPath);

		const std::filesystem::path DuplicatePath = "Assets/Models/ship-copy.gltf";
		const auto DuplicateResult = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, DuplicateResult.has_value());
		PF_CHECK(Tests, DuplicateResult && DuplicateResult->ID != SourceMetadata->ID);
		PF_CHECK(Tests, DuplicateResult && DuplicateResult->ProjectRelativePath == DuplicatePath);
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 2);
		const std::filesystem::path DuplicateSource = ProjectRoot / DuplicatePath;
		PF_CHECK(Tests, ReadFile(DuplicateSource) == std::optional<std::string>("managed model source"));
		const auto DuplicateSidecar = ReadFile(AssetMetadataSerializer::GetSidecarPath(DuplicateSource));
		PF_CHECK(Tests, DuplicateSidecar && DuplicateSidecar->find("\"scale\": 2.5") != std::string::npos);
		const auto DuplicateMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(DuplicateSource));
		PF_CHECK(Tests, DuplicateMetadata && DuplicateMetadata->ID == DuplicateResult->ID);

		const auto ExistingDestination = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, !ExistingDestination.has_value());
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		const auto ExistingMoveDestination = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, DuplicatePath);
		PF_CHECK(Tests, !ExistingMoveDestination.has_value());
		PF_CHECK(Tests, !ExistingMoveDestination &&
			ExistingMoveDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, std::filesystem::exists(MovedSource));
		PF_CHECK(Tests, std::filesystem::exists(DuplicateSource));
		const auto InvalidTraversal = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, "Assets/../escaped.gltf");
		PF_CHECK(Tests, !InvalidTraversal.has_value());
		PF_CHECK(Tests, !InvalidTraversal && InvalidTraversal.error().Code == AssetOperationErrorCode::InvalidPath);

		const std::filesystem::path CaseChangedPath = "Assets/Models/SHIP-RENAMED.gltf";
		const auto CaseChangedMove = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, CaseChangedPath);
		PF_CHECK(Tests, CaseChangedMove && CaseChangedMove->ID == SourceMetadata->ID);
		PF_CHECK(Tests, CaseChangedMove && CaseChangedMove->ProjectRelativePath == CaseChangedPath);
		const std::filesystem::path CaseChangedSource = ProjectRoot / CaseChangedPath;
		PF_CHECK(Tests, std::filesystem::exists(CaseChangedSource));
		PF_CHECK(Tests, ReadFile(CaseChangedSource) == std::optional<std::string>("managed model source"));
		PF_CHECK(Tests, ReadFile(AssetMetadataSerializer::GetSidecarPath(CaseChangedSource)) ==
			std::optional<std::string>(MetadataWithImportSettings));
		const auto CaseChangedRecord = InitialRegistry.Find(SourceMetadata->ID);
		PF_CHECK(Tests, CaseChangedRecord && CaseChangedRecord->ProjectRelativePath == CaseChangedPath);

		const auto CaseRestoredMove = AssetOperations::Move(InitialRegistry, ProjectRoot, SourceMetadata->ID, MovedPath);
		PF_CHECK(Tests, CaseRestoredMove && CaseRestoredMove->ID == SourceMetadata->ID);
		PF_CHECK(Tests, CaseRestoredMove && CaseRestoredMove->ProjectRelativePath == MovedPath);
		PF_CHECK(Tests, ReadFile(MovedSource) == std::optional<std::string>("managed model source"));
		PF_CHECK(Tests, ReadFile(MovedSidecarPath) == std::optional<std::string>(MetadataWithImportSettings));

		PF_CHECK(Tests, DuplicateMetadata && AssetOperations::Delete(InitialRegistry, ProjectRoot, DuplicateMetadata->ID).has_value());
		PF_CHECK(Tests, !std::filesystem::exists(DuplicateSource));
		PF_CHECK(Tests, !std::filesystem::exists(AssetMetadataSerializer::GetSidecarPath(DuplicateSource)));
		PF_CHECK(Tests, InitialRegistry.GetAssetCount() == 1);
		PF_CHECK(Tests, !InitialRegistry.Find(DuplicateMetadata->ID).has_value());
		const bool TransactionDirectoryEmpty = std::filesystem::is_empty(
			ProjectRoot / ".pulseforge" / "cache" / "asset-operations", FileError);
		PF_CHECK(Tests, !FileError && TransactionDirectoryEmpty);

		const std::filesystem::path AliasedSource = AssetDirectory / "aliased.gltf";
		PF_CHECK(Tests, WriteFile(AliasedSource, "duplicate identity source"));
		PF_CHECK(Tests, WriteFile(
			AssetMetadataSerializer::GetSidecarPath(AliasedSource),
			"{\"format\":\"PulseForgeAssetMeta\",\"version\":1,\"uuid\":\"" +
				SourceMetadata->ID.ToString() + "\",\"importSettings\":{}}"));
		const std::filesystem::path CollisionCopyPath = "Assets/Models/collision-copy.gltf";
		const auto DuplicateInInvalidProject = AssetOperations::Duplicate(
			InitialRegistry, ProjectRoot, SourceMetadata->ID, CollisionCopyPath);
		PF_CHECK(Tests, !DuplicateInInvalidProject.has_value());
		PF_CHECK(Tests, !DuplicateInInvalidProject &&
			DuplicateInInvalidProject.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / CollisionCopyPath));
		std::filesystem::remove(AliasedSource, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(AliasedSource), FileError);
		PF_CHECK(Tests, !FileError);

		std::filesystem::remove(AssetMetadataSerializer::GetSidecarPath(MovedSource), FileError);
		PF_CHECK(Tests, !FileError);
		const auto MissingMetadataOperation = AssetOperations::Duplicate(
			InitialRegistry, ProjectRoot, SourceMetadata->ID, "Assets/Models/untracked.gltf");
		PF_CHECK(Tests, !MissingMetadataOperation.has_value());
		PF_CHECK(Tests, !MissingMetadataOperation &&
			MissingMetadataOperation.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/untracked.gltf"));
	}

	void TestAssetFolderOperations(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code Error;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = std::filesystem::temp_directory_path() /
			("PulseForgeAssetFolderOperations-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code CleanupError;
				std::filesystem::remove_all(Path, CleanupError);
			}
		} Cleanup{ ProjectRoot };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Source" / "Nested", Error);
		std::filesystem::create_directories(ProjectRoot / "Assets" / "Destination", Error);
		PF_CHECK(Tests, !Error);
		if (Error)
			return;

		const auto WriteManagedAsset = [](const std::filesystem::path& Path, std::string_view Contents)
			-> std::optional<AssetID>
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output)
				return std::nullopt;
			Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
			Output.close();
			if (!Output)
				return std::nullopt;
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? std::optional<AssetID>(Metadata->ID) : std::nullopt;
		};

		const auto RootAsset = WriteManagedAsset(ProjectRoot / "Assets" / "Source" / "root.material", "root");
		const auto NestedAsset = WriteManagedAsset(
			ProjectRoot / "Assets" / "Source" / "Nested" / "nested.png", "nested");
		PF_CHECK(Tests, RootAsset.has_value() && NestedAsset.has_value());
		if (!RootAsset || !NestedAsset)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 2);

		const auto SelfMove = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/Source");
		PF_CHECK(Tests, !SelfMove && SelfMove.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto DescendantMove = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/Source/Nested");
		PF_CHECK(Tests, !DescendantMove && DescendantMove.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto TraversalMove = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/../Destination");
		PF_CHECK(Tests, !TraversalMove && TraversalMove.error().Code == AssetOperationErrorCode::InvalidPath);
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Source" / "root.material"));

		const auto Moved = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Source", "Assets/Destination");
		PF_CHECK(Tests, Moved.has_value());
		PF_CHECK(Tests, Moved && *Moved == std::filesystem::path("Assets/Destination/Source"));
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Source"));
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Destination" / "Source" / "Nested" / "nested.png"));
		const auto MovedRootRecord = Registry.Find(*RootAsset);
		const auto MovedNestedRecord = Registry.Find(*NestedAsset);
		PF_CHECK(Tests, MovedRootRecord && MovedRootRecord->ProjectRelativePath ==
			std::filesystem::path("Assets/Destination/Source/root.material"));
		PF_CHECK(Tests, MovedNestedRecord && MovedNestedRecord->ProjectRelativePath ==
			std::filesystem::path("Assets/Destination/Source/Nested/nested.png"));
		PF_CHECK(Tests, MovedRootRecord && MovedRootRecord->ID == *RootAsset);
		PF_CHECK(Tests, MovedNestedRecord && MovedNestedRecord->ID == *NestedAsset);

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Other" / "Source", Error);
		PF_CHECK(Tests, !Error);
		const auto Collision = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Other/Source", "Assets/Destination");
		PF_CHECK(Tests, !Collision && Collision.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, std::filesystem::exists(ProjectRoot / "Assets" / "Destination" / "Source" / "root.material"));

		const auto EmptySource = AssetOperations::MoveFolder(
			Registry, ProjectRoot, "Assets/Destination/Source", "Assets");
		PF_CHECK(Tests, EmptySource.has_value());
		PF_CHECK(Tests, EmptySource && *EmptySource == std::filesystem::path("Assets/Source"));
		const auto ReturnedToRoot = Registry.Find(*RootAsset);
		PF_CHECK(Tests, ReturnedToRoot && ReturnedToRoot->ProjectRelativePath ==
			std::filesystem::path("Assets/Source/root.material"));

		const std::filesystem::path LinkPath = ProjectRoot / "Assets" / "Linked";
		Error.clear();
		std::filesystem::create_directory_symlink(ProjectRoot / "Assets" / "Destination", LinkPath, Error);
		if (!Error)
		{
			const auto ReparseTarget = AssetOperations::MoveFolder(
				Registry, ProjectRoot, "Assets/Source", "Assets/Linked");
			PF_CHECK(Tests, !ReparseTarget && ReparseTarget.error().Code == AssetOperationErrorCode::InvalidPath);
			std::filesystem::remove(LinkPath, Error);
			PF_CHECK(Tests, !Error);
		}
	}

	void TestEditorDocumentManager(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto OtherProject = UUID::Generate();
		const auto Material = UUID::Generate();
		const auto OtherMaterial = UUID::Generate();
		const auto Scene = UUID::Generate();
		PF_CHECK(Tests, Project && OtherProject && Material && OtherMaterial && Scene);
		if (!Project || !OtherProject || !Material || !OtherMaterial || !Scene)
			return;

		EditorDocumentManager Manager;
		PF_CHECK(Tests, ShouldProcessNativeDocumentHostCloseRequests(false));
		PF_CHECK(Tests, !ShouldProcessNativeDocumentHostCloseRequests(true));
		const std::vector<int> BatchDocuments{ 1, 2, 3, 4 };
		const auto DirtyBatch = FilterDirtyDocumentIdentities(BatchDocuments,
			[](int Document) { return Document == 2 || Document == 4; });
		PF_CHECK(Tests, DirtyBatch.size() == 2 && DirtyBatch[0] == 2 && DirtyBatch[1] == 4);
		const auto CleanBatch = FilterDirtyDocumentIdentities(BatchDocuments,
			[](int) { return false; });
		PF_CHECK(Tests, CleanBatch.empty());
		// Tab capture must start from a hovered press, even when ImGui does not activate the item.
		PF_CHECK(Tests, ShouldCaptureDocumentTabDrag(true, false, true));
		PF_CHECK(Tests, !ShouldCaptureDocumentTabDrag(false, false, true));
		PF_CHECK(Tests, !ShouldCaptureDocumentTabDrag(true, true, true));
		PF_CHECK(Tests, !ShouldCaptureDocumentTabDrag(true, false, false));
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			false, true, true) == DocumentTabReleaseAction::TransferToStrip);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::TransferToStrip);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, true, false) == DocumentTabReleaseAction::CreateNativeHost);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			false, true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, false, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 100.0f, 6.0f,
			true, false, true) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, true, 100.0f, 6.0f,
			true, true, false) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(false, true, false, 100.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, false, false, 100.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::None);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, 35.0f, 6.0f,
			true, true, true) == DocumentTabReleaseAction::None);
		const DocumentTabPointerPosition DragStart{ 100.0f, 100.0f };
		const DocumentTabPointerPosition LastPointer{ 117.0f, 112.0f };
		const std::optional<DocumentTabPointerPosition> NoPointer;
		const std::optional<DocumentTabPointerPosition> LastValidPointer = LastPointer;
		PF_CHECK(Tests, IsUsableDocumentTabPointerPosition(LastPointer.X, LastPointer.Y));
		PF_CHECK(Tests, !IsUsableDocumentTabPointerPosition(-100001.0f, -100001.0f));
		const auto ReleasePointer = ResolveDocumentTabReleasePosition(
			-100001.0f, -100001.0f, LastValidPointer);
		PF_CHECK(Tests, ReleasePointer && ReleasePointer->X == LastPointer.X && ReleasePointer->Y == LastPointer.Y);
		PF_CHECK(Tests, !ResolveDocumentTabReleasePosition(-100001.0f, -100001.0f, NoPointer));
		const float LastObservedDragDistance = CalculateDocumentTabDragDistanceSquared(
			DragStart, NoPointer, LastValidPointer, 0.0f);
		PF_CHECK(Tests, LastObservedDragDistance == 433.0f);
		PF_CHECK(Tests, CalculateDocumentTabDragDistanceSquared(
			DragStart, NoPointer, LastValidPointer, 900.0f) == 900.0f);
		PF_CHECK(Tests, ResolveDocumentTabReleaseAction(true, true, false, LastObservedDragDistance, 6.0f,
			true, true, false) == DocumentTabReleaseAction::CreateNativeHost);
		const EditorDocumentIdentity MaterialA{ *Project, *Material, EditorDocumentKind::Material };
		const EditorDocumentIdentity MaterialB{ *Project, *OtherMaterial, EditorDocumentKind::Material };
		const EditorDocumentIdentity SameNameOtherFolder{ *Project, *Scene, EditorDocumentKind::Material };
		const EditorDocumentIdentity OtherProjectMaterial{ *OtherProject, *Material, EditorDocumentKind::Material };
		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const auto First = Manager.Open(MaterialA, "Assets/Materials/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		const auto Second = Manager.Open(MaterialB, "Assets/Materials/Blue.material", "Blue.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, First && Second && First->Created && Second->Created);
		PF_CHECK(Tests, First && Second && First->HostID != Second->HostID);
		PF_CHECK(Tests, Manager.GetHosts().size() == 3);

		const auto Reopened = Manager.Open(MaterialA, "Assets/OtherFolder/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, Reopened && !Reopened->Created);
		PF_CHECK(Tests, Reopened && Reopened->HostID == First->HostID);
		PF_CHECK(Tests, Manager.GetSessions().size() == 2);
		PF_CHECK(Tests, Manager.Find(MaterialA)->ProjectRelativePath ==
			std::filesystem::path("Assets/OtherFolder/Amber.material"));
		PF_CHECK(Tests, Manager.GetFocusedHost() && Manager.GetFocusedHost()->ID == First->HostID);
		const auto* FocusedMaterialHost = Manager.GetFocusedHost();
		const auto* FocusedMaterialIdentity = FocusedMaterialHost && FocusedMaterialHost->ActiveDocument
			? &*FocusedMaterialHost->ActiveDocument : nullptr;
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(FocusedMaterialIdentity, *Project) ==
			EditorDocumentCommandTarget::Material);
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(nullptr, *Project) == EditorDocumentCommandTarget::Level);
		const EditorDocumentIdentity TextureCommandDocument{ *Project, *Material, EditorDocumentKind::Texture };
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(&TextureCommandDocument, *Project) ==
			EditorDocumentCommandTarget::Unsupported);
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(&OtherProjectMaterial, *Project) ==
			EditorDocumentCommandTarget::Unsupported);
		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t LocationUpdateRevision = Manager.Find(MaterialA)->Revision;
		const bool LocationUpdated = Manager.UpdateAssetLocation(
			MaterialA, "Assets/Renamed/Amber-Renamed.material", "Amber-Renamed.material");
		PF_CHECK(Tests, LocationUpdated);
		PF_CHECK(Tests, Manager.Find(MaterialA)->ProjectRelativePath ==
			std::filesystem::path("Assets/Renamed/Amber-Renamed.material"));
		PF_CHECK(Tests, Manager.Find(MaterialA)->DisplayName == "Amber-Renamed.material");
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == LocationUpdateRevision && Manager.Find(MaterialA)->IsDirty());

		const auto OtherFolderOpen = Manager.Open(SameNameOtherFolder, "Assets/Sub/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::MainDocumentStrip);
		const auto OtherProjectOpen = Manager.Open(OtherProjectMaterial, "Assets/Materials/Amber.material", "Amber.material",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, OtherFolderOpen && OtherProjectOpen);
		PF_CHECK(Tests, Manager.GetSessions().size() == 4);
		PF_CHECK(Tests, Manager.Find(MaterialA) != Manager.Find(SameNameOtherFolder));
		PF_CHECK(Tests, Manager.Find(MaterialA) != Manager.Find(OtherProjectMaterial));
		PF_CHECK(Tests, Manager.FindHost(EditorDocumentManager::MainHostID)->Documents.size() == 1);
		PF_CHECK(Tests, OtherProjectOpen && OtherProjectOpen->HostID != OtherFolderOpen->HostID);
		PF_CHECK(Tests, !Manager.Transfer(MaterialA, OtherProjectOpen->HostID, 0));
		if (OtherProjectOpen)
		{
			const auto RejectedRecovery = Manager.TransferAll(
				OtherProjectOpen->HostID, EditorDocumentManager::MainHostID);
			PF_CHECK(Tests, !RejectedRecovery);
			PF_CHECK(Tests, Manager.Find(OtherProjectMaterial)->HostID == OtherProjectOpen->HostID);
			const auto* RejectedRecoveryHost = Manager.FindHost(OtherProjectOpen->HostID);
			PF_CHECK(Tests, RejectedRecoveryHost && RejectedRecoveryHost->Documents.size() == 1 &&
				RejectedRecoveryHost->Documents.front() == OtherProjectMaterial);
			PF_CHECK(Tests, Manager.FindHost(EditorDocumentManager::MainHostID)->Documents.size() == 1);
		}

		uint64_t TextureIdentity = 90;
		const auto ManagerPreferenceOpen = [&Manager, &Project, &TextureIdentity]()
		{
			const UUID PreferenceAsset(0, TextureIdentity++);
			return Manager.Open({ *Project, PreferenceAsset, EditorDocumentKind::Texture },
				"Assets/Textures/Albedo.png", "Albedo.png",
				EditorDocumentOpenPreference::RememberedPlacement);
		};
		const auto RememberedOne = ManagerPreferenceOpen();
		const auto RememberedTwo = ManagerPreferenceOpen();
		PF_CHECK(Tests, RememberedOne && RememberedTwo);
		PF_CHECK(Tests, RememberedOne && RememberedTwo && RememberedOne->HostID == RememberedTwo->HostID);
		const EditorDocumentIdentity OtherProjectTexture{ *OtherProject, *Scene, EditorDocumentKind::Texture };
		const auto OtherProjectTextureOpen = Manager.Open(OtherProjectTexture, "Assets/Textures/Normal.png", "Normal.png",
			EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, OtherProjectTextureOpen && OtherProjectTextureOpen->HostID != EditorDocumentManager::MainHostID);
		PF_CHECK(Tests, !Manager.Transfer(MaterialA, OtherProjectTextureOpen->HostID, 0));

		const auto RecoveryTextureAID = UUID::Generate();
		const auto RecoveryTextureBID = UUID::Generate();
		PF_CHECK(Tests, RecoveryTextureAID && RecoveryTextureBID);
		if (RecoveryTextureAID && RecoveryTextureBID)
		{
			const EditorDocumentIdentity RecoveryTextureA{ *Project, *RecoveryTextureAID, EditorDocumentKind::Texture };
			const EditorDocumentIdentity RecoveryTextureB{ *Project, *RecoveryTextureBID, EditorDocumentKind::Texture };
			const auto RecoveryAOpen = Manager.Open(RecoveryTextureA, "Assets/Textures/A.png", "A.png",
				EditorDocumentOpenPreference::NewNativeWindow);
			PF_CHECK(Tests, RecoveryAOpen.has_value());
			if (RecoveryAOpen)
			{
				PF_CHECK(Tests, Manager.SetRememberedHost(EditorDocumentKind::Texture, RecoveryAOpen->HostID));
				const auto RecoveryBOpen = Manager.Open(RecoveryTextureB, "Assets/Textures/B.png", "B.png",
					EditorDocumentOpenPreference::RememberedPlacement);
				PF_CHECK(Tests, RecoveryBOpen && RecoveryBOpen->HostID == RecoveryAOpen->HostID);
				if (RecoveryBOpen)
				{
					PF_CHECK(Tests, Manager.MarkEdited(RecoveryTextureA));
					PF_CHECK(Tests, Manager.MarkEdited(RecoveryTextureB));
					const uint64_t RevisionA = Manager.Find(RecoveryTextureA)->Revision;
					const uint64_t RevisionB = Manager.Find(RecoveryTextureB)->Revision;
					PF_CHECK(Tests, Manager.Activate(RecoveryTextureA));
					const auto RecoveredCount = Manager.TransferAll(
						RecoveryAOpen->HostID, EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, RecoveredCount && *RecoveredCount == 2);
					PF_CHECK(Tests, Manager.Find(RecoveryTextureA)->HostID == EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, Manager.Find(RecoveryTextureB)->HostID == EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, Manager.Find(RecoveryTextureA)->Revision == RevisionA &&
						Manager.Find(RecoveryTextureA)->IsDirty());
					PF_CHECK(Tests, Manager.Find(RecoveryTextureB)->Revision == RevisionB &&
						Manager.Find(RecoveryTextureB)->IsDirty());
					const auto* MainHost = Manager.FindHost(EditorDocumentManager::MainHostID);
					PF_CHECK(Tests, MainHost && MainHost->ActiveDocument == RecoveryTextureA);
					const auto* RecoveredSource = Manager.FindHost(RecoveryAOpen->HostID);
					PF_CHECK(Tests, RecoveredSource && RecoveredSource->Documents.empty());
					PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(RecoveryAOpen->HostID));
				}
			}
		}

		const auto DetachAsset = UUID::Generate();
		PF_CHECK(Tests, DetachAsset.has_value());
		if (!DetachAsset)
			return;
		const EditorDocumentIdentity DetachMaterial{ *Project, *DetachAsset, EditorDocumentKind::Material };
		const auto DetachOpen = Manager.Open(DetachMaterial, "Assets/Materials/Detach.material", "Detach.material",
			EditorDocumentOpenPreference::MainDocumentStrip);
		const auto NewMaterialHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
		PF_CHECK(Tests, DetachOpen && DetachOpen->HostID == EditorDocumentManager::MainHostID);
		PF_CHECK(Tests, NewMaterialHost.has_value());
		if (DetachOpen && NewMaterialHost)
		{
			PF_CHECK(Tests, Manager.MarkEdited(DetachMaterial));
			const uint64_t DetachRevision = Manager.Find(DetachMaterial)->Revision;
			const auto RejectedDetach = Manager.Transfer(DetachMaterial, *NewMaterialHost, 0,
				[](const EditorDocumentSession&, const EditorDocumentHost&) { return false; });
			PF_CHECK(Tests, !RejectedDetach);
			PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == EditorDocumentManager::MainHostID);
			PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision);
			PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(*NewMaterialHost));

			const auto PreparedHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
			PF_CHECK(Tests, PreparedHost.has_value());
			if (PreparedHost)
			{
				const auto Detached = Manager.Transfer(DetachMaterial, *PreparedHost, 0,
					[](const EditorDocumentSession& Session, const EditorDocumentHost& Host)
					{
						return Host.Role == EditorDocumentHostRole::Native &&
							Host.PlacementKind == Session.Identity.Kind;
					});
				PF_CHECK(Tests, Detached && Detached->Changed);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == *PreparedHost);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision &&
					Manager.Find(DetachMaterial)->IsDirty());
				const auto Reattached = Manager.Transfer(
					DetachMaterial, EditorDocumentManager::MainHostID, 0);
				PF_CHECK(Tests, Reattached && Reattached->Changed);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == EditorDocumentManager::MainHostID);
				PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision &&
					Manager.Find(DetachMaterial)->IsDirty());

				const auto Redetached = Manager.Transfer(DetachMaterial, *PreparedHost, 0);
				PF_CHECK(Tests, Redetached && Redetached->Changed);
				const auto TearOffHost = Manager.CreateNativeHost(EditorDocumentKind::Material);
				PF_CHECK(Tests, TearOffHost.has_value());
				if (TearOffHost)
				{
					const auto TearOff = Manager.Transfer(DetachMaterial, *TearOffHost, 0);
					PF_CHECK(Tests, TearOff && TearOff->EmptiedSourceHost == *PreparedHost);
					PF_CHECK(Tests, Manager.Find(DetachMaterial)->HostID == *TearOffHost);
					PF_CHECK(Tests, Manager.Find(DetachMaterial)->Revision == DetachRevision &&
						Manager.Find(DetachMaterial)->IsDirty());
					PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(*PreparedHost));
				}
			}
		}

		const auto LevelOpen = Manager.Open(Level, {}, "Level Editor", EditorDocumentOpenPreference::NewNativeWindow);
		PF_CHECK(Tests, LevelOpen && LevelOpen->HostID == EditorDocumentManager::MainHostID);
		const auto* FocusedLevelHost = Manager.GetFocusedHost();
		const auto* FocusedLevelIdentity = FocusedLevelHost && FocusedLevelHost->ActiveDocument
			? &*FocusedLevelHost->ActiveDocument : nullptr;
		PF_CHECK(Tests, ResolveEditorDocumentCommandTarget(FocusedLevelIdentity, *Project) ==
			EditorDocumentCommandTarget::Level);
		const auto ProtectedTransfer = Manager.Transfer(Level, First->HostID, 0);
		PF_CHECK(Tests, !ProtectedTransfer);

		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t DirtyRevision = Manager.Find(MaterialA)->Revision;
		const auto RejectedTransfer = Manager.Transfer(MaterialA, Second->HostID, 0,
			[](const EditorDocumentSession&, const EditorDocumentHost&) { return false; });
		PF_CHECK(Tests, !RejectedTransfer);
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == DirtyRevision);

		const auto Moved = Manager.Transfer(MaterialA, Second->HostID, 0,
			[](const EditorDocumentSession&, const EditorDocumentHost&) { return true; });
		PF_CHECK(Tests, Moved && Moved->Changed);
		PF_CHECK(Tests, Moved && Moved->EmptiedSourceHost == First->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->HostID == Second->HostID);
		PF_CHECK(Tests, Manager.Find(MaterialA)->Revision == DirtyRevision && Manager.Find(MaterialA)->IsDirty());
		PF_CHECK(Tests, Manager.RemoveEmptyNativeHost(First->HostID));
		PF_CHECK(Tests, !Manager.FindHost(First->HostID));
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->HostID == Second->HostID);

		PF_CHECK(Tests, Manager.MarkSaved(MaterialA, DirtyRevision));
		PF_CHECK(Tests, !Manager.Find(MaterialA)->IsDirty());
		PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
		const uint64_t LaterRevision = Manager.Find(MaterialA)->Revision;
		const auto Cancelled = Manager.Close(MaterialA, EditorDocumentCloseChoice::Cancel, {}, {});
		PF_CHECK(Tests, Cancelled == EditorDocumentCloseStatus::Cancelled);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->Revision == LaterRevision);
		const auto SaveFailed = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[](const EditorDocumentSession&, uint64_t) { return false; }, {});
		PF_CHECK(Tests, SaveFailed == EditorDocumentCloseStatus::SaveFailed);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->IsDirty());
		const auto SavedOlderRevision = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[&Tests, &Manager, &MaterialA](const EditorDocumentSession&, uint64_t Revision)
			{
				PF_CHECK(Tests, Revision == Manager.Find(MaterialA)->Revision);
				PF_CHECK(Tests, Manager.MarkEdited(MaterialA));
				return true;
			}, {});
		PF_CHECK(Tests, SavedOlderRevision == EditorDocumentCloseStatus::StillDirty);
		PF_CHECK(Tests, Manager.Find(MaterialA) && Manager.Find(MaterialA)->IsDirty());
		const auto Saved = Manager.Close(MaterialA, EditorDocumentCloseChoice::Save,
			[](const EditorDocumentSession&, uint64_t) { return true; }, {});
		PF_CHECK(Tests, Saved == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, !Manager.Find(MaterialA));

		PF_CHECK(Tests, Manager.MarkEdited(MaterialB));
		const auto Discarded = Manager.Close(MaterialB, EditorDocumentCloseChoice::Discard, {},
			[](const EditorDocumentSession&) { return true; });
		PF_CHECK(Tests, Discarded == EditorDocumentCloseStatus::Closed);
		PF_CHECK(Tests, Manager.Close(Level, EditorDocumentCloseChoice::Discard, {}, {}) ==
			EditorDocumentCloseStatus::Protected);

		const auto RemovedProjectHosts = Manager.ClearProject(*OtherProject);
		PF_CHECK(Tests, RemovedProjectHosts.size() == 2);
		PF_CHECK(Tests, OtherProjectTextureOpen && OtherProjectOpen &&
			std::find(RemovedProjectHosts.begin(), RemovedProjectHosts.end(), OtherProjectTextureOpen->HostID) !=
				RemovedProjectHosts.end() &&
			std::find(RemovedProjectHosts.begin(), RemovedProjectHosts.end(), OtherProjectOpen->HostID) !=
				RemovedProjectHosts.end());
		PF_CHECK(Tests, Manager.GetSessions().size() == 7);
		PF_CHECK(Tests, !Manager.Find(OtherProjectMaterial));
		PF_CHECK(Tests, !Manager.Find(OtherProjectTexture));
		PF_CHECK(Tests, !Manager.FindHost(OtherProjectTextureOpen->HostID));
		PF_CHECK(Tests, OtherProjectOpen && !Manager.FindHost(OtherProjectOpen->HostID));
		PF_CHECK(Tests, !Manager.Find(MaterialA) && !Manager.Find(MaterialB));
	}

	void TestEditorSnapshotHistory(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorSnapshotHistory;
		EditorSnapshotHistory<int> History;
		History.Reset(10);
		PF_CHECK(Tests, !History.IsDirty() && !History.CanUndo() && !History.CanRedo());
		History.Record(11);
		PF_CHECK(Tests, History.IsDirty() && History.GetCurrent() == 11 && History.CanUndo());
		PF_CHECK(Tests, History.Undo());
		PF_CHECK(Tests, History.GetCurrent() == 10 && !History.IsDirty() && History.CanRedo());
		PF_CHECK(Tests, History.Redo());
		History.MarkSaved();
		PF_CHECK(Tests, History.GetCurrent() == 11 && !History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.IsDirty());
		History.Record(12);
		PF_CHECK(Tests, History.GetCurrent() == 12 && !History.CanRedo() && History.IsDirty());

		History.Reset(0);
		History.Record(1, 3);
		History.MarkSaved();
		History.Record(2, 3);
		History.Record(3, 3);
		PF_CHECK(Tests, History.GetStateCount() == 3 && History.GetCurrent() == 3 && History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.GetCurrent() == 2 && History.IsDirty());
		PF_CHECK(Tests, History.Undo() && History.GetCurrent() == 1 && !History.IsDirty());
		History.Record(4, 3);
		PF_CHECK(Tests, History.GetStateCount() == 2 && History.GetCurrent() == 4 && !History.CanRedo());
		History.Record(5, 1);
		PF_CHECK(Tests, History.GetStateCount() == 1 && History.GetCurrent() == 5 && History.IsDirty());
	}

	void TestEditorDocumentWorkspaceCodec(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Project = UUID::Generate();
		const auto MaterialA = UUID::Generate();
		const auto MaterialB = UUID::Generate();
		PF_CHECK(Tests, Project && MaterialA && MaterialB);
		if (!Project || !MaterialA || !MaterialB)
			return;

		const EditorDocumentIdentity Level{ *Project, UUID{}, EditorDocumentKind::Level };
		const EditorDocumentIdentity Amber{ *Project, *MaterialA, EditorDocumentKind::Material };
		const EditorDocumentIdentity Blue{ *Project, *MaterialB, EditorDocumentKind::Material };
		EditorDocumentHost Main;
		Main.ID = std::string(EditorDocumentManager::MainHostID);
		Main.Role = EditorDocumentHostRole::Main;
		Main.PlacementKind = EditorDocumentKind::Level;
		Main.Documents = { Level };
		Main.ActiveDocument = Level;
		EditorDocumentHost Native;
		Native.ID = "native:01234567-89ab-cdef-0123-456789abcdef";
		Native.Role = EditorDocumentHostRole::Native;
		Native.PlacementKind = EditorDocumentKind::Material;
		Native.Documents = { Amber, Blue };
		Native.ActiveDocument = Blue;
		Native.Bounds = { -1920.0f, 80.0f, 900.0f, 760.0f, 1.5f, "Display-Left", true, true };

		EditorDocumentWorkspaceState State;
		State.Project = *Project;
		State.MaterialDetailsAsset = *MaterialA;
		State.Hosts = { Main, Native };
		State.RememberedHosts.emplace_back(EditorDocumentKind::Material, Native.ID);
		State.FocusedHostID = Native.ID;
		const auto Serialized = EditorDocumentWorkspaceCodec::Serialize(State);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;

		const auto Restored = EditorDocumentWorkspaceCodec::Deserialize(*Serialized, *Project);
		PF_CHECK(Tests, Restored.has_value());
		if (Restored)
		{
			PF_CHECK(Tests, Restored->Hosts.size() == 2 && Restored->Hosts[1].Documents.size() == 2);
			PF_CHECK(Tests, Restored->MaterialDetailsAsset == MaterialA);
			PF_CHECK(Tests, Restored->Hosts[1].ActiveDocument == Blue);
			PF_CHECK(Tests, Restored->Hosts[1].Bounds.X == -1920.0f && Restored->Hosts[1].Bounds.Maximized);
			PF_CHECK(Tests, Restored->FocusedHostID == Native.ID && Restored->RememberedHosts.size() == 1);
			const auto Candidates = GetMaterialDetailsMigrationCandidates(*Restored);
			PF_CHECK(Tests, Candidates.size() == 2 && Candidates[0] == *MaterialB && Candidates[1] == *MaterialA);
		}
		auto LegacyWorkspace = nlohmann::json::parse(*Serialized);
		LegacyWorkspace["version"] = 1;
		LegacyWorkspace.erase("materialDetailsAsset");
		const auto MigratedLegacy = EditorDocumentWorkspaceCodec::Deserialize(LegacyWorkspace.dump(), *Project);
		PF_CHECK(Tests, MigratedLegacy.has_value());
		if (MigratedLegacy)
		{
			PF_CHECK(Tests, MigratedLegacy->Version == 2 && !MigratedLegacy->MaterialDetailsAsset);
			PF_CHECK(Tests, MigratedLegacy->Hosts[1].Documents.size() == 2);
		}

		EditorDocumentWorkspaceState LegacyMainStripState = State;
		LegacyMainStripState.MaterialDetailsAsset.reset();
		LegacyMainStripState.Hosts[0].Documents = { Level, Amber, Blue };
		LegacyMainStripState.Hosts[0].ActiveDocument = Blue;
		LegacyMainStripState.Hosts.erase(LegacyMainStripState.Hosts.begin() + 1);
		LegacyMainStripState.RememberedHosts.clear();
		LegacyMainStripState.FocusedHostID = std::string(EditorDocumentManager::MainHostID);
		const auto LegacyMainStripJSON = EditorDocumentWorkspaceCodec::Serialize(LegacyMainStripState);
		PF_CHECK(Tests, LegacyMainStripJSON.has_value());
		if (LegacyMainStripJSON)
		{
			auto LegacyMainStrip = nlohmann::json::parse(*LegacyMainStripJSON);
			LegacyMainStrip["version"] = 1;
			LegacyMainStrip.erase("materialDetailsAsset");
			const auto MigratedMainStrip = EditorDocumentWorkspaceCodec::Deserialize(
				LegacyMainStrip.dump(), *Project);
			PF_CHECK(Tests, MigratedMainStrip.has_value());
			if (MigratedMainStrip)
			{
				const auto Candidates = GetMaterialDetailsMigrationCandidates(*MigratedMainStrip);
				PF_CHECK(Tests, Candidates.size() == 2 && Candidates[0] == *MaterialB && Candidates[1] == *MaterialA);
				PF_CHECK(Tests, MigratedMainStrip->Hosts.size() == 1 &&
					MigratedMainStrip->Hosts[0].Documents[0] == Level);
			}
		}
		const auto OtherProject = UUID::Generate();
		PF_CHECK(Tests, OtherProject &&
			!EditorDocumentWorkspaceCodec::Deserialize(*Serialized, *OtherProject));
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize("{not-json", *Project));

		auto Duplicate = nlohmann::json::parse(*Serialized);
		Duplicate["hosts"][1]["documents"].push_back(Duplicate["hosts"][1]["documents"][0]);
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize(Duplicate.dump(), *Project));
		auto InvalidBounds = nlohmann::json::parse(*Serialized);
		InvalidBounds["hosts"][1]["bounds"]["width"] = 0.0f;
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize(InvalidBounds.dump(), *Project));
		auto MissingLevel = nlohmann::json::parse(*Serialized);
		MissingLevel["hosts"][0]["documents"] = nlohmann::json::array();
		MissingLevel["hosts"][0]["active"] = nullptr;
		PF_CHECK(Tests, !EditorDocumentWorkspaceCodec::Deserialize(MissingLevel.dump(), *Project));
	}

	void TestEditorNativeWindowBoundsPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;

		EditorDocumentHost::WindowBounds Saved;
		Saved.X = -1900.0f;
		Saved.Y = -80.0f;
		Saved.Width = 800.0f;
		Saved.Height = 600.0f;
		Saved.DpiScale = 1.0f;
		Saved.HasValue = true;

		const EditorNativeWindowWorkArea LeftMonitor{ -1920.0f, -40.0f, 1920.0f, 1040.0f };
		const EditorNativeWindowRect NegativeCoordinate = ResolveNativeWindowRect(Saved, LeftMonitor, 1.5f);
		PF_CHECK(Tests, NegativeCoordinate.X == -1900.0f && NegativeCoordinate.Y == -40.0f);
		PF_CHECK(Tests, NegativeCoordinate.Width == 1200.0f && NegativeCoordinate.Height == 900.0f);

		Saved.X = 4000.0f;
		Saved.Y = -1000.0f;
		const EditorNativeWindowRect Recovered = ResolveNativeWindowRect(Saved, LeftMonitor, 1.0f);
		PF_CHECK(Tests, Recovered.X == -64.0f && Recovered.Y == -40.0f);
		PF_CHECK(Tests, Recovered.Width == 800.0f && Recovered.Height == 600.0f);

		Saved.DpiScale = 0.5f;
		const EditorNativeWindowRect BoundedScale = ResolveNativeWindowRect(Saved,
			{ -2000.0f, -100.0f, 3000.0f, 2000.0f }, 4.0f);
		PF_CHECK(Tests, BoundedScale.Width == 1600.0f && BoundedScale.Height == 1200.0f);

		Saved.DpiScale = 0.0f;
		const EditorNativeWindowRect InvalidScale = ResolveNativeWindowRect(Saved, {}, 2.0f);
		PF_CHECK(Tests, InvalidScale.Width == 800.0f && InvalidScale.Height == 600.0f);
		PF_CHECK(Tests, InvalidScale.X == 4000.0f && InvalidScale.Y == -1000.0f);
	}

	void TestAssetImportOperation(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		const auto SourceIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier && SourceIdentifier);
		if (!ProjectIdentifier || !SourceIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeImportProject-" + ProjectIdentifier->ToString());
		const std::filesystem::path SourceDirectory = TemporaryDirectory / ("PulseForgeImportSource-" + SourceIdentifier->ToString());
		struct TemporaryDirectoryCleanup
		{
			std::filesystem::path Path;
			~TemporaryDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		};
		TemporaryDirectoryCleanup ProjectCleanup{ ProjectRoot };
		TemporaryDirectoryCleanup SourceCleanup{ SourceDirectory };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Imported", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		std::filesystem::create_directories(SourceDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path SourceFile = SourceDirectory / "image.bin";
		const std::string SourceContents("source bytes\0with binary data", 29);
		{
			std::ofstream Output(SourceFile, std::ios::binary | std::ios::trunc);
			Output.write(SourceContents.data(), static_cast<std::streamsize>(SourceContents.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		PF_CHECK(Tests, Registry.GetAssetCount() == 0);
		const auto Imported = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/image.bin");
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported)
			return;

		const std::filesystem::path ImportedPath = ProjectRoot / "Assets" / "Imported" / "image.bin";
		const std::filesystem::path ImportedSidecar = AssetMetadataSerializer::GetSidecarPath(ImportedPath);
		const auto ImportedMetadata = AssetMetadataSerializer::LoadFromFile(ImportedSidecar);
		PF_CHECK(Tests, ImportedMetadata && ImportedMetadata->ID == Imported->ID);
		PF_CHECK(Tests, ImportedMetadata && ImportedMetadata->Version == AssetMetadataSerializer::CurrentVersion);
		PF_CHECK(Tests, Imported->ProjectRelativePath == std::filesystem::path("Assets/Imported/image.bin"));
		PF_CHECK(Tests, Registry.Find(Imported->ID).has_value());
		PF_CHECK(Tests, std::filesystem::exists(SourceFile));
		const auto ReadFile = [](const std::filesystem::path& Path) -> std::optional<std::string>
		{
			std::ifstream Input(Path, std::ios::binary);
			if (!Input)
				return std::nullopt;
			std::string Contents{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
			if (Input.bad())
				return std::nullopt;
			return Contents;
		};
		PF_CHECK(Tests, ReadFile(ImportedPath) == SourceContents);
		PF_CHECK(Tests, ReadFile(SourceFile) == SourceContents);

		const auto SecondImport = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/second-copy.bin");
		PF_CHECK(Tests, SecondImport.has_value());
		PF_CHECK(Tests, SecondImport && SecondImport->ID != Imported->ID);
		PF_CHECK(Tests, SecondImport && ReadFile(ProjectRoot / SecondImport->ProjectRelativePath) == SourceContents);
		PF_CHECK(Tests, Registry.GetAssetCount() == 2);

		const auto ExistingDestination = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/image.bin");
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, ReadFile(ImportedPath) == SourceContents);

		const auto InvalidTraversal = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/../escaped.bin");
		PF_CHECK(Tests, !InvalidTraversal && InvalidTraversal.error().Code == AssetOperationErrorCode::InvalidPath);
		const auto MissingSource = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceDirectory / "missing.bin",
			"Assets/Imported/missing.bin");
		PF_CHECK(Tests, !MissingSource && MissingSource.error().Code == AssetOperationErrorCode::ImportSourceInvalid);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Imported" / "missing.bin"));

		const std::span<const char> BinaryContents(SourceContents.data(), SourceContents.size());
		const auto Created = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			std::as_bytes(BinaryContents),
			"Assets/Imported/generated.bin");
		PF_CHECK(Tests, Created.has_value());
		PF_CHECK(Tests, Created && Created->ID != Imported->ID && Created->ID != SecondImport->ID);
		const std::filesystem::path CreatedPath = ProjectRoot / "Assets" / "Imported" / "generated.bin";
		PF_CHECK(Tests, ReadFile(CreatedPath) == SourceContents);
		const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(CreatedPath));
		PF_CHECK(Tests, Created && CreatedMetadata && CreatedMetadata->ID == Created->ID);
		PF_CHECK(Tests, Registry.GetAssetCount() == 3);
		PF_CHECK(Tests, Created && Registry.Find(Created->ID).has_value());
		if (Created)
		{
			const auto DuplicateCreation = AssetOperations::CreateAssetFromBytes(
				Registry,
				ProjectRoot,
				std::as_bytes(BinaryContents),
				"Assets/Imported/generated.bin");
			PF_CHECK(Tests, !DuplicateCreation &&
				DuplicateCreation.error().Code == AssetOperationErrorCode::DestinationExists);
			PF_CHECK(Tests, !DuplicateCreation.error().CommittedAsset.has_value());
			PF_CHECK(Tests, ReadFile(CreatedPath) == SourceContents);

			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			PF_CHECK(Tests, Registry.Find(Created->ID).has_value());
		}

		const auto EmptyCreated = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			{},
			"Assets/Imported/empty.bin");
		PF_CHECK(Tests, EmptyCreated.has_value());
		PF_CHECK(Tests, EmptyCreated && std::filesystem::file_size(ProjectRoot / EmptyCreated->ProjectRelativePath) == 0);

		const std::filesystem::path UntrackedFile = ProjectRoot / "Assets" / "Imported" / "untracked.bin";
		{
			std::ofstream Output(UntrackedFile, std::ios::binary | std::ios::trunc);
			Output << "missing sidecar";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto RefusedByRegistry = AssetOperations::ImportFile(
			Registry,
			ProjectRoot,
			SourceFile,
			"Assets/Imported/refused.bin");
		PF_CHECK(Tests, !RefusedByRegistry && RefusedByRegistry.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets" / "Imported" / "refused.bin"));
	}

	void TestMaterialAssets(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto TextureAsset = UUID::Generate();
		PF_CHECK(Tests, TextureAsset.has_value());
		if (!TextureAsset)
			return;

		MaterialAssetDesc Description;
		Description.BaseColorTexture = *TextureAsset;
		Description.BaseColorFactor = { 0.8f, 0.6f, 0.4f, 1.0f };
		Description.MetallicFactor = 0.65f;
		Description.RoughnessFactor = 0.25f;
		const auto Serialized = MaterialAssetSerializer::Serialize(Description);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		const auto Deserialized = MaterialAssetSerializer::Deserialize(*Serialized);
		PF_CHECK(Tests, Deserialized && Deserialized->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Deserialized && glm::all(glm::equal(Deserialized->BaseColorFactor, Description.BaseColorFactor)));
		PF_CHECK(Tests, Deserialized && Deserialized->MetallicFactor == Description.MetallicFactor &&
			Deserialized->RoughnessFactor == Description.RoughnessFactor);
		PF_CHECK(Tests, Serialized && Serialized->find("\"version\": 4") != std::string::npos);
		const std::string Version1Document = "{\"format\":\"PulseForgeMaterial\",\"version\":1,\"baseColorTexture\":\"" +
			TextureAsset->ToString() + "\",\"baseColorFactor\":[1,1,1,1]}";
		const auto Version1Material = MaterialAssetSerializer::Deserialize(Version1Document);
		PF_CHECK(Tests, Version1Material && Version1Material->MetallicFactor == 0.0f &&
			Version1Material->RoughnessFactor == 1.0f);
		const std::string Version2Document = "{\"format\":\"PulseForgeMaterial\",\"version\":2,\"baseColorTexture\":\"" +
			TextureAsset->ToString() + "\",\"baseColorFactor\":[1,1,1,1],\"metallicFactor\":0.4,\"roughnessFactor\":0.6}";
		const auto Version2Material = MaterialAssetSerializer::Deserialize(Version2Document);
		PF_CHECK(Tests, Version2Material && Version2Material->MetallicFactor == 0.4f &&
			Version2Material->RoughnessFactor == 0.6f && Version2Material->NormalTexture.IsNil());
		const std::string Version3Document = "{\"format\":\"PulseForgeMaterial\",\"version\":3,\"baseColorFactor\":[1,1,1,1],\"baseColorTexture\":null,\"metallicFactor\":0.4,\"roughnessFactor\":0.6}";
		const auto Version3Material = MaterialAssetSerializer::Deserialize(Version3Document);
		PF_CHECK(Tests, Version3Material && Version3Material->AlphaMode == MaterialAlphaMode::Opaque &&
			Version3Material->AlphaCutoff == 0.5f && !Version3Material->DoubleSided);
		MaterialAssetDesc TexturedDescription = Description;
		TexturedDescription.BaseColorTexture = {};
		TexturedDescription.MetallicRoughnessTexture = *TextureAsset;
		TexturedDescription.NormalTexture = *TextureAsset;
		TexturedDescription.OcclusionTexture = *TextureAsset;
		TexturedDescription.EmissiveTexture = *TextureAsset;
		TexturedDescription.NormalScale = 0.75f;
		TexturedDescription.OcclusionStrength = 0.35f;
		TexturedDescription.EmissiveFactor = { 0.5f, 1.25f, 2.0f };
		TexturedDescription.AlphaMode = MaterialAlphaMode::Mask;
		TexturedDescription.AlphaCutoff = 0.42f;
		TexturedDescription.DoubleSided = true;
		const auto TexturedSerialized = MaterialAssetSerializer::Serialize(TexturedDescription);
		PF_CHECK(Tests, TexturedSerialized.has_value());
		if (!TexturedSerialized)
			return;
		const auto TexturedDeserialized = MaterialAssetSerializer::Deserialize(*TexturedSerialized);
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->BaseColorTexture.IsNil() &&
			TexturedDeserialized->MetallicRoughnessTexture == TexturedDescription.MetallicRoughnessTexture &&
			TexturedDeserialized->NormalTexture == TexturedDescription.NormalTexture &&
			TexturedDeserialized->OcclusionTexture == TexturedDescription.OcclusionTexture &&
			TexturedDeserialized->EmissiveTexture == TexturedDescription.EmissiveTexture);
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->NormalScale == 0.75f &&
			TexturedDeserialized->OcclusionStrength == 0.35f &&
			glm::all(glm::equal(TexturedDeserialized->EmissiveFactor, TexturedDescription.EmissiveFactor)));
		PF_CHECK(Tests, TexturedDeserialized && TexturedDeserialized->AlphaMode == MaterialAlphaMode::Mask &&
			TexturedDeserialized->AlphaCutoff == 0.42f && TexturedDeserialized->DoubleSided);
		MaterialAssetDesc BlendDescription = Description;
		BlendDescription.AlphaMode = MaterialAlphaMode::Blend;
		const auto BlendSerialized = MaterialAssetSerializer::Serialize(BlendDescription);
		PF_CHECK(Tests, BlendSerialized.has_value());
		if (BlendSerialized)
		{
			const auto BlendDeserialized = MaterialAssetSerializer::Deserialize(*BlendSerialized);
			PF_CHECK(Tests, BlendDeserialized && BlendDeserialized->AlphaMode == MaterialAlphaMode::Blend);
		}
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(
			"{\"format\":\"PulseForgeMaterial\",\"version\":4,\"baseColorFactor\":[1,1,1,1],\"alphaMode\":\"HASH\"}"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(
			"{\"format\":\"PulseForgeMaterial\",\"version\":3,\"baseColorFactor\":[1,1,1,1],\"normalTexture\":7}"));
		std::string InvalidMetallicDocument = *Serialized;
		const size_t MetallicField = InvalidMetallicDocument.find("\"metallicFactor\":");
		const size_t MetallicValue = MetallicField == std::string::npos ? MetallicField :
			InvalidMetallicDocument.find_first_not_of(" :", MetallicField + std::string("\"metallicFactor\"").size());
		const size_t MetallicEnd = MetallicValue == std::string::npos ? MetallicValue :
			InvalidMetallicDocument.find_first_of(",\n", MetallicValue);
		PF_CHECK(Tests, MetallicValue != std::string::npos && MetallicEnd != std::string::npos);
		if (MetallicEnd != std::string::npos)
			InvalidMetallicDocument.replace(MetallicValue, MetallicEnd - MetallicValue, "-0.1");
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize(InvalidMetallicDocument));

		MaterialAssetDesc MissingTexture = Description;
		MissingTexture.BaseColorTexture = {};
		PF_CHECK(Tests, ValidateMaterialAssetDescription(MissingTexture).has_value());
		MaterialAssetDesc InvalidFactor = Description;
		InvalidFactor.BaseColorFactor.g = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.BaseColorFactor = Description.BaseColorFactor;
		InvalidFactor.BaseColorFactor.r = 1.1f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.MetallicFactor = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.MetallicFactor = 1.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.RoughnessFactor = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.NormalScale = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.NormalScale = 4.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.OcclusionStrength = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.AlphaCutoff = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.AlphaCutoff = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.AlphaMode = static_cast<MaterialAlphaMode>(0xff);
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor = Description;
		InvalidFactor.EmissiveFactor.g = -0.01f;
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.EmissiveFactor = { 10000.1f, 0.0f, 0.0f };
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{invalid"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{\"format\":\"OtherMaterial\",\"version\":1}"));
		PF_CHECK(Tests, !MaterialAssetSerializer::Deserialize("{\"format\":\"PulseForgeMaterial\",\"version\":99}"));

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeMaterials-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };
		const std::filesystem::path MaterialsDirectory = ProjectRoot / "Assets" / "Materials";
		const std::filesystem::path TexturesDirectory = ProjectRoot / "Assets" / "Textures";
		std::filesystem::create_directories(MaterialsDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		std::filesystem::create_directories(TexturesDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path TexturePath = TexturesDirectory / "base-color.png";
		{
			std::ofstream Output(TexturePath, std::ios::binary | std::ios::trunc);
			Output << "texture placeholder";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto TextureMetadata = AssetMetadataSerializer::CreateForNewAsset(TexturePath);
		PF_CHECK(Tests, TextureMetadata.has_value());
		if (!TextureMetadata)
			return;
		Description.BaseColorTexture = TextureMetadata->ID;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Created = MaterialAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Materials/validation.material",
			Description);
		PF_CHECK(Tests, Created.has_value());
		PF_CHECK(Tests, Created && Created->ID != TextureMetadata->ID);
		if (!Created)
			return;

		const std::filesystem::path MaterialPath = ProjectRoot / "Assets" / "Materials" / "validation.material";
		const auto Metadata = AssetMetadataSerializer::LoadFromFile(AssetMetadataSerializer::GetSidecarPath(MaterialPath));
		PF_CHECK(Tests, Metadata && Metadata->ID == Created->ID);
		PF_CHECK(Tests, Registry.Find(Created->ID).has_value());
		const auto Loaded = MaterialAssetService::Load(Created->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Loaded && Loaded->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Loaded && glm::all(glm::equal(Loaded->BaseColorFactor, Description.BaseColorFactor)));
		PF_CHECK(Tests, Loaded && std::abs(Loaded->MetallicFactor - Description.MetallicFactor) < 1.0e-5f);
		PF_CHECK(Tests, Loaded && std::abs(Loaded->RoughnessFactor - Description.RoughnessFactor) < 1.0e-5f);

		MaterialAssetCache Cache(ProjectRoot, Registry);
		const auto Cached = Cache.GetOrLoad(Created->ID);
		const auto CachedAgain = Cache.GetOrLoad(Created->ID);
		PF_CHECK(Tests, Cached && CachedAgain && &Cached->get() == &CachedAgain->get());
		PF_CHECK(Tests, Cache.GetLoadedCount() == 1);

		MaterialAssetDesc Updated = Description;
		Updated.BaseColorFactor = { 0.3f, 0.5f, 0.7f, 1.0f };
		PF_CHECK(Tests, MaterialAssetService::Save(Created->ID, ProjectRoot, Registry, Updated).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(MaterialPath));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == Created->ID);
		PF_CHECK(Tests, Cached && glm::all(glm::equal(Cached->get().BaseColorFactor, Description.BaseColorFactor)));
		Cache.Clear();
		const auto Reloaded = Cache.GetOrLoad(Created->ID);
		PF_CHECK(Tests, Reloaded && glm::all(glm::equal(Reloaded->get().BaseColorFactor, Updated.BaseColorFactor)));

		const auto Moved = AssetOperations::Move(Registry, ProjectRoot, Created->ID, "Assets/Materials/renamed.material");
		PF_CHECK(Tests, Moved && Moved->ID == Created->ID);
		const auto LoadedAfterMove = MaterialAssetService::Load(Created->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, LoadedAfterMove && glm::all(glm::equal(LoadedAfterMove->BaseColorFactor, Updated.BaseColorFactor)));

		const auto InvalidExtension = MaterialAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Materials/invalid.json",
			Description);
		PF_CHECK(Tests, !InvalidExtension && InvalidExtension.error().Code == MaterialAssetErrorCode::UnsupportedAssetType);
		const auto Duplicate = MaterialAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Materials/renamed.material",
			Description);
		PF_CHECK(Tests, !Duplicate && Duplicate.error().Code == MaterialAssetErrorCode::AssetOperationFailed);

		const std::filesystem::path WrongTypePath = MaterialsDirectory / "not-material.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "not a material";
		}
		const auto WrongTypeMetadata = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeMetadata.has_value());
		const std::filesystem::path BrokenPath = MaterialsDirectory / "broken.material";
		{
			std::ofstream Output(BrokenPath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
		}
		const auto BrokenMetadata = AssetMetadataSerializer::CreateForNewAsset(BrokenPath);
		PF_CHECK(Tests, BrokenMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (WrongTypeMetadata)
		{
			const auto WrongType = MaterialAssetService::Load(WrongTypeMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == MaterialAssetErrorCode::UnsupportedAssetType);
		}
		if (BrokenMetadata)
		{
			const auto Broken = MaterialAssetService::Load(BrokenMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !Broken && Broken.error().Code == MaterialAssetErrorCode::InvalidDocument);
		}
	}

	void TestProjectFiles(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path Root = TemporaryDirectory / ("PulseForgeProject-" + ProjectIdentifier->ToString());
		const std::filesystem::path RelocatedRoot = Root.parent_path() / (Root.filename().string() + "-relocated");
		struct ProjectCleanup
		{
			std::filesystem::path First;
			std::filesystem::path Second;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(First, Error);
				Error.clear();
				std::filesystem::remove_all(Second, Error);
			}
		} Cleanup{ Root, RelocatedRoot };

		const std::filesystem::path ProjectFile = Root / "Example.pfproj";
		auto Created = Project::Create(ProjectFile, "Example Project");
		PF_CHECK(Tests, Created.has_value());
		if (!Created)
			return;
		PF_CHECK(Tests, Created->GetDescription().Name == "Example Project");
		PF_CHECK(Tests, !Created->GetDescription().StartScene.has_value());
		PF_CHECK(Tests, std::filesystem::is_directory(Root / "Assets"));
		PF_CHECK(Tests, !Project::Create(ProjectFile, "Duplicate Project"));
		PF_CHECK(Tests, !ProjectSerializer::Deserialize("{broken"));
		PF_CHECK(Tests, !ProjectSerializer::Deserialize("{\"format\":\"PulseForgeProject\",\"version\":99}"));

		const std::filesystem::path ScenesDirectory = Root / "Assets" / "Scenes";
		std::filesystem::create_directories(ScenesDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		const std::filesystem::path ScenePath = ScenesDirectory / "start.scene";
		{
			std::ofstream Output(ScenePath, std::ios::binary | std::ios::trunc);
			Output << "{}";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto SceneMetadata = AssetMetadataSerializer::CreateForNewAsset(ScenePath);
		PF_CHECK(Tests, SceneMetadata.has_value());
		if (!SceneMetadata)
			return;
		PF_CHECK(Tests, Created->GetAssetRegistry().Rebuild(Root).has_value());
		PF_CHECK(Tests, Created->SetStartScene(SceneMetadata->ID).has_value());
		PF_CHECK(Tests, Created->GetDescription().StartScene == SceneMetadata->ID);

		const std::filesystem::path WrongTypePath = Root / "Assets" / "not-a-scene.txt";
		{
			std::ofstream Output(WrongTypePath, std::ios::binary | std::ios::trunc);
			Output << "asset";
		}
		const auto WrongTypeMetadata = AssetMetadataSerializer::CreateForNewAsset(WrongTypePath);
		PF_CHECK(Tests, WrongTypeMetadata.has_value());
		PF_CHECK(Tests, Created->GetAssetRegistry().Rebuild(Root).has_value());
		if (WrongTypeMetadata)
		{
			const auto WrongType = Created->SetStartScene(WrongTypeMetadata->ID);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == ProjectErrorCode::StartSceneWrongType);
			PF_CHECK(Tests, Created->GetDescription().StartScene == SceneMetadata->ID);
		}

		Created = Project::Open(ProjectFile);
		PF_CHECK(Tests, Created && Created->GetDescription().StartScene == SceneMetadata->ID);
		if (!Created)
			return;
		std::filesystem::rename(Root, RelocatedRoot, FileError);
		PF_CHECK(Tests, !FileError);
		const auto RelocatedProject = Project::Open(RelocatedRoot / "Example.pfproj");
		PF_CHECK(Tests, RelocatedProject.has_value());
		PF_CHECK(Tests, RelocatedProject && RelocatedProject->GetAssetRegistry().Find(SceneMetadata->ID).has_value());
		PF_CHECK(Tests, RelocatedProject && RelocatedProject->GetRootPath() == RelocatedRoot);
	}

	void TestEditorCommandLineAndStartupProjectLoading(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;
		const auto Identifier = UUID::Generate();
		PF_CHECK(Tests, Identifier.has_value());
		if (!Identifier)
			return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path Root =
			TemporaryDirectory / ("PulseForge CLI Test " + Identifier->ToString());
		struct DirectoryCleanup
		{
			std::filesystem::path Path;
			~DirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ Root };

		std::filesystem::create_directories(Root, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path ProjectFile = Root / "Project With Spaces.pfproj";
		auto Created = Project::Create(ProjectFile, "CLI Project With Spaces");
		PF_CHECK(Tests, Created.has_value());
		if (!Created)
			return;

		const std::span<const std::string_view> NoArguments;
		const auto DefaultOptions = ParseEditorCommandLine(NoArguments, Root);
		PF_CHECK(Tests, DefaultOptions.has_value());
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->ProjectFile.has_value());
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTestFixture);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostResetLayoutTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabTearOffTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabReattachTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostTabReorderTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostPresentationFailureTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostAcquireOutOfDateTest);
		PF_CHECK(Tests, DefaultOptions && !DefaultOptions->EnableDocumentHostImageAllocationFailureTest);

		const std::string AbsoluteProjectPath = ProjectFile.string();
		const std::array AbsoluteArguments = { std::string_view("--project"), std::string_view(AbsoluteProjectPath) };
		const auto AbsoluteOptions = ParseEditorCommandLine(AbsoluteArguments, Root);
		PF_CHECK(Tests, AbsoluteOptions.has_value());
		PF_CHECK(Tests, AbsoluteOptions && AbsoluteOptions->ProjectFile == ProjectFile);

		const std::string RelativeProjectPath = ProjectFile.filename().string();
		const std::array RelativeArguments = { std::string_view("--project"), std::string_view(RelativeProjectPath) };
		const auto RelativeOptions = ParseEditorCommandLine(RelativeArguments, Root);
		PF_CHECK(Tests, RelativeOptions.has_value());
		PF_CHECK(Tests, RelativeOptions && RelativeOptions->ProjectFile == ProjectFile);

		const std::array HostFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture") };
		const auto HostFixtureOptions = ParseEditorCommandLine(HostFixtureArguments, Root);
		PF_CHECK(Tests, HostFixtureOptions.has_value());
		PF_CHECK(Tests, HostFixtureOptions && HostFixtureOptions->EnableDocumentHostTestFixture);
		const std::array HostFixtureWithoutProject = { std::string_view("--document-host-test-fixture") };
		const auto HostFixtureWithoutProjectResult = ParseEditorCommandLine(HostFixtureWithoutProject, Root);
		PF_CHECK(Tests, !HostFixtureWithoutProjectResult);
		PF_CHECK(Tests, !HostFixtureWithoutProjectResult &&
			HostFixtureWithoutProjectResult.error().Code == EditorCommandLineErrorCode::MissingProjectPath);
		const std::array DuplicateHostFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-fixture") };
		const auto DuplicateHostFixtureOptions = ParseEditorCommandLine(DuplicateHostFixtureArguments, Root);
		PF_CHECK(Tests, !DuplicateHostFixtureOptions);
		PF_CHECK(Tests, !DuplicateHostFixtureOptions &&
			DuplicateHostFixtureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ResetLayoutFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-reset-layout-test") };
		const auto ResetLayoutFixtureOptions = ParseEditorCommandLine(ResetLayoutFixtureArguments, Root);
		PF_CHECK(Tests, ResetLayoutFixtureOptions.has_value());
		PF_CHECK(Tests, ResetLayoutFixtureOptions && ResetLayoutFixtureOptions->EnableDocumentHostResetLayoutTest);
		const std::array ResetLayoutWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-reset-layout-test") };
		const auto ResetLayoutWithoutFixtureOptions = ParseEditorCommandLine(ResetLayoutWithoutFixture, Root);
		PF_CHECK(Tests, !ResetLayoutWithoutFixtureOptions);
		PF_CHECK(Tests, !ResetLayoutWithoutFixtureOptions &&
			ResetLayoutWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabTearOffFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-tear-off") };
		const auto TabTearOffFixtureOptions = ParseEditorCommandLine(TabTearOffFixtureArguments, Root);
		PF_CHECK(Tests, TabTearOffFixtureOptions && TabTearOffFixtureOptions->EnableDocumentHostTabTearOffTest);
		const std::array TabTearOffWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-tear-off") };
		const auto TabTearOffWithoutFixtureOptions = ParseEditorCommandLine(TabTearOffWithoutFixture, Root);
		PF_CHECK(Tests, !TabTearOffWithoutFixtureOptions &&
			TabTearOffWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabReattachFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reattach") };
		const auto TabReattachFixtureOptions = ParseEditorCommandLine(TabReattachFixtureArguments, Root);
		PF_CHECK(Tests, TabReattachFixtureOptions && TabReattachFixtureOptions->EnableDocumentHostTabReattachTest);
		const std::array TabReattachWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-reattach") };
		const auto TabReattachWithoutFixtureOptions = ParseEditorCommandLine(TabReattachWithoutFixture, Root);
		PF_CHECK(Tests, !TabReattachWithoutFixtureOptions &&
			TabReattachWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array ConflictingTabTransferFixtures = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-tear-off"),
			std::string_view("--document-host-test-tab-reattach") };
		const auto ConflictingTabTransferOptions = ParseEditorCommandLine(ConflictingTabTransferFixtures, Root);
		PF_CHECK(Tests, !ConflictingTabTransferOptions &&
			ConflictingTabTransferOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array TabReorderFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto TabReorderFixtureOptions = ParseEditorCommandLine(TabReorderFixtureArguments, Root);
		PF_CHECK(Tests, TabReorderFixtureOptions && TabReorderFixtureOptions->EnableDocumentHostTabReorderTest);
		const std::array TabReorderWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-tab-reorder") };
		const auto TabReorderWithoutFixtureOptions = ParseEditorCommandLine(TabReorderWithoutFixture, Root);
		PF_CHECK(Tests, !TabReorderWithoutFixtureOptions &&
			TabReorderWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateTabReorderFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reorder"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto DuplicateTabReorderOptions = ParseEditorCommandLine(DuplicateTabReorderFixture, Root);
		PF_CHECK(Tests, !DuplicateTabReorderOptions &&
			DuplicateTabReorderOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingTabReorderFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-tab-reattach"),
			std::string_view("--document-host-test-tab-reorder") };
		const auto ConflictingTabReorderOptions = ParseEditorCommandLine(ConflictingTabReorderFixture, Root);
		PF_CHECK(Tests, !ConflictingTabReorderOptions &&
			ConflictingTabReorderOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array PresentationFailureFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-presentation-failure") };
		const auto PresentationFailureFixtureOptions = ParseEditorCommandLine(PresentationFailureFixtureArguments, Root);
		PF_CHECK(Tests, PresentationFailureFixtureOptions &&
			PresentationFailureFixtureOptions->EnableDocumentHostPresentationFailureTest);
		const std::array PresentationFailureWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-presentation-failure") };
		const auto PresentationFailureWithoutFixtureOptions = ParseEditorCommandLine(PresentationFailureWithoutFixture, Root);
		PF_CHECK(Tests, !PresentationFailureWithoutFixtureOptions &&
			PresentationFailureWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicatePresentationFailureFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-presentation-failure"),
			std::string_view("--document-host-test-presentation-failure") };
		const auto DuplicatePresentationFailureOptions = ParseEditorCommandLine(DuplicatePresentationFailureFixture, Root);
		PF_CHECK(Tests, !DuplicatePresentationFailureOptions &&
			DuplicatePresentationFailureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ConflictingPresentationFailureFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-presentation-failure"),
			std::string_view("--document-host-reset-layout-test") };
		const auto ConflictingPresentationFailureOptions = ParseEditorCommandLine(ConflictingPresentationFailureFixture, Root);
		PF_CHECK(Tests, !ConflictingPresentationFailureOptions &&
			ConflictingPresentationFailureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array AcquireOutOfDateFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto AcquireOutOfDateFixtureOptions = ParseEditorCommandLine(AcquireOutOfDateFixtureArguments, Root);
		PF_CHECK(Tests, AcquireOutOfDateFixtureOptions &&
			AcquireOutOfDateFixtureOptions->EnableDocumentHostAcquireOutOfDateTest);
		const std::array AcquireOutOfDateWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto AcquireOutOfDateWithoutFixtureOptions = ParseEditorCommandLine(AcquireOutOfDateWithoutFixture, Root);
		PF_CHECK(Tests, !AcquireOutOfDateWithoutFixtureOptions &&
			AcquireOutOfDateWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array DuplicateAcquireOutOfDateFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-acquire-out-of-date"),
			std::string_view("--document-host-test-acquire-out-of-date") };
		const auto DuplicateAcquireOutOfDateFixtureOptions =
			ParseEditorCommandLine(DuplicateAcquireOutOfDateFixture, Root);
		PF_CHECK(Tests, !DuplicateAcquireOutOfDateFixtureOptions &&
			DuplicateAcquireOutOfDateFixtureOptions.error().Code == EditorCommandLineErrorCode::DuplicateOption);
		const std::array ImageAllocationFailureFixtureArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ImageAllocationFailureFixtureOptions =
			ParseEditorCommandLine(ImageAllocationFailureFixtureArguments, Root);
		PF_CHECK(Tests, ImageAllocationFailureFixtureOptions &&
			ImageAllocationFailureFixtureOptions->EnableDocumentHostImageAllocationFailureTest);
		const std::array ImageAllocationFailureWithoutFixture = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ImageAllocationFailureWithoutFixtureOptions =
			ParseEditorCommandLine(ImageAllocationFailureWithoutFixture, Root);
		PF_CHECK(Tests, !ImageAllocationFailureWithoutFixtureOptions &&
			ImageAllocationFailureWithoutFixtureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);
		const std::array ConflictingPresentationFailureFixtures = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--document-host-test-fixture"),
			std::string_view("--document-host-test-acquire-out-of-date"),
			std::string_view("--document-host-test-image-allocation-failure") };
		const auto ConflictingAllocationFailureOptions =
			ParseEditorCommandLine(ConflictingPresentationFailureFixtures, Root);
		PF_CHECK(Tests, !ConflictingAllocationFailureOptions &&
			ConflictingAllocationFailureOptions.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::filesystem::path StressManifest = Root / "stress.manifest";
		{
			std::ofstream Manifest(StressManifest, std::ios::binary | std::ios::trunc);
			Manifest << "fixture|fixture.png\n";
			PF_CHECK(Tests, static_cast<bool>(Manifest));
		}
		const std::string StressManifestPath = StressManifest.string();
		const std::array StressArguments = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--asset-preview-stress"), std::string_view(StressManifestPath), std::string_view("entries") };
		const auto StressOptions = ParseEditorCommandLine(StressArguments, Root);
		PF_CHECK(Tests, StressOptions.has_value());
		PF_CHECK(Tests, StressOptions && StressOptions->AssetPreviewStressManifest == StressManifest);
		PF_CHECK(Tests, StressOptions && StressOptions->AssetPreviewStressBudget == "entries");
		const std::array MissingStressMode = {
			std::string_view("--asset-preview-stress"), std::string_view(StressManifestPath) };
		const auto MissingStressModeResult = ParseEditorCommandLine(MissingStressMode, Root);
		PF_CHECK(Tests, !MissingStressModeResult);

		const std::array MissingValue = { std::string_view("--project") };
		const auto MissingValueResult = ParseEditorCommandLine(MissingValue, Root);
		PF_CHECK(Tests, !MissingValueResult);
		PF_CHECK(Tests, !MissingValueResult &&
			MissingValueResult.error().Code == EditorCommandLineErrorCode::MissingProjectPath);

		const std::array Unexpected = { std::string_view("--unknown") };
		const auto UnexpectedResult = ParseEditorCommandLine(Unexpected, Root);
		PF_CHECK(Tests, !UnexpectedResult);
		PF_CHECK(Tests, !UnexpectedResult &&
			UnexpectedResult.error().Code == EditorCommandLineErrorCode::UnexpectedArgument);

		const std::array Duplicate = {
			std::string_view("--project"), std::string_view(AbsoluteProjectPath),
			std::string_view("--project"), std::string_view(AbsoluteProjectPath) };
		const auto DuplicateResult = ParseEditorCommandLine(Duplicate, Root);
		PF_CHECK(Tests, !DuplicateResult);
		PF_CHECK(Tests, !DuplicateResult &&
			DuplicateResult.error().Code == EditorCommandLineErrorCode::DuplicateProjectOption);

		const std::string MissingProjectPath = (Root / "Missing.pfproj").string();
		const std::array InvalidProject = { std::string_view("--project"), std::string_view(MissingProjectPath) };
		const auto InvalidProjectResult = ParseEditorCommandLine(InvalidProject, Root);
		PF_CHECK(Tests, !InvalidProjectResult);
		PF_CHECK(Tests, !InvalidProjectResult &&
			InvalidProjectResult.error().Code == EditorCommandLineErrorCode::InvalidProjectPath);

		auto Loaded = OpenEditorProject(ProjectFile);
		PF_CHECK(Tests, Loaded.has_value());
		PF_CHECK(Tests, Loaded && Loaded->Project.GetDescription().Name == "CLI Project With Spaces");
		PF_CHECK(Tests, Loaded && !Loaded->StartupScene && !Loaded->StartupSceneAsset);

		const std::filesystem::path BrokenProjectFile = Root / "Broken Project.pfproj";
		{
			std::ofstream BrokenProject(BrokenProjectFile, std::ios::binary | std::ios::trunc);
			BrokenProject << "{broken";
			PF_CHECK(Tests, static_cast<bool>(BrokenProject));
		}
		const std::string BrokenProjectPath = BrokenProjectFile.string();
		const std::array BrokenArguments = {
			std::string_view("--project"), std::string_view(BrokenProjectPath) };
		const auto BrokenOptions = ParseEditorCommandLine(BrokenArguments, Root);
		PF_CHECK(Tests, BrokenOptions.has_value());
		if (BrokenOptions)
		{
			const auto BrokenLoad = OpenEditorProject(*BrokenOptions->ProjectFile);
			PF_CHECK(Tests, !BrokenLoad);
			PF_CHECK(Tests, !BrokenLoad && BrokenLoad.error().Message.find("could not be opened") != std::string::npos);
		}
	}

	void TestBundledValidationProjectAssetRegistry(TestRunner& Tests)
	{
		using namespace PulseForge;
		const std::filesystem::path RepositoryRoot = std::filesystem::path(__FILE__).parent_path().parent_path();
		const std::filesystem::path ProjectFile = RepositoryRoot / "PulseForgeGame" / "PulseForgeGame.pfproj";
		auto OpenedProject = Project::Open(ProjectFile);
		PF_CHECK(Tests, OpenedProject.has_value());
		if (!OpenedProject)
			return;

		const std::filesystem::path AssetsRoot = ProjectFile.parent_path() / "Assets";
		const std::vector<AssetRecord> RegisteredAssets = OpenedProject->GetAssetRegistry().GetAssets();
		size_t ManagedSourceCount = 0;
		std::error_code FileError;
		for (std::filesystem::recursive_directory_iterator Iterator(AssetsRoot, FileError), End;
			!FileError && Iterator != End; Iterator.increment(FileError))
		{
			if (!Iterator->is_regular_file(FileError) || FileError || Iterator->path().extension() == ".meta")
				continue;

			++ManagedSourceCount;
			const std::filesystem::path RelativePath = Iterator->path().lexically_relative(ProjectFile.parent_path());
			const auto Record = std::find_if(RegisteredAssets.begin(), RegisteredAssets.end(),
				[&RelativePath](const AssetRecord& Asset) { return Asset.ProjectRelativePath == RelativePath; });
			PF_CHECK(Tests, Record != RegisteredAssets.end());
		}
		PF_CHECK(Tests, !FileError);
		PF_CHECK(Tests, RegisteredAssets.size() == ManagedSourceCount);

		const std::filesystem::path DemoProjectFile = RepositoryRoot / "Samples" / "VisualValidation" /
			"PointLightShadows" / "PointLightShadows.pfproj";
		auto DemoProject = Project::Open(DemoProjectFile);
		PF_CHECK(Tests, DemoProject.has_value());
		if (!DemoProject)
			return;
		PF_CHECK(Tests, DemoProject->GetDescription().StartScene.has_value());
		if (!DemoProject->GetDescription().StartScene)
			return;
		const auto StartupRecord = DemoProject->GetAssetRegistry().Find(*DemoProject->GetDescription().StartScene);
		PF_CHECK(Tests, StartupRecord.has_value());
		PF_CHECK(Tests, StartupRecord && StartupRecord->ProjectRelativePath.filename() == "PointLightShadowsOverview.scene");
		size_t LoadedDemoSceneCount = 0;
		for (const AssetRecord& Asset : DemoProject->GetAssetRegistry().GetAssets())
		{
			if (Asset.ProjectRelativePath.extension() != ".scene")
				continue;

			Scene LoadedDemoScene;
			PF_CHECK(Tests, SceneAssetService::Load(Asset.ID, DemoProject->GetRootPath(),
				DemoProject->GetAssetRegistry(), LoadedDemoScene).has_value());
			const auto ReferenceIssues = AssetReferenceValidator::Validate(
				LoadedDemoScene, DemoProject->GetAssetRegistry());
			PF_CHECK(Tests, ReferenceIssues.has_value());
			PF_CHECK(Tests, ReferenceIssues && ReferenceIssues->empty());
			++LoadedDemoSceneCount;
		}
		PF_CHECK(Tests, LoadedDemoSceneCount == 4);

		const std::filesystem::path EditorWorkflowProjectFile = RepositoryRoot / "Samples" / "VisualValidation" /
			"EditorWorkflow" / "EditorWorkflow.pfproj";
		auto EditorWorkflowProject = Project::Open(EditorWorkflowProjectFile);
		PF_CHECK(Tests, EditorWorkflowProject.has_value());
		if (!EditorWorkflowProject)
			return;
		PF_CHECK(Tests, EditorWorkflowProject->GetDescription().StartScene.has_value());
		if (!EditorWorkflowProject->GetDescription().StartScene)
			return;
		const auto EditorWorkflowStartup = EditorWorkflowProject->GetAssetRegistry().Find(
			*EditorWorkflowProject->GetDescription().StartScene);
		PF_CHECK(Tests, EditorWorkflowStartup.has_value());
		PF_CHECK(Tests, EditorWorkflowStartup &&
			EditorWorkflowStartup->ProjectRelativePath.filename() == "EditorWorkflowOverview.scene");
		Scene EditorWorkflowScene;
		PF_CHECK(Tests, SceneAssetService::Load(*EditorWorkflowProject->GetDescription().StartScene,
			EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry(), EditorWorkflowScene).has_value());
		const auto EditorWorkflowReferences = AssetReferenceValidator::Validate(
			EditorWorkflowScene, EditorWorkflowProject->GetAssetRegistry());
		PF_CHECK(Tests, EditorWorkflowReferences.has_value());
		PF_CHECK(Tests, EditorWorkflowReferences && EditorWorkflowReferences->empty());
		PF_CHECK(Tests, EditorWorkflowScene.GetEntityCount() == 13);
		const std::vector<AssetRecord> WorkflowAssets = EditorWorkflowProject->GetAssetRegistry().GetAssets();
		size_t WorkflowMaterialCount = 0;
		for (const AssetRecord& Asset : WorkflowAssets)
			if (Asset.ProjectRelativePath.extension() == ".material")
			{
				const auto Material = MaterialAssetService::Load(
					Asset.ID, EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry());
				PF_CHECK(Tests, Material.has_value());
				++WorkflowMaterialCount;
			}
		PF_CHECK(Tests, WorkflowMaterialCount == 4);
		const auto WorkflowMesh = std::find_if(WorkflowAssets.begin(), WorkflowAssets.end(), [](const AssetRecord& Asset)
			{
				return Asset.ProjectRelativePath == std::filesystem::path("Assets/Models/WorkflowCube.gltf");
			});
		PF_CHECK(Tests, WorkflowMesh != WorkflowAssets.end());
		if (WorkflowMesh != WorkflowAssets.end())
		{
			const auto ImportedWorkflowMesh = GltfMeshImporter::ImportStaticPrimitive(
				WorkflowMesh->ID, EditorWorkflowProject->GetRootPath(), EditorWorkflowProject->GetAssetRegistry());
			PF_CHECK(Tests, ImportedWorkflowMesh.has_value());
			PF_CHECK(Tests, ImportedWorkflowMesh && !ImportedWorkflowMesh->Vertices.empty() &&
				!ImportedWorkflowMesh->Indices.empty());
		}
	}

	void TestAssetReferenceValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		const auto MissingAssetIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier && MissingAssetIdentifier);
		if (!ProjectIdentifier || !MissingAssetIdentifier)
			return;

		const std::filesystem::path ProjectRoot =
			TemporaryDirectory / ("PulseForgeAssetReferences-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const auto CreateManagedAsset = [&](std::string_view RelativePath, std::string_view Contents)
			-> std::optional<AssetMetadata>
		{
			const std::filesystem::path Path = ProjectRoot / RelativePath;
			std::filesystem::create_directories(Path.parent_path(), FileError);
			PF_CHECK(Tests, !FileError);
			if (FileError)
				return std::nullopt;
			{
				std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
				Output.write(Contents.data(), static_cast<std::streamsize>(Contents.size()));
				PF_CHECK(Tests, static_cast<bool>(Output));
				if (!Output)
					return std::nullopt;
			}
			auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			PF_CHECK(Tests, Metadata.has_value());
			return Metadata ? std::optional<AssetMetadata>{ *Metadata } : std::nullopt;
		};

		const auto MeshMetadata = CreateManagedAsset("Assets/Meshes/mesh.Gltf", "mesh source");
		const auto MaterialMetadata = CreateManagedAsset("Assets/Materials/surface.material", "material");
		const auto WrongTypeMetadata = CreateManagedAsset("Assets/Misc/not-an-asset.txt", "other data");
		const auto ScriptMetadata = CreateManagedAsset("Assets/Scripts/controller.lua", "return {}\n");
		const auto AudioMetadata = CreateManagedAsset("Assets/Audio/sound.WAV", "audio data");
		const auto HdrMetadata = CreateManagedAsset("Assets/Environments/studio.hdr", "hdr data");
		PF_CHECK(Tests, MeshMetadata && MaterialMetadata && WrongTypeMetadata && ScriptMetadata && AudioMetadata && HdrMetadata);
		if (!MeshMetadata || !MaterialMetadata || !WrongTypeMetadata || !ScriptMetadata || !AudioMetadata || !HdrMetadata)
			return;

		Scene TestScene;
		const auto ValidScriptOnly = TestScene.CreateEntity("Valid script only");
		const auto MissingScriptOnly = TestScene.CreateEntity("Missing script only");
		const auto WrongScriptOnly = TestScene.CreateEntity("Wrong script only");
		const auto ValidAudioOnly = TestScene.CreateEntity("Valid audio only");
		const auto MissingAudioOnly = TestScene.CreateEntity("Missing audio only");
		const auto WrongAudioOnly = TestScene.CreateEntity("Wrong audio only");
		const auto WrongMeshType = TestScene.CreateEntity("Wrong mesh type");
		const auto WrongMaterialType = TestScene.CreateEntity("Wrong material type");
		const auto ValidMeshAndMaterial = TestScene.CreateEntity("Valid mesh and material");
		const auto MultipleReferences = TestScene.CreateEntity("Multiple references");
		const auto ValidEnvironment = TestScene.CreateEntity("Valid environment");
		const auto WrongEnvironment = TestScene.CreateEntity("Wrong environment");
		const auto Unrelated = TestScene.CreateEntity("Unrelated");
		PF_CHECK(Tests, ValidScriptOnly && MissingScriptOnly && WrongScriptOnly && ValidAudioOnly && MissingAudioOnly &&
			WrongAudioOnly && WrongMeshType && WrongMaterialType && ValidMeshAndMaterial && MultipleReferences &&
			ValidEnvironment && WrongEnvironment && Unrelated);
		if (!ValidScriptOnly || !MissingScriptOnly || !WrongScriptOnly || !ValidAudioOnly || !MissingAudioOnly ||
			!WrongAudioOnly || !WrongMeshType || !WrongMaterialType || !ValidMeshAndMaterial || !MultipleReferences ||
			!ValidEnvironment || !WrongEnvironment || !Unrelated)
			return;

		PF_CHECK(Tests, ValidScriptOnly->SetScript(ScriptComponent{ ScriptMetadata->ID }).has_value());
		PF_CHECK(Tests, MissingScriptOnly->SetScript(ScriptComponent{ *MissingAssetIdentifier, false }).has_value());
		PF_CHECK(Tests, WrongScriptOnly->SetScript(ScriptComponent{ WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, ValidAudioOnly->SetAudioSource(AudioSourceComponent{ AudioMetadata->ID }).has_value());
		PF_CHECK(Tests, MissingAudioOnly->SetAudioSource(AudioSourceComponent{ *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, WrongAudioOnly->SetAudioSource(AudioSourceComponent{ WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, WrongMeshType->SetMeshRenderer(MeshRendererComponent{ WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, WrongMaterialType->SetMeshRenderer(
			MeshRendererComponent{ MeshMetadata->ID, WrongTypeMetadata->ID }).has_value());
		PF_CHECK(Tests, ValidMeshAndMaterial->SetMeshRenderer(
			MeshRendererComponent{ MeshMetadata->ID, MaterialMetadata->ID }).has_value());
		PF_CHECK(Tests, MultipleReferences->SetMeshRenderer(
			MeshRendererComponent{ *MissingAssetIdentifier, *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, MultipleReferences->SetScript(ScriptComponent{ *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, MultipleReferences->SetAudioSource(AudioSourceComponent{ *MissingAssetIdentifier }).has_value());
		PF_CHECK(Tests, ValidEnvironment->SetEnvironmentLight(EnvironmentLightComponent{ HdrMetadata->ID, 0.8f }).has_value());
		PF_CHECK(Tests, WrongEnvironment->SetEnvironmentLight(EnvironmentLightComponent{ WrongTypeMetadata->ID, 1.0f }).has_value());

		const auto HasIssue = [](const std::vector<AssetReferenceIssue>& Issues,
			const Entity& Owner,
			AssetReferenceKind Kind,
			AssetReferenceIssueCode Code,
			AssetID Asset)
		{
			return std::ranges::any_of(Issues, [&](const AssetReferenceIssue& Issue)
			{
				return Issue.Entity == Owner.GetUUID() && Issue.Kind == Kind && Issue.Code == Code && Issue.Asset == Asset;
			});
		};

		AssetRegistry EmptyRegistry;
		const auto AllMissing = AssetReferenceValidator::Validate(TestScene, EmptyRegistry);
		PF_CHECK(Tests, AllMissing && AllMissing->size() == 17);
		PF_CHECK(Tests, AllMissing && HasIssue(*AllMissing, *ValidScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, ScriptMetadata->ID));
		PF_CHECK(Tests, AllMissing && HasIssue(*AllMissing, *ValidAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, AudioMetadata->ID));

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Validation = AssetReferenceValidator::Validate(TestScene, Registry);
		PF_CHECK(Tests, Validation && Validation->size() == 11);
		if (!Validation)
			return;

		PF_CHECK(Tests, HasIssue(*Validation, *MissingScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *MissingAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongEnvironment, AssetReferenceKind::Environment,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidEnvironment, AssetReferenceKind::Environment,
			AssetReferenceIssueCode::WrongAssetType, HdrMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongMeshType, AssetReferenceKind::Mesh,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *WrongMaterialType, AssetReferenceKind::Material,
			AssetReferenceIssueCode::WrongAssetType, WrongTypeMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::WrongAssetType, ScriptMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::WrongAssetType, AudioMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidMeshAndMaterial, AssetReferenceKind::Mesh,
			AssetReferenceIssueCode::WrongAssetType, MeshMetadata->ID));
		PF_CHECK(Tests, !HasIssue(*Validation, *ValidMeshAndMaterial, AssetReferenceKind::Material,
			AssetReferenceIssueCode::WrongAssetType, MaterialMetadata->ID));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Mesh,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Material,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, HasIssue(*Validation, *MultipleReferences, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, *MissingAssetIdentifier));
		PF_CHECK(Tests, std::ranges::none_of(*Validation, [&](const AssetReferenceIssue& Issue)
		{
			return Issue.Entity == Unrelated->GetUUID();
		}));
	}

	void TestScriptRuntime(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeScriptRuntime-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		auto ProjectResult = Project::Create(ProjectRoot / "ScriptProject.pfproj", "Script Runtime Tests");
		PF_CHECK(Tests, ProjectResult.has_value());
		if (!ProjectResult)
			return;
		std::filesystem::create_directories(ProjectRoot / "Assets/Scripts", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto CreateScriptAsset = [&](std::string_view FileName, std::string_view Source)
		{
			const auto Contents = std::as_bytes(std::span(Source.data(), Source.size()));
			return AssetOperations::CreateAssetFromBytes(
				ProjectResult->GetAssetRegistry(),
				ProjectResult->GetRootPath(),
				Contents,
				std::filesystem::path("Assets/Scripts") / FileName);
		};

		constexpr std::string_view TransformScript = R"(
local elapsed = 0
function OnCreate()
  assert(io == nil and os == nil and debug == nil and package == nil)
  assert(load == nil and loadfile == nil and dofile == nil and collectgarbage == nil)
  assert(pcall == nil and xpcall == nil)
  assert(string.dump == nil)
  local found = scene.find_entity(entity:uuid())
  assert(found ~= nil and found:uuid() == entity:uuid())
end
function OnUpdate(dt)
  elapsed = elapsed + dt
  assert(entity:set_translation(elapsed, 2, 3))
end
function OnDestroy()
  assert(entity:set_translation(-1, 2, 3))
end
)";
		const auto TransformScriptAsset = CreateScriptAsset("transform.lua", TransformScript);
		PF_CHECK(Tests, TransformScriptAsset.has_value());
		if (!TransformScriptAsset)
			return;

		const auto RelativeMotionScriptAsset = CreateScriptAsset(
			"relative_motion.lua",
			"local elapsed = 0\n"
			"local initialX, initialY, initialZ = 0, 0, 0\n"
			"function OnCreate()\n"
			"  initialX, initialY, initialZ = entity:get_translation()\n"
			"  assert(initialX ~= nil and initialY ~= nil and initialZ ~= nil)\n"
			"end\n"
			"function OnUpdate(dt)\n"
			"  elapsed = elapsed + dt\n"
			"  assert(entity:set_translation(initialX + math.sin(elapsed) * 0.25, initialY, initialZ))\n"
			"end\n");
		PF_CHECK(Tests, RelativeMotionScriptAsset.has_value());
		if (!RelativeMotionScriptAsset)
			return;

		Scene RelativeMotionScene;
		const auto OriginalMover = RelativeMotionScene.CreateEntity("Original mover");
		PF_CHECK(Tests, OriginalMover.has_value());
		if (!OriginalMover)
			return;
		TransformComponent OriginalTransform;
		OriginalTransform.Translation = { 3.0f, 2.0f, -4.0f };
		PF_CHECK(Tests, OriginalMover->SetTransform(OriginalTransform).has_value());
		PF_CHECK(Tests, OriginalMover->SetScript(ScriptComponent{ RelativeMotionScriptAsset->ID }).has_value());
		const auto DuplicatedMover = RelativeMotionScene.DuplicateEntity(*OriginalMover);
		PF_CHECK(Tests, DuplicatedMover.has_value());
		if (!DuplicatedMover)
			return;
		TransformComponent DuplicatedTransform;
		DuplicatedTransform.Translation = { -7.0f, 5.0f, 11.0f };
		PF_CHECK(Tests, DuplicatedMover->SetTransform(DuplicatedTransform).has_value());
		ScriptRuntime RelativeMotionRuntime(*ProjectResult);
		PF_CHECK(Tests, RelativeMotionRuntime.Start(RelativeMotionScene).has_value());
		PF_CHECK(Tests, RelativeMotionRuntime.Advance(RelativeMotionScene, Timestep(0.5)).has_value());
		const auto OriginalMovedTransform = OriginalMover->GetTransform();
		const auto DuplicatedMovedTransform = DuplicatedMover->GetTransform();
		const float ExpectedOscillation = std::sin(0.5f) * 0.25f;
		PF_CHECK(Tests, OriginalMovedTransform &&
			std::abs(OriginalMovedTransform->Translation.x - (3.0f + ExpectedOscillation)) < 0.0001f &&
			std::abs(OriginalMovedTransform->Translation.y - 2.0f) < 0.0001f &&
			std::abs(OriginalMovedTransform->Translation.z + 4.0f) < 0.0001f);
		PF_CHECK(Tests, DuplicatedMovedTransform &&
			std::abs(DuplicatedMovedTransform->Translation.x - (-7.0f + ExpectedOscillation)) < 0.0001f &&
			std::abs(DuplicatedMovedTransform->Translation.y - 5.0f) < 0.0001f &&
			std::abs(DuplicatedMovedTransform->Translation.z - 11.0f) < 0.0001f);
		PF_CHECK(Tests, RelativeMotionRuntime.GetDiagnostics().empty());
		RelativeMotionRuntime.Stop();

		std::filesystem::create_directories(ProjectRoot / "Assets/Prefabs", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		const auto PrefabChildScriptAsset = CreateScriptAsset(
			"prefab_child.lua",
			"function OnCreate() assert(entity:set_translation(9, 8, 7)) end\n");
		PF_CHECK(Tests, PrefabChildScriptAsset.has_value());
		if (!PrefabChildScriptAsset)
			return;

		Scene ScriptSpawnPrefabSource;
		const auto ScriptSpawnPrefabRoot = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab root");
		const auto ScriptSpawnPrefabChild = ScriptSpawnPrefabSource.CreateEntity("Script-spawned prefab child");
		PF_CHECK(Tests, ScriptSpawnPrefabRoot && ScriptSpawnPrefabChild);
		if (!ScriptSpawnPrefabRoot || !ScriptSpawnPrefabChild)
			return;
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetParent(*ScriptSpawnPrefabRoot).has_value());
		PF_CHECK(Tests, ScriptSpawnPrefabChild->SetScript(ScriptComponent{ PrefabChildScriptAsset->ID }).has_value());
		const auto ScriptSpawnPrefab = PrefabAssetService::Create(
			ProjectResult->GetAssetRegistry(),
			ProjectResult->GetRootPath(),
			"Assets/Prefabs/ScriptSpawn.prefab",
			ScriptSpawnPrefabSource,
			*ScriptSpawnPrefabRoot);
		PF_CHECK(Tests, ScriptSpawnPrefab.has_value());
		if (!ScriptSpawnPrefab)
			return;

		const std::string SceneMutationScript =
			"local CreatedEntity\n"
			"local SpawnedPrefab\n"
			"local Destroyed = false\n"
			"function OnCreate()\n"
			"  local invalidName, nameMessage = scene.create_entity(17)\n"
			"  assert(invalidName == nil and nameMessage ~= nil)\n"
			"  CreatedEntity, nameMessage = scene.create_entity('Lua-created entity')\n"
			"  assert(CreatedEntity, nameMessage)\n"
			"  local found = scene.find_entity(CreatedEntity:uuid())\n"
			"  assert(found ~= nil and found:uuid() == CreatedEntity:uuid())\n"
			"  local invalidPrefab, prefabMessage = scene.spawn_prefab('not-a-uuid')\n"
			"  assert(invalidPrefab == nil and prefabMessage ~= nil)\n"
			"  local wrongType = scene.spawn_prefab('" + TransformScriptAsset->ID.ToString() + "')\n"
			"  assert(wrongType == nil)\n"
			"  SpawnedPrefab, prefabMessage = scene.spawn_prefab('" + ScriptSpawnPrefab->ID.ToString() + "')\n"
			"  assert(SpawnedPrefab, prefabMessage)\n"
			"end\n"
			"function OnUpdate()\n"
			"  if not Destroyed then\n"
			"    assert(CreatedEntity:destroy())\n"
			"    Destroyed = true\n"
			"    assert(scene.find_entity(CreatedEntity:uuid()) == nil)\n"
			"    local x, staleMessage = CreatedEntity:get_translation()\n"
			"    assert(x == nil and staleMessage ~= nil)\n"
			"    local transient, message = scene.create_entity('Transient Lua entity')\n"
			"    assert(transient, message)\n"
			"    assert(transient:destroy())\n"
			"    assert(entity:destroy())\n"
			"  end\n"
			"  assert(SpawnedPrefab:set_translation(4, 5, 6))\n"
			"end\n"
			"function OnDestroy()\n"
			"  local x, staleMessage = entity:get_translation()\n"
			"  assert(x == nil and staleMessage ~= nil)\n"
			"end\n";
		const auto SceneMutationScriptAsset = CreateScriptAsset("scene_mutation.lua", SceneMutationScript);
		PF_CHECK(Tests, SceneMutationScriptAsset.has_value());
		if (!SceneMutationScriptAsset)
			return;

		Scene SceneMutationTestScene;
		const auto SceneMutationController = SceneMutationTestScene.CreateEntity("Scene mutation controller");
		PF_CHECK(Tests, SceneMutationController.has_value());
		if (!SceneMutationController)
			return;
		PF_CHECK(Tests, SceneMutationController->SetScript(ScriptComponent{ SceneMutationScriptAsset->ID }).has_value());
		ScriptRuntime SceneMutationRuntime(*ProjectResult);
		PF_CHECK(Tests, SceneMutationRuntime.Start(SceneMutationTestScene).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 4);

		std::optional<Entity> SpawnedPrefabRoot;
		std::optional<Entity> SpawnedPrefabChild;
		for (const Entity& Current : SceneMutationTestScene.GetEntities())
		{
			const auto Tag = Current.GetTag();
			if (Tag && Tag->Name == "Script-spawned prefab root")
				SpawnedPrefabRoot = Current;
			else if (Tag && Tag->Name == "Script-spawned prefab child")
				SpawnedPrefabChild = Current;
		}
		PF_CHECK(Tests, SpawnedPrefabRoot.has_value() && SpawnedPrefabChild.has_value());
		if (SpawnedPrefabRoot && SpawnedPrefabChild)
		{
			const auto SpawnedChildParent = SpawnedPrefabChild->GetParent();
			PF_CHECK(Tests, SpawnedChildParent && SpawnedChildParent->has_value() &&
				(**SpawnedChildParent).GetUUID() == SpawnedPrefabRoot->GetUUID());
		}

		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationTestScene.GetEntityCount() == 2);
		if (SpawnedPrefabRoot)
		{
			const auto SpawnedTransform = SpawnedPrefabRoot->GetTransform();
			PF_CHECK(Tests, SpawnedTransform &&
				std::abs(SpawnedTransform->Translation.x - 4.0f) < 0.0001f &&
				std::abs(SpawnedTransform->Translation.y - 5.0f) < 0.0001f &&
				std::abs(SpawnedTransform->Translation.z - 6.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 2);
		if (SpawnedPrefabChild)
		{
			const auto SpawnedChildTransform = SpawnedPrefabChild->GetTransform();
			PF_CHECK(Tests, SpawnedChildTransform &&
				std::abs(SpawnedChildTransform->Translation.x - 9.0f) < 0.0001f &&
				std::abs(SpawnedChildTransform->Translation.y - 8.0f) < 0.0001f &&
				std::abs(SpawnedChildTransform->Translation.z - 7.0f) < 0.0001f);
		}
		PF_CHECK(Tests, SceneMutationRuntime.Advance(SceneMutationTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, SceneMutationRuntime.GetScriptCount() == 1);
		PF_CHECK(Tests, SceneMutationRuntime.GetDiagnostics().empty());
		SceneMutationRuntime.Stop();

		Scene TestScene;
		const auto FirstEntity = TestScene.CreateEntity("First scripted entity");
		const auto SecondEntity = TestScene.CreateEntity("Second scripted entity");
		const auto DisabledEntity = TestScene.CreateEntity("Disabled scripted entity");
		PF_CHECK(Tests, FirstEntity && SecondEntity && DisabledEntity);
		if (!FirstEntity || !SecondEntity || !DisabledEntity)
			return;
		PF_CHECK(Tests, FirstEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, SecondEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, DisabledEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID, false }).has_value());

		ScriptRuntime Runtime(*ProjectResult);
		const auto BeforeStart = Runtime.Advance(TestScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeStart && BeforeStart.error().Code == ScriptRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, Runtime.Start(TestScene).has_value());
		PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetScriptCount() == 2);
		PF_CHECK(Tests, Runtime.GetDiagnostics().empty());
		const auto DuplicateStart = Runtime.Start(TestScene);
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == ScriptRuntimeErrorCode::AlreadyRunning);
		Scene OtherScene;
		const auto DifferentScene = Runtime.Advance(OtherScene, Timestep(0.1));
		PF_CHECK(Tests, !DifferentScene && DifferentScene.error().Code == ScriptRuntimeErrorCode::DifferentScene);
		const auto NegativeDelta = Runtime.Advance(TestScene, Timestep(-0.1));
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == ScriptRuntimeErrorCode::InvalidDeltaTime);
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.25)).has_value());
		const auto FirstTransform = FirstEntity->GetTransform();
		const auto SecondTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, FirstTransform && std::abs(FirstTransform->Translation.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, SecondTransform && std::abs(SecondTransform->Translation.x - 0.25f) < 0.0001f);

		PF_CHECK(Tests, FirstEntity->RemoveScript().has_value());
		PF_CHECK(Tests, Runtime.Advance(TestScene, Timestep(0.1)).has_value());
		PF_CHECK(Tests, Runtime.GetScriptCount() == 1);
		const auto DetachedTransform = FirstEntity->GetTransform();
		const auto ActiveTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, DetachedTransform && DetachedTransform->Translation.x == -1.0f);
		PF_CHECK(Tests, ActiveTransform && std::abs(ActiveTransform->Translation.x - 0.35f) < 0.0001f);
		Runtime.Stop();
		PF_CHECK(Tests, !Runtime.IsRunning() && Runtime.GetScriptCount() == 0);
		const auto StoppedTransform = SecondEntity->GetTransform();
		PF_CHECK(Tests, StoppedTransform && StoppedTransform->Translation.x == -1.0f);
		Runtime.Stop();

		Scene CoordinatedScene;
		const auto CoordinatedScriptEntity = CoordinatedScene.CreateEntity("Coordinated script");
		const auto CoordinatedPhysicsEntity = CoordinatedScene.CreateEntity("Coordinated body");
		PF_CHECK(Tests, CoordinatedScriptEntity && CoordinatedPhysicsEntity);
		if (!CoordinatedScriptEntity || !CoordinatedPhysicsEntity)
			return;
		PF_CHECK(Tests, CoordinatedScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, CoordinatedPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());

		SceneRuntimeDesc RuntimeDescription;
		RuntimeDescription.Audio.OutputBackend = AudioOutputBackend::Null;
		RuntimeDescription.Physics.FixedStepSeconds = 0.1;
		RuntimeDescription.Physics.MaxSubsteps = 2;
		RuntimeDescription.Physics.MaxFrameDeltaSeconds = 0.25;
		SceneRuntime CoordinatedRuntime(*ProjectResult, RuntimeDescription);
		const auto BeforeRuntimeStart = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.1));
		PF_CHECK(Tests, !BeforeRuntimeStart && BeforeRuntimeStart.error().Code == SceneRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, CoordinatedRuntime.Start(CoordinatedScene).has_value());
		PF_CHECK(Tests, CoordinatedRuntime.IsRunning());
		const auto DuplicateRuntimeStart = CoordinatedRuntime.Start(CoordinatedScene);
		PF_CHECK(Tests, !DuplicateRuntimeStart && DuplicateRuntimeStart.error().Code == SceneRuntimeErrorCode::AlreadyRunning);
		const auto CoordinatedNegativeDelta = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(-0.1));
		PF_CHECK(Tests,
			!CoordinatedNegativeDelta && CoordinatedNegativeDelta.error().Code == SceneRuntimeErrorCode::InvalidDeltaTime);
		const auto CoordinatedUpdate = CoordinatedRuntime.Advance(CoordinatedScene, Timestep(0.25));
		PF_CHECK(Tests, CoordinatedUpdate && CoordinatedUpdate->PhysicsSteps == 2);
		const auto CoordinatedTransform = CoordinatedScriptEntity->GetTransform();
		const auto PhysicsTransform = CoordinatedPhysicsEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedTransform && std::abs(CoordinatedTransform->Translation.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, PhysicsTransform && PhysicsTransform->Translation.y < 0.0f);
		Scene OtherRuntimeScene;
		const auto WrongRuntimeScene = CoordinatedRuntime.Advance(OtherRuntimeScene, Timestep(0.1));
		PF_CHECK(Tests, !WrongRuntimeScene && WrongRuntimeScene.error().Code == SceneRuntimeErrorCode::DifferentScene);
		CoordinatedRuntime.Stop();
		PF_CHECK(Tests, !CoordinatedRuntime.IsRunning());
		const auto CoordinatedStoppedTransform = CoordinatedScriptEntity->GetTransform();
		PF_CHECK(Tests,
			CoordinatedStoppedTransform && CoordinatedStoppedTransform->Translation.x == -1.0f);

		constexpr std::string_view ForceScript = R"(
function OnUpdate()
  local applied, message = entity:apply_force(0, 100, 0)
  assert(applied, message)
end
)";
		const auto ForceScriptAsset = CreateScriptAsset("force.lua", ForceScript);
		PF_CHECK(Tests, ForceScriptAsset.has_value());
		if (!ForceScriptAsset)
			return;
		Scene ScriptedPhysicsScene;
		const auto ScriptedBody = ScriptedPhysicsScene.CreateEntity("Script-driven body");
		PF_CHECK(Tests, ScriptedBody.has_value());
		if (!ScriptedBody)
			return;
		PF_CHECK(Tests, ScriptedBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, ScriptedBody->SetScript(ScriptComponent{ ForceScriptAsset->ID }).has_value());
		SceneRuntime ScriptedPhysicsRuntime(*ProjectResult, RuntimeDescription);
		PF_CHECK(Tests, ScriptedPhysicsRuntime.Start(ScriptedPhysicsScene).has_value());
		const auto ScriptedPhysicsFrame = ScriptedPhysicsRuntime.Advance(ScriptedPhysicsScene, Timestep(0.1));
		const auto ScriptedBodyTransform = ScriptedBody->GetTransform();
		PF_CHECK(Tests, ScriptedPhysicsFrame && ScriptedPhysicsFrame->PhysicsSteps == 1);
		PF_CHECK(Tests, ScriptedBodyTransform && ScriptedBodyTransform->Translation.y > 0.0f);
		ScriptedPhysicsRuntime.Stop();

		constexpr std::string_view UnavailableServicesScript = R"(
function OnUpdate()
  local applied, forceMessage = entity:apply_force(0, 1, 0)
  assert(applied == nil and string.find(forceMessage, "unavailable", 1, true) ~= nil)
  local played, audioMessage = entity:play_audio()
  assert(played == nil and string.find(audioMessage, "unavailable", 1, true) ~= nil)
  local pressed, inputMessage = input.is_key_pressed(87)
  assert(pressed == nil and string.find(inputMessage, "unavailable", 1, true) ~= nil)
end
)";
		const auto UnavailableServicesAsset = CreateScriptAsset("services_unavailable.lua", UnavailableServicesScript);
		PF_CHECK(Tests, UnavailableServicesAsset.has_value());
		if (!UnavailableServicesAsset)
			return;
		Scene UnavailableServicesScene;
		const auto UnavailableServicesEntity = UnavailableServicesScene.CreateEntity("No runtime services");
		PF_CHECK(Tests, UnavailableServicesEntity.has_value());
		if (!UnavailableServicesEntity)
			return;
		PF_CHECK(Tests, UnavailableServicesEntity->SetScript(ScriptComponent{ UnavailableServicesAsset->ID }).has_value());
		ScriptRuntime UnavailableServicesRuntime(*ProjectResult);
		PF_CHECK(Tests, UnavailableServicesRuntime.Start(UnavailableServicesScene).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.Advance(UnavailableServicesScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, UnavailableServicesRuntime.GetDiagnostics().empty());
		UnavailableServicesRuntime.Stop();

		constexpr std::string_view InputScript = R"(
local Updated = false
function OnCreate()
  local invalidKey, keyMessage = input.is_key_pressed("W")
  assert(invalidKey == nil and keyMessage ~= nil)
  local invalidButton, buttonMessage = input.is_mouse_button_pressed("left")
  assert(invalidButton == nil and buttonMessage ~= nil)
end
function OnUpdate()
  local x, y = input.get_mouse_position()
  if not Updated then
    assert(input.is_key_pressed(87))
    assert(not input.is_key_pressed(83))
    assert(input.is_mouse_button_pressed(0))
    assert(not input.is_mouse_button_pressed(1))
    assert(x == 12.5 and y == 9.25)
    Updated = true
  else
    assert(not input.is_key_pressed(87))
    assert(not input.is_mouse_button_pressed(0))
    assert(x == 14.5 and y == 7.25)
  end
  assert(entity:set_translation(x, y, 0))
end
)";
		const auto InputScriptAsset = CreateScriptAsset("input.lua", InputScript);
		PF_CHECK(Tests, InputScriptAsset.has_value());
		if (!InputScriptAsset)
			return;

		Input TestInput;
		KeyPressedEvent WPressed(87, 0);
		MouseButtonPressedEvent LeftPressed(0);
		MouseMovedEvent InitialMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(WPressed);
		TestInput.OnEvent(LeftPressed);
		TestInput.OnEvent(InitialMousePosition);

		Scene InputTestScene;
		const auto InputTestEntity = InputTestScene.CreateEntity("Input-driven script");
		PF_CHECK(Tests, InputTestEntity && InputTestEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!InputTestEntity)
			return;
		ScriptRuntimeServices InputServices;
		InputServices.InputState = &TestInput;
		ScriptRuntime InputRuntime(*ProjectResult, {}, InputServices);
		PF_CHECK(Tests, InputRuntime.Start(InputTestScene).has_value());
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		KeyReleasedEvent WReleased(87);
		MouseButtonReleasedEvent LeftReleased(0);
		MouseMovedEvent UpdatedMousePosition(14.5f, 7.25f);
		TestInput.OnEvent(WReleased);
		TestInput.OnEvent(LeftReleased);
		TestInput.OnEvent(UpdatedMousePosition);
		PF_CHECK(Tests, InputRuntime.Advance(InputTestScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, InputRuntime.GetDiagnostics().empty());
		const auto InputUpdatedTransform = InputTestEntity->GetTransform();
		PF_CHECK(Tests, InputUpdatedTransform &&
			std::abs(InputUpdatedTransform->Translation.x - 14.5f) < 0.0001f &&
			std::abs(InputUpdatedTransform->Translation.y - 7.25f) < 0.0001f);
		InputRuntime.Stop();

		KeyPressedEvent CoordinatorWPressed(87, 0);
		MouseButtonPressedEvent CoordinatorLeftPressed(0);
		MouseMovedEvent CoordinatorMousePosition(12.5f, 9.25f);
		TestInput.OnEvent(CoordinatorWPressed);
		TestInput.OnEvent(CoordinatorLeftPressed);
		TestInput.OnEvent(CoordinatorMousePosition);
		Scene CoordinatedInputScene;
		const auto CoordinatedInputEntity = CoordinatedInputScene.CreateEntity("Coordinated input script");
		PF_CHECK(Tests, CoordinatedInputEntity &&
			CoordinatedInputEntity->SetScript(ScriptComponent{ InputScriptAsset->ID }));
		if (!CoordinatedInputEntity)
			return;
		SceneRuntimeServices HostServices{ .InputState = &TestInput };
		SceneRuntime CoordinatedInputRuntime(*ProjectResult, RuntimeDescription, HostServices);
		PF_CHECK(Tests, CoordinatedInputRuntime.Start(CoordinatedInputScene).has_value());
		PF_CHECK(Tests,
			CoordinatedInputRuntime.Advance(CoordinatedInputScene, Timestep(1.0 / 60.0)).has_value());
		const auto CoordinatedInputTransform = CoordinatedInputEntity->GetTransform();
		PF_CHECK(Tests, CoordinatedInputTransform &&
			std::abs(CoordinatedInputTransform->Translation.x - 12.5f) < 0.0001f &&
			std::abs(CoordinatedInputTransform->Translation.y - 9.25f) < 0.0001f);
		CoordinatedInputRuntime.Stop();

		Scene RollbackScene;
		const auto RollbackScriptEntity = RollbackScene.CreateEntity("Rollback script");
		const auto RollbackPhysicsEntity = RollbackScene.CreateEntity("Rollback physics entity");
		PF_CHECK(Tests, RollbackScriptEntity && RollbackPhysicsEntity);
		if (!RollbackScriptEntity || !RollbackPhysicsEntity)
			return;
		PF_CHECK(Tests, RollbackScriptEntity->SetScript(ScriptComponent{ TransformScriptAsset->ID }).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, RollbackPhysicsEntity->SetBoxCollider(BoxColliderComponent{}).has_value());
		SceneRuntimeDesc InvalidScriptDescription = RuntimeDescription;
		InvalidScriptDescription.Scripting.MaxSourceBytes = 0;
		SceneRuntime RollbackRuntime(*ProjectResult, InvalidScriptDescription);
		const auto FailedRuntimeStart = RollbackRuntime.Start(RollbackScene);
		PF_CHECK(Tests,
			!FailedRuntimeStart && FailedRuntimeStart.error().Subsystem == SceneRuntimeSubsystem::Scripting);
		PF_CHECK(Tests, !RollbackRuntime.IsRunning());
		const auto RolledBackTransform = RollbackScriptEntity->GetTransform();
		PF_CHECK(Tests, RolledBackTransform && RolledBackTransform->Translation.x == 0.0f);

		constexpr std::string_view SyntaxErrorScript = "function OnUpdate(\n";
		const auto SyntaxErrorAsset = CreateScriptAsset("syntax_error.lua", SyntaxErrorScript);
		PF_CHECK(Tests, SyntaxErrorAsset.has_value());
		if (!SyntaxErrorAsset)
			return;
		Scene CompileErrorScene;
		const auto CompileErrorEntity = CompileErrorScene.CreateEntity("Compile error");
		PF_CHECK(Tests, CompileErrorEntity && CompileErrorEntity->SetScript(ScriptComponent{ SyntaxErrorAsset->ID }));
		ScriptRuntime CompileErrorRuntime(*ProjectResult);
		PF_CHECK(Tests, CompileErrorRuntime.Start(CompileErrorScene).has_value());
		PF_CHECK(Tests, CompileErrorRuntime.GetScriptCount() == 0 && CompileErrorRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::CompileFailed);
		CompileErrorRuntime.Stop();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().size() == 1);
		CompileErrorRuntime.ClearDiagnostics();
		PF_CHECK(Tests, CompileErrorRuntime.GetDiagnostics().empty());

		constexpr std::string_view InfiniteScript = "function OnUpdate() while true do end end\n";
		const auto InfiniteScriptAsset = CreateScriptAsset("instruction_limit.lua", InfiniteScript);
		PF_CHECK(Tests, InfiniteScriptAsset.has_value());
		if (!InfiniteScriptAsset)
			return;
		Scene InfiniteScene;
		const auto InfiniteEntity = InfiniteScene.CreateEntity("Instruction-limited script");
		PF_CHECK(Tests, InfiniteEntity && InfiniteEntity->SetScript(ScriptComponent{ InfiniteScriptAsset->ID }));
		ScriptRuntimeDesc LimitedDescription;
		LimitedDescription.MaxInstructionsPerCallback = 1000;
		ScriptRuntime LimitedRuntime(*ProjectResult, LimitedDescription);
		PF_CHECK(Tests, LimitedRuntime.Start(InfiniteScene).has_value());
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::UpdateFailed);
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().front().Message.find("instruction budget") != std::string::npos);
		PF_CHECK(Tests, LimitedRuntime.Advance(InfiniteScene, Timestep(1.0 / 60.0)).has_value());
		PF_CHECK(Tests, LimitedRuntime.GetDiagnostics().size() == 1);
		LimitedRuntime.Stop();

		constexpr std::string_view MemoryLimitScript = R"(
function OnCreate()
  local values = {}
  while true do
    values[#values + 1] = string.rep("x", 4096)
  end
end
)";
		const auto MemoryScriptAsset = CreateScriptAsset("memory_limit.lua", MemoryLimitScript);
		PF_CHECK(Tests, MemoryScriptAsset.has_value());
		if (!MemoryScriptAsset)
			return;
		Scene MemoryScene;
		const auto MemoryEntity = MemoryScene.CreateEntity("Memory-limited script");
		PF_CHECK(Tests, MemoryEntity && MemoryEntity->SetScript(ScriptComponent{ MemoryScriptAsset->ID }));
		ScriptRuntimeDesc MemoryDescription;
		MemoryDescription.MaxLuaMemoryBytes = 512u * 1024u;
		MemoryDescription.MaxTotalLuaMemoryBytes = 2u * 1024u * 1024u;
		ScriptRuntime MemoryRuntime(*ProjectResult, MemoryDescription);
		PF_CHECK(Tests, MemoryRuntime.Start(MemoryScene).has_value());
		PF_CHECK(Tests, MemoryRuntime.GetScriptCount() == 0 && MemoryRuntime.GetDiagnostics().size() == 1);
		PF_CHECK(Tests, MemoryRuntime.GetDiagnostics().front().Code == ScriptDiagnosticCode::InitializationFailed);
		MemoryRuntime.Stop();
	}

	void TestGltfMeshImport(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeGltfImport-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path ModelDirectory = ProjectRoot / "Assets" / "Models";
		std::filesystem::create_directories(ModelDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		std::filesystem::path SourcePath = ModelDirectory / "Triangle.gltf";
		const std::filesystem::path BufferPath = ModelDirectory / "Triangle.bin";
		const std::string GltfDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"Triangle.bin","byteLength":66}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}]
		})";
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(GltfDocument.data(), static_cast<std::streamsize>(GltfDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		std::vector<std::byte> BufferBytes;
		const auto AppendUInt32LE = [&BufferBytes](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				BufferBytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		};
		const auto AppendFloatLE = [&AppendUInt32LE](float Value)
		{
			AppendUInt32LE(std::bit_cast<uint32_t>(Value));
		};
		for (const float Value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			AppendFloatLE(Value);
		for (const float Value : { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f })
			AppendFloatLE(Value);
		for (const uint16_t Value : { 0, 1, 2 })
		{
			BufferBytes.push_back(static_cast<std::byte>(Value & 0xff));
			BufferBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, BufferBytes.size() == 66);
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BufferBytes.data()), static_cast<std::streamsize>(BufferBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}

		const auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		const auto BufferMetadata = AssetMetadataSerializer::CreateForNewAsset(BufferPath);
		PF_CHECK(Tests, SourceMetadata.has_value() && BufferMetadata.has_value());
		if (!SourceMetadata || !BufferMetadata)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Imported = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported)
			return;

		PF_CHECK(Tests, Imported->Vertices.size() == 3);
		PF_CHECK(Tests, Imported->Indices == std::vector<uint32_t>({ 0, 1, 2 }));
		PF_CHECK(Tests, Imported->Vertices[1].Position[0] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[2].TexCoord[1] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[0].Color[0] == 1.0f && Imported->Vertices[0].Color[1] == 1.0f);
		PF_CHECK(Tests, Imported->Vertices[0].Normal[0] == 0.0f && Imported->Vertices[0].Normal[1] == 0.0f &&
			Imported->Vertices[0].Normal[2] == 1.0f);
		for (const GltfMeshVertex& Vertex : Imported->Vertices)
			PF_CHECK(Tests, std::abs(glm::length(glm::vec3(Vertex.Normal[0], Vertex.Normal[1], Vertex.Normal[2])) - 1.0f) < 1.0e-6f);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes.size() == 5);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes[3].Semantic == VertexSemantic::Normal);
		PF_CHECK(Tests, Imported->GetMeshDescription().VertexLayout.Attributes[4].Semantic == VertexSemantic::Tangent);
		for (const GltfMeshVertex& Vertex : Imported->Vertices)
		{
			PF_CHECK(Tests, std::abs(Vertex.Tangent[0] - 1.0f) < 1.0e-5f &&
				std::abs(Vertex.Tangent[1]) < 1.0e-5f && std::abs(Vertex.Tangent[2]) < 1.0e-5f);
			PF_CHECK(Tests, Vertex.Tangent[3] == 1.0f);
		}
		PF_CHECK(Tests, ValidateMeshDescription(Imported->GetMeshDescription()).has_value());

		const std::filesystem::path VertexAlphaPath = ModelDirectory / "VertexColorAlpha.gltf";
		const std::filesystem::path VertexAlphaBufferPath = ModelDirectory / "VertexColorAlpha.bin";
		const std::string VertexAlphaDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"VertexColorAlpha.bin","byteLength":114}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":48},
				{"buffer":0,"byteOffset":108,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
				{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Vertex alpha","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"COLOR_0":2},"indices":3}]}]
		})";
		{
			std::ofstream Output(VertexAlphaPath, std::ios::binary | std::ios::trunc);
			Output.write(VertexAlphaDocument.data(), static_cast<std::streamsize>(VertexAlphaDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::vector<std::byte> VertexAlphaBytes;
		const auto AppendVertexAlphaFloat = [&VertexAlphaBytes](float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				VertexAlphaBytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		};
		for (const float Value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			AppendVertexAlphaFloat(Value);
		for (const float Value : { 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f })
			AppendVertexAlphaFloat(Value);
		for (const float Value : {
			1.0f, 1.0f, 1.0f, 0.0f,
			0.5f, 0.75f, 1.0f, 0.5f,
			0.25f, 0.5f, 0.75f, 1.0f })
			AppendVertexAlphaFloat(Value);
		for (const uint16_t Value : { 0, 1, 2 })
		{
			VertexAlphaBytes.push_back(static_cast<std::byte>(Value & 0xff));
			VertexAlphaBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, VertexAlphaBytes.size() == 114);
		{
			std::ofstream Output(VertexAlphaBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(VertexAlphaBytes.data()),
				static_cast<std::streamsize>(VertexAlphaBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto VertexAlphaMetadata = AssetMetadataSerializer::CreateForNewAsset(VertexAlphaPath);
		const auto VertexAlphaBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(VertexAlphaBufferPath);
		PF_CHECK(Tests, VertexAlphaMetadata.has_value() && VertexAlphaBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (VertexAlphaMetadata)
		{
			const auto VertexAlpha = GltfMeshImporter::ImportStaticPrimitive(VertexAlphaMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, VertexAlpha.has_value());
			if (VertexAlpha)
			{
				PF_CHECK(Tests, VertexAlpha->Vertices[0].Color[3] == 0.0f);
				PF_CHECK(Tests, VertexAlpha->Vertices[1].Color[0] == 0.5f && VertexAlpha->Vertices[1].Color[1] == 0.75f &&
					VertexAlpha->Vertices[1].Color[3] == 0.5f);
				PF_CHECK(Tests, VertexAlpha->Vertices[2].Color[3] == 1.0f);
				PF_CHECK(Tests, VertexAlpha->GetMeshDescription().VertexLayout.Attributes[1].Format == VertexFormat::Float4);
			}
		}

		const std::filesystem::path AuthoredTangentPath = ModelDirectory / "AuthoredTangent.gltf";
		const std::filesystem::path AuthoredTangentBufferPath = ModelDirectory / "AuthoredTangent.bin";
		const std::string AuthoredTangentDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"AuthoredTangent.bin","byteLength":114}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":36},
				{"buffer":0,"byteOffset":36,"byteLength":24},
				{"buffer":0,"byteOffset":60,"byteLength":48},
				{"buffer":0,"byteOffset":108,"byteLength":6}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},
				{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},
				{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Authored tangent","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1,"TANGENT":2},"indices":3}]}]
		})";
		{
			std::ofstream Output(AuthoredTangentPath, std::ios::binary | std::ios::trunc);
			Output.write(AuthoredTangentDocument.data(), static_cast<std::streamsize>(AuthoredTangentDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::vector<std::byte> AuthoredTangentBytes(BufferBytes.begin(), BufferBytes.begin() + 60);
		const auto AppendAuthoredTangentFloat = [&AuthoredTangentBytes](float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				AuthoredTangentBytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		};
		for (const float Value : { 1.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 0.0f, -1.0f })
			AppendAuthoredTangentFloat(Value);
		AuthoredTangentBytes.insert(AuthoredTangentBytes.end(), BufferBytes.begin() + 60, BufferBytes.end());
		PF_CHECK(Tests, AuthoredTangentBytes.size() == 114);
		{
			std::ofstream Output(AuthoredTangentBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(AuthoredTangentBytes.data()),
				static_cast<std::streamsize>(AuthoredTangentBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto AuthoredTangentMetadata = AssetMetadataSerializer::CreateForNewAsset(AuthoredTangentPath);
		const auto AuthoredTangentBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(AuthoredTangentBufferPath);
		PF_CHECK(Tests, AuthoredTangentMetadata.has_value() && AuthoredTangentBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (AuthoredTangentMetadata)
		{
			const auto AuthoredTangent = GltfMeshImporter::ImportStaticPrimitive(
				AuthoredTangentMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, AuthoredTangent.has_value());
			if (AuthoredTangent)
			{
				PF_CHECK(Tests, AuthoredTangent->Vertices.size() == 3);
				for (const GltfMeshVertex& Vertex : AuthoredTangent->Vertices)
					PF_CHECK(Tests, std::abs(Vertex.Tangent[0] - 1.0f) < 1.0e-5f &&
						std::abs(Vertex.Tangent[1]) < 1.0e-5f && std::abs(Vertex.Tangent[2]) < 1.0e-5f &&
						Vertex.Tangent[3] == -1.0f);
			}
		}

		const std::filesystem::path MirroredPath = ModelDirectory / "MirroredUv.gltf";
		const std::filesystem::path MirroredBufferPath = ModelDirectory / "MirroredUv.bin";
		const std::string MirroredDocument = R"({
			"asset":{"version":"2.0"},
			"buffers":[{"uri":"MirroredUv.bin","byteLength":92}],
			"bufferViews":[
				{"buffer":0,"byteOffset":0,"byteLength":48},
				{"buffer":0,"byteOffset":48,"byteLength":32},
				{"buffer":0,"byteOffset":80,"byteLength":12}],
			"accessors":[
				{"bufferView":0,"componentType":5126,"count":4,"type":"VEC3"},
				{"bufferView":1,"componentType":5126,"count":4,"type":"VEC2"},
				{"bufferView":2,"componentType":5123,"count":6,"type":"SCALAR"}],
			"meshes":[{"name":"Mirrored UV seam","primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2}]}]
		})";
		{
			std::ofstream Output(MirroredPath, std::ios::binary | std::ios::trunc);
			Output.write(MirroredDocument.data(), static_cast<std::streamsize>(MirroredDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		std::vector<std::byte> MirroredBytes;
		const auto AppendMirroredFloatLE = [&MirroredBytes](float Value)
		{
			const uint32_t Bits = std::bit_cast<uint32_t>(Value);
			for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
				MirroredBytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
		};
		for (const float Value : {
			0.0f, 0.0f, 0.0f,  1.0f, 0.0f, 0.0f,  0.0f, 1.0f, 0.0f,  1.0f, 1.0f, 0.0f,
			0.0f, 0.0f,  1.0f, 0.0f,  0.0f, 1.0f,  0.0f, 0.0f })
			AppendMirroredFloatLE(Value);
		for (const uint16_t Value : { 0, 1, 2, 1, 3, 2 })
		{
			MirroredBytes.push_back(static_cast<std::byte>(Value & 0xff));
			MirroredBytes.push_back(static_cast<std::byte>(Value >> 8));
		}
		PF_CHECK(Tests, MirroredBytes.size() == 92);
		{
			std::ofstream Output(MirroredBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(MirroredBytes.data()), static_cast<std::streamsize>(MirroredBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto MirroredMetadata = AssetMetadataSerializer::CreateForNewAsset(MirroredPath);
		const auto MirroredBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(MirroredBufferPath);
		PF_CHECK(Tests, MirroredMetadata.has_value() && MirroredBufferMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (MirroredMetadata)
		{
			const auto Mirrored = GltfMeshImporter::ImportStaticPrimitive(MirroredMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, Mirrored.has_value());
			if (Mirrored)
			{
				PF_CHECK(Tests, Mirrored->Vertices.size() == 6);
				PF_CHECK(Tests, Mirrored->Indices[0] != Mirrored->Indices[3] && Mirrored->Indices[1] != Mirrored->Indices[5]);
				PF_CHECK(Tests, Mirrored->Vertices[Mirrored->Indices[0]].Tangent[3] == 1.0f);
				PF_CHECK(Tests, Mirrored->Vertices[Mirrored->Indices[3]].Tangent[3] == -1.0f);
				PF_CHECK(Tests, ValidateMeshDescription(Mirrored->GetMeshDescription()).has_value());
			}
		}
		std::vector<std::byte> DegenerateBytes = BufferBytes;
		std::fill_n(DegenerateBytes.begin(), sizeof(float) * 9, std::byte{ 0 });
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(DegenerateBytes.data()), static_cast<std::streamsize>(DegenerateBytes.size()));
		}
		const auto DegenerateImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, DegenerateImport.has_value());
		if (DegenerateImport)
			for (const GltfMeshVertex& Vertex : DegenerateImport->Vertices)
				PF_CHECK(Tests, Vertex.Normal[0] == 0.0f && Vertex.Normal[1] == 0.0f && Vertex.Normal[2] == 1.0f);
		{
			std::ofstream Output(BufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(BufferBytes.data()), static_cast<std::streamsize>(BufferBytes.size()));
		}

		std::string ExplicitNormalDocument = GltfDocument;
		const size_t ExplicitBufferUri = ExplicitNormalDocument.find("Triangle.bin");
		PF_CHECK(Tests, ExplicitBufferUri != std::string::npos);
		if (ExplicitBufferUri != std::string::npos)
			ExplicitNormalDocument.replace(ExplicitBufferUri, std::string("Triangle.bin").size(), "ExplicitNormals.bin");
		ExplicitNormalDocument.replace(ExplicitNormalDocument.find("\"byteLength\":66"), 15, "\"byteLength\":104");
		ExplicitNormalDocument.replace(
			ExplicitNormalDocument.find("{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}"),
			std::string("{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6}").size(),
			"{\"buffer\":0,\"byteOffset\":60,\"byteLength\":6},{\"buffer\":0,\"byteOffset\":68,\"byteLength\":36}");
		ExplicitNormalDocument.replace(
			ExplicitNormalDocument.find("{\"bufferView\":2,\"componentType\":5123,"),
			std::string("{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}").size(),
			"{\"bufferView\":2,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"},"
			"{\"bufferView\":3,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}");
		ExplicitNormalDocument.replace(
			ExplicitNormalDocument.find("\"POSITION\":0,\"TEXCOORD_0\":1"),
			std::string("\"POSITION\":0,\"TEXCOORD_0\":1").size(),
			"\"POSITION\":0,\"TEXCOORD_0\":1,\"NORMAL\":3");
		const std::filesystem::path ExplicitNormalPath = ModelDirectory / "ExplicitNormals.gltf";
		std::vector<std::byte> ExplicitNormalBytes = BufferBytes;
		BufferBytes.push_back(std::byte{ 0 });
		BufferBytes.push_back(std::byte{ 0 });
		for (const float Value : { 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 2.0f })
			AppendFloatLE(Value);
		ExplicitNormalBytes = BufferBytes;
		BufferBytes.resize(66);
		const std::filesystem::path ExplicitNormalBufferPath = ModelDirectory / "ExplicitNormals.bin";
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(ExplicitNormalDocument.data(), static_cast<std::streamsize>(ExplicitNormalDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		{
			std::ofstream Output(ExplicitNormalBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(ExplicitNormalBytes.data()),
				static_cast<std::streamsize>(ExplicitNormalBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ExplicitNormalMetadata = AssetMetadataSerializer::CreateForNewAsset(ExplicitNormalPath);
		const auto ExplicitNormalBufferMetadata = AssetMetadataSerializer::CreateForNewAsset(ExplicitNormalBufferPath);
		PF_CHECK(Tests, ExplicitNormalMetadata.has_value());
		PF_CHECK(Tests, ExplicitNormalBufferMetadata.has_value());
		const auto ExplicitRegistryResult = Registry.Rebuild(ProjectRoot);
		PF_CHECK(Tests, ExplicitRegistryResult.has_value());
		if (!ExplicitNormalMetadata)
			return;
		const auto ExplicitNormals = GltfMeshImporter::ImportStaticPrimitive(
			ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ExplicitNormals.has_value());
		PF_CHECK(Tests, ExplicitNormals && std::abs(ExplicitNormals->Vertices[0].Normal[2] - 1.0f) < 1.0e-6f);
		std::string InvalidNormalDocument = ExplicitNormalDocument;
		const size_t NormalAccessor = InvalidNormalDocument.find("\"bufferView\":3,\"componentType\":5126");
		const size_t NormalType = InvalidNormalDocument.find("\"type\":\"VEC3\"", NormalAccessor);
		PF_CHECK(Tests, NormalAccessor != std::string::npos && NormalType != std::string::npos);
		if (NormalType == std::string::npos)
			return;
		InvalidNormalDocument.replace(NormalType + std::string("\"type\":\"").size(), 4, "VEC2");
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(InvalidNormalDocument.data(), static_cast<std::streamsize>(InvalidNormalDocument.size()));
		}
		const auto InvalidNormals = GltfMeshImporter::ImportStaticPrimitive(
			ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !InvalidNormals && InvalidNormals.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);
		{
			std::ofstream Output(ExplicitNormalPath, std::ios::binary | std::ios::trunc);
			Output.write(ExplicitNormalDocument.data(), static_cast<std::streamsize>(ExplicitNormalDocument.size()));
		}
		ExplicitNormalBytes[68] = static_cast<std::byte>(0x00);
		ExplicitNormalBytes[69] = static_cast<std::byte>(0x00);
		ExplicitNormalBytes[70] = static_cast<std::byte>(0xc0);
		ExplicitNormalBytes[71] = static_cast<std::byte>(0x7f);
		{
			std::ofstream Output(ExplicitNormalBufferPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(ExplicitNormalBytes.data()),
				static_cast<std::streamsize>(ExplicitNormalBytes.size()));
		}
		const auto NonFiniteNormals = GltfMeshImporter::ImportStaticPrimitive(
			ExplicitNormalMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !NonFiniteNormals && NonFiniteNormals.error().Code == GltfMeshImportErrorCode::InvalidAccessor);

		const std::filesystem::path RenamedSourcePath = ModelDirectory / "RenamedTriangle.gltf";
		const auto MovedSource = AssetOperations::Move(
			Registry,
			ProjectRoot,
			SourceMetadata->ID,
			"Assets/Models/RenamedTriangle.gltf");
		PF_CHECK(Tests, MovedSource.has_value() && MovedSource->ID == SourceMetadata->ID);
		if (!MovedSource)
			return;
		SourcePath = RenamedSourcePath;
		const auto ImportedAfterMove = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedAfterMove.has_value());
		PF_CHECK(Tests, ImportedAfterMove && ImportedAfterMove->Vertices.size() == 3);

		std::string EmbeddedGltfDocument = GltfDocument;
		const size_t EmbeddedUriPosition = EmbeddedGltfDocument.find("Triangle.bin");
		PF_CHECK(Tests, EmbeddedUriPosition != std::string::npos);
		if (EmbeddedUriPosition == std::string::npos)
			return;
		EmbeddedGltfDocument.replace(
			EmbeddedUriPosition,
			std::string("Triangle.bin").size(),
			"data:application/octet-stream;base64,AAAAAAAAAAAAAAAAAACAPwAAAAAAAAAAAAAAAAAAgD8AAAAAAAAAAAAAAAAAAIA/AAAAAAAAAAAAAIA/AAABAAIA");
		const std::filesystem::path EmbeddedSourcePath = ModelDirectory / "Embedded.gltf";
		{
			std::ofstream Output(EmbeddedSourcePath, std::ios::binary | std::ios::trunc);
			Output.write(EmbeddedGltfDocument.data(), static_cast<std::streamsize>(EmbeddedGltfDocument.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto EmbeddedMetadata = AssetMetadataSerializer::CreateForNewAsset(EmbeddedSourcePath);
		PF_CHECK(Tests, EmbeddedMetadata.has_value());
		if (!EmbeddedMetadata)
			return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto ImportedEmbedded = GltfMeshImporter::ImportStaticPrimitive(EmbeddedMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedEmbedded && ImportedEmbedded->Vertices.size() == 3 && ImportedEmbedded->Indices.size() == 3);

		std::string GlbDocument = GltfDocument;
		const size_t ExternalUriPosition = GlbDocument.find("\"uri\":\"Triangle.bin\",");
		PF_CHECK(Tests, ExternalUriPosition != std::string::npos);
		if (ExternalUriPosition == std::string::npos)
			return;
		GlbDocument.erase(ExternalUriPosition, std::string("\"uri\":\"Triangle.bin\",").size());
		while (GlbDocument.size() % 4 != 0)
			GlbDocument.push_back(' ');

		std::vector<std::byte> GlbBytes;
		const auto AppendGlbUInt32 = [&GlbBytes](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				GlbBytes.push_back(static_cast<std::byte>((Value >> (Byte * 8)) & 0xff));
		};
		const uint32_t PaddedBinaryLength = static_cast<uint32_t>((BufferBytes.size() + 3) & ~size_t(3));
		const uint32_t JsonLength = static_cast<uint32_t>(GlbDocument.size());
		AppendGlbUInt32(0x46546C67);
		AppendGlbUInt32(2);
		AppendGlbUInt32(12 + 8 + JsonLength + 8 + PaddedBinaryLength);
		AppendGlbUInt32(JsonLength);
		AppendGlbUInt32(0x4E4F534A);
		for (const char Character : GlbDocument)
			GlbBytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
		AppendGlbUInt32(PaddedBinaryLength);
		AppendGlbUInt32(0x004E4942);
		GlbBytes.insert(GlbBytes.end(), BufferBytes.begin(), BufferBytes.end());
		while (GlbBytes.size() % 4 != 0)
			GlbBytes.push_back(std::byte{ 0 });

		const std::filesystem::path GlbPath = ModelDirectory / "Triangle.glb";
		{
			std::ofstream Output(GlbPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(GlbBytes.data()), static_cast<std::streamsize>(GlbBytes.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto GlbMetadata = AssetMetadataSerializer::CreateForNewAsset(GlbPath);
		PF_CHECK(Tests, GlbMetadata.has_value());
		if (!GlbMetadata)
			return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto ImportedGlb = GltfMeshImporter::ImportStaticPrimitive(GlbMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, ImportedGlb.has_value());
		PF_CHECK(Tests, ImportedGlb && ImportedGlb->Vertices.size() == 3 && ImportedGlb->Indices.size() == 3);

		std::vector<std::byte> OverpaddedGlb = GlbBytes;
		const size_t BinaryChunkHeaderOffset = 20 + JsonLength;
		const auto WriteUInt32LE = [&OverpaddedGlb](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				OverpaddedGlb[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};
		WriteUInt32LE(8, static_cast<uint32_t>(OverpaddedGlb.size() + 4));
		WriteUInt32LE(BinaryChunkHeaderOffset, PaddedBinaryLength + 4);
		OverpaddedGlb.insert(OverpaddedGlb.end(), 4, std::byte{ 0 });
		const std::filesystem::path OverpaddedGlbPath = ModelDirectory / "Overpadded.glb";
		{
			std::ofstream Output(OverpaddedGlbPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(OverpaddedGlb.data()),
				static_cast<std::streamsize>(OverpaddedGlb.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OverpaddedGlbMetadata = AssetMetadataSerializer::CreateForNewAsset(OverpaddedGlbPath);
		PF_CHECK(Tests, OverpaddedGlbMetadata.has_value());
		if (!OverpaddedGlbMetadata)
			return;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto OverpaddedGlbImport = GltfMeshImporter::ImportStaticPrimitive(
			OverpaddedGlbMetadata->ID,
			ProjectRoot,
			Registry);
		PF_CHECK(Tests, !OverpaddedGlbImport.has_value());
		PF_CHECK(Tests, !OverpaddedGlbImport &&
			OverpaddedGlbImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string OverflowingAccessor = GltfDocument;
		const size_t AccessorsStart = OverflowingAccessor.find("\"accessors\"");
		const size_t FirstAccessor = OverflowingAccessor.find("{\"bufferView\":0,", AccessorsStart);
		PF_CHECK(Tests, AccessorsStart != std::string::npos && FirstAccessor != std::string::npos);
		if (AccessorsStart == std::string::npos || FirstAccessor == std::string::npos)
			return;
		OverflowingAccessor.replace(
			FirstAccessor,
			std::string("{\"bufferView\":0,").size(),
			"{\"bufferView\":0,\"byteOffset\":18446744073709551615,");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(OverflowingAccessor.data(), static_cast<std::streamsize>(OverflowingAccessor.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OverflowingAccessorImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !OverflowingAccessorImport.has_value());
		PF_CHECK(Tests, !OverflowingAccessorImport &&
			OverflowingAccessorImport.error().Code == GltfMeshImportErrorCode::InvalidAccessor);

		std::string SparseAccessor = GltfDocument;
		const size_t SparseFirstAccessor = SparseAccessor.find("{\"bufferView\":0,", SparseAccessor.find("\"accessors\""));
		SparseAccessor.replace(
			SparseFirstAccessor,
			std::string("{\"bufferView\":0,").size(),
			"{\"bufferView\":0,\"sparse\":{},");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(SparseAccessor.data(), static_cast<std::streamsize>(SparseAccessor.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto SparseAccessorImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !SparseAccessorImport.has_value());
		PF_CHECK(Tests, !SparseAccessorImport && SparseAccessorImport.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);

		std::string RequiredExtension = GltfDocument;
		const size_t AssetHeaderPosition = RequiredExtension.find("\"asset\":{\"version\":\"2.0\"},");
		PF_CHECK(Tests, AssetHeaderPosition != std::string::npos);
		if (AssetHeaderPosition == std::string::npos)
			return;
		RequiredExtension.replace(
			AssetHeaderPosition,
			std::string("\"asset\":{\"version\":\"2.0\"},").size(),
			"\"asset\":{\"version\":\"2.0\"},\"extensionsRequired\":[\"EXT_unknown_required\"],");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(RequiredExtension.data(), static_cast<std::streamsize>(RequiredExtension.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto RequiredExtensionImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !RequiredExtensionImport.has_value());
		PF_CHECK(Tests, !RequiredExtensionImport &&
			RequiredExtensionImport.error().Code == GltfMeshImportErrorCode::UnsupportedFeature);

		std::string NullBufferUri = GltfDocument;
		const size_t NullUriPosition = NullBufferUri.find("Triangle.bin");
		PF_CHECK(Tests, NullUriPosition != std::string::npos);
		if (NullUriPosition == std::string::npos)
			return;
		NullBufferUri.replace(NullUriPosition, std::string("Triangle.bin").size(), "Triangle\\u0000.bin");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(NullBufferUri.data(), static_cast<std::streamsize>(NullBufferUri.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto NullUriImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !NullUriImport.has_value());
		PF_CHECK(Tests, !NullUriImport && NullUriImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string AmbiguousBufferUri = GltfDocument;
		const size_t AmbiguousUriPosition = AmbiguousBufferUri.find("Triangle.bin");
		PF_CHECK(Tests, AmbiguousUriPosition != std::string::npos);
		if (AmbiguousUriPosition == std::string::npos)
			return;
		AmbiguousBufferUri.replace(AmbiguousUriPosition, std::string("Triangle.bin").size(), "Triangle.bin.");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(AmbiguousBufferUri.data(), static_cast<std::streamsize>(AmbiguousBufferUri.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto AmbiguousUriImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !AmbiguousUriImport.has_value());
		PF_CHECK(Tests, !AmbiguousUriImport &&
			AmbiguousUriImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string TruncatedBufferReference = GltfDocument;
		const size_t BufferLengthPosition = TruncatedBufferReference.find("\"byteLength\":66");
		PF_CHECK(Tests, BufferLengthPosition != std::string::npos);
		if (BufferLengthPosition != std::string::npos)
			TruncatedBufferReference.replace(BufferLengthPosition, std::string("\"byteLength\":66").size(), "\"byteLength\":65");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(TruncatedBufferReference.data(), static_cast<std::streamsize>(TruncatedBufferReference.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto TruncatedImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !TruncatedImport.has_value());
		PF_CHECK(Tests, !TruncatedImport && TruncatedImport.error().Code == GltfMeshImportErrorCode::InvalidBuffer);

		std::string EscapingBufferReference = GltfDocument;
		const size_t UriPosition = EscapingBufferReference.find("Triangle.bin");
		PF_CHECK(Tests, UriPosition != std::string::npos);
		if (UriPosition != std::string::npos)
			EscapingBufferReference.replace(UriPosition, std::string("Triangle.bin").size(), "../../outside.bin");
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(EscapingBufferReference.data(), static_cast<std::streamsize>(EscapingBufferReference.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto EscapingImport = GltfMeshImporter::ImportStaticPrimitive(SourceMetadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, !EscapingImport.has_value());
		PF_CHECK(Tests, !EscapingImport && EscapingImport.error().Code == GltfMeshImportErrorCode::InvalidProjectPath);
	}

	void TestImageAssetImport(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeImageImport-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path TextureDirectory = ProjectRoot / "Assets" / "Textures";
		std::filesystem::create_directories(TextureDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		std::vector<uint8_t> Bitmap;
		const auto AppendUInt16 = [&Bitmap](uint16_t Value)
		{
			Bitmap.push_back(static_cast<uint8_t>(Value & 0xff));
			Bitmap.push_back(static_cast<uint8_t>(Value >> 8));
		};
		const auto AppendUInt32 = [&Bitmap](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				Bitmap.push_back(static_cast<uint8_t>((Value >> (Byte * 8)) & 0xff));
		};
		Bitmap.push_back('B');
		Bitmap.push_back('M');
		AppendUInt32(70);
		AppendUInt32(0);
		AppendUInt32(54);
		AppendUInt32(40);
		AppendUInt32(2);
		AppendUInt32(2);
		AppendUInt16(1);
		AppendUInt16(24);
		AppendUInt32(0);
		AppendUInt32(16);
		AppendUInt32(0);
		AppendUInt32(0);
		AppendUInt32(0);
		AppendUInt32(0);
		// BMP storage is bottom-up and BGR: bottom row red/green, top row blue/white.
		Bitmap.insert(Bitmap.end(), { 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0 });
		PF_CHECK(Tests, Bitmap.size() == 70);

		const std::filesystem::path SourcePath = TextureDirectory / "checker.bmp";
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Imported = ImageAssetImporter::ImportRGBA8(Metadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported)
			return;
		PF_CHECK(Tests, Imported->Width == 2 && Imported->Height == 2);
		PF_CHECK(Tests, Imported->RGBA8Pixels.size() == 16);
		PF_CHECK(Tests, Imported->RGBA8Pixels == std::vector<uint8_t>({
			0, 0, 255, 255, 255, 255, 255, 255,
			255, 0, 0, 255, 0, 255, 0, 255 }));
		const auto Preview = ImageAssetImporter::DecodePreviewRGBA8(SourcePath, 192);
		PF_CHECK(Tests, Preview && Preview->Width == 2 && Preview->Height == 2);
		PF_CHECK(Tests, Preview && Preview->RGBA8Pixels == Imported->RGBA8Pixels);
		PF_CHECK(Tests, Preview && Preview->SourceFingerprint == Imported->SourceFingerprint);
		const auto ZeroEdgePreview = ImageAssetImporter::DecodePreviewRGBA8(SourcePath, 0);
		PF_CHECK(Tests, !ZeroEdgePreview &&
			ZeroEdgePreview.error().Code == ImageAssetImportErrorCode::ResourceLimitExceeded);

		const auto MissingAssetID = UUID::Generate();
		PF_CHECK(Tests, MissingAssetID.has_value());
		if (MissingAssetID)
		{
			const auto Missing = ImageAssetImporter::ImportRGBA8(*MissingAssetID, ProjectRoot, Registry);
			PF_CHECK(Tests, !Missing && Missing.error().Code == ImageAssetImportErrorCode::AssetNotFound);
		}

		const std::filesystem::path RenamedPath = TextureDirectory / "checker-renamed.bmp";
		const auto Move = AssetOperations::Move(
			Registry,
			ProjectRoot,
			Metadata->ID,
			"Assets/Textures/checker-renamed.bmp");
		PF_CHECK(Tests, Move.has_value() && Move->ID == Metadata->ID);
		if (Move)
		{
			const auto ImportedAfterMove = ImageAssetImporter::ImportRGBA8(Metadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, ImportedAfterMove.has_value());
			PF_CHECK(Tests, ImportedAfterMove && ImportedAfterMove->RGBA8Pixels == Imported->RGBA8Pixels);
		}
		PF_CHECK(Tests, std::filesystem::exists(RenamedPath));
		const auto PreviewAfterMove = ImageAssetImporter::DecodePreviewRGBA8(RenamedPath);
		PF_CHECK(Tests, PreviewAfterMove && PreviewAfterMove->SourceFingerprint == Imported->SourceFingerprint);
		Bitmap.back() ^= 0x7fu;
		{
			std::ofstream Output(RenamedPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ReplacedPreview = ImageAssetImporter::DecodePreviewRGBA8(RenamedPath);
		PF_CHECK(Tests, ReplacedPreview && ReplacedPreview->SourceFingerprint != Imported->SourceFingerprint);

		const std::filesystem::path LargePreviewPath = TextureDirectory / "wide.ppm";
		{
			const std::string Header = "P6\n400 200\n255\n";
			std::vector<uint8_t> Ppm(Header.begin(), Header.end());
			Ppm.reserve(Ppm.size() + 400u * 200u * 3u);
			for (uint32_t Y = 0; Y < 200; ++Y)
				for (uint32_t X = 0; X < 400; ++X)
				{
					Ppm.push_back(static_cast<uint8_t>(X & 0xffu));
					Ppm.push_back(static_cast<uint8_t>(Y & 0xffu));
					Ppm.push_back(127);
				}
			std::ofstream Output(LargePreviewPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Ppm.data()), static_cast<std::streamsize>(Ppm.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(LargePreviewPath).has_value());
		const auto DownsampledPreview = ImageAssetImporter::DecodePreviewRGBA8(LargePreviewPath, 192);
		PF_CHECK(Tests, DownsampledPreview && DownsampledPreview->Width == 192 && DownsampledPreview->Height == 96);
		PF_CHECK(Tests, DownsampledPreview && DownsampledPreview->RGBA8Pixels.size() == 192u * 96u * 4u);

		const std::filesystem::path AlphaPreviewPath = TextureDirectory / "alpha.tga";
		{
			std::array<uint8_t, 26> Tga{};
			Tga[2] = 2;
			Tga[12] = 2;
			Tga[14] = 1;
			Tga[16] = 32;
			Tga[17] = 0x28;
			Tga[18] = 255;	// Blue, fully transparent.
			Tga[23] = 0;
			Tga[24] = 255;
			Tga[25] = 128;
			std::ofstream Output(AlphaPreviewPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Tga.data()), static_cast<std::streamsize>(Tga.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(AlphaPreviewPath).has_value());
		const auto AlphaPreview = ImageAssetImporter::DecodePreviewRGBA8(AlphaPreviewPath);
		PF_CHECK(Tests, AlphaPreview && AlphaPreview->Width == 2 && AlphaPreview->Height == 1);
		PF_CHECK(Tests, AlphaPreview && AlphaPreview->RGBA8Pixels[0] == 0 && AlphaPreview->RGBA8Pixels[2] == 255 &&
			AlphaPreview->RGBA8Pixels[3] == 0 && AlphaPreview->RGBA8Pixels[4] == 255 && AlphaPreview->RGBA8Pixels[7] == 128);

		const std::filesystem::path InvalidImagePath = TextureDirectory / "invalid.bmp";
		{
			std::ofstream Output(InvalidImagePath, std::ios::binary | std::ios::trunc);
			Output.write("not an image", 12);
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto InvalidImageMetadata = AssetMetadataSerializer::CreateForNewAsset(InvalidImagePath);
		PF_CHECK(Tests, InvalidImageMetadata.has_value());
		const auto InvalidPreview = ImageAssetImporter::DecodePreviewRGBA8(InvalidImagePath);
		PF_CHECK(Tests, !InvalidPreview && InvalidPreview.error().Code == ImageAssetImportErrorCode::InvalidImage);
		if (InvalidImageMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto InvalidImage = ImageAssetImporter::ImportRGBA8(InvalidImageMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !InvalidImage && InvalidImage.error().Code == ImageAssetImportErrorCode::InvalidImage);
		}

		const std::filesystem::path HdrPath = TextureDirectory / "tiny.hdr";
		{
			constexpr std::string_view HdrHeader = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
			std::ofstream Output(HdrPath, std::ios::binary | std::ios::trunc);
			Output.write(HdrHeader.data(), static_cast<std::streamsize>(HdrHeader.size()));
			constexpr std::array<uint8_t, 8> RgbePixels = { 128, 64, 32, 130, 128, 128, 128, 129 };
			Output.write(reinterpret_cast<const char*>(RgbePixels.data()), static_cast<std::streamsize>(RgbePixels.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto HdrMetadata = AssetMetadataSerializer::CreateForNewAsset(HdrPath);
		PF_CHECK(Tests, HdrMetadata.has_value());
		if (HdrMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto ImportedHdr = ImageAssetImporter::ImportRGBA32F(HdrMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, ImportedHdr.has_value());
			if (ImportedHdr)
			{
				PF_CHECK(Tests, ImportedHdr->Width == 2 && ImportedHdr->Height == 1);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels.size() == 8);
				PF_CHECK(Tests, std::abs(ImportedHdr->RGBA32FPixels[0] - 2.0f) < 0.001f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[1] > 0.99f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[2] > 0.49f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[3] == 1.0f);
			}
			const auto RejectedLdrAsHdr = ImageAssetImporter::ImportRGBA32F(Metadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !RejectedLdrAsHdr && RejectedLdrAsHdr.error().Code == ImageAssetImportErrorCode::InvalidImage);
		}
	}

	std::vector<std::byte> MakeSilentWave(uint32_t SampleCount)
	{
		constexpr uint32_t Channels = 1;
		constexpr uint32_t SampleRate = 44100;
		constexpr uint16_t BitsPerSample = 16;
		constexpr uint16_t BlockAlign = Channels * (BitsPerSample / 8);
		const uint32_t DataSize = SampleCount * BlockAlign;
		std::vector<std::byte> Data(44 + DataSize);

		auto WriteFourCC = [&Data](size_t Offset, std::string_view Value)
		{
			for (size_t Index = 0; Index < Value.size(); ++Index)
				Data[Offset + Index] = static_cast<std::byte>(static_cast<unsigned char>(Value[Index]));
		};
		auto Write16 = [&Data](size_t Offset, uint16_t Value)
		{
			Data[Offset] = static_cast<std::byte>(Value & 0xff);
			Data[Offset + 1] = static_cast<std::byte>((Value >> 8) & 0xff);
		};
		auto Write32 = [&Data](size_t Offset, uint32_t Value)
		{
			for (size_t Byte = 0; Byte < 4; ++Byte)
				Data[Offset + Byte] = static_cast<std::byte>((Value >> (Byte * 8)) & 0xff);
		};

		WriteFourCC(0, "RIFF");
		Write32(4, static_cast<uint32_t>(Data.size() - 8));
		WriteFourCC(8, "WAVE");
		WriteFourCC(12, "fmt ");
		Write32(16, 16);
		Write16(20, 1);
		Write16(22, Channels);
		Write32(24, SampleRate);
		Write32(28, SampleRate * BlockAlign);
		Write16(32, BlockAlign);
		Write16(34, BitsPerSample);
		WriteFourCC(36, "data");
		Write32(40, DataSize);
		return Data;
	}

	void TestAudioEngineAndAssetCache(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeAudio-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		std::filesystem::create_directories(ProjectRoot / "Assets" / "Audio", FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;
		auto AudioProject = Project::Create(ProjectRoot / "AudioProject.pfproj", "Audio Script Tests");
		PF_CHECK(Tests, AudioProject.has_value());
		if (!AudioProject)
			return;

		const std::vector<std::byte> WaveData = MakeSilentWave(44100 * 5);
		AudioPlayback Playback;
		{
			auto CreatedEngine = AudioEngine::Create({ AudioOutputBackend::Null });
			PF_CHECK(Tests, CreatedEngine.has_value());
			if (!CreatedEngine)
				return;
			AudioEngine Engine = std::move(*CreatedEngine);
			PF_CHECK(Tests, Engine.IsInitialized());

			const std::array<std::byte, 4> InvalidData{};
			const auto InvalidClip = Engine.CreateClip("Invalid", InvalidData);
			PF_CHECK(Tests, !InvalidClip && InvalidClip.error().Code == AudioErrorCode::InvalidClip);
			const auto Clip = Engine.CreateClip("Test WAV", WaveData);
			PF_CHECK(Tests, Clip.has_value());
			if (!Clip)
				return;
			PF_CHECK(Tests, Clip->IsValid() && Clip->GetDebugName() == "Test WAV");

			AudioPlaybackDesc InvalidDescription;
			InvalidDescription.Volume = 1.1f;
			PF_CHECK(Tests, !InvalidDescription.Validate());
			PF_CHECK(Tests, !Engine.Play(*Clip, InvalidDescription));
			AudioPlaybackDesc Description;
			Description.Spatialized = true;
			Description.Position = { 2.0f, 0.0f, -1.0f };
			Description.Volume = 0.5f;
			auto StartedPlayback = Engine.Play(*Clip, Description);
			PF_CHECK(Tests, StartedPlayback.has_value());
			if (!StartedPlayback)
				return;
			Playback = std::move(*StartedPlayback);
			PF_CHECK(Tests, Playback.IsValid());
			auto ConcurrentPlayback = Engine.Play(*Clip, Description);
			PF_CHECK(Tests, ConcurrentPlayback.has_value() && ConcurrentPlayback->IsValid());
			if (ConcurrentPlayback)
				PF_CHECK(Tests, ConcurrentPlayback->Stop().has_value());
			PF_CHECK(Tests, Playback.Pause().has_value());
			PF_CHECK(Tests, Playback.Resume().has_value());
			PF_CHECK(Tests, Playback.SetVolume(0.25f).has_value());
			PF_CHECK(Tests, Playback.SetLooping(true).has_value());
			PF_CHECK(Tests, Playback.SetSpatialized(false).has_value());
			PF_CHECK(Tests, Playback.SetPosition({ 0.0f, 1.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Playback.Stop().has_value());
			PF_CHECK(Tests, Engine.SetListenerPosition({ 0.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Engine.SetListenerDirection({ 0.0f, 0.0f, -1.0f }).has_value());
			PF_CHECK(Tests, !Engine.SetListenerDirection({ 0.0f, 0.0f, 0.0f }));

			AssetRegistry& Registry = AudioProject->GetAssetRegistry();
			auto Imported = AssetOperations::CreateAssetFromBytes(
				Registry,
				ProjectRoot,
				WaveData,
				"Assets/Audio/test.wav");
			PF_CHECK(Tests, Imported.has_value());
			if (!Imported)
				return;

			AudioAssetCache Cache(Engine, ProjectRoot, Registry);
			const auto Loaded = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, Loaded && Loaded->IsValid());
			PF_CHECK(Tests, Cache.GetLoadedCount() == 1);
			const auto CachedAgain = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, CachedAgain && Cache.GetLoadedCount() == 1);

			const auto Moved = AssetOperations::Move(
				Registry,
				ProjectRoot,
				Imported->ID,
				"Assets/Audio/renamed.wav");
			PF_CHECK(Tests, Moved && Moved->ID == Imported->ID);
			Cache.Clear();
			const auto LoadedAfterMove = Cache.GetOrLoad(Imported->ID);
			PF_CHECK(Tests, LoadedAfterMove && LoadedAfterMove->IsValid());
			const auto MovedRecord = Registry.Find(Imported->ID);
			PF_CHECK(Tests, MovedRecord && MovedRecord->ProjectRelativePath == "Assets/Audio/renamed.wav");

			Scene AudioScene;
			auto EmitterResult = AudioScene.CreateEntity("Audio emitter and listener");
			PF_CHECK(Tests, EmitterResult.has_value());
			if (!EmitterResult)
				return;
			Entity Emitter = *EmitterResult;
			PF_CHECK(Tests, Emitter.SetAudioSource(AudioSourceComponent{ Imported->ID, 0.5f, true, true, true }).has_value());
			PF_CHECK(Tests, Emitter.SetAudioListener(AudioListenerComponent{}).has_value());

			AudioSceneRuntime Runtime(Engine, Cache);
			PF_CHECK(Tests, Runtime.Start(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.IsRunning() && Runtime.GetPlaybackCount() == 1);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			Scene OtherAudioScene;
			PF_CHECK(Tests, !Runtime.Advance(OtherAudioScene));
			PF_CHECK(Tests, Runtime.Pause(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.Resume(Emitter.GetUUID()).has_value());

			AudioSourceComponent UpdatedSource{ Imported->ID, 0.25f, false, true, false };
			PF_CHECK(Tests, Emitter.SetAudioSource(UpdatedSource).has_value());
			TransformComponent MovedEmitter;
			MovedEmitter.Translation = { 3.0f, 2.0f, 1.0f };
			PF_CHECK(Tests, Emitter.SetTransform(MovedEmitter).has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value());
			PF_CHECK(Tests, Runtime.StopPlayback(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 0);
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 0);
			PF_CHECK(Tests, Runtime.Play(Emitter.GetUUID()).has_value());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);

			std::filesystem::create_directories(ProjectRoot / "Assets" / "Scripts", FileError);
			PF_CHECK(Tests, !FileError);
			if (FileError)
				return;
			constexpr std::string_view AudioControlSource = R"(
function OnUpdate()
  assert(entity:stop_audio())
  assert(entity:play_audio())
  assert(entity:pause_audio())
  assert(entity:resume_audio())
end
)";
			const auto AudioControlBytes = std::as_bytes(std::span(AudioControlSource.data(), AudioControlSource.size()));
			const auto AudioControlAsset = AssetOperations::CreateAssetFromBytes(
				Registry,
				ProjectRoot,
				AudioControlBytes,
				"Assets/Scripts/audio_controls.lua");
			PF_CHECK(Tests, AudioControlAsset.has_value());
			if (!AudioControlAsset)
				return;
			PF_CHECK(Tests, Emitter.SetScript(ScriptComponent{ AudioControlAsset->ID }).has_value());
			ScriptRuntimeServices AudioServices;
			AudioServices.Audio = &Runtime;
			ScriptRuntime AudioControlRuntime(*AudioProject, {}, AudioServices);
			PF_CHECK(Tests, AudioControlRuntime.Start(AudioScene).has_value());
			PF_CHECK(Tests, AudioControlRuntime.Advance(AudioScene, Timestep(1.0 / 60.0)).has_value());
			PF_CHECK(Tests, AudioControlRuntime.GetDiagnostics().empty());
			PF_CHECK(Tests, Runtime.GetPlaybackCount() == 1);
			AudioControlRuntime.Stop();
			PF_CHECK(Tests, Emitter.RemoveScript().has_value());

			PF_CHECK(Tests, Emitter.RemoveAudioSource().has_value());
			PF_CHECK(Tests, Runtime.Advance(AudioScene).has_value() && Runtime.GetPlaybackCount() == 0);
			Runtime.Stop();
			PF_CHECK(Tests, !Runtime.IsRunning());

			Scene PartialStartScene;
			const UUID ValidEmitterId{ 0x5300000000000000ull, 1 };
			const UUID MissingEmitterId{ 0x5300000000000000ull, 2 };
			const auto ValidEmitterResult = PartialStartScene.CreateEntityWithUUID(ValidEmitterId, "Valid audio source");
			const auto MissingEmitterResult = PartialStartScene.CreateEntityWithUUID(MissingEmitterId, "Missing audio source");
			PF_CHECK(Tests, ValidEmitterResult && MissingEmitterResult);
			if (ValidEmitterResult && MissingEmitterResult)
			{
				PF_CHECK(Tests, ValidEmitterResult->SetAudioSource(AudioSourceComponent{ Imported->ID }).has_value());
				PF_CHECK(Tests, MissingEmitterResult->SetAudioSource(AudioSourceComponent{
					AssetID{ 0x5300000000000000ull, 3 } }).has_value());
				AudioSceneRuntime RollbackRuntime(Engine, Cache);
				const auto FailedStart = RollbackRuntime.Start(PartialStartScene);
				PF_CHECK(Tests, !FailedStart && FailedStart.error().Code == AudioSceneRuntimeErrorCode::AssetLoadFailed);
				PF_CHECK(Tests, !RollbackRuntime.IsRunning() && RollbackRuntime.GetPlaybackCount() == 0);
				PF_CHECK(Tests, MissingEmitterResult->RemoveAudioSource().has_value());
				PF_CHECK(Tests, RollbackRuntime.Start(PartialStartScene).has_value());
				PF_CHECK(Tests, RollbackRuntime.GetPlaybackCount() == 1);
				RollbackRuntime.Stop();
			}

			Scene InvalidListenerScene;
			auto FirstListener = InvalidListenerScene.CreateEntity("First listener");
			auto SecondListener = InvalidListenerScene.CreateEntity("Second listener");
			PF_CHECK(Tests, FirstListener && SecondListener);
			if (FirstListener && SecondListener)
			{
				PF_CHECK(Tests, FirstListener->SetAudioListener(AudioListenerComponent{}).has_value());
				PF_CHECK(Tests, SecondListener->SetAudioListener(AudioListenerComponent{}).has_value());
				AudioSceneRuntime InvalidRuntime(Engine, Cache);
				const auto InvalidStart = InvalidRuntime.Start(InvalidListenerScene);
				PF_CHECK(Tests, !InvalidStart &&
					InvalidStart.error().Code == AudioSceneRuntimeErrorCode::MultiplePrimaryListeners);
				PF_CHECK(Tests, !InvalidRuntime.IsRunning());
			}

			Scene DuplicatedPrimaryListenerScene;
			auto PrimaryListener = DuplicatedPrimaryListenerScene.CreateEntity("Primary listener");
			PF_CHECK(Tests, PrimaryListener.has_value());
			if (PrimaryListener)
			{
				PF_CHECK(Tests, PrimaryListener->SetAudioListener(AudioListenerComponent{ true }).has_value());
				auto DuplicatedListener = DuplicatedPrimaryListenerScene.DuplicateEntity(*PrimaryListener);
				PF_CHECK(Tests, DuplicatedListener.has_value());
				if (DuplicatedListener)
				{
					const auto OriginalListenerComponent = PrimaryListener->GetAudioListener();
					const auto DuplicatedListenerComponent = DuplicatedListener->GetAudioListener();
					PF_CHECK(Tests, OriginalListenerComponent && OriginalListenerComponent->has_value() &&
						OriginalListenerComponent->value().IsPrimary);
					PF_CHECK(Tests, DuplicatedListenerComponent && DuplicatedListenerComponent->has_value() &&
						!DuplicatedListenerComponent->value().IsPrimary);
					AudioSceneRuntime RuntimeAfterListenerDuplication(Engine, Cache);
					PF_CHECK(Tests, RuntimeAfterListenerDuplication.Start(DuplicatedPrimaryListenerScene).has_value());
					PF_CHECK(Tests, RuntimeAfterListenerDuplication.Advance(DuplicatedPrimaryListenerScene).has_value());
				}
			}
		}

		// A playback retains the initialized device state even after its AudioEngine owner goes away.
		PF_CHECK(Tests, Playback.SetVolume(0.75f).has_value());
		PF_CHECK(Tests, Playback.Stop().has_value());
	}

	void TestSceneSerializerClone(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const UUID RootIdentifier{ 0x7400000000000000ull, 74 };
		const UUID ChildIdentifier{ 0x7500000000000000ull, 75 };
		const AssetID MeshIdentifier{ 0x7600000000000000ull, 76 };
		const AssetID MaterialIdentifier{ 0x7700000000000000ull, 77 };
		auto Root = Source.CreateEntityWithUUID(RootIdentifier, "Runtime root");
		auto Child = Source.CreateEntityWithUUID(ChildIdentifier, "Runtime child");
		PF_CHECK(Tests, Root.has_value() && Child.has_value());
		if (!Root || !Child)
			return;

		TransformComponent RootTransform;
		RootTransform.Translation = { 2.0f, 3.0f, 4.0f };
		PF_CHECK(Tests, Root->SetTransform(RootTransform).has_value());
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ MeshIdentifier, MaterialIdentifier }).has_value());

		auto Clone = SceneSerializer::Clone(Source);
		PF_CHECK(Tests, Clone.has_value() && Clone->get() != &Source);
		if (!Clone)
			return;

		PF_CHECK(Tests, (*Clone)->GetEntityCount() == Source.GetEntityCount());
		auto ClonedRoot = (*Clone)->FindEntity(RootIdentifier);
		auto ClonedChild = (*Clone)->FindEntity(ChildIdentifier);
		PF_CHECK(Tests, ClonedRoot.has_value() && ClonedChild.has_value());
		if (!ClonedRoot || !ClonedChild)
			return;

		const auto ClonedTransform = ClonedRoot->GetTransform();
		const auto ClonedRenderer = ClonedChild->GetMeshRenderer();
		const auto ClonedParent = ClonedChild->GetParent();
		PF_CHECK(Tests, ClonedTransform && glm::all(glm::equal(ClonedTransform->Translation, RootTransform.Translation)));
		PF_CHECK(Tests, ClonedRenderer && ClonedRenderer->has_value() &&
			ClonedRenderer->value().MeshAsset == MeshIdentifier && ClonedRenderer->value().MaterialAsset == MaterialIdentifier);
		PF_CHECK(Tests, ClonedParent && ClonedParent->has_value() && **ClonedParent == *ClonedRoot);

		TransformComponent RuntimeTransform = *ClonedTransform;
		RuntimeTransform.Translation.x = 20.0f;
		PF_CHECK(Tests, ClonedRoot->SetTransform(RuntimeTransform).has_value());
		const auto AuthoredTransform = Root->GetTransform();
		PF_CHECK(Tests, AuthoredTransform && AuthoredTransform->Translation.x == RootTransform.Translation.x);

		Scene Empty;
		auto EmptyClone = SceneSerializer::Clone(Empty);
		PF_CHECK(Tests, EmptyClone.has_value() && (*EmptyClone)->GetEntityCount() == 0);
	}

	void TestPrefabSerialization(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const auto ExternalParent = Source.CreateEntity("Outside prefab");
		const auto Root = Source.CreateEntityWithUUID(UUID{ 0x7000000000000000ull, 7 }, "Prefab root");
		const auto Child = Source.CreateEntityWithUUID(UUID{ 0x7100000000000000ull, 7 }, "Prefab child");
		const auto Grandchild = Source.CreateEntityWithUUID(UUID{ 0x7200000000000000ull, 7 }, "Prefab grandchild");
		PF_CHECK(Tests, ExternalParent && Root && Child && Grandchild);
		if (!ExternalParent || !Root || !Child || !Grandchild)
			return;

		PF_CHECK(Tests, Root->SetParent(*ExternalParent).has_value());
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Grandchild->SetParent(*Child).has_value());
		CameraComponent RootCamera;
		RootCamera.VerticalFieldOfViewRadians = 0.95f;
		RootCamera.IsPrimary = true;
		PF_CHECK(Tests, Root->SetCamera(RootCamera).has_value());
		const DirectionalLightComponent PrefabLight{ glm::vec3(0.8f, 0.9f, 1.0f), 1.75f };
		PF_CHECK(Tests, Root->SetDirectionalLight(PrefabLight).has_value());
		PointLightComponent PrefabPoint{ glm::vec3(1.0f, 0.5f, 0.25f), 3.0f, 9.0f };
		PrefabPoint.CastShadows = true;
		PrefabPoint.ShadowBias = 0.0025f;
		PrefabPoint.ShadowNormalBias = 0.04f;
		PrefabPoint.ShadowSoftness = 2.25f;
		PF_CHECK(Tests, Root->SetPointLight(PrefabPoint).has_value());
		SpotLightComponent PrefabSpot{ glm::vec3(0.3f, 0.5f, 1.0f), 7.0f, 14.0f, 8.0f, 22.0f };
		PrefabSpot.CastShadows = true;
		PrefabSpot.ShadowBias = 0.003f;
		PF_CHECK(Tests, Grandchild->SetSpotLight(PrefabSpot).has_value());
		const EnvironmentLightComponent PrefabEnvironment{ AssetID{ 0x7200000000000000ull, 7 }, 0.65f };
		PF_CHECK(Tests, Root->SetEnvironmentLight(PrefabEnvironment).has_value());
		const AssetID MeshAssetID{ 0x7300000000000000ull, 7 };
		const AssetID MaterialAssetID{ 0x7400000000000000ull, 7 };
		const AssetID AudioAssetID{ 0x7500000000000000ull, 7 };
		const AssetID ScriptAssetID{ 0x7600000000000000ull, 7 };
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ MeshAssetID, MaterialAssetID }).has_value());
		PF_CHECK(Tests, Child->SetAudioSource(AudioSourceComponent{ AudioAssetID, 0.6f, true, false, true }).has_value());
		PF_CHECK(Tests, Child->SetScript(ScriptComponent{ ScriptAssetID, false }).has_value());
		PF_CHECK(Tests, Root->SetAudioListener(AudioListenerComponent{ true }).has_value());

		const auto PrefabData = PrefabSerializer::Serialize(Source, *Root);
		PF_CHECK(Tests, PrefabData.has_value());
		if (!PrefabData)
			return;
		PF_CHECK(Tests, PrefabData->find("\"format\": \"PulseForgePrefab\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"version\": 3") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"root\": \"" + Root->GetUUID().ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"meshAsset\": \"" + MeshAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"materialAsset\": \"" + MaterialAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"asset\": \"" + AudioAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"asset\": \"" + ScriptAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"primary\": false") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"primary\": true") == std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"directionalLight\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"pointLight\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"spotLight\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"environmentLight\"") != std::string::npos);
		const auto SourceCamera = Root->GetCamera();
		const auto SourceListener = Root->GetAudioListener();
		PF_CHECK(Tests, SourceCamera && SourceCamera->has_value() && SourceCamera->value().IsPrimary);
		PF_CHECK(Tests, SourceListener && SourceListener->has_value() && SourceListener->value().IsPrimary);
		PF_CHECK(Tests, PrefabData->find("Outside prefab") == std::string::npos);
		PF_CHECK(Tests, PrefabData->find(ExternalParent->GetUUID().ToString()) == std::string::npos);

		std::string PrimaryRolePrefab = *PrefabData;
		size_t PrimaryRoleCount = 0;
		for (size_t PrimaryFlag = PrimaryRolePrefab.find("\"primary\": false");
			PrimaryFlag != std::string::npos;
			PrimaryFlag = PrimaryRolePrefab.find("\"primary\": false", PrimaryFlag))
		{
			PrimaryRolePrefab.replace(PrimaryFlag, std::string("\"primary\": false").size(), "\"primary\": true");
			PrimaryFlag += std::string("\"primary\": true").size();
			++PrimaryRoleCount;
		}
		PF_CHECK(Tests, PrimaryRoleCount == 2);
		Scene PrimaryRoleDestination;
		const auto PrimaryRoleInstance = PrefabSerializer::Instantiate(PrimaryRolePrefab, PrimaryRoleDestination);
		PF_CHECK(Tests, PrimaryRoleInstance.has_value());
		if (PrimaryRoleInstance)
		{
			const auto InstanceCamera = PrimaryRoleInstance->GetCamera();
			const auto InstanceListener = PrimaryRoleInstance->GetAudioListener();
			PF_CHECK(Tests, InstanceCamera && InstanceCamera->has_value() && !InstanceCamera->value().IsPrimary);
			PF_CHECK(Tests, InstanceListener && InstanceListener->has_value() && !InstanceListener->value().IsPrimary);
		}

		std::string LegacyPrefabV1 = *PrefabData;
		const size_t CameraPrimaryPosition = LegacyPrefabV1.find("\"primary\": false");
		PF_CHECK(Tests, CameraPrimaryPosition != std::string::npos);
		if (CameraPrimaryPosition != std::string::npos)
		{
			LegacyPrefabV1.replace(CameraPrimaryPosition, std::string("\"primary\": false").size(), "\"primary\": true");
			const size_t PrimaryCommaPosition = LegacyPrefabV1.rfind(',', CameraPrimaryPosition);
			const size_t PrimaryLineEnd = LegacyPrefabV1.find('\n', CameraPrimaryPosition);
			PF_CHECK(Tests, PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos);
			if (PrimaryCommaPosition != std::string::npos && PrimaryLineEnd != std::string::npos)
				LegacyPrefabV1.erase(PrimaryCommaPosition, PrimaryLineEnd - PrimaryCommaPosition);
		}
		const size_t EmbeddedSceneVersionPosition = LegacyPrefabV1.find("\"version\": 7", LegacyPrefabV1.find("\"scene\""));
		PF_CHECK(Tests, EmbeddedSceneVersionPosition != std::string::npos);
		if (EmbeddedSceneVersionPosition != std::string::npos)
			LegacyPrefabV1.replace(EmbeddedSceneVersionPosition, std::string("\"version\": 7").size(), "\"version\": 3");
		const size_t OuterPrefabVersionPosition = LegacyPrefabV1.find("\"version\": 3");
		PF_CHECK(Tests, OuterPrefabVersionPosition != std::string::npos);
		if (OuterPrefabVersionPosition != std::string::npos)
			LegacyPrefabV1.replace(OuterPrefabVersionPosition, std::string("\"version\": 3").size(), "\"version\": 1");
		Scene LegacyPrefabDestination;
		const auto LegacyPrefabInstance = PrefabSerializer::Instantiate(LegacyPrefabV1, LegacyPrefabDestination);
		PF_CHECK(Tests, LegacyPrefabInstance.has_value());
		if (LegacyPrefabInstance)
		{
			const auto LegacyCamera = LegacyPrefabInstance->GetCamera();
			PF_CHECK(Tests, LegacyCamera && LegacyCamera->has_value() && !LegacyCamera->value().IsPrimary);
		}

		Scene Destination;
		const auto Existing = Destination.CreateEntity("Existing destination entity");
		PF_CHECK(Tests, Existing.has_value());
		const size_t OriginalEntityCount = Destination.GetEntityCount();
		const auto FirstInstance = PrefabSerializer::Instantiate(*PrefabData, Destination);
		PF_CHECK(Tests, FirstInstance.has_value());
		PF_CHECK(Tests, Destination.GetEntityCount() == OriginalEntityCount + 3);
		if (!FirstInstance)
			return;
		PF_CHECK(Tests, FirstInstance->GetUUID() != Root->GetUUID());
		const auto FirstParent = FirstInstance->GetParent();
		PF_CHECK(Tests, FirstParent && !FirstParent->has_value());
		const auto FirstCamera = FirstInstance->GetCamera();
		const auto FirstLight = FirstInstance->GetDirectionalLight();
		const auto FirstEnvironment = FirstInstance->GetEnvironmentLight();
		const auto FirstPointLight = FirstInstance->GetPointLight();
		const auto FirstAudioListener = FirstInstance->GetAudioListener();
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() &&
			FirstCamera->value().VerticalFieldOfViewRadians == RootCamera.VerticalFieldOfViewRadians);
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() && !FirstCamera->value().IsPrimary);
		PF_CHECK(Tests, FirstLight && FirstLight->has_value() &&
			FirstLight->value().Intensity == PrefabLight.Intensity);
		PF_CHECK(Tests, FirstEnvironment && FirstEnvironment->has_value() &&
			FirstEnvironment->value().HdrImage == PrefabEnvironment.HdrImage &&
			FirstEnvironment->value().Intensity == PrefabEnvironment.Intensity);
		PF_CHECK(Tests, FirstPointLight && FirstPointLight->has_value() &&
			FirstPointLight->value().Range == 9.0f && FirstPointLight->value().CastShadows &&
			FirstPointLight->value().ShadowBias == PrefabPoint.ShadowBias &&
			FirstPointLight->value().ShadowNormalBias == PrefabPoint.ShadowNormalBias &&
			FirstPointLight->value().ShadowSoftness == PrefabPoint.ShadowSoftness);
		PF_CHECK(Tests, FirstAudioListener && FirstAudioListener->has_value() && !FirstAudioListener->value().IsPrimary);
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1)
			return;
		const Entity FirstChild = FirstChildren->front();
		const auto FirstChildTag = FirstChild.GetTag();
		const auto FirstChildMesh = FirstChild.GetMeshRenderer();
		const auto FirstChildAudioSource = FirstChild.GetAudioSource();
		const auto FirstChildScript = FirstChild.GetScript();
		const auto FirstGrandchildren = FirstChild.GetChildren();
		PF_CHECK(Tests, FirstChildTag && FirstChildTag->Name == "Prefab child");
		PF_CHECK(Tests, FirstChildMesh && FirstChildMesh->has_value() && FirstChildMesh->value().MeshAsset == MeshAssetID);
		PF_CHECK(Tests, FirstChildMesh && FirstChildMesh->has_value() && FirstChildMesh->value().MaterialAsset == MaterialAssetID);
		PF_CHECK(Tests, FirstChildAudioSource && FirstChildAudioSource->has_value() &&
			FirstChildAudioSource->value().AudioAsset == AudioAssetID && !FirstChildAudioSource->value().PlayOnStart);
		PF_CHECK(Tests, FirstChildScript && FirstChildScript->has_value() &&
			FirstChildScript->value().ScriptAsset == ScriptAssetID && !FirstChildScript->value().Enabled);
		PF_CHECK(Tests, FirstGrandchildren && FirstGrandchildren->size() == 1);
		if (FirstGrandchildren && FirstGrandchildren->size() == 1)
		{
			const auto FirstGrandchildSpot = FirstGrandchildren->front().GetSpotLight();
			PF_CHECK(Tests, FirstGrandchildSpot && FirstGrandchildSpot->has_value() &&
				FirstGrandchildSpot->value().OuterConeAngleDegrees == 22.0f &&
				FirstGrandchildSpot->value().CastShadows && FirstGrandchildSpot->value().ShadowBias == 0.003f);
		}

		const auto SecondInstance = PrefabSerializer::Instantiate(*PrefabData, Destination);
		PF_CHECK(Tests, SecondInstance.has_value());
		PF_CHECK(Tests, Destination.GetEntityCount() == OriginalEntityCount + 6);
		PF_CHECK(Tests, SecondInstance && FirstInstance->GetUUID() != SecondInstance->GetUUID());
		if (SecondInstance)
		{
			const auto SecondCamera = SecondInstance->GetCamera();
			const auto SecondListener = SecondInstance->GetAudioListener();
			PF_CHECK(Tests, SecondCamera && SecondCamera->has_value() && !SecondCamera->value().IsPrimary);
			PF_CHECK(Tests, SecondListener && SecondListener->has_value() && !SecondListener->value().IsPrimary);
		}

		const size_t CountBeforeFailure = Destination.GetEntityCount();
		const auto InvalidJSON = PrefabSerializer::Instantiate("{ invalid", Destination);
		PF_CHECK(Tests, !InvalidJSON.has_value());
		PF_CHECK(Tests, !InvalidJSON && InvalidJSON.error().Code == PrefabErrorCode::InvalidDocument);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeFailure);

		std::string MissingPrefabRoot = *PrefabData;
		const size_t RootFieldPosition = MissingPrefabRoot.find("\"root\": \"");
		const size_t RootIdentifierPosition = MissingPrefabRoot.find(Root->GetUUID().ToString(), RootFieldPosition);
		PF_CHECK(Tests, RootFieldPosition != std::string::npos && RootIdentifierPosition != std::string::npos);
		if (RootIdentifierPosition != std::string::npos)
			MissingPrefabRoot.replace(
				RootIdentifierPosition,
				Root->GetUUID().ToString().size(),
				UUID{ 0x7400000000000000ull, 7 }.ToString());
		const auto MissingRootResult = PrefabSerializer::Instantiate(MissingPrefabRoot, Destination);
		PF_CHECK(Tests, !MissingRootResult.has_value());
		PF_CHECK(Tests, !MissingRootResult && MissingRootResult.error().Code == PrefabErrorCode::InvalidDocument);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeFailure);

		std::string UnsupportedVersion = *PrefabData;
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 3");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 3").size(), "\"version\": 99");
		const auto UnsupportedPrefabVersion = PrefabSerializer::Instantiate(UnsupportedVersion, Destination);
		PF_CHECK(Tests, !UnsupportedPrefabVersion.has_value());
		PF_CHECK(Tests, !UnsupportedPrefabVersion &&
			UnsupportedPrefabVersion.error().Code == PrefabErrorCode::UnsupportedVersion);
		PF_CHECK(Tests, Destination.GetEntityCount() == CountBeforeFailure);

		Scene OtherScene;
		const auto ForeignRoot = OtherScene.CreateEntityWithUUID(Root->GetUUID(), "Foreign prefab root");
		PF_CHECK(Tests, ForeignRoot.has_value());
		if (ForeignRoot)
		{
			const auto ForeignPrefab = PrefabSerializer::Serialize(Source, *ForeignRoot);
			PF_CHECK(Tests, !ForeignPrefab.has_value());
			PF_CHECK(Tests, !ForeignPrefab && ForeignPrefab.error().Code == PrefabErrorCode::InvalidRootEntity);
		}
	}

	void TestPrefabAssetsByUUID(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier)
			return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgePrefabAssets-" + ProjectIdentifier->ToString());
		struct ProjectCleanup
		{
			std::filesystem::path Path;
			~ProjectCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path PrefabDirectory = ProjectRoot / "Assets" / "Prefabs";
		std::filesystem::create_directories(PrefabDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		const auto RootID = UUID::Parse("15261902-2c12-4a92-9e99-0e355a16af4e");
		const auto ChildID = UUID::Parse("df753d9d-1024-40f1-980f-5ca589fd87d8");
		const auto MeshAssetID = UUID::Parse("d3ce3630-b87a-4dab-b8e0-59c1f258ae78");
		PF_CHECK(Tests, RootID && ChildID && MeshAssetID);
		if (!RootID || !ChildID || !MeshAssetID)
			return;

		Scene Source;
		const auto Root = Source.CreateEntityWithUUID(*RootID, "Prefab Root");
		const auto Child = Source.CreateEntityWithUUID(*ChildID, "Prefab Child");
		PF_CHECK(Tests, Root && Child);
		if (!Root || !Child)
			return;
		PF_CHECK(Tests, Child->SetParent(*Root).has_value());
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ *MeshAssetID }).has_value());

		const std::filesystem::path SourcePath = PrefabDirectory / "crate.prefab";
		PF_CHECK(Tests, PrefabSerializer::SaveToFile(Source, *Root, SourcePath).has_value());
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata)
			return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		Scene Destination;
		const auto Existing = Destination.CreateEntity("Existing entity");
		PF_CHECK(Tests, Existing.has_value());
		const auto FirstInstance = PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, Destination);
		PF_CHECK(Tests, FirstInstance.has_value());
		PF_CHECK(Tests, FirstInstance && FirstInstance->GetUUID() != *RootID);
		PF_CHECK(Tests, Destination.GetEntityCount() == 3);
		if (!FirstInstance)
			return;
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1)
			return;
		const auto ChildRenderer = FirstChildren->front().GetMeshRenderer();
		PF_CHECK(Tests, ChildRenderer && ChildRenderer->has_value());
		PF_CHECK(Tests, ChildRenderer && ChildRenderer->has_value() && ChildRenderer->value().MeshAsset == *MeshAssetID);

		const auto MoveResult = AssetOperations::Move(
			Registry,
			ProjectRoot,
			Metadata->ID,
			"Assets/Prefabs/renamed.prefab");
		PF_CHECK(Tests, MoveResult && MoveResult->ID == Metadata->ID);
		PF_CHECK(Tests, PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, Destination).has_value());

		auto RootTag = Root->GetTag();
		PF_CHECK(Tests, RootTag.has_value());
		if (!RootTag)
			return;
		RootTag->Name = "Saved through prefab UUID";
		PF_CHECK(Tests, Root->SetTag(*RootTag).has_value());
		PF_CHECK(Tests, PrefabAssetService::Save(Metadata->ID, ProjectRoot, Registry, Source, *Root).has_value());
		const auto PreservedMetadata = AssetMetadataSerializer::LoadFromFile(
			AssetMetadataSerializer::GetSidecarPath(ProjectRoot / "Assets" / "Prefabs" / "renamed.prefab"));
		PF_CHECK(Tests, PreservedMetadata && PreservedMetadata->ID == Metadata->ID);
		Scene UpdatedInstance;
		const auto UpdatedRoot = PrefabAssetService::Instantiate(Metadata->ID, ProjectRoot, Registry, UpdatedInstance);
		PF_CHECK(Tests, UpdatedRoot.has_value());
		if (UpdatedRoot)
		{
			const auto UpdatedTag = UpdatedRoot->GetTag();
			PF_CHECK(Tests, UpdatedTag && UpdatedTag->Name == "Saved through prefab UUID");
		}

		const auto CreatedPrefab = PrefabAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Prefabs/generated.prefab",
			Source,
			*Root);
		PF_CHECK(Tests, CreatedPrefab.has_value());
		PF_CHECK(Tests, CreatedPrefab && CreatedPrefab->ID != Metadata->ID &&
			CreatedPrefab->ProjectRelativePath == "Assets/Prefabs/generated.prefab");
		if (CreatedPrefab)
		{
			const auto CreatedMetadata = AssetMetadataSerializer::LoadFromFile(
				ProjectRoot / "Assets/Prefabs/generated.prefab.meta");
			PF_CHECK(Tests, CreatedMetadata && CreatedMetadata->ID == CreatedPrefab->ID);
			PF_CHECK(Tests, Registry.Find(CreatedPrefab->ID).has_value());

			Scene CreatedPrefabScene;
			const auto CreatedRoot = PrefabAssetService::Instantiate(
				CreatedPrefab->ID,
				ProjectRoot,
				Registry,
				CreatedPrefabScene);
			PF_CHECK(Tests, CreatedRoot && CreatedRoot->GetUUID() != Root->GetUUID());
			PF_CHECK(Tests, CreatedPrefabScene.GetEntityCount() == 2);
			if (CreatedRoot)
			{
				const auto CreatedChildren = CreatedRoot->GetChildren();
				PF_CHECK(Tests, CreatedChildren && CreatedChildren->size() == 1);
			}

			const auto DuplicateCreation = PrefabAssetService::Create(
				Registry,
				ProjectRoot,
				"Assets/Prefabs/generated.prefab",
				Source,
				*Root);
			PF_CHECK(Tests, !DuplicateCreation &&
				DuplicateCreation.error().Code == PrefabAssetErrorCode::AssetOperationFailed);
			PF_CHECK(Tests, Registry.Find(CreatedPrefab->ID).has_value());
		}
		const auto InvalidPrefabCreation = PrefabAssetService::Create(
			Registry,
			ProjectRoot,
			"Assets/Prefabs/generated.json",
			Source,
			*Root);
		PF_CHECK(Tests, !InvalidPrefabCreation &&
			InvalidPrefabCreation.error().Code == PrefabAssetErrorCode::UnsupportedAssetType);

		const auto Duplicate = AssetOperations::Duplicate(
			Registry,
			ProjectRoot,
			Metadata->ID,
			"Assets/Prefabs/copy.prefab");
		PF_CHECK(Tests, Duplicate.has_value());
		PF_CHECK(Tests, Duplicate && Duplicate->ID != Metadata->ID);
		PF_CHECK(Tests, Duplicate && PrefabAssetService::Instantiate(Duplicate->ID, ProjectRoot, Registry, UpdatedInstance).has_value());

		const std::filesystem::path OtherAssetPath = PrefabDirectory / "notes.txt";
		{
			std::ofstream Output(OtherAssetPath, std::ios::binary | std::ios::trunc);
			Output << "not a prefab";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto OtherMetadata = AssetMetadataSerializer::CreateForNewAsset(OtherAssetPath);
		PF_CHECK(Tests, OtherMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (OtherMetadata)
		{
			const auto WrongType = PrefabAssetService::Instantiate(OtherMetadata->ID, ProjectRoot, Registry, UpdatedInstance);
			PF_CHECK(Tests, !WrongType && WrongType.error().Code == PrefabAssetErrorCode::UnsupportedAssetType);
		}

		const std::filesystem::path BrokenPath = PrefabDirectory / "broken.prefab";
		{
			std::ofstream Output(BrokenPath, std::ios::binary | std::ios::trunc);
			Output << "{broken";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto BrokenMetadata = AssetMetadataSerializer::CreateForNewAsset(BrokenPath);
		PF_CHECK(Tests, BrokenMetadata.has_value());
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		if (BrokenMetadata)
		{
			const size_t CountBeforeFailure = UpdatedInstance.GetEntityCount();
			const auto Broken = PrefabAssetService::Instantiate(BrokenMetadata->ID, ProjectRoot, Registry, UpdatedInstance);
			PF_CHECK(Tests, !Broken && Broken.error().Code == PrefabAssetErrorCode::SerializationFailed);
			PF_CHECK(Tests, UpdatedInstance.GetEntityCount() == CountBeforeFailure);
		}

		const auto MissingID = UUID::Generate();
		PF_CHECK(Tests, MissingID.has_value());
		if (MissingID)
		{
			const auto Missing = PrefabAssetService::Instantiate(*MissingID, ProjectRoot, Registry, UpdatedInstance);
			PF_CHECK(Tests, !Missing && Missing.error().Code == PrefabAssetErrorCode::AssetNotFound);
		}
	}

	void TestGraphicsPipelineAndDrawValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(Pipeline).has_value());
		PF_CHECK(Tests, Pipeline.VertexShader != nullptr && Pipeline.FragmentShader != nullptr);

		auto MissingFragmentShader = Pipeline;
		MissingFragmentShader.FragmentShader.reset();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(MissingFragmentShader).has_value());

		auto WrongShaderStage = Pipeline;
		WrongShaderStage.VertexShader = std::make_shared<TestShader>(ShaderStage::Fragment);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(WrongShaderStage).has_value());

		auto UnsupportedTopology = Pipeline;
		UnsupportedTopology.Topology = static_cast<PrimitiveTopology>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(UnsupportedTopology).has_value());

		auto UnsupportedTarget = Pipeline;
		UnsupportedTarget.ColorFormat = static_cast<ColorTargetFormat>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(UnsupportedTarget).has_value());

		auto UnormTarget = Pipeline;
		UnormTarget.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(UnormTarget).has_value());
		auto SrgbTarget = Pipeline;
		SrgbTarget.ColorFormat = ColorTargetFormat::RGBA8_Srgb;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(SrgbTarget).has_value());
		auto HalfFloatPipeline = Pipeline;
		HalfFloatPipeline.ColorFormat = ColorTargetFormat::RGBA16_Float;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());
		HalfFloatPipeline.DepthAttachmentEnabled = false;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());
		HalfFloatPipeline.DepthAttachmentEnabled = true;
		HalfFloatPipeline.Depth.TestEnabled = true;
		HalfFloatPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(HalfFloatPipeline).has_value());

		auto DepthTestedPipeline = Pipeline;
		DepthTestedPipeline.Depth.TestEnabled = true;
		DepthTestedPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(DepthTestedPipeline).has_value());

		auto InvalidDepthComparison = DepthTestedPipeline;
		InvalidDepthComparison.Depth.Compare = static_cast<DepthCompareOperation>(0xff);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(InvalidDepthComparison).has_value());

		auto DepthWriteWithoutTest = Pipeline;
		DepthWriteWithoutTest.Depth.WriteEnabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthWriteWithoutTest).has_value());

		auto DepthOnlyPipeline = Pipeline;
		DepthOnlyPipeline.FragmentShader.reset();
		DepthOnlyPipeline.ColorFormat = ColorTargetFormat::None;
		DepthOnlyPipeline.Depth.TestEnabled = true;
		DepthOnlyPipeline.Depth.WriteEnabled = true;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(DepthOnlyPipeline).has_value());
		auto DepthOnlyWithColor = DepthOnlyPipeline;
		DepthOnlyWithColor.ColorFormat = ColorTargetFormat::RGBA8_UNorm;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithColor).has_value());
		auto DepthOnlyWithoutTesting = DepthOnlyPipeline;
		DepthOnlyWithoutTesting.Depth.TestEnabled = false;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithoutTesting).has_value());
		auto DepthOnlyWithoutWrites = DepthOnlyPipeline;
		DepthOnlyWithoutWrites.Depth.WriteEnabled = false;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyWithoutWrites).has_value());
		auto ColorPipelineWithoutFragment = Pipeline;
		ColorPipelineWithoutFragment.FragmentShader.reset();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(ColorPipelineWithoutFragment).has_value());
		auto DepthOnlyBlending = DepthOnlyPipeline;
		DepthOnlyBlending.Blend.Enabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DepthOnlyBlending).has_value());
		auto ColorOnlyPipeline = Pipeline;
		ColorOnlyPipeline.DepthAttachmentEnabled = false;
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(ColorOnlyPipeline).has_value());
		auto ColorOnlyWithDepthTest = ColorOnlyPipeline;
		ColorOnlyWithDepthTest.Depth.TestEnabled = true;
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(ColorOnlyWithDepthTest).has_value());
		auto NonFiniteBias = DepthOnlyPipeline;
		NonFiniteBias.Rasterizer.SlopeScaledDepthBias = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(NonFiniteBias).has_value());

		BufferDesc VertexBuffer{ sizeof(float) * 6 * 3, BufferUsage::Vertex, "Test triangle vertices" };
		DrawArguments Triangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateDrawArguments(Triangle, Pipeline, VertexBuffer).has_value());

		auto ZeroVertices = Triangle;
		ZeroVertices.VertexCount = 0;
		PF_CHECK(Tests, !ValidateDrawArguments(ZeroVertices, Pipeline, VertexBuffer).has_value());

		auto BeyondBuffer = Triangle;
		BeyondBuffer.VertexCount = 4;
		PF_CHECK(Tests, !ValidateDrawArguments(BeyondBuffer, Pipeline, VertexBuffer).has_value());

		auto IndexBuffer = VertexBuffer;
		IndexBuffer.Usage = BufferUsage::Index;
		PF_CHECK(Tests, !ValidateDrawArguments(Triangle, Pipeline, IndexBuffer).has_value());

		auto ZeroInstances = Triangle;
		ZeroInstances.InstanceCount = 0;
		PF_CHECK(Tests, !ValidateDrawArguments(ZeroInstances, Pipeline, VertexBuffer).has_value());

		BufferDesc IndexedVertexBuffer{ sizeof(float) * 6 * 3, BufferUsage::Vertex, "Raw indexed draw vertices" };
		BufferDesc RawIndexBuffer{ sizeof(uint32_t) * 6, BufferUsage::Index, "Raw indexed draw indices" };
		const DrawIndexedArguments RawIndexedTriangle{ 3, 1, 0, 0 };
		PF_CHECK(Tests, ValidateIndexedBufferDrawArguments(
			RawIndexedTriangle,
			Pipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto IndexRangePastEnd = RawIndexedTriangle;
		IndexRangePastEnd.FirstIndex = 4;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			IndexRangePastEnd,
			Pipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto WrongRawIndexBuffer = RawIndexBuffer;
		WrongRawIndexBuffer.Usage = BufferUsage::Vertex;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			RawIndexedTriangle,
			Pipeline,
			IndexedVertexBuffer,
			WrongRawIndexBuffer).has_value());

		auto MisalignedIndexBuffer = RawIndexBuffer;
		++MisalignedIndexBuffer.ByteSize;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			RawIndexedTriangle,
			Pipeline,
			IndexedVertexBuffer,
			MisalignedIndexBuffer).has_value());

		auto ScissoredPipeline = Pipeline;
		ScissoredPipeline.Rasterizer.ScissorEnabled = true;
		auto ScissoredDraw = RawIndexedTriangle;
		ScissoredDraw.Scissor = ScissorRect{ 10, 12, 64, 48 };
		PF_CHECK(Tests, ValidateIndexedBufferDrawArguments(
			ScissoredDraw,
			ScissoredPipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto MissingScissorState = Pipeline;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			ScissoredDraw,
			MissingScissorState,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());

		auto EmptyScissor = ScissoredDraw;
		EmptyScissor.Scissor->Width = 0;
		PF_CHECK(Tests, !ValidateIndexedBufferDrawArguments(
			EmptyScissor,
			ScissoredPipeline,
			IndexedVertexBuffer,
			RawIndexBuffer).has_value());
	}

	void TestToneMappingMathAndOutputEncoding(TestRunner& Tests)
	{
		using namespace PulseForge;
		ToneMappingSettings Settings;
		PF_CHECK(Tests, Settings.ExposureEV == 0.0f);
		PF_CHECK(Tests, IsValidToneMappingSettings(Settings));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -16.0f }));
		PF_CHECK(Tests, Settings.ExposureEV == -16.0f);
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 16.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 0.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -2.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { -1.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 1.0f }));
		PF_CHECK(Tests, TryUpdateToneMappingSettings(Settings, { 2.0f }));
		for (float Invalid : { std::numeric_limits<float>::quiet_NaN(),
			std::numeric_limits<float>::infinity(), -std::numeric_limits<float>::infinity(), -16.01f, 16.01f })
		{
			const ToneMappingSettings Previous = Settings;
			PF_CHECK(Tests, !TryUpdateToneMappingSettings(Settings, { Invalid }));
			PF_CHECK(Tests, Settings.ExposureEV == Previous.ExposureEV);
		}

		const auto Black = ApplyAcesFittedToneMapping({ 0.0f, -1.0f, -10.0f }, 0.0f);
		PF_CHECK(Tests, Black[0] == 0.0f && Black[1] == 0.0f && Black[2] == 0.0f);
		const auto NonFinite = ApplyAcesFittedToneMapping({
			std::numeric_limits<float>::infinity(),
			-std::numeric_limits<float>::infinity(),
			std::numeric_limits<float>::quiet_NaN() }, 0.0f);
		PF_CHECK(Tests, NonFinite[0] == 1.0f && NonFinite[1] == 0.0f && NonFinite[2] == 0.0f);
		const auto Reference = ApplyAcesFittedToneMapping({ 0.18f, 1.0f, 4.0f }, 0.0f);
		PF_CHECK(Tests, std::abs(Reference[0] - 0.2669f) < 0.0002f);
		PF_CHECK(Tests, std::abs(Reference[1] - 0.803797f) < 0.00001f);
		PF_CHECK(Tests, std::abs(Reference[2] - 0.973417f) < 0.00002f);
		const auto NegativeExposure = ApplyAcesFittedToneMapping({ 1.0f, 1.0f, 1.0f }, -1.0f);
		const auto PositiveExposure = ApplyAcesFittedToneMapping({ 1.0f, 1.0f, 1.0f }, 1.0f);
		PF_CHECK(Tests, NegativeExposure[0] < Reference[1] && PositiveExposure[0] > Reference[1]);
		PF_CHECK(Tests, PositiveExposure[0] < 1.0f);
		const auto Large = ApplyAcesFittedToneMapping({ (std::numeric_limits<float>::max)(), 1.0e20f, 65504.0f }, 16.0f);
		PF_CHECK(Tests, Large[0] == 1.0f && Large[1] == 1.0f && Large[2] == 1.0f);
		float Previous = -1.0f;
		for (float Value : { 0.0f, 0.01f, 0.1f, 0.18f, 0.5f, 1.0f, 2.0f, 16.0f, 65504.0f })
		{
			const auto Mapped = ApplyAcesFittedToneMapping({ Value, Value, Value }, 0.0f);
			PF_CHECK(Tests, std::isfinite(Mapped[0]) && Mapped[0] >= 0.0f && Mapped[0] <= 1.0f);
			PF_CHECK(Tests, Mapped[0] >= Previous);
			Previous = Mapped[0];
		}
		PF_CHECK(Tests, LinearToSrgb(0.0f) == 0.0f);
		PF_CHECK(Tests, std::abs(LinearToSrgb(0.0031308f) - 0.0404499f) < 1.0e-5f);
		PF_CHECK(Tests, std::abs(LinearToSrgb(0.04045f) - 0.222205f) < 1.0e-5f);
		PF_CHECK(Tests, LinearToSrgb(1.0f) == 1.0f);
		PF_CHECK(Tests, ShouldShaderEncodeSrgb(ColorTargetFormat::RGBA8_UNorm, OutputColorEncoding::SrgbAttachment));
		PF_CHECK(Tests, !ShouldShaderEncodeSrgb(ColorTargetFormat::RGBA8_Srgb, OutputColorEncoding::UnormAttachment));
		PF_CHECK(Tests, !ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, OutputColorEncoding::SrgbAttachment));
		PF_CHECK(Tests, ShouldShaderEncodeSrgb(ColorTargetFormat::Swapchain, OutputColorEncoding::UnormAttachment));
		PF_CHECK(Tests, !IsToneMappingBindingCurrent(0, 0));
		PF_CHECK(Tests, IsToneMappingBindingCurrent(5, 5));
		PF_CHECK(Tests, !IsToneMappingBindingCurrent(4, 5));
	}

	void TestConstantBufferBindingValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "Test fragment constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateSlot = LayoutDescription;
		DuplicateSlot.Items.push_back({ BindingResourceType::ConstantBuffer, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateSlot).has_value());

		auto InvalidVisibility = LayoutDescription;
		InvalidVisibility.Visibility = static_cast<ShaderVisibility>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(InvalidVisibility).has_value());

		auto SharedVisibility = LayoutDescription;
		SharedVisibility.Visibility = ShaderVisibility::AllGraphics;
		PF_CHECK(Tests, ValidateBindingLayout(SharedVisibility).has_value());

		auto UnsupportedType = LayoutDescription;
		UnsupportedType.Items[0].Type = static_cast<BindingResourceType>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(UnsupportedType).has_value());

		BindingLayoutHandle Layout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });
		TestBuffer VertexBuffer(BufferDesc{ 48, BufferUsage::Vertex, "Test vertices" });
		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto MissingBuffer = SetDescription;
		MissingBuffer.Buffers.clear();
		PF_CHECK(Tests, !ValidateBindingSet(MissingBuffer).has_value());

		auto WrongBufferType = SetDescription;
		WrongBufferType.Buffers[0].Resource = std::cref(static_cast<const Buffer&>(VertexBuffer));
		PF_CHECK(Tests, !ValidateBindingSet(WrongBufferType).has_value());

		auto WrongSlot = SetDescription;
		WrongSlot.Buffers[0].Slot = 1;
		PF_CHECK(Tests, !ValidateBindingSet(WrongSlot).has_value());

		GraphicsPipelineDesc Pipeline = MakeTestPipelineDescription();
		Pipeline.BindingLayouts.push_back(Layout);
		PF_CHECK(Tests, ValidateGraphicsPipelineDescription(Pipeline).has_value());
		auto DuplicateRegisterSpace = Pipeline;
		DuplicateRegisterSpace.BindingLayouts.push_back(Layout);
		PF_CHECK(Tests, !ValidateGraphicsPipelineDescription(DuplicateRegisterSpace).has_value());

		TestBindingSet CreatedTestBindingSet(Layout);
		const std::array<const BindingSet*, 1> ValidSets = { &CreatedTestBindingSet };
		PF_CHECK(Tests, ValidateDrawBindingSets(Pipeline, ValidSets).has_value());
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, {}).has_value());

		BindingLayoutHandle DifferentLayout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBindingSet MismatchedSet(DifferentLayout);
		const std::array<const BindingSet*, 1> MismatchedSets = { &MismatchedSet };
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, MismatchedSets).has_value());
	}

	void TestScreenSpaceAmbientOcclusionMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		const AmbientOcclusionKernel Kernel = GenerateAmbientOcclusionKernel();
		const AmbientOcclusionKernel RepeatedKernel = GenerateAmbientOcclusionKernel();
		for (size_t Index = 0; Index < Kernel.Samples.size(); ++Index)
		{
			const glm::vec4& Sample = Kernel.Samples[Index];
			PF_CHECK(Tests, std::isfinite(Sample.x) && std::isfinite(Sample.y) &&
				std::isfinite(Sample.z) && Sample.z >= 0.0f);
			PF_CHECK(Tests, glm::length(glm::vec3(Sample)) <= 1.0f + 1.0e-5f);
			PF_CHECK(Tests, Sample == RepeatedKernel.Samples[Index]);
		}
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(1920, 1080) == std::array<uint32_t, 2>{ 960, 540 }));
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(5, 3) == std::array<uint32_t, 2>{ 3, 2 }));
		PF_CHECK(Tests, (CalculateAmbientOcclusionExtent(1, 1) == std::array<uint32_t, 2>{ 1, 1 }));
		PF_CHECK(Tests, CalculateAmbientOcclusionExtent(std::numeric_limits<uint32_t>::max(), 9)[0] ==
			std::numeric_limits<uint32_t>::max() / 2u + 1u);

		const glm::mat4 Projection = glm::perspective(glm::radians(60.0f), 1.6f, 0.1f, 100.0f);
		const glm::mat4 InverseProjection = glm::inverse(Projection);
		for (const glm::vec3 ViewPosition : { glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.5f, -0.3f, -5.0f) })
		{
			const glm::vec4 Clip = Projection * glm::vec4(ViewPosition, 1.0f);
			const glm::vec3 Ndc = glm::vec3(Clip) / Clip.w;
			const glm::vec2 UV{ Ndc.x * 0.5f + 0.5f, 0.5f - Ndc.y * 0.5f };
			const auto Reconstructed = ReconstructViewPosition(InverseProjection, UV, Ndc.z);
			PF_CHECK(Tests, Reconstructed && glm::length(*Reconstructed - ViewPosition) < 1.0e-4f);
		}
		PF_CHECK(Tests, !ReconstructViewPosition(InverseProjection, glm::vec2(0.5f), 1.1f));

		for (const glm::vec3 Normal : { glm::vec3(1, 0, 0), glm::vec3(-1, 0, 0), glm::vec3(0, 1, 0),
			glm::vec3(0, -1, 0), glm::vec3(0, 0, 1), glm::vec3(0, 0, -1), glm::normalize(glm::vec3(1, 2, 3)) })
		{
			const glm::vec3 Decoded = DecodeViewNormal(EncodeViewNormal(Normal));
			PF_CHECK(Tests, glm::length(Decoded - Normal) < 1.0e-5f);
		}
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(0.0f, 0.6f) == 1.0f);
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(0.6f, 0.6f) == 0.0f);
		PF_CHECK(Tests, AmbientOcclusionRangeWeight(std::numeric_limits<float>::infinity(), 0.6f) == 0.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(1.0f, 0.2f, 0.8f) == 1.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(0.2f, 1.0f, 0.05f) == 1.0f);
		PF_CHECK(Tests, ComputeSpecularOcclusion(0.2f, 0.0f, 1.0f) < 0.21f);
	}

	void TestEnvironmentLightingProcessing(TestRunner& Tests)
	{
		using namespace PulseForge;
		const glm::vec2 PositiveX = EquirectangularUvFromDirection({ 1.0f, 0.0f, 0.0f });
		const glm::vec2 NegativeX = EquirectangularUvFromDirection({ -1.0f, 0.0f, 0.0f });
		const glm::vec2 PositiveZ = EquirectangularUvFromDirection({ 0.0f, 0.0f, 1.0f });
		const glm::vec2 NegativeZ = EquirectangularUvFromDirection({ 0.0f, 0.0f, -1.0f });
		const glm::vec2 PositiveY = EquirectangularUvFromDirection({ 0.0f, 1.0f, 0.0f });
		const glm::vec2 NegativeY = EquirectangularUvFromDirection({ 0.0f, -1.0f, 0.0f });
		PF_CHECK(Tests, std::abs(PositiveX.x - 0.5f) < 0.0001f && std::abs(PositiveX.y - 0.5f) < 0.0001f);
		PF_CHECK(Tests, NegativeX.x < 0.0001f || NegativeX.x > 0.9999f);
		PF_CHECK(Tests, std::abs(PositiveZ.x - 0.75f) < 0.0001f);
		PF_CHECK(Tests, std::abs(NegativeZ.x - 0.25f) < 0.0001f);
		PF_CHECK(Tests, std::abs(PositiveY.y) < 0.0001f && std::abs(NegativeY.y - 1.0f) < 0.0001f);

		constexpr std::array<glm::vec3, 6> ExpectedFaceCenters = {
			glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f),
			glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, -1.0f, 0.0f),
			glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 0.0f, -1.0f)
		};
		for (uint32_t Face = 0; Face < ExpectedFaceCenters.size(); ++Face)
		{
			const glm::vec3 Direction = CubeFaceTexelDirection(static_cast<CubeFace>(Face), 0.5f, 0.5f);
			PF_CHECK(Tests, glm::dot(Direction, ExpectedFaceCenters[Face]) > 0.9999f);
		}

		ImportedFloatImage ConstantEnvironment;
		ConstantEnvironment.Width = 8;
		ConstantEnvironment.Height = 4;
		ConstantEnvironment.RGBA32FPixels.resize(8 * 4 * 4);
		for (size_t Pixel = 0; Pixel < 8 * 4; ++Pixel)
		{
			ConstantEnvironment.RGBA32FPixels[Pixel * 4] = 2.0f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 1] = 0.5f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 2] = 0.25f;
			ConstantEnvironment.RGBA32FPixels[Pixel * 4 + 3] = 1.0f;
		}
		EnvironmentProcessingDesc Settings;
		Settings.EnvironmentFaceSize = 8;
		Settings.IrradianceFaceSize = 4;
		Settings.SpecularFaceSize = 8;
		Settings.BrdfLutSize = 8;
		Settings.IrradianceSamples = 32;
		Settings.PrefilterSamples = 32;
		Settings.BrdfSamples = 32;
		auto Processed = ProcessEnvironmentImage(ConstantEnvironment, Settings);
		PF_CHECK(Tests, Processed.has_value());
		if (Processed)
		{
			PF_CHECK(Tests, Processed->Environment.Size == 8 && Processed->DiffuseIrradiance.Size == 4);
			PF_CHECK(Tests, Processed->PrefilteredSpecular.size() == 4);
			PF_CHECK(Tests, Processed->PrefilteredSpecular.front().Size == 8 && Processed->PrefilteredSpecular.back().Size == 1);
			PF_CHECK(Tests, Processed->BrdfIntegrationLut.Width == 8 && Processed->BrdfIntegrationLut.Height == 8);
			for (const float Channel : Processed->Environment.Faces[0])
				PF_CHECK(Tests, std::isfinite(Channel));
			for (const float Channel : Processed->BrdfIntegrationLut.RGBA32FPixels)
				PF_CHECK(Tests, std::isfinite(Channel));
			const std::vector<float>& IrradiancePositiveX = Processed->DiffuseIrradiance.Faces[0];
			PF_CHECK(Tests, std::abs(IrradiancePositiveX[0] - 2.0f * std::numbers::pi_v<float>) < 0.05f);
			PF_CHECK(Tests, std::abs(IrradiancePositiveX[1] - 0.5f * std::numbers::pi_v<float>) < 0.05f);
			for (const FloatCubeLevel& Level : Processed->PrefilteredSpecular)
			{
				PF_CHECK(Tests, std::abs(Level.Faces[0][0] - 2.0f) < 0.01f);
				PF_CHECK(Tests, std::abs(Level.Faces[0][1] - 0.5f) < 0.01f);
			}
		}
		std::stop_source CancelSource;
		CancelSource.request_stop();
		const auto CancelledProcessing = ProcessEnvironmentImage(ConstantEnvironment, Settings, CancelSource.get_token());
		PF_CHECK(Tests, !CancelledProcessing &&
			CancelledProcessing.error().Code == EnvironmentProcessingErrorCode::Cancelled);
		auto InvalidSize = Settings;
		InvalidSize.EnvironmentFaceSize = 7;
		PF_CHECK(Tests, !ProcessEnvironmentImage(ConstantEnvironment, InvalidSize).has_value());
		ConstantEnvironment.RGBA32FPixels[0] = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ProcessEnvironmentImage(ConstantEnvironment, Settings).has_value());
	}

	void TestTextureAndSamplerValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		TextureDesc Description;
		Description.Width = 2;
		Description.Height = 2;
		Description.Format = TextureFormat::RGBA8_Srgb;
		PF_CHECK(Tests, ValidateTextureUpload(Description, 16).value() == 16);
		PF_CHECK(Tests, !ValidateTextureUpload(Description, 15).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(Description, 17).has_value());
		PF_CHECK(Tests, Description.Dimension == TextureDimension::Texture2D && Description.MipLevels == 1);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_UNorm) == 4);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA8_Srgb) == 4);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA16_Float) == 8);
		PF_CHECK(Tests, GetTextureFormatBytesPerPixel(TextureFormat::RGBA32_Float) == 16);
		PF_CHECK(Tests, !GetTextureFormatBytesPerPixel(TextureFormat::Depth32Float).has_value());

		TextureDesc HalfFloatDescription;
		HalfFloatDescription.Format = TextureFormat::RGBA16_Float;
		HalfFloatDescription.Width = 1;
		HalfFloatDescription.Height = 1;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 8).value() == 8);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 7).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 9).has_value());
		HalfFloatDescription.Width = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 0).has_value());
		HalfFloatDescription.Width = 2;
		HalfFloatDescription.Height = 3;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 48).value() == 48);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 47).has_value());
		HalfFloatDescription.Width = 4;
		HalfFloatDescription.Height = 4;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 128).value() == 128);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 127).has_value());

		const std::array<std::byte, 72> HalfFloatUploadBytes{};
		HalfFloatDescription.Width = 2;
		HalfFloatDescription.Height = 3;
		const TextureSubresourceData HalfFloatSubresource{ 0, 0, HalfFloatUploadBytes, 24 };
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&HalfFloatSubresource, 1)).has_value());
		const TextureSubresourceData InvalidHalfFloatPitch{ 0, 0, HalfFloatUploadBytes, 15 };
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&InvalidHalfFloatPitch, 1)).has_value());
		const TextureSubresourceData InvalidHalfFloatSize{ 0, 0,
			std::span<const std::byte>(HalfFloatUploadBytes).first(71), 24 };
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&InvalidHalfFloatSize, 1)).has_value());
		const std::array<std::byte, 73> OversizedHalfFloatUploadBytes{};
		const TextureSubresourceData OversizedHalfFloatSubresource{ 0, 0, OversizedHalfFloatUploadBytes, 24 };
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>(&OversizedHalfFloatSubresource, 1)).has_value());
		HalfFloatDescription.Width = 4;
		HalfFloatDescription.Height = 4;
		HalfFloatDescription.MipLevels = 3;
		const std::array<std::byte, 168> HalfFloatMips{};
		const std::array<TextureSubresourceData, 3> HalfFloatMipSubresources = {
			TextureSubresourceData{ 0, 0, std::span(HalfFloatMips).first(128), 0 },
			TextureSubresourceData{ 1, 0, std::span(HalfFloatMips).subspan(128, 32), 0 },
			TextureSubresourceData{ 2, 0, std::span(HalfFloatMips).last(8), 0 }
		};
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, HalfFloatMipSubresources).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span(HalfFloatMipSubresources).first(2)).has_value());
		HalfFloatDescription.MipLevels = 4;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>{}).has_value());
		HalfFloatDescription.MipLevels = 1;
		HalfFloatDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 0).value() == 0);
		HalfFloatDescription.Usage = TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, 0).value() == 0);
		HalfFloatDescription.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, 0).has_value());
		HalfFloatDescription.Usage = static_cast<TextureUsage>(0x80);
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription,
			std::span<const TextureSubresourceData>{}).has_value());
		HalfFloatDescription.Usage = TextureUsage::ShaderResource;
		HalfFloatDescription.Dimension = TextureDimension::TextureCube;
		HalfFloatDescription.Width = 1;
		HalfFloatDescription.Height = 1;
		std::array<std::byte, 48> HalfFloatCubeBytes{};
		std::array<TextureSubresourceData, 6> HalfFloatCube{};
		for (uint32_t Face = 0; Face < HalfFloatCube.size(); ++Face)
			HalfFloatCube[Face] = { 0, Face, std::span(HalfFloatCubeBytes).subspan(Face * 8, 8), 0 };
		PF_CHECK(Tests, ValidateTextureUpload(HalfFloatDescription, HalfFloatCube).has_value());
		HalfFloatDescription.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatDescription, HalfFloatCube).has_value());

		TextureDesc CubeDescription;
		CubeDescription.Width = 1;
		CubeDescription.Height = 1;
		CubeDescription.Format = TextureFormat::RGBA32_Float;
		CubeDescription.Dimension = TextureDimension::TextureCube;
		std::array<std::byte, 96> CubeUploadBytes{};
		std::array<TextureSubresourceData, 6> CubeSubresources{};
		for (uint32_t Face = 0; Face < CubeSubresources.size(); ++Face)
			CubeSubresources[Face] = { 0, Face, std::span(CubeUploadBytes).subspan(Face * 16, 16), 0 };
		PF_CHECK(Tests, ValidateTextureUpload(CubeDescription, CubeSubresources).has_value());
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, std::span(CubeSubresources).first(5)).has_value());
		auto DuplicateFaceUploads = CubeSubresources;
		DuplicateFaceUploads[5].ArraySlice = 4;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, DuplicateFaceUploads).has_value());
		auto InvalidFaceUpload = CubeSubresources;
		InvalidFaceUpload[0].ArraySlice = 6;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, InvalidFaceUpload).has_value());
		auto InvalidCubePitch = CubeSubresources;
		InvalidCubePitch[0].RowPitch = 15;
		PF_CHECK(Tests, !ValidateTextureUpload(CubeDescription, InvalidCubePitch).has_value());
		auto RectangularCube = CubeDescription;
		RectangularCube.Height = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(RectangularCube, CubeSubresources).has_value());
		auto ExcessMips = CubeDescription;
		ExcessMips.MipLevels = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(ExcessMips, CubeSubresources).has_value());

		TextureDesc MippedFloat2D = CubeDescription;
		MippedFloat2D.Dimension = TextureDimension::Texture2D;
		MippedFloat2D.Width = 2;
		MippedFloat2D.Height = 2;
		MippedFloat2D.MipLevels = 2;
		const std::array<std::byte, 80> MippedUploadBytes{};
		const std::array<TextureSubresourceData, 2> MippedSubresources = {
			TextureSubresourceData{ 0, 0, std::span(MippedUploadBytes).first(64), 0 },
			TextureSubresourceData{ 1, 0, std::span(MippedUploadBytes).last(16), 0 }
		};
		PF_CHECK(Tests, ValidateTextureUpload(MippedFloat2D, MippedSubresources).has_value());
		auto MissingMipUpload = MippedSubresources;
		MissingMipUpload[1].MipLevel = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(MippedFloat2D, MissingMipUpload).has_value());

		auto ZeroWidth = Description;
		ZeroWidth.Width = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(ZeroWidth, 0).has_value());

		auto UnsupportedFormat = Description;
		UnsupportedFormat.Format = static_cast<TextureFormat>(0xff);
		PF_CHECK(Tests, !ValidateTextureUpload(UnsupportedFormat, 16).has_value());

		auto OverflowDimensions = Description;
		OverflowDimensions.Width = UINT32_MAX;
		OverflowDimensions.Height = UINT32_MAX;
		PF_CHECK(Tests, !ValidateTextureUpload(OverflowDimensions, 0).has_value());
		auto HalfFloatOverflowDimensions = OverflowDimensions;
		HalfFloatOverflowDimensions.Format = TextureFormat::RGBA16_Float;
		PF_CHECK(Tests, !ValidateTextureUpload(HalfFloatOverflowDimensions, 0).has_value());

		TextureDesc DepthDescription;
		DepthDescription.Width = 128;
		DepthDescription.Height = 64;
		DepthDescription.Format = TextureFormat::Depth32Float;
		DepthDescription.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(DepthDescription, 0).value() == 0);

		auto ZeroDepthHeight = DepthDescription;
		ZeroDepthHeight.Height = 0;
		PF_CHECK(Tests, !ValidateTextureUpload(ZeroDepthHeight, 0).has_value());

		auto DepthAsShaderResource = DepthDescription;
		DepthAsShaderResource.Usage = TextureUsage::ShaderResource;
		PF_CHECK(Tests, !ValidateTextureUpload(DepthAsShaderResource, 0).has_value());

		PF_CHECK(Tests, !ValidateTextureUpload(DepthDescription, sizeof(float)).has_value());

		auto ColorAsDepthAttachment = Description;
		ColorAsDepthAttachment.Usage = TextureUsage::DepthStencilAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(ColorAsDepthAttachment, 16).has_value());

		auto RenderableColor = Description;
		RenderableColor.Usage = TextureUsage::ShaderResource | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(RenderableColor, 0).value() == 0);
		PF_CHECK(Tests, ValidateTextureUpload(RenderableColor, 16).value() == 16);
		PF_CHECK(Tests, !ValidateTextureUpload(RenderableColor, 15).has_value());

		auto AttachmentOnly = Description;
		AttachmentOnly.Usage = TextureUsage::ColorAttachment;
		PF_CHECK(Tests, ValidateTextureUpload(AttachmentOnly, 0).value() == 0);

		auto MissingShaderUpload = Description;
		PF_CHECK(Tests, !ValidateTextureUpload(MissingShaderUpload, 0).has_value());

		auto UnknownUsage = Description;
		UnknownUsage.Usage = static_cast<TextureUsage>(0x80);
		PF_CHECK(Tests, !ValidateTextureUpload(UnknownUsage, 16).has_value());

		auto SampleableDepth = DepthDescription;
		SampleableDepth.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		PF_CHECK(Tests, ValidateTextureUpload(SampleableDepth, 0).has_value());
		auto SampleableDepthCube = SampleableDepth;
		SampleableDepthCube.Dimension = TextureDimension::TextureCube;
		SampleableDepthCube.Width = 512;
		SampleableDepthCube.Height = 512;
		PF_CHECK(Tests, ValidateTextureUpload(SampleableDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto NonSquareDepthCube = SampleableDepthCube;
		NonSquareDepthCube.Width = 256;
		PF_CHECK(Tests, !ValidateTextureUpload(NonSquareDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto MippedDepthCube = SampleableDepthCube;
		MippedDepthCube.MipLevels = 2;
		PF_CHECK(Tests, !ValidateTextureUpload(MippedDepthCube, std::span<const TextureSubresourceData>{}).has_value());
		auto InvalidDepthUsage = DepthDescription;
		InvalidDepthUsage.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ColorAttachment;
		PF_CHECK(Tests, !ValidateTextureUpload(InvalidDepthUsage, 0).has_value());

		RenderTargetDesc TargetDescription;
		TargetDescription.Width = 800;
		TargetDescription.Height = 600;
		PF_CHECK(Tests, ValidateRenderTargetDescription(TargetDescription).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
			TargetDescription, 800, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
			TargetDescription, 0, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
			TargetDescription, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment));
		PF_CHECK(Tests, !MatchesRenderTargetConfiguration(
			TargetDescription, 800, 600, ColorTargetFormat::RGBA8_Srgb, DepthAttachmentMode::None));
		auto HalfFloatColorTarget = TargetDescription;
		HalfFloatColorTarget.ColorFormat = ColorTargetFormat::RGBA16_Float;
		HalfFloatColorTarget.DepthMode = DepthAttachmentMode::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(HalfFloatColorTarget).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
			HalfFloatColorTarget, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::None));
		HalfFloatColorTarget.DepthMode = DepthAttachmentMode::Attachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(HalfFloatColorTarget).has_value());
		PF_CHECK(Tests, MatchesRenderTargetConfiguration(
			HalfFloatColorTarget, 800, 600, ColorTargetFormat::RGBA16_Float, DepthAttachmentMode::Attachment));
		auto DepthOnlyTarget = TargetDescription;
		DepthOnlyTarget.ColorFormat = ColorTargetFormat::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(DepthOnlyTarget).has_value());
		PF_CHECK(Tests, DepthOnlyTarget.DepthMode == DepthAttachmentMode::Attachment);
		auto SampleableDepthOnlyTarget = DepthOnlyTarget;
		SampleableDepthOnlyTarget.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(SampleableDepthOnlyTarget).has_value());
		auto ColorOnlyTarget = TargetDescription;
		ColorOnlyTarget.DepthMode = DepthAttachmentMode::None;
		PF_CHECK(Tests, ValidateRenderTargetDescription(ColorOnlyTarget).has_value());
		auto ColorSampleableDepthTarget = TargetDescription;
		ColorSampleableDepthTarget.DepthMode = DepthAttachmentMode::ShaderReadableAttachment;
		PF_CHECK(Tests, ValidateRenderTargetDescription(ColorSampleableDepthTarget).has_value());
		auto NoAttachmentsTarget = ColorOnlyTarget;
		NoAttachmentsTarget.ColorFormat = ColorTargetFormat::None;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(NoAttachmentsTarget).has_value());
		auto InvalidDepthMode = TargetDescription;
		InvalidDepthMode.DepthMode = static_cast<DepthAttachmentMode>(0xff);
		PF_CHECK(Tests, !ValidateRenderTargetDescription(InvalidDepthMode).has_value());
		auto ZeroTargetWidth = TargetDescription;
		ZeroTargetWidth.Width = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetWidth).has_value());
		auto ZeroTargetHeight = TargetDescription;
		ZeroTargetHeight.Height = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetHeight).has_value());
		auto SwapchainTarget = TargetDescription;
		SwapchainTarget.ColorFormat = ColorTargetFormat::Swapchain;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(SwapchainTarget).has_value());
		auto UnknownTargetFormat = TargetDescription;
		UnknownTargetFormat.ColorFormat = static_cast<ColorTargetFormat>(0xff);
		PF_CHECK(Tests, !ValidateRenderTargetDescription(UnknownTargetFormat).has_value());

		RenderTargetClearValue ClearValue;
		PF_CHECK(Tests, ValidateRenderTargetClearValue(ClearValue).has_value());
		auto InvalidClearDepth = ClearValue;
		InvalidClearDepth.Depth = 1.1f;
		PF_CHECK(Tests, !ValidateRenderTargetClearValue(InvalidClearDepth).has_value());
		auto NonFiniteClearColor = ClearValue;
		NonFiniteClearColor.Color[2] = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !ValidateRenderTargetClearValue(NonFiniteClearColor).has_value());

		SamplerDesc SamplerDescription;
		PF_CHECK(Tests, ValidateSamplerDescription(SamplerDescription).has_value());
		auto UnsupportedFilter = SamplerDescription;
		UnsupportedFilter.Minification = static_cast<SamplerFilter>(0xff);
		PF_CHECK(Tests, !ValidateSamplerDescription(UnsupportedFilter).has_value());
		auto UnsupportedAddressMode = SamplerDescription;
		UnsupportedAddressMode.AddressV = static_cast<SamplerAddressMode>(0xff);
		PF_CHECK(Tests, !ValidateSamplerDescription(UnsupportedAddressMode).has_value());
	}

	void TestTextureSamplerAndBufferBindingValidation(TestRunner& Tests)
	{
		using namespace PulseForge;
		BindingLayoutDesc LayoutDescription;
		LayoutDescription.Visibility = ShaderVisibility::Fragment;
		LayoutDescription.Items = {
			{ BindingResourceType::Texture2D, 0 },
			{ BindingResourceType::Sampler, 0 },
			{ BindingResourceType::ConstantBuffer, 0 }
		};
		LayoutDescription.DebugName = "Test texture/sampler/constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateTextureSlot = LayoutDescription;
		DuplicateTextureSlot.Items.push_back({ BindingResourceType::Texture2D, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateTextureSlot).has_value());

		BindingLayoutHandle Layout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TextureDesc TextureDescription;
		TextureDescription.Width = 1;
		TextureDescription.Height = 1;
		TextureDescription.DebugName = "Test texture";
		TestTexture FakeTexture(TextureDescription);
		TextureDesc DepthTextureDescription;
		DepthTextureDescription.Width = 1;
		DepthTextureDescription.Height = 1;
		DepthTextureDescription.Format = TextureFormat::Depth32Float;
		DepthTextureDescription.Usage = TextureUsage::DepthStencilAttachment;
		TestTexture FakeDepthTexture(DepthTextureDescription);
		DepthTextureDescription.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		TestTexture FakeSampleableDepthTexture(DepthTextureDescription);
		TestSampler Sampler;
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });

		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Textures.push_back({ 0, std::cref(static_cast<const PulseForge::Texture&>(FakeTexture)) });
		SetDescription.Samplers.push_back({ 0, std::cref(static_cast<const PulseForge::Sampler&>(Sampler)) });
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto CubeLayoutDescription = LayoutDescription;
		CubeLayoutDescription.Items[0].Type = BindingResourceType::TextureCube;
		BindingLayoutHandle CubeLayout = std::make_shared<TestBindingLayout>(CubeLayoutDescription);
		TextureDesc CubeTextureDescription = TextureDescription;
		CubeTextureDescription.Dimension = TextureDimension::TextureCube;
		TestTexture FakeCubeTexture(CubeTextureDescription);
		auto CubeBindingDescription = SetDescription;
		CubeBindingDescription.Layout = CubeLayout;
		CubeBindingDescription.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeCubeTexture));
		CubeBindingDescription.Textures[0].Type = BindingResourceType::TextureCube;
		PF_CHECK(Tests, ValidateBindingSet(CubeBindingDescription).has_value());
		auto WrongCubeDimension = CubeBindingDescription;
		WrongCubeDimension.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeTexture));
		PF_CHECK(Tests, !ValidateBindingSet(WrongCubeDimension).has_value());
		auto Wrong2DDimension = SetDescription;
		Wrong2DDimension.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeCubeTexture));
		PF_CHECK(Tests, !ValidateBindingSet(Wrong2DDimension).has_value());

		auto DepthTextureBinding = SetDescription;
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeDepthTexture));
		PF_CHECK(Tests, !ValidateBindingSet(DepthTextureBinding).has_value());
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeSampleableDepthTexture));
		PF_CHECK(Tests, ValidateBindingSet(DepthTextureBinding).has_value());
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeSampleableDepthTexture));
		DepthTextureBinding.Textures[0].Type = BindingResourceType::TextureCube;
		PF_CHECK(Tests, !ValidateBindingSet(DepthTextureBinding).has_value());

		auto MissingTexture = SetDescription;
		MissingTexture.Textures.clear();
		PF_CHECK(Tests, !ValidateBindingSet(MissingTexture).has_value());

		auto WrongSamplerSlot = SetDescription;
		WrongSamplerSlot.Samplers[0].Slot = 1;
		PF_CHECK(Tests, !ValidateBindingSet(WrongSamplerSlot).has_value());

		auto DuplicateSamplerSlot = SetDescription;
		DuplicateSamplerSlot.Samplers.push_back(DuplicateSamplerSlot.Samplers[0]);
		PF_CHECK(Tests, !ValidateBindingSet(DuplicateSamplerSlot).has_value());
	}

	void TestVulkanQueueFamilySelection(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const std::vector<QueueFamily> SameFamily = {
			{ 0, 0, true, true },
			{ 1, 1, true, true }
		};
		auto Same = ChooseQueueFamilies(SameFamily);
		PF_CHECK(Tests, Same.has_value());
		PF_CHECK(Tests, Same->GraphicsIndex == 1 && Same->PresentIndex == 1);

		const std::vector<QueueFamily> SeparateFamilies = {
			{ 3, 1, true, false },
			{ 5, 1, false, true }
		};
		auto Separate = ChooseQueueFamilies(SeparateFamilies);
		PF_CHECK(Tests, Separate.has_value());
		PF_CHECK(Tests, Separate->GraphicsIndex == 3 && Separate->PresentIndex == 5);

		const std::vector<QueueFamily> MissingPresent = { { 0, 1, true, false } };
		PF_CHECK(Tests, !ChooseQueueFamilies(MissingPresent).has_value());
	}

	void TestVulkanRequiredDeviceFeatures(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		PF_CHECK(Tests, SupportsRequiredDeviceFeatures({ true, true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ false, true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, false, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, false, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, true, false }));
	}

	void TestVulkanFormatUsageCapabilities(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const FormatFeatureSupport AllFeatures{ true, true, true };
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, true, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, false, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, false, true, false));
		PF_CHECK(Tests, SupportsFormatUsage(AllFeatures, true, true, true));
		PF_CHECK(Tests, SupportsFormatUsage({}, false, false, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ false, true, true }, true, true, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, false, true }, true, true, false));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, true, false }, true, true, true));
		PF_CHECK(Tests, !SupportsFormatUsage({ true, true, false }, false, false, true));
		PF_CHECK(Tests, !SupportsFormatUsage({ false, false, true }, false, false, true));
	}

	void TestVulkanSurfaceAndPresentationSelection(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const std::vector<SurfaceFormat> Formats = { { 10, 1 }, { 20, 3 } };
		auto Preferred = ChooseSurfaceFormat(Formats, 20, 3, 0);
		PF_CHECK(Tests, Preferred.has_value());
		PF_CHECK(Tests, Preferred->Format == 20 && Preferred->ColorSpace == 3);

		const std::vector<SurfaceFormat> UndefinedFormat = { { 0, 5 } };
		auto ChosenForUndefined = ChooseSurfaceFormat(UndefinedFormat, 20, 3, 0);
		PF_CHECK(Tests, ChosenForUndefined.has_value());
		PF_CHECK(Tests, ChosenForUndefined->Format == 20 && ChosenForUndefined->ColorSpace == 5);

		const std::vector<PresentMode> Modes = {
			PresentMode::Immediate, PresentMode::Mailbox, PresentMode::Fifo
		};
		PF_CHECK(Tests, ChoosePresentMode(Modes, true) == PresentMode::Fifo);
		PF_CHECK(Tests, ChoosePresentMode(Modes, false) == PresentMode::Mailbox);
		const std::vector<PresentMode> NoMailbox = { PresentMode::Immediate, PresentMode::Fifo };
		PF_CHECK(Tests, ChoosePresentMode(NoMailbox, false) == PresentMode::Immediate);
	}

	void TestVulkanExtentAndImageCountSelection(TestRunner& Tests)
	{
		using namespace PulseForge::VulkanSupport;
		const auto Clamped = ChooseExtent({ 2400, 200 }, {}, false, { 640, 480 }, { 1920, 1080 });
		PF_CHECK(Tests, Clamped.Width == 1920 && Clamped.Height == 480);

		const auto Fixed = ChooseExtent({ 800, 600 }, { 1600, 900 }, true, {}, {});
		PF_CHECK(Tests, Fixed.Width == 1600 && Fixed.Height == 900);

		const auto Minimized = ChooseExtent({ 0, 0 }, {}, false, { 640, 480 }, { 1920, 1080 });
		PF_CHECK(Tests, Minimized.Width == 0 && Minimized.Height == 0);
		PF_CHECK(Tests, ChooseImageCount(2, 0) == 3);
		PF_CHECK(Tests, ChooseImageCount(2, 2) == 2);
		PF_CHECK(Tests, ChooseImageCount(3, 5) == 4);
	}

	void TestConsoleFormatting(TestRunner& Tests)
	{
		using PulseForgeEditor::ConsoleEntryView;
		using PulseForgeEditor::FormatConsoleEntry;
		using PulseForgeEditor::FormatConsoleHistory;

		PF_CHECK(Tests, FormatConsoleEntry({ "info", "PULSEFORGE", "Renderer ready" }) ==
			"[info] [PULSEFORGE] Renderer ready");
		PF_CHECK(Tests, FormatConsoleEntry({ "warning", "APP", "Slow asset" }) ==
			"[warning] [APP] Slow asset");
		PF_CHECK(Tests, FormatConsoleEntry({ "error", "PULSEFORGE", "Failed" }) ==
			"[error] [PULSEFORGE] Failed");
		PF_CHECK(Tests, FormatConsoleEntry({ "critical", "APP", "Fatal" }) ==
			"[critical] [APP] Fatal");
		PF_CHECK(Tests, FormatConsoleEntry({ "trace", "APP", "Trace" }) == "[trace] [APP] Trace");
		PF_CHECK(Tests, FormatConsoleEntry({ "debug", "APP", "Debug" }) == "[debug] [APP] Debug");
		PF_CHECK(Tests, FormatConsoleEntry({ {}, "APP", "Unknown severity" }) == "[unknown] [APP] Unknown severity");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", {}, "No logger" }) == "[info] No logger");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", "APP", {} }) == "[info] [APP]");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", {}, {} }) == "[info]");
		PF_CHECK(Tests, FormatConsoleEntry({ "info", "APP", "UTF-8: \xE2\x98\x83 and punctuation !?[]" }) ==
			"[info] [APP] UTF-8: \xE2\x98\x83 and punctuation !?[]");
		PF_CHECK(Tests, FormatConsoleEntry({ "debug", "APP", "tabs\tremain" }) == "[debug] [APP] tabs\tremain");

		const std::string TruncatedPayload = std::string(8192, 'x') + " [truncated]";
		PF_CHECK(Tests, FormatConsoleEntry({ "warning", "APP", TruncatedPayload }) ==
			"[warning] [APP] " + TruncatedPayload);

		PF_CHECK(Tests, FormatConsoleHistory({}).empty());
		const std::array Single = { ConsoleEntryView{ "info", "PULSEFORGE", "Only entry" } };
		PF_CHECK(Tests, FormatConsoleHistory(Single) == "[info] [PULSEFORGE] Only entry");
		const std::array Ordered = {
			ConsoleEntryView{ "info", "PULSEFORGE", "First" },
			ConsoleEntryView{ "warning", "APP", "Second" },
			ConsoleEntryView{ "error", "PULSEFORGE", "Third" }
		};
		const std::string FormattedHistory = FormatConsoleHistory(Ordered);
		PF_CHECK(Tests, FormattedHistory ==
			"[info] [PULSEFORGE] First\n[warning] [APP] Second\n[error] [PULSEFORGE] Third");
		PF_CHECK(Tests, !FormattedHistory.empty() && FormattedHistory.back() != '\n');
		PF_CHECK(Tests, std::count(FormattedHistory.begin(), FormattedHistory.end(), '\n') == 2);

		std::vector<ConsoleEntryView> CapacityBoundary;
		CapacityBoundary.reserve(2000);
		for (size_t Index = 0; Index < 2000; ++Index)
			CapacityBoundary.push_back({ "info", "APP", "retained" });
		const std::string CapacityHistory = FormatConsoleHistory(CapacityBoundary);
		PF_CHECK(Tests, std::count(CapacityHistory.begin(), CapacityHistory.end(), '\n') == 1999);
		PF_CHECK(Tests, CapacityHistory.starts_with("[info] [APP] retained\n"));
		PF_CHECK(Tests, CapacityHistory.ends_with("[info] [APP] retained"));
	}

	void TestLocalLightingMathAndSelection(TestRunner& Tests)
	{
		using namespace PulseForge;
		static_assert(sizeof(LocalLightingConstants) == 2064);
		PointLightComponent Point;
		SpotLightComponent Spot;
		PF_CHECK(Tests, Point.Validate().has_value() && Spot.Validate().has_value());
		Point.Intensity = 0.0f;
		PF_CHECK(Tests, Point.Validate().has_value());
		Point.Intensity = -1.0f;
		PF_CHECK(Tests, !Point.Validate());
		Point.Intensity = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !Point.Validate());
		Point.Intensity = 1000000.0f;
		Point.Range = 10000.0f;
		PF_CHECK(Tests, Point.Validate().has_value());
		Point.Color.r = -0.01f;
		PF_CHECK(Tests, !Point.Validate());
		Point.Color.r = 1.0f;
		Point.Intensity = 1.0f;
		Point.Range = 0.0f;
		PF_CHECK(Tests, !Point.Validate());
		Point.Range = 1.0f;
		Point.Color.g = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !Point.Validate());
		Spot.OuterConeAngleDegrees = Spot.InnerConeAngleDegrees;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.InnerConeAngleDegrees = 30.0f;
		Spot.OuterConeAngleDegrees = 20.0f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.InnerConeAngleDegrees = 10.0f;
		Spot.OuterConeAngleDegrees = 90.0f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.InnerConeAngleDegrees = 0.5f;
		Spot.OuterConeAngleDegrees = 89.5f;
		Spot.Intensity = 0.0f;
		PF_CHECK(Tests, Spot.Validate().has_value());
		Spot.OuterConeAngleDegrees = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !Spot.Validate());
		Spot.OuterConeAngleDegrees = 25.0f;
		Spot.ShadowBias = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !Spot.Validate());
		Spot.ShadowBias = 0.001f;
		Spot.ShadowNormalBias = -0.01f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.ShadowNormalBias = 0.025f;
		Spot.ShadowSoftness = 4.1f;
		PF_CHECK(Tests, !Spot.Validate());
		Spot.ShadowSoftness = 1.5f;

		const auto Projection = BuildSpotlightShadowProjection(
			glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, 30.0f);
		PF_CHECK(Tests, Projection.has_value());
		if (Projection)
		{
			glm::vec3 UvDepth;
			PF_CHECK(Tests, ProjectSpotlightShadowCoordinate(Projection->ViewProjection, { 0.0f, 0.0f, -5.0f }, UvDepth));
			PF_CHECK(Tests, glm::abs(UvDepth.x - 0.5f) < 0.0001f && glm::abs(UvDepth.y - 0.5f) < 0.0001f);
			PF_CHECK(Tests, UvDepth.z > 0.0f && UvDepth.z < 1.0f);
			PF_CHECK(Tests, !ProjectSpotlightShadowCoordinate(Projection->ViewProjection, { 0.0f, 0.0f, 1.0f }, UvDepth));
			PF_CHECK(Tests, !ProjectSpotlightShadowCoordinate(Projection->ViewProjection, { 0.0f, 0.0f, -12.0f }, UvDepth));
		}
		PF_CHECK(Tests, !BuildSpotlightShadowProjection(
			glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 10.0f, 85.0f));
		PF_CHECK(Tests, !BuildSpotlightShadowProjection(
			glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 0.0f), 10.0f, 30.0f));
		PF_CHECK(Tests, !BuildSpotlightShadowProjection(
			glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, -1.0f), 0.01f, 30.0f));
		const auto VerticalProjection = BuildSpotlightShadowProjection(
			glm::vec3(1.0f), glm::vec3(0.0f, 1.0f, 0.00001f), 10.0f, 45.0f);
		PF_CHECK(Tests, VerticalProjection.has_value());
		PF_CHECK(Tests, VerticalProjection && std::isfinite(VerticalProjection->ViewProjection[0][0]));
		PF_CHECK(Tests, MaxSpotlightShadowMapCount == 4 && SpotlightShadowMapResolution == 1024);
		PF_CHECK(Tests, !Spot.CastShadows);

		SpotlightShadowSlotOwners ShadowOwners{};
		const std::array<SpotlightShadowCandidate, 5> ShadowCandidates = {{
			{ UUID(0, 10), 0 }, { UUID(0, 20), 1 }, { UUID(0, 30), 2 },
			{ UUID(0, 40), 3 }, { UUID(0, 50), 4 }
		}};
		const auto InitialAssignments = AssignSpotlightShadowSlots(ShadowCandidates, ShadowOwners);
		PF_CHECK(Tests, InitialAssignments[0] == 0 && InitialAssignments[1] == 1 &&
			InitialAssignments[2] == 2 && InitialAssignments[3] == 3 && InitialAssignments[4] == -1);
		const std::array<SpotlightShadowCandidate, 4> ReorderedCandidates = {{
			{ UUID(0, 40), 0 }, { UUID(0, 30), 1 }, { UUID(0, 20), 2 }, { UUID(0, 10), 3 }
		}};
		const auto StableAssignments = AssignSpotlightShadowSlots(ReorderedCandidates, ShadowOwners);
		PF_CHECK(Tests, StableAssignments[0] == 3 && StableAssignments[1] == 2 &&
			StableAssignments[2] == 1 && StableAssignments[3] == 0);
		const std::array<SpotlightShadowCandidate, 4> ReassignedCandidates = {{
			{ UUID(0, 40), 0 }, { UUID(0, 30), 1 }, { UUID(0, 20), 2 }, { UUID(0, 60), 3 }
		}};
		const auto Reassigned = AssignSpotlightShadowSlots(ReassignedCandidates, ShadowOwners);
		PF_CHECK(Tests, Reassigned[0] == 3 && Reassigned[1] == 2 && Reassigned[2] == 1 && Reassigned[3] == 0);
		const auto ClearedAssignments = AssignSpotlightShadowSlots({}, ShadowOwners);
		PF_CHECK(Tests, std::ranges::all_of(ClearedAssignments, [](int32_t Slot) { return Slot == -1; }));
		PF_CHECK(Tests, std::ranges::all_of(ShadowOwners, [](const auto& Owner) { return !Owner.has_value(); }));

		// Over-budget lights are selected by candidate rank every update; slot persistence only applies inside
		// that selected set. A previously waiting light must not retain a slot over a higher-ranked light.
		const std::array<SpotlightShadowCandidate, 5> RankedCandidates = {{
			{ UUID(0, 101), 0 }, { UUID(0, 102), 1 }, { UUID(0, 103), 2 },
			{ UUID(0, 104), 3 }, { UUID(0, 105), 4 }
		}};
		SpotlightShadowSlotOwners RankedOwners{};
		const auto RankedInitial = AssignSpotlightShadowSlots(RankedCandidates, RankedOwners);
		PF_CHECK(Tests, RankedInitial[0] == 0 && RankedInitial[1] == 1 &&
			RankedInitial[2] == 2 && RankedInitial[3] == 3 && RankedInitial[4] == -1);
		const std::array<SpotlightShadowCandidate, 4> WithoutThird = {{
			{ UUID(0, 101), 0 }, { UUID(0, 102), 1 }, { UUID(0, 104), 3 }, { UUID(0, 105), 4 }
		}};
		const auto PromotedFifth = AssignSpotlightShadowSlots(WithoutThird, RankedOwners);
		PF_CHECK(Tests, PromotedFifth[0] == 0 && PromotedFifth[1] == 1 &&
			PromotedFifth[2] == -1 && PromotedFifth[3] == 3 && PromotedFifth[4] == 2);
		PF_CHECK(Tests, RankedOwners[2] == UUID(0, 105) && RankedOwners[3] == UUID(0, 104));
		const auto RestoredThird = AssignSpotlightShadowSlots(RankedCandidates, RankedOwners);
		PF_CHECK(Tests, RestoredThird[0] == 0 && RestoredThird[1] == 1 &&
			RestoredThird[2] == 2 && RestoredThird[3] == 3 && RestoredThird[4] == -1);
		PF_CHECK(Tests, RankedOwners[2] == UUID(0, 103) && RankedOwners[3] == UUID(0, 104));
		const std::array<SpotlightShadowCandidate, 4> WithoutFirst = {{
			{ UUID(0, 102), 1 }, { UUID(0, 103), 2 }, { UUID(0, 104), 3 }, { UUID(0, 105), 4 }
		}};
		const auto FirstDisabled = AssignSpotlightShadowSlots(WithoutFirst, RankedOwners);
		PF_CHECK(Tests, FirstDisabled[0] == -1 && FirstDisabled[1] == 1 &&
			FirstDisabled[2] == 2 && FirstDisabled[3] == 3 && FirstDisabled[4] == 0);
		const auto FirstRestored = AssignSpotlightShadowSlots(RankedCandidates, RankedOwners);
		PF_CHECK(Tests, FirstRestored[0] == 0 && FirstRestored[1] == 1 &&
			FirstRestored[2] == 2 && FirstRestored[3] == 3 && FirstRestored[4] == -1);
		PF_CHECK(Tests, std::ranges::count_if(RankedOwners,
			[](const auto& Owner) { return Owner.has_value(); }) == MaxSpotlightShadowMapCount);

		const std::array<SpotlightShadowCandidate, 2> UnderCapacityCandidates = {{
			{ UUID(0, 201), 0 }, { UUID(0, 202), 1 }
		}};
		SpotlightShadowSlotOwners UnderCapacityOwners{};
		const auto UnderCapacityOn = AssignSpotlightShadowSlots(UnderCapacityCandidates, UnderCapacityOwners);
		PF_CHECK(Tests, UnderCapacityOn[0] >= 0 && UnderCapacityOn[1] >= 0);
		const auto UnderCapacityOff = AssignSpotlightShadowSlots({}, UnderCapacityOwners);
		PF_CHECK(Tests, UnderCapacityOff[0] == -1 && UnderCapacityOff[1] == -1 &&
			std::ranges::none_of(UnderCapacityOwners, [](const auto& Owner) { return Owner.has_value(); }));
		const auto UnderCapacityRestored = AssignSpotlightShadowSlots(UnderCapacityCandidates, UnderCapacityOwners);
		PF_CHECK(Tests, UnderCapacityRestored[0] >= 0 && UnderCapacityRestored[1] >= 0);

		const std::array<SpotlightShadowCandidate, 4> AtCapacityCandidates = {{
			{ UUID(0, 301), 0 }, { UUID(0, 302), 1 }, { UUID(0, 303), 2 }, { UUID(0, 304), 3 }
		}};
		SpotlightShadowSlotOwners AtCapacityOwners{};
		const auto AtCapacityInitial = AssignSpotlightShadowSlots(AtCapacityCandidates, AtCapacityOwners);
		PF_CHECK(Tests, std::ranges::count_if(AtCapacityInitial, [](int32_t Slot) { return Slot >= 0; }) == 4);
		for (size_t DisabledIndex = 0; DisabledIndex < AtCapacityCandidates.size(); ++DisabledIndex)
		{
			std::array<SpotlightShadowCandidate, 3> ReducedCandidates{};
			size_t WriteIndex = 0;
			for (size_t CandidateIndex = 0; CandidateIndex < AtCapacityCandidates.size(); ++CandidateIndex)
				if (CandidateIndex != DisabledIndex)
					ReducedCandidates[WriteIndex++] = AtCapacityCandidates[CandidateIndex];
			const auto Reduced = AssignSpotlightShadowSlots(ReducedCandidates, AtCapacityOwners);
			PF_CHECK(Tests, std::ranges::count_if(Reduced, [](int32_t Slot) { return Slot >= 0; }) == 3);
			const auto Restored = AssignSpotlightShadowSlots(AtCapacityCandidates, AtCapacityOwners);
			PF_CHECK(Tests, std::ranges::count_if(Restored, [](int32_t Slot) { return Slot >= 0; }) == 4);
			std::array<bool, MaxSpotlightShadowMapCount> UsedSlots{};
			for (size_t CandidateIndex = 0; CandidateIndex < AtCapacityCandidates.size(); ++CandidateIndex)
			{
				const int32_t Slot = Restored[CandidateIndex];
				PF_CHECK(Tests, Slot >= 0 && static_cast<size_t>(Slot) < UsedSlots.size());
				if (Slot >= 0 && static_cast<size_t>(Slot) < UsedSlots.size())
				{
					PF_CHECK(Tests, !UsedSlots[static_cast<size_t>(Slot)]);
					UsedSlots[static_cast<size_t>(Slot)] = true;
				}
			}
		}

		const float AtZero = EvaluateLocalLightRangeAttenuation(0.0f, 10.0f);
		const float AtOne = EvaluateLocalLightRangeAttenuation(1.0f, 10.0f);
		const float AtFive = EvaluateLocalLightRangeAttenuation(5.0f, 10.0f);
		PF_CHECK(Tests, std::isfinite(AtZero) && AtZero > 0.0f);
		PF_CHECK(Tests, AtZero > AtOne && AtOne > AtFive && AtFive > 0.0f);
		PF_CHECK(Tests, EvaluateLocalLightRangeAttenuation(10.0f, 10.0f) == 0.0f);
		PF_CHECK(Tests, EvaluateLocalLightRangeAttenuation(std::numeric_limits<float>::quiet_NaN(), 10.0f) == 0.0f);
		PF_CHECK(Tests, EvaluateLocalLightRangeAttenuation(1.0f, 0.0f) == 0.0f);
		const float InnerCos = std::cos(glm::radians(15.0f));
		const float OuterCos = std::cos(glm::radians(30.0f));
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(1.0f, 15.0f, 30.0f) == 1.0f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(InnerCos, 15.0f, 30.0f) > 0.999f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(OuterCos, 15.0f, 30.0f) < 0.001f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(-1.0f, 15.0f, 30.0f) == 0.0f);
		PF_CHECK(Tests, EvaluateSpotLightAngularAttenuation(
			std::numeric_limits<float>::quiet_NaN(), 15.0f, 30.0f) == 0.0f);

		std::array<LocalLightRelevance, 4> Lights = {{
			{ UUID{ 0, 4 }, { 0.0f, 0.0f, 0.0f }, glm::vec3(1.0f), 1.0f, 10.0f },
			{ UUID{ 0, 2 }, { 0.0f, 0.0f, 0.0f }, glm::vec3(1.0f), 1.0f, 10.0f },
			{ UUID{ 0, 3 }, { 0.0f, 0.0f, 0.0f }, glm::vec3(1.0f), 0.0f, 10.0f },
			{ UUID{ 0, 1 }, { 2.0f, 0.0f, 0.0f }, glm::vec3(2.0f), 10.0f, 8.0f }
		}};
		const auto Selected = SelectLocalLightIndices(Lights, glm::vec3(0.0f), 2);
		PF_CHECK(Tests, Selected.size() == 2 && Selected[0] == 3);
		PF_CHECK(Tests, Selected.size() == 2 && Selected[1] == 1);
		PF_CHECK(Tests, SelectLocalLightIndices({}, glm::vec3(0.0f)).empty());
		std::array<LocalLightRelevance, 2> SameEntityTypes = {{
			{ UUID{ 0, 9 }, glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 1.0f, SpotLocalLightTypeOrder },
			{ UUID{ 0, 9 }, glm::vec3(0.0f), glm::vec3(1.0f), 1.0f, 1.0f, PointLocalLightTypeOrder }
		}};
		const auto TypeTied = SelectLocalLightIndices(SameEntityTypes, glm::vec3(0.0f));
		PF_CHECK(Tests, TypeTied.size() == 2 && TypeTied[0] == 1 && TypeTied[1] == 0);
		const UUID SharedEntity{ 0, 9 };
		const auto SelectedIndexForType = [&](uint32_t TypeOrder, const auto& Order)
		{
			for (size_t Rank = 0; Rank < Order.size(); ++Rank)
				if (MatchesLocalLightIdentity(SameEntityTypes[Order[Rank]], SharedEntity, TypeOrder))
					return Rank;
			return Order.size();
		};
		SameEntityTypes[0].Intensity = 1.0f;
		SameEntityTypes[1].Intensity = 10.0f;
		const auto PointRanksFirst = SelectLocalLightIndices(SameEntityTypes, glm::vec3(0.0f));
		PF_CHECK(Tests, SelectedIndexForType(PointLocalLightTypeOrder, PointRanksFirst) == 0);
		PF_CHECK(Tests, SelectedIndexForType(SpotLocalLightTypeOrder, PointRanksFirst) == 1);
		SameEntityTypes[0].Intensity = 10.0f;
		SameEntityTypes[1].Intensity = 1.0f;
		const auto SpotRanksFirst = SelectLocalLightIndices(SameEntityTypes, glm::vec3(0.0f));
		PF_CHECK(Tests, SelectedIndexForType(SpotLocalLightTypeOrder, SpotRanksFirst) == 0);
		PF_CHECK(Tests, SelectedIndexForType(PointLocalLightTypeOrder, SpotRanksFirst) == 1);

		PointLightComponent SharedPoint;
		SpotLightComponent SharedSpot;
		SharedPoint.CastShadows = true;
		PointShadowSlotOwners SharedPointOwners{};
		SpotlightShadowSlotOwners SharedSpotOwners{};
		const std::array<PointShadowCandidate, 1> SharedPointCandidate = {{ { SharedEntity, 1 } }};
		const auto PointOnlySlots = AssignPointShadowSlots(SharedPointCandidate, SharedPointOwners);
		const auto NoSpotSlots = AssignSpotlightShadowSlots({}, SharedSpotOwners);
		PF_CHECK(Tests, SharedPoint.CastShadows && !SharedSpot.CastShadows && PointOnlySlots[1] == 0);
		PF_CHECK(Tests, SharedPointOwners[0] == SharedEntity && !SharedSpotOwners[0] && NoSpotSlots[0] == -1);

		SharedPoint.CastShadows = false;
		SharedSpot.CastShadows = true;
		const auto NoPointSlots = AssignPointShadowSlots({}, SharedPointOwners);
		const std::array<SpotlightShadowCandidate, 1> SharedSpotCandidate = {{ { SharedEntity, 0 } }};
		const auto SpotOnlySlots = AssignSpotlightShadowSlots(SharedSpotCandidate, SharedSpotOwners);
		PF_CHECK(Tests, !SharedPoint.CastShadows && SharedSpot.CastShadows && NoPointSlots[0] == -1);
		PF_CHECK(Tests, !SharedPointOwners[0] && SharedSpotOwners[0] == SharedEntity && SpotOnlySlots[0] == 0);

		SharedPoint.CastShadows = true;
		const auto BothPointSlots = AssignPointShadowSlots(SharedPointCandidate, SharedPointOwners);
		const auto BothSpotSlots = AssignSpotlightShadowSlots(SharedSpotCandidate, SharedSpotOwners);
		PF_CHECK(Tests, BothPointSlots[1] == 0 && BothSpotSlots[0] == 0);
		PF_CHECK(Tests, SharedPointOwners[0] == SharedEntity && SharedSpotOwners[0] == SharedEntity);
		PF_CHECK(Tests, MatchesLocalLightIdentity(SameEntityTypes[SpotRanksFirst[0]], SharedEntity, SpotLocalLightTypeOrder));
		PF_CHECK(Tests, MatchesLocalLightIdentity(SameEntityTypes[SpotRanksFirst[1]], SharedEntity, PointLocalLightTypeOrder));
		std::vector<LocalLightRelevance> CapacityLights(MaxLocalLightCount + 1);
		for (size_t Index = 0; Index < CapacityLights.size(); ++Index)
		{
			CapacityLights[Index].Entity = UUID{ 0, Index + 1 };
			CapacityLights[Index].Intensity = 1.0f;
			CapacityLights[Index].Range = 1.0f;
		}
		const auto CapacityBounded = SelectLocalLightIndices(CapacityLights, glm::vec3(0.0f));
		PF_CHECK(Tests, CapacityBounded.size() == MaxLocalLightCount);
		PF_CHECK(Tests, std::find(CapacityBounded.begin(), CapacityBounded.end(), MaxLocalLightCount) == CapacityBounded.end());
	}

	void TestPointLightShadowMath(TestRunner& Tests)
	{
		using namespace PulseForge;
		PointLightComponent Light;
		PF_CHECK(Tests, !Light.CastShadows && Light.Validate());
		Light.CastShadows = true;
		PF_CHECK(Tests, Light.Validate().has_value());
		Light.ShadowBias = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !Light.Validate());
		Light.ShadowBias = 0.001f;
		Light.ShadowNormalBias = -0.1f;
		PF_CHECK(Tests, !Light.Validate());
		Light.ShadowNormalBias = 0.025f;
		Light.ShadowSoftness = MaxPointLightShadowSoftness + 0.1f;
		PF_CHECK(Tests, !Light.Validate());
		Light.ShadowSoftness = 1.5f;

		const auto Projections = BuildPointShadowProjections({ 1.0f, 2.0f, 3.0f }, 10.0f);
		PF_CHECK(Tests, Projections.has_value());
		if (Projections)
		{
			PF_CHECK(Tests, Projections->NearPlane > 0.0f && Projections->NearPlane < Projections->FarPlane);
			PF_CHECK(Tests, Projections->FarPlane == 10.0f);
			for (size_t Face = 0; Face < Projections->Faces.size(); ++Face)
			{
				const auto& Projection = Projections->Faces[Face];
				const glm::vec4 Clip = Projection.ViewProjection * glm::vec4(
					glm::vec3(1.0f, 2.0f, 3.0f) + Projection.Direction * 5.0f, 1.0f);
				PF_CHECK(Tests, std::isfinite(Clip.x) && std::isfinite(Clip.y) && std::isfinite(Clip.z) && Clip.w > 0.0f);
				PF_CHECK(Tests, glm::abs(Clip.x / Clip.w) < 0.0001f && glm::abs(Clip.y / Clip.w) < 0.0001f);
				PF_CHECK(Tests, Clip.z / Clip.w > 0.0f && Clip.z / Clip.w < 1.0f);
			}
		}
		const auto ShortRange = BuildPointShadowProjections(glm::vec3(0.0f), 0.01f);
		PF_CHECK(Tests, ShortRange.has_value() && ShortRange->NearPlane < ShortRange->FarPlane);
		PF_CHECK(Tests, !BuildPointShadowProjections({ std::numeric_limits<float>::infinity(), 0.0f, 0.0f }, 1.0f));
		PF_CHECK(Tests, !BuildPointShadowProjections(glm::vec3(0.0f), 0.0f));
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 1.0f, 0.0f, 0.0f }) == PointShadowCubeFace::PositiveX);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ -1.0f, 0.0f, 0.0f }) == PointShadowCubeFace::NegativeX);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, 1.0f, 0.0f }) == PointShadowCubeFace::PositiveY);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, -1.0f, 0.0f }) == PointShadowCubeFace::NegativeY);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, 0.0f, 1.0f }) == PointShadowCubeFace::PositiveZ);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 0.0f, 0.0f, -1.0f }) == PointShadowCubeFace::NegativeZ);
		PF_CHECK(Tests, SelectPointShadowCubeFace({ 1.0f, 1.0f, 0.0f }) == PointShadowCubeFace::PositiveX);
		PF_CHECK(Tests, EncodePointShadowRadialDepth(5.0f, 10.0f) == 0.5f);
		PF_CHECK(Tests, EncodePointShadowRadialDepth(20.0f, 10.0f) == 1.0f);
		PF_CHECK(Tests, EncodePointShadowRadialDepth(std::numeric_limits<float>::quiet_NaN(), 10.0f) == 1.0f);
		PF_CHECK(Tests, IsPointShadowOccluded(0.8f, 0.5f, 0.01f));
		PF_CHECK(Tests, !IsPointShadowOccluded(0.5f, 0.5f, 0.01f));

		const std::array<PointShadowCandidate, 3> Candidates = {{
			{ UUID{ 0, 11 }, 0 }, { UUID{ 0, 12 }, 1 }, { UUID{ 0, 13 }, 2 }
		}};
		PointShadowSlotOwners Owners{};
		const auto Initial = AssignPointShadowSlots(Candidates, Owners);
		PF_CHECK(Tests, Initial[0] == 0 && Initial[1] == 1 && Initial[2] == -1);
		const std::array<PointShadowCandidate, 2> Replaced = {{
			{ UUID{ 0, 12 }, 1 }, { UUID{ 0, 13 }, 2 }
		}};
		const auto Reassignment = AssignPointShadowSlots(Replaced, Owners);
		PF_CHECK(Tests, Reassignment[0] == -1 && Reassignment[1] == 1 && Reassignment[2] == 0);
		const auto Restored = AssignPointShadowSlots(Candidates, Owners);
		PF_CHECK(Tests, Restored[0] == 0 && Restored[1] == 1 && Restored[2] == -1);
		PF_CHECK(Tests, Owners[0] == (UUID{ 0, 11 }) && Owners[1] == (UUID{ 0, 12 }));
		const auto Cleared = AssignPointShadowSlots({}, Owners);
		PF_CHECK(Tests, Cleared[0] == -1 && Cleared[1] == -1 && !Owners[0] && !Owners[1]);
		PF_CHECK(Tests, MaxPointLightShadowCount == 2 && PointLightShadowMapResolution == 512);
	}

	void TestEditorCommandHistory(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorCommandHistory;
		EditorCommandHistory History(2, 1024);
		History.Reset();
		PF_CHECK(Tests, !History.IsDirty() && !History.CanUndo() && !History.CanRedo());

		std::string Current = "scene-0";
		PF_CHECK(Tests, History.Begin("Create entity", Current));
		Current = "scene-1";
		const auto FirstCommit = History.Commit(Current);
		PF_CHECK(Tests, FirstCommit && *FirstCommit && History.IsDirty() && History.CanUndo());
		PF_CHECK(Tests, History.GetUndoCommand() && History.GetUndoCommand()->Description == "Create entity");

		auto Restore = [&Current](std::string_view State) -> std::expected<void, std::string>
		{
			Current.assign(State);
			return {};
		};
		const auto Undone = History.Undo(Restore);
		PF_CHECK(Tests, Undone && *Undone && Current == "scene-0" && !History.IsDirty());
		const auto Redone = History.Redo(Restore);
		PF_CHECK(Tests, Redone && *Redone && Current == "scene-1" && History.IsDirty());

		History.MarkSaved();
		PF_CHECK(Tests, !History.IsDirty());
		PF_CHECK(Tests, History.Undo(Restore) && History.IsDirty());
		PF_CHECK(Tests, History.Redo(Restore) && !History.IsDirty());

		PF_CHECK(Tests, History.Undo(Restore));
		PF_CHECK(Tests, History.Begin("Rename entity", Current));
		Current = "renamed";
		PF_CHECK(Tests, History.Commit(Current));
		PF_CHECK(Tests, !History.CanRedo() && Current == "renamed");

		const size_t BeforeNoOp = History.GetCommandCount();
		PF_CHECK(Tests, History.Begin("No-op", Current));
		const auto NoOp = History.Commit(Current);
		PF_CHECK(Tests, NoOp && !*NoOp && History.GetCommandCount() == BeforeNoOp);

		PF_CHECK(Tests, History.Begin("Third command", Current));
		Current = "third";
		PF_CHECK(Tests, History.Commit(Current));
		PF_CHECK(Tests, History.GetCommandCount() == 2 && History.GetStoredBytes() <= History.GetMaximumBytes());
		PF_CHECK(Tests, History.Undo(Restore) && Current == "renamed");
		PF_CHECK(Tests, History.Undo(Restore));
		PF_CHECK(Tests, !History.CanUndo());

		EditorCommandHistory ByteLimited(4, 8);
		ByteLimited.Reset();
		PF_CHECK(Tests, ByteLimited.Begin("Large command", "1234"));
		const auto Rejected = ByteLimited.Commit("12345678");
		PF_CHECK(Tests, !Rejected && ByteLimited.IsActive() && !ByteLimited.CanUndo());
		Current = "12345678";
		const auto Cancelled = ByteLimited.Cancel([&Current](std::string_view State) -> std::expected<void, std::string>
		{
			Current.assign(State);
			return {};
		});
		PF_CHECK(Tests, Cancelled && Current == "1234" && !ByteLimited.IsActive());

		PF_CHECK(Tests, ByteLimited.Begin("Failed restore", "a"));
		Current = "b";
		PF_CHECK(Tests, ByteLimited.Commit(Current));
		const auto FailedUndo = ByteLimited.Undo([](std::string_view) -> std::expected<void, std::string>
		{
			return std::unexpected("restore rejected");
		});
		PF_CHECK(Tests, !FailedUndo && ByteLimited.CanUndo() && Current == "b");
	}

	void TestEditorInteractionPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const EditorShortcutContext OrdinarySceneFocus{ .SceneAvailable = true };
		PF_CHECK(Tests, CanRouteEditorShortcut(OrdinarySceneFocus));
		PF_CHECK(Tests, CanRouteEditorShortcut({ .SceneAvailable = true, .GizmoInteractionActive = false }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .GizmoInteractionActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .TextInputActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .ImGuiItemActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .PopupOpen = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .MenuNavigationActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .WindowNavigationActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .RuntimeActive = true }));
		PF_CHECK(Tests, !CanRouteEditorShortcut({ .SceneAvailable = true, .CameraNavigationActive = true }));

		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, false }, false) ==
			ViewportGizmoPointerOutcome::Continue);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) ==
			ViewportGizmoPointerOutcome::Complete);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, false }, false) ==
			ViewportGizmoPointerOutcome::Complete);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, true }, true) ==
			ViewportGizmoPointerOutcome::Cancel);
		PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ false, false, true }, false) ==
			ViewportGizmoPointerOutcome::Idle);
		PF_CHECK(Tests, ShouldFinishSceneEditForInspector(true, 71, 0));
		PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(true, 71, 71));
		PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(true, NoInspectorHistoryItem, 0));
		PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(false, 71, 0));
	}

	void TestAltGizmoDragHistoryIntegration(TestRunner& Tests)
	{
		using namespace PulseForge;
		using namespace PulseForgeEditor;

		const auto Capture = [](const Scene& SceneValue, const EditorSelection& Selection)
			-> std::expected<std::string, std::string>
		{
			auto SerializedScene = SceneSerializer::Serialize(SceneValue);
			if (!SerializedScene)
				return std::unexpected(SerializedScene.error().Message);
			try
			{
				nlohmann::json State;
				State["scene"] = nlohmann::json::parse(*SerializedScene);
				State["selection"] = nlohmann::json::array();
				for (const UUID& Identifier : Selection.GetEntities())
					State["selection"].push_back(Identifier.ToString());
				State["primary"] = Selection.GetPrimary()
					? Selection.GetPrimary()->ToString()
					: std::string{};
				return State.dump();
			}
			catch (const std::exception& Error)
			{
				return std::unexpected(Error.what());
			}
		};
		const auto Restore = [](std::string_view StateData, Scene& SceneValue, EditorSelection& Selection)
			-> std::expected<void, std::string>
		{
			try
			{
				const nlohmann::json State = nlohmann::json::parse(StateData);
				if (auto Result = SceneSerializer::Deserialize(State.at("scene").dump(), SceneValue); !Result)
					return std::unexpected(Result.error().Message);

				std::vector<UUID> Identifiers;
				for (const auto& SerializedIdentifier : State.at("selection"))
				{
					auto Identifier = UUID::Parse(SerializedIdentifier.get<std::string>());
					if (!Identifier)
						return std::unexpected(Identifier.error().Message);
					if (SceneValue.FindEntity(*Identifier))
						Identifiers.push_back(*Identifier);
				}
				std::optional<UUID> Primary;
				const std::string SerializedPrimary = State.at("primary").get<std::string>();
				if (!SerializedPrimary.empty())
				{
					auto Identifier = UUID::Parse(SerializedPrimary);
					if (!Identifier)
						return std::unexpected(Identifier.error().Message);
					if (SceneValue.FindEntity(*Identifier))
						Primary = *Identifier;
				}
				Selection.Set(std::move(Identifiers), Primary);
				return {};
			}
			catch (const std::exception& Error)
			{
				return std::unexpected(Error.what());
			}
		};

		const auto RunSingleGesture = [&](TransformGizmoOperation Operation, bool Duplicate, std::string Description)
		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original)
				return;
			TransformComponent Initial;
			Initial.Translation = { 1.0f, 2.0f, 3.0f };
			Initial.Scale = { 1.5f, 0.8f, 1.2f };
			PF_CHECK(Tests, Original->SetTransform(Initial));
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before || !History.Begin(Description, *Before))
				return;

			// The viewport is drawn before the Inspector. The production Inspector policy must leave
			// a gizmo-owned transaction active while the gesture is still changing scene state.
			const uint32_t GizmoInspectorItem = NoInspectorHistoryItem;
			const bool PrematurelyFinished = ShouldFinishSceneEditForInspector(
				History.IsActive(), GizmoInspectorItem, 0);
			PF_CHECK(Tests, !PrematurelyFinished && History.IsActive() && !History.CanUndo());
			if (PrematurelyFinished)
			{
				auto InspectorBoundary = Capture(SceneValue, Selection);
				if (InspectorBoundary)
					(void)History.Commit(std::move(*InspectorBoundary));
			}

			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, false }, false) ==
				ViewportGizmoPointerOutcome::Continue);
			std::optional<UUID> DuplicateID;
			std::optional<Entity> Target;
			if (Duplicate)
			{
				auto Duplicated = SceneValue.DuplicateEntity(*Original);
				PF_CHECK(Tests, Duplicated.has_value());
				if (!Duplicated)
				{
					(void)History.Cancel([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
					return;
				}
				DuplicateID = Duplicated->GetUUID();
				Selection.SelectSingle(*DuplicateID);
				Target = *Duplicated;
			}
			else
				Target = *Original;

			TransformComponent Final = Initial;
			if (Operation == TransformGizmoOperation::Translate)
				Final.Translation += glm::vec3(2.5f, -0.25f, 1.0f);
			else if (Operation == TransformGizmoOperation::Rotate)
				Final.Rotation = glm::normalize(glm::angleAxis(glm::radians(37.0f), glm::vec3(0.0f, 1.0f, 0.0f)) *
					Initial.Rotation);
			else
				Final.Scale *= glm::vec3(1.25f, 0.75f, 1.5f);
			PF_CHECK(Tests, Target->SetTransform(Final));
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(
				History.IsActive(), GizmoInspectorItem, 0));
			PF_CHECK(Tests, History.IsActive() && !History.CanUndo());

			const auto Release = GetViewportGizmoPointerOutcome({ true, false, true }, false);
			PF_CHECK(Tests, Release == ViewportGizmoPointerOutcome::Complete);
			if (Release != ViewportGizmoPointerOutcome::Complete)
				return;
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After)
				return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed);
			PF_CHECK(Tests, !History.IsActive() && History.GetCommandCount() == 1 && History.GetCursor() == 1);
			PF_CHECK(Tests, History.CanUndo() && History.GetUndoCommand() &&
				History.GetUndoCommand()->Description == Description);
			PF_CHECK(Tests, CanRouteEditorShortcut({ .SceneAvailable = true }) && History.CanUndo());

			const auto Undone = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undone && *Undone);
			PF_CHECK(Tests, SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID);
			const auto RestoredOriginal = SceneValue.FindEntity(OriginalID);
			PF_CHECK(Tests, RestoredOriginal.has_value());
			if (!RestoredOriginal)
				return;
			const auto RestoredOriginalTransform = RestoredOriginal->GetTransform();
			PF_CHECK(Tests, RestoredOriginalTransform &&
				glm::length(RestoredOriginalTransform->Translation - Initial.Translation) < 1.0e-5f &&
				glm::length(RestoredOriginalTransform->Scale - Initial.Scale) < 1.0e-5f &&
				1.0f - std::abs(glm::dot(RestoredOriginalTransform->Rotation, Initial.Rotation)) < 1.0e-5f);

			const auto Redone = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redone && *Redone);
			if (Duplicate)
			{
				PF_CHECK(Tests, DuplicateID.has_value() && SceneValue.GetEntityCount() == 2);
				if (!DuplicateID)
					return;
				const auto RedoneDuplicate = SceneValue.FindEntity(*DuplicateID);
				PF_CHECK(Tests, RedoneDuplicate.has_value() && Selection.GetPrimary() == *DuplicateID);
				if (!RedoneDuplicate)
					return;
				const auto RedoneTransform = RedoneDuplicate->GetTransform();
				PF_CHECK(Tests, RedoneTransform &&
					glm::length(RedoneTransform->Translation - Final.Translation) < 1.0e-5f &&
					glm::length(RedoneTransform->Scale - Final.Scale) < 1.0e-5f &&
					1.0f - std::abs(glm::dot(RedoneTransform->Rotation, Final.Rotation)) < 1.0e-5f);
			}
			else
			{
				const auto RedoneOriginal = SceneValue.FindEntity(OriginalID);
				PF_CHECK(Tests, SceneValue.GetEntityCount() == 1 && RedoneOriginal.has_value());
				if (!RedoneOriginal)
					return;
				const auto RedoneTransform = RedoneOriginal->GetTransform();
				PF_CHECK(Tests, RedoneTransform &&
					glm::length(RedoneTransform->Translation - Final.Translation) < 1.0e-5f &&
					glm::length(RedoneTransform->Scale - Final.Scale) < 1.0e-5f &&
					1.0f - std::abs(glm::dot(RedoneTransform->Rotation, Final.Rotation)) < 1.0e-5f);
			}
		};

		RunSingleGesture(TransformGizmoOperation::Translate, false, "Move Selection");
		RunSingleGesture(TransformGizmoOperation::Translate, true, "Move Selection");
		RunSingleGesture(TransformGizmoOperation::Rotate, false, "Rotate Selection");
		RunSingleGesture(TransformGizmoOperation::Rotate, true, "Rotate Selection");
		RunSingleGesture(TransformGizmoOperation::Scale, false, "Scale Selection");

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original)
				return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto BeforeDuplicate = Capture(SceneValue, Selection);
			PF_CHECK(Tests, BeforeDuplicate && History.Begin("Duplicate Entities", *BeforeDuplicate));
			if (!BeforeDuplicate || !History.IsActive())
				return;
			auto Duplicate = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, Duplicate.has_value());
			if (!Duplicate)
				return;
			const UUID DuplicateID = Duplicate->GetUUID();
			Selection.SelectSingle(DuplicateID);
			auto AfterDuplicate = Capture(SceneValue, Selection);
			PF_CHECK(Tests, AfterDuplicate && History.Commit(std::move(*AfterDuplicate)));
			if (!History.CanUndo())
				return;

			auto DuplicateTransform = Duplicate->GetTransform();
			PF_CHECK(Tests, DuplicateTransform.has_value());
			if (!DuplicateTransform)
				return;
			const TransformComponent BeforeMoveTransform = *DuplicateTransform;
			auto BeforeMove = Capture(SceneValue, Selection);
			PF_CHECK(Tests, BeforeMove && History.Begin("Move Selection", *BeforeMove));
			if (!BeforeMove || !History.IsActive())
				return;
			const bool InspectorPrematurelyFinishesMove = ShouldFinishSceneEditForInspector(
				History.IsActive(), NoInspectorHistoryItem, 0);
			PF_CHECK(Tests, !InspectorPrematurelyFinishesMove && History.IsActive() && !History.CanUndo());
			if (InspectorPrematurelyFinishesMove)
			{
				auto InspectorBoundary = Capture(SceneValue, Selection);
				if (InspectorBoundary)
					(void)History.Commit(std::move(*InspectorBoundary));
			}
			TransformComponent AfterMoveTransform = BeforeMoveTransform;
			AfterMoveTransform.Translation += glm::vec3(0.5f, 1.0f, -0.25f);
			PF_CHECK(Tests, Duplicate->SetTransform(AfterMoveTransform));
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) ==
				ViewportGizmoPointerOutcome::Complete);
			auto AfterMove = Capture(SceneValue, Selection);
			PF_CHECK(Tests, AfterMove && History.Commit(std::move(*AfterMove)) && History.GetCommandCount() == 2 &&
				History.GetCursor() == 2 && History.CanUndo() &&
				History.GetUndoCommand()->Description == "Move Selection");

			const auto UndoMove = History.Undo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, UndoMove && *UndoMove && SceneValue.FindEntity(DuplicateID) &&
				Selection.GetPrimary() == DuplicateID);
			auto MovedBackDuplicate = SceneValue.FindEntity(DuplicateID);
			PF_CHECK(Tests, MovedBackDuplicate && MovedBackDuplicate->GetTransform() &&
				glm::length(MovedBackDuplicate->GetTransform()->Translation - BeforeMoveTransform.Translation) < 1.0e-5f);
			const auto UndoDuplicate = History.Undo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, UndoDuplicate && *UndoDuplicate && !SceneValue.FindEntity(DuplicateID) &&
				Selection.GetPrimary() == OriginalID);
			const auto RedoDuplicate = History.Redo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, RedoDuplicate && *RedoDuplicate && SceneValue.FindEntity(DuplicateID));
			const auto RedoMove = History.Redo([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, RedoMove && *RedoMove && Selection.GetPrimary() == DuplicateID);
			auto MovedDuplicate = SceneValue.FindEntity(DuplicateID);
			PF_CHECK(Tests, MovedDuplicate && MovedDuplicate->GetTransform() &&
				glm::length(MovedDuplicate->GetTransform()->Translation - AfterMoveTransform.Translation) < 1.0e-5f);
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original)
				return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before)
				return;
			const auto Begun = History.Begin("Duplicate Entities", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun)
				return;
			auto Duplicate = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, Duplicate.has_value());
			if (!Duplicate)
				return;
			const UUID DuplicateID = Duplicate->GetUUID();
			Selection.SelectSingle(DuplicateID);
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After)
				return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed && History.CanUndo() &&
				History.GetUndoCommand()->Description == "Duplicate Entities");
			const auto Undo = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undo && *Undo && SceneValue.GetEntityCount() == 1 &&
				Selection.GetPrimary() == OriginalID && !SceneValue.FindEntity(DuplicateID));
			const auto Redo = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redo && *Redo && SceneValue.GetEntityCount() == 2 &&
				Selection.GetPrimary() == DuplicateID && SceneValue.FindEntity(DuplicateID));
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto OriginalParent = SceneValue.CreateEntity("Parent");
			auto OriginalChild = SceneValue.CreateEntity("Child");
			auto SecondRoot = SceneValue.CreateEntity("Second root");
			PF_CHECK(Tests, OriginalParent && OriginalChild && SecondRoot);
			if (!OriginalParent || !OriginalChild || !SecondRoot)
				return;
			PF_CHECK(Tests, OriginalChild->SetParent(*OriginalParent));
			PointLightComponent ChildLight;
			ChildLight.Intensity = 4.0f;
			PF_CHECK(Tests, OriginalChild->SetPointLight(ChildLight));
			const UUID ParentID = OriginalParent->GetUUID();
			const UUID ChildID = OriginalChild->GetUUID();
			const UUID SecondID = SecondRoot->GetUUID();
			Selection.Set({ ParentID, SecondID }, ParentID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before)
				return;
			const auto Begun = History.Begin("Move Selection", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun)
				return;
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(
				History.IsActive(), NoInspectorHistoryItem, 0) && History.IsActive());

			auto DuplicateParent = SceneValue.DuplicateEntity(*OriginalParent);
			auto DuplicateChild = SceneValue.DuplicateEntity(*OriginalChild);
			auto DuplicateSecond = SceneValue.DuplicateEntity(*SecondRoot);
			PF_CHECK(Tests, DuplicateParent && DuplicateChild && DuplicateSecond);
			if (!DuplicateParent || !DuplicateChild || !DuplicateSecond)
				return;
			PF_CHECK(Tests, DuplicateChild->SetParent(*DuplicateParent));
			const UUID DuplicateParentID = DuplicateParent->GetUUID();
			const UUID DuplicateChildID = DuplicateChild->GetUUID();
			const UUID DuplicateSecondID = DuplicateSecond->GetUUID();
			for (Entity* Root : std::array<Entity*, 2>{ &*DuplicateParent, &*DuplicateSecond })
			{
				const auto Transform = Root->GetTransform();
				PF_CHECK(Tests, Transform.has_value());
				if (Transform)
				{
					auto Moved = *Transform;
					Moved.Translation.x += 2.0f;
					PF_CHECK(Tests, Root->SetTransform(Moved));
				}
			}
			Selection.Set({ DuplicateParentID, DuplicateSecondID }, DuplicateParentID);
			PF_CHECK(Tests, !ShouldFinishSceneEditForInspector(
				History.IsActive(), NoInspectorHistoryItem, 0) && History.IsActive() && !History.CanUndo());
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, false, true }, false) ==
				ViewportGizmoPointerOutcome::Complete);
			auto After = Capture(SceneValue, Selection);
			PF_CHECK(Tests, After.has_value());
			if (!After)
				return;
			const auto Committed = History.Commit(std::move(*After));
			PF_CHECK(Tests, Committed && *Committed);
			PF_CHECK(Tests, History.GetCommandCount() == 1 && History.CanUndo() && SceneValue.GetEntityCount() == 6);
			const auto Undo = History.Undo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Undo && *Undo && SceneValue.GetEntityCount() == 3 &&
				Selection.GetPrimary() == ParentID && Selection.Contains(SecondID));
			const auto Redo = History.Redo([&](std::string_view State) { return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Redo && *Redo && SceneValue.GetEntityCount() == 6 &&
				Selection.GetPrimary() == DuplicateParentID && Selection.Contains(DuplicateSecondID));
			const auto RestoredDuplicateChild = SceneValue.FindEntity(DuplicateChildID);
			PF_CHECK(Tests, RestoredDuplicateChild.has_value());
			if (RestoredDuplicateChild)
			{
				const auto Parent = RestoredDuplicateChild->GetParent();
				const auto Light = RestoredDuplicateChild->GetPointLight();
				PF_CHECK(Tests, Parent && Parent->has_value() && (**Parent).GetUUID() == DuplicateParentID);
				PF_CHECK(Tests, Light && Light->has_value() && Light->value().Intensity == 4.0f);
			}
			PF_CHECK(Tests, SceneValue.FindEntity(ParentID) && SceneValue.FindEntity(ChildID) &&
				SceneValue.FindEntity(SecondID) && SceneValue.FindEntity(DuplicateSecondID));
		}

		{
			Scene SceneValue;
			EditorSelection Selection;
			EditorCommandHistory History;
			History.Reset();
			auto Original = SceneValue.CreateEntity("Original");
			PF_CHECK(Tests, Original.has_value());
			if (!Original)
				return;
			const UUID OriginalID = Original->GetUUID();
			Selection.SelectSingle(OriginalID);
			auto Before = Capture(SceneValue, Selection);
			PF_CHECK(Tests, Before.has_value());
			if (!Before)
				return;
			const auto Begun = History.Begin("Move Selection", *Before);
			PF_CHECK(Tests, Begun.has_value());
			if (!Begun)
				return;
			auto AbandonedCopy = SceneValue.DuplicateEntity(*Original);
			PF_CHECK(Tests, AbandonedCopy.has_value());
			if (!AbandonedCopy)
				return;
			const UUID AbandonedID = AbandonedCopy->GetUUID();
			Selection.SelectSingle(AbandonedID);
			PF_CHECK(Tests, GetViewportGizmoPointerOutcome({ true, true, true }, true) ==
				ViewportGizmoPointerOutcome::Cancel);
			const auto Cancelled = History.Cancel([&](std::string_view State)
				{ return Restore(State, SceneValue, Selection); });
			PF_CHECK(Tests, Cancelled && !History.IsActive() && History.GetCommandCount() == 0 &&
				!History.CanUndo() && SceneValue.GetEntityCount() == 1 && Selection.GetPrimary() == OriginalID &&
				!SceneValue.FindEntity(AbandonedID));
		}
	}

	void TestEditorSelection(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorSelection;
		using PulseForge::UUID;
		const UUID First{ 0, 201 };
		const UUID Second{ 0, 202 };
		const UUID Third{ 0, 203 };
		const UUID Fourth{ 0, 204 };
		const std::array<UUID, 4> VisibleOrder{ First, Second, Third, Fourth };

		EditorSelection Selection;
		Selection.SelectSingle(Second);
		PF_CHECK(Tests, Selection.GetEntities().size() == 1 && Selection.GetPrimary() == Second);
		Selection.Toggle(Third);
		PF_CHECK(Tests, Selection.GetEntities().size() == 2 && Selection.GetPrimary() == Third);
		PF_CHECK(Tests, Selection.SelectRange(VisibleOrder, Fourth));
		PF_CHECK(Tests, Selection.GetEntities().size() == 3 && Selection.Contains(Second) &&
			Selection.Contains(Third) && Selection.Contains(Fourth) && Selection.GetPrimary() == Fourth);
		PF_CHECK(Tests, Selection.SelectRange(VisibleOrder, First, true));
		PF_CHECK(Tests, Selection.GetEntities().size() == 4 && Selection.GetPrimary() == First);
		Selection.Remove(First);
		PF_CHECK(Tests, Selection.GetEntities().size() == 3 && Selection.GetPrimary() == Fourth);
		Selection.Toggle(Fourth);
		PF_CHECK(Tests, !Selection.Contains(Fourth) && Selection.GetPrimary() == Third);
		Selection.Set({ First, First, Third }, First);
		PF_CHECK(Tests, Selection.GetEntities().size() == 2 && Selection.GetPrimary() == First);
		Selection.Clear();
		PF_CHECK(Tests, Selection.GetEntities().empty() && !Selection.GetPrimary() && !Selection.GetAnchor());
	}

	void TestEditorVisibility(TestRunner& Tests)
	{
		using PulseForgeEditor::EditorVisibility;
		using PulseForge::UUID;
		const UUID Parent{ 0, 301 };
		const UUID Child{ 0, 302 };
		const UUID Sibling{ 0, 303 };
		const std::unordered_map<UUID, UUID, PulseForge::UUIDHash> Parents{ { Child, Parent } };
		const auto ParentLookup = [&Parents](const UUID& Identifier) -> std::optional<UUID>
		{
			const auto Found = Parents.find(Identifier);
			return Found == Parents.end() ? std::nullopt : std::optional<UUID>{ Found->second };
		};

		EditorVisibility Visibility;
		Visibility.SetVisible(Child, false);
		Visibility.SetVisible(Parent, false);
		PF_CHECK(Tests, !Visibility.IsEffectivelyVisible(Child, ParentLookup));
		Visibility.SetVisible(Parent, true);
		PF_CHECK(Tests, !Visibility.IsEffectivelyVisible(Child, ParentLookup));
		Visibility.SetVisible(Child, true);
		PF_CHECK(Tests, Visibility.IsEffectivelyVisible(Child, ParentLookup));

		Visibility.SetVisible(Sibling, false);
		const std::array<UUID, 2> Keep{ Parent, Child };
		const std::array<UUID, 3> All{ Parent, Child, Sibling };
		PF_CHECK(Tests, Visibility.Isolate(Keep, All));
		PF_CHECK(Tests, Visibility.IsLocallyVisible(Parent) && Visibility.IsLocallyVisible(Child));
		PF_CHECK(Tests, !Visibility.IsLocallyVisible(Sibling));
		PF_CHECK(Tests, Visibility.RestoreIsolation());
		PF_CHECK(Tests, !Visibility.IsLocallyVisible(Sibling));
		Visibility.ShowAll();
		PF_CHECK(Tests, Visibility.IsLocallyVisible(Sibling) && !Visibility.GetIsolationBaseline());

		PF_CHECK(Tests, Visibility.IsEffectivelyVisible(Parent, [Parent, Child](const UUID& Identifier)
		{
			return Identifier == Parent ? std::optional<UUID>{ Child } : std::optional<UUID>{ Parent };
		}) == false);
	}

	void TestEditorHierarchyRowLayout(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		constexpr float FramePaddingX = 7.0f;
		constexpr float TextStartX = 50.0f;
		constexpr float TreeTrailingHitWidth = FramePaddingX;

		const auto WideRow = ComputeEditorHierarchyRowLayout(
			8.0f, 240.0f, TextStartX, 100.0f, 108.0f, FramePaddingX);
		PF_CHECK(Tests, WideRow.EyeStartX == 210.0f);
		PF_CHECK(Tests, WideRow.EyeStartX >= 100.0f + EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, WideRow.EyeStartX + EditorHierarchyEyeButtonWidth <= 240.0f);
		PF_CHECK(Tests, WideRow.MaximumNameWidth > 80.0f);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX == 108.0f && WideRow.SelectionGutterWidth > 0.0f);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX >= 100.0f);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX + WideRow.SelectionGutterWidth <=
			WideRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 8.0f + WideRow.SelectionBackgroundWidth ==
			WideRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, WideRow.SelectionGutterStartX + WideRow.SelectionGutterWidth ==
			8.0f + WideRow.SelectionBackgroundWidth);
		const float LongNameTreeItemRightX = TextStartX + WideRow.MaximumNameWidth + TreeTrailingHitWidth;
		PF_CHECK(Tests, LongNameTreeItemRightX <= WideRow.EyeStartX - EditorHierarchyHitRegionGap -
			EditorHierarchyHitRegionSafetyInset);

		const auto NarrowRow = ComputeEditorHierarchyRowLayout(
			8.0f, 150.0f, 50.0f, 94.0f, 102.0f, FramePaddingX);
		PF_CHECK(Tests, NarrowRow.EyeStartX >= 94.0f + EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, NarrowRow.EyeStartX + EditorHierarchyEyeButtonWidth <= 150.0f);
		PF_CHECK(Tests, NarrowRow.MaximumNameWidth >= 0.0f && NarrowRow.SelectionGutterWidth >= 0.0f);
		PF_CHECK(Tests, NarrowRow.SelectionGutterStartX >= 94.0f);
		PF_CHECK(Tests, NarrowRow.SelectionGutterStartX + NarrowRow.SelectionGutterWidth <=
			NarrowRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 8.0f + NarrowRow.SelectionBackgroundWidth ==
			NarrowRow.EyeStartX - EditorHierarchyHitRegionGap);

		const auto DeepRow = ComputeEditorHierarchyRowLayout(
			98.0f, 240.0f, 140.0f, 175.0f, 183.0f, FramePaddingX);
		PF_CHECK(Tests, DeepRow.EyeStartX == 210.0f);
		PF_CHECK(Tests, DeepRow.SelectionGutterStartX >= 175.0f);
		PF_CHECK(Tests, DeepRow.SelectionGutterStartX + DeepRow.SelectionGutterWidth <=
			DeepRow.EyeStartX - EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 98.0f + DeepRow.SelectionBackgroundWidth ==
			DeepRow.EyeStartX - EditorHierarchyHitRegionGap);

		const auto ConstrainedDeepRow = ComputeEditorHierarchyRowLayout(
			170.0f, 260.0f, 215.0f, 230.0f, 238.0f, FramePaddingX);
		PF_CHECK(Tests, ConstrainedDeepRow.EyeStartX >= 234.0f);
		PF_CHECK(Tests, ConstrainedDeepRow.EyeStartX + EditorHierarchyEyeButtonWidth <= 260.0f);
		PF_CHECK(Tests, ConstrainedDeepRow.SelectionGutterWidth == 0.0f);
		PF_CHECK(Tests, ConstrainedDeepRow.EyeStartX >= 230.0f + EditorHierarchyHitRegionGap);
		PF_CHECK(Tests, 170.0f + ConstrainedDeepRow.SelectionBackgroundWidth ==
			ConstrainedDeepRow.EyeStartX - EditorHierarchyHitRegionGap);
	}

	void TestSceneCommandHistoryRestoresIdentity(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene SceneValue;
		const auto Root = SceneValue.CreateEntity("Parent");
		PF_CHECK(Tests, Root.has_value());
		if (!Root)
			return;

		PulseForgeEditor::EditorCommandHistory History;
		History.Reset();
		const auto Before = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, Before.has_value());
		if (!Before)
			return;
		PF_CHECK(Tests, History.Begin("Create child", *Before));

		auto Child = SceneValue.CreateEntity("Shadow Caster");
		PF_CHECK(Tests, Child.has_value());
		if (!Child)
			return;
		const UUID ChildIdentifier = Child->GetUUID();
		TransformComponent Transform;
		Transform.Translation = { 1.0f, 2.0f, 3.0f };
		PF_CHECK(Tests, Child->SetTransform(Transform));
		PointLightComponent Light;
		Light.CastShadows = true;
		PF_CHECK(Tests, Child->SetPointLight(Light));
		PF_CHECK(Tests, Child->SetParent(*Root));
		const auto After = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, After.has_value());
		if (!After)
			return;
		PF_CHECK(Tests, History.Commit(*After));

		auto Restore = [&SceneValue](std::string_view State) -> std::expected<void, std::string>
		{
			if (auto Result = SceneSerializer::Deserialize(State, SceneValue); !Result)
				return std::unexpected(Result.error().Message);
			return {};
		};
		const auto UndoCreate = History.Undo(Restore);
		PF_CHECK(Tests, UndoCreate && *UndoCreate && !SceneValue.FindEntity(ChildIdentifier));
		PF_CHECK(Tests, SceneValue.FindEntity(Root->GetUUID()).has_value());
		const auto RedoCreate = History.Redo(Restore);
		PF_CHECK(Tests, RedoCreate && *RedoCreate);
		const auto RestoredChild = SceneValue.FindEntity(ChildIdentifier);
		PF_CHECK(Tests, RestoredChild.has_value());
		if (RestoredChild)
		{
			const auto RestoredTransform = RestoredChild->GetTransform();
			const auto RestoredLight = RestoredChild->GetPointLight();
			const auto RestoredParent = RestoredChild->GetParent();
			PF_CHECK(Tests, RestoredTransform && RestoredTransform->Translation == glm::vec3(1.0f, 2.0f, 3.0f));
			PF_CHECK(Tests, RestoredLight && RestoredLight->has_value() && RestoredLight->value().CastShadows);
			PF_CHECK(Tests, RestoredParent && RestoredParent->has_value() && (**RestoredParent).GetUUID() == Root->GetUUID());
		}

		History.MarkSaved();
		const auto DeleteBefore = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, DeleteBefore && History.Begin("Delete child", *DeleteBefore));
		const auto CurrentChild = SceneValue.FindEntity(ChildIdentifier);
		PF_CHECK(Tests, CurrentChild && SceneValue.DestroyEntity(*CurrentChild));
		const auto DeleteAfter = SceneSerializer::Serialize(SceneValue);
		PF_CHECK(Tests, DeleteAfter && History.Commit(*DeleteAfter));
		PF_CHECK(Tests, History.IsDirty() && !SceneValue.FindEntity(ChildIdentifier));
		const auto UndoDelete = History.Undo(Restore);
		PF_CHECK(Tests, UndoDelete && *UndoDelete && SceneValue.FindEntity(ChildIdentifier));
		PF_CHECK(Tests, !History.IsDirty());
	}

	void TestEditorAssetPreviewPolicy(TestRunner& Tests)
	{
		using namespace PulseForgeEditor;
		const AssetPreviewDimensions Wide = CalculateAssetPreviewDimensions(4096, 2048, 192);
		PF_CHECK(Tests, Wide.Width == 192 && Wide.Height == 96);
		const AssetPreviewDimensions Tall = CalculateAssetPreviewDimensions(64, 4096, 192);
		PF_CHECK(Tests, Tall.Width == 3 && Tall.Height == 192);
		const AssetPreviewDimensions Tiny = CalculateAssetPreviewDimensions(3, 2, 192);
		PF_CHECK(Tests, Tiny.Width == 3 && Tiny.Height == 2);
		PF_CHECK(Tests, CalculateAssetPreviewDimensions(0, 4, 192).Width == 0);
		PF_CHECK(Tests, CalculateAssetPreviewDimensions(4, 4, 0).Height == 0);

		const AssetPreviewRect Fitted = FitAssetPreview(10.0f, 20.0f, 100.0f, 60.0f, 200, 100);
		PF_CHECK(Tests, std::isfinite(Fitted.X) && std::isfinite(Fitted.Y) &&
			std::isfinite(Fitted.Width) && std::isfinite(Fitted.Height));
		PF_CHECK(Tests, Fitted.Width == 100.0f && Fitted.Height == 50.0f && Fitted.Y == 25.0f);
		const AssetPreviewRect Invalid = FitAssetPreview(0.0f, 0.0f, -1.0f, 10.0f, 1, 1);
		PF_CHECK(Tests, Invalid.Width == 0.0f && Invalid.Height == 0.0f);

		AssetPreviewKey First{ "project-a", "asset", { 1, 2 }, AssetPreviewKind::Image, 192, 96, 1 };
		AssetPreviewKey Same = First;
		AssetPreviewKey OtherProject = First;
		OtherProject.ProjectID = "project-b";
		AssetPreviewKey OtherRevision = First;
		OtherRevision.ContentRevision[1]++;
		PF_CHECK(Tests, First == Same && AssetPreviewKeyHash{}(First) == AssetPreviewKeyHash{}(Same));
		PF_CHECK(Tests, !(First == OtherProject) && !(First == OtherRevision));
		const auto OpaqueSwatch = MakeMaterialBaseColorSwatch(0.5f, 1.0f, 0.0f, 0.2f, true);
		PF_CHECK(Tests, OpaqueSwatch[0] == 188 && OpaqueSwatch[1] == 255 && OpaqueSwatch[2] == 0 && OpaqueSwatch[3] == 255);
		const auto BlendSwatch = MakeMaterialBaseColorSwatch(-1.0f, 0.0f, 0.0f, 0.25f, false);
		PF_CHECK(Tests, BlendSwatch[0] == 0 && BlendSwatch[3] == 64);

		AssetPreviewBudget Budget(100);
		PF_CHECK(Tests, Budget.Add(60) && Budget.GetUsed() == 60);
		PF_CHECK(Tests, !Budget.Add(41) && Budget.GetUsed() == 60);
		Budget.Remove(20);
		PF_CHECK(Tests, Budget.CanFit(60) && !Budget.CanFit(81));
		Budget.SetLimit(0);
		Budget.Clear();
		PF_CHECK(Tests, !Budget.Add(1) && Budget.Add(0));
		PF_CHECK(Tests, IsSupportedImagePreviewExtension("sample.PNG"));
		PF_CHECK(Tests, IsSupportedImagePreviewExtension("sample.ppm"));
		PF_CHECK(Tests, !IsSupportedImagePreviewExtension("sample.hdr"));
		PF_CHECK(Tests, !IsSupportedImagePreviewExtension("sample.material"));

		bool Visible = false;
		uint64_t LastUse = 0;
		uint64_t UseCounter = 0;
		TouchAssetPreviewEntry(Visible, LastUse, UseCounter);
		PF_CHECK(Tests, Visible && LastUse == 1);
		BeginAssetPreviewFrame(Visible);
		PF_CHECK(Tests, !Visible);
		TouchAssetPreviewEntry(Visible, LastUse, UseCounter);
		PF_CHECK(Tests, Visible && LastUse == 2);
		std::array<bool, 4> StateVisibility{}; // Queued, Decoding, Ready, and Failed entries use the same touch path.
		for (bool& StateVisible : StateVisibility)
		{
			BeginAssetPreviewFrame(StateVisible);
			TouchAssetPreviewEntry(StateVisible, LastUse, UseCounter);
		}
		PF_CHECK(Tests, std::ranges::all_of(StateVisibility, [](bool StateVisible) { return StateVisible; }));

		const std::array<AssetPreviewEvictionCandidate, 3> VisibilityCandidates = {{
			{ "ready", true, false, LastUse, 8 },
			{ "queued", true, true, 1, 0 },
			{ "unused", false, false, 0, 8 }
		}};
		const auto ProtectedVisible = PlanAssetPreviewEvictions(
			VisibilityCandidates, 3, 2, 16, 16, 0, 0);
		PF_CHECK(Tests, ProtectedVisible && ProtectedVisible->size() == 1 && (*ProtectedVisible)[0] == "unused");
		const std::array<AssetPreviewEvictionCandidate, 2> FullCacheCandidates = {{
			{ "old-cache-entry", false, false, 1, 0 },
			{ "visible-cache-entry", true, false, 2, 0 }
		}};
		const auto FullEntryCache = PlanAssetPreviewEvictions(
			FullCacheCandidates, 2, 2, 0, 0, 0, 1);
		PF_CHECK(Tests, FullEntryCache && FullEntryCache->size() == 1 && (*FullEntryCache)[0] == "old-cache-entry");

		const std::array<AssetPreviewEvictionCandidate, 3> UploadCandidates = {{
			{ "uploading", false, false, 1, 4 },
			{ "recent-unused", false, false, 9, 8 },
			{ "old-unused", false, false, 2, 8 }
		}};
		const auto UploadEviction = PlanAssetPreviewEvictions(
			UploadCandidates, 3, 3, 20, 20, 12, 0, "uploading");
		PF_CHECK(Tests, UploadEviction && UploadEviction->size() == 2 &&
			(*UploadEviction)[0] == "old-unused" && (*UploadEviction)[1] == "recent-unused");
		const auto NoUploadVictim = PlanAssetPreviewEvictions(
			UploadCandidates, 3, 3, 20, 20, 17, 0, "uploading");
		PF_CHECK(Tests, !NoUploadVictim);

		const std::array<AssetPreviewEvictionCandidate, 2> BusyCache = {{
			{ "visible", true, false, 4, 10 },
			{ "decoding", false, true, 1, 0 }
		}};
		PF_CHECK(Tests, !PlanAssetPreviewEvictions(BusyCache, 2, 2, 10, 10, 1, 0));
		const auto StaleQueuedResult = [](uint64_t Work, uint64_t Entry, uint64_t Current, bool Stopping)
		{
			return IsCurrentAssetPreviewResult(Work, Entry, Current, Stopping);
		};
		PF_CHECK(Tests, StaleQueuedResult(5, 5, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(4, 4, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(5, 4, 5, false));
		PF_CHECK(Tests, !StaleQueuedResult(5, 5, 5, true));

		const std::array<AssetPreviewEvictionCandidate, 1> AfterEviction = {{
			{ "remaining", true, false, 5, 0 }
		}};
		const auto ReRequest = PlanAssetPreviewEvictions(AfterEviction, 1, 2, 0, 0, 0, 1);
		PF_CHECK(Tests, ReRequest && ReRequest->empty());
	}
}

int main()
{
	PulseForge::Log::Init();
	TestRunner Tests;
	TestLayerStackOwnership(Tests);
	TestLayerStackClearDetachesInReverseOrder(Tests);
	TestLayerAndOverlayIterationOrder(Tests);
	TestEventDispatchAndHandledState(Tests);
	TestLayerAttachFailureRollsBackOwnership(Tests);
	TestLayerPopDetachesAndDestroysWhenDetachThrows(Tests);
	TestLayerClearSurvivesDetachFailure(Tests);
	TestInputState(Tests);
	TestTimestepConversions(Tests);
	TestBufferDescriptionValidation(Tests);
	TestShaderDescriptionAndSpirVValidation(Tests);
	TestVertexLayoutValidation(Tests);
	TestMeshDescriptionAndDrawValidation(Tests);
	TestUUIDBehavior(Tests);
	TestSceneEntityAndHierarchy(Tests);
	TestPhysicsComponentsAndPersistence(Tests);
	TestPhysicsSceneRuntime(Tests);
	TestEntityHandleIdentityAndLifetime(Tests);
	TestSceneRenderSnapshot(Tests);
	TestLocalLightingMathAndSelection(Tests);
	TestEditorViewportMath(Tests);
	TestEditorInteractionPolicy(Tests);
	TestEditorAssetPreviewPolicy(Tests);
	TestEditorWorkspacePanelState(Tests);
	TestEditorAssetDestinationPolicy(Tests);
	TestEditorAssetSelectorPolicy(Tests);
	TestEditorLayoutOpeningPreferencePersistence(Tests);
	TestEditorWorkspaceViewHelpers(Tests);
	TestViewportMouseCaptureState(Tests);
	TestEditorFontAndIconDescriptors(Tests);
	TestEditorImGuiRendererMath(Tests);
	TestEditorPlatformViewportSubmissionOrder(Tests);
	TestSceneSerializationRoundTrip(Tests);
	TestSceneSerializerClone(Tests);
	TestSceneAssetsByUUID(Tests);
	TestPrefabSerialization(Tests);
	TestPrefabAssetsByUUID(Tests);
	TestAssetMetadataAndRegistry(Tests);
	TestAssetOperations(Tests);
	TestAssetFolderOperations(Tests);
	TestEditorDocumentManager(Tests);
	TestEditorSnapshotHistory(Tests);
	TestEditorDocumentWorkspaceCodec(Tests);
	TestEditorNativeWindowBoundsPolicy(Tests);
	TestAssetImportOperation(Tests);
	TestMaterialAssets(Tests);
	TestProjectFiles(Tests);
	TestEditorCommandLineAndStartupProjectLoading(Tests);
	TestBundledValidationProjectAssetRegistry(Tests);
	TestAssetReferenceValidation(Tests);
	TestGltfMeshImport(Tests);
	TestImageAssetImport(Tests);
	TestScreenSpaceAmbientOcclusionMath(Tests);
	TestEnvironmentLightingProcessing(Tests);
	TestAudioEngineAndAssetCache(Tests);
	TestScriptRuntime(Tests);
	TestGraphicsPipelineAndDrawValidation(Tests);
	TestToneMappingMathAndOutputEncoding(Tests);
	TestConstantBufferBindingValidation(Tests);
	TestTextureAndSamplerValidation(Tests);
	TestTextureSamplerAndBufferBindingValidation(Tests);
	TestVulkanQueueFamilySelection(Tests);
	TestVulkanRequiredDeviceFeatures(Tests);
	TestVulkanFormatUsageCapabilities(Tests);
	TestVulkanSurfaceAndPresentationSelection(Tests);
	TestVulkanExtentAndImageCountSelection(Tests);
	TestConsoleFormatting(Tests);
	TestPointLightShadowMath(Tests);
	TestEditorCommandHistory(Tests);
	TestAltGizmoDragHistoryIntegration(Tests);
	TestEditorSelection(Tests);
	TestEditorVisibility(Tests);
	TestEditorHierarchyRowLayout(Tests);
	TestSceneCommandHistoryRestoresIdentity(Tests);
	return Tests.Finish();
}
