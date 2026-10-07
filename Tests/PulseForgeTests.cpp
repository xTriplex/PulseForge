#include <algorithm>
#include <iostream>
#include <limits>
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
#include "Renderer/Vulkan/VulkanSupport.h"
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
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

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
		const AssetID FirstMeshAssetID{ 0x5000000000000000ull, 5 };
		const AssetID SecondMeshAssetID{ 0x6000000000000000ull, 6 };
		const AssetID MaterialAssetID{ 0x6100000000000000ull, 61 };
		auto Camera = TestScene.CreateEntityWithUUID(CameraID, "Camera");
		auto Parent = TestScene.CreateEntityWithUUID(ParentID, "Parent");
		auto FirstMesh = TestScene.CreateEntityWithUUID(FirstMeshID, "First mesh");
		auto SharedMesh = TestScene.CreateEntityWithUUID(SharedMeshID, "Shared mesh instance");
		auto SecondMesh = TestScene.CreateEntityWithUUID(SecondMeshID, "Second mesh");
		PF_CHECK(Tests, Camera && Parent && FirstMesh && SharedMesh && SecondMesh);
		if (!Camera || !Parent || !FirstMesh || !SharedMesh || !SecondMesh)
			return;

		TransformComponent CameraTransform;
		CameraTransform.Translation = { 0.0f, 0.0f, 5.0f };
		CameraComponent PrimaryCameraComponent;
		PrimaryCameraComponent.IsPrimary = true;
		PF_CHECK(Tests, Camera->SetTransform(CameraTransform).has_value());
		PF_CHECK(Tests, Camera->SetCamera(PrimaryCameraComponent).has_value());

		TransformComponent ParentTransform;
		ParentTransform.Translation = { 2.0f, 1.0f, 0.0f };
		TransformComponent FirstMeshTransform;
		FirstMeshTransform.Translation = { -1.0f, 0.0f, -2.0f };
		TransformComponent SharedMeshTransform;
		SharedMeshTransform.Translation = { 3.0f, 0.0f, -1.0f };
		TransformComponent SecondMeshTransform;
		SecondMeshTransform.Translation = { 0.0f, -1.0f, -4.0f };
		PF_CHECK(Tests, Parent->SetTransform(ParentTransform).has_value());
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
		PF_CHECK(Tests, SourceProjection && (*SourceProjection)[1][1] < 0.0f);
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
		const auto Serialized = MaterialAssetSerializer::Serialize(Description);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		const auto Deserialized = MaterialAssetSerializer::Deserialize(*Serialized);
		PF_CHECK(Tests, Deserialized && Deserialized->BaseColorTexture == Description.BaseColorTexture);
		PF_CHECK(Tests, Deserialized && glm::all(glm::equal(Deserialized->BaseColorFactor, Description.BaseColorFactor)));

		MaterialAssetDesc MissingTexture = Description;
		MissingTexture.BaseColorTexture = {};
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(MissingTexture));
		MaterialAssetDesc InvalidFactor = Description;
		InvalidFactor.BaseColorFactor.g = std::numeric_limits<float>::quiet_NaN();
		PF_CHECK(Tests, !ValidateMaterialAssetDescription(InvalidFactor));
		InvalidFactor.BaseColorFactor = Description.BaseColorFactor;
		InvalidFactor.BaseColorFactor.r = 1.1f;
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
		PF_CHECK(Tests, MeshMetadata && MaterialMetadata && WrongTypeMetadata && ScriptMetadata && AudioMetadata);
		if (!MeshMetadata || !MaterialMetadata || !WrongTypeMetadata || !ScriptMetadata || !AudioMetadata)
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
		const auto Unrelated = TestScene.CreateEntity("Unrelated");
		PF_CHECK(Tests, ValidScriptOnly && MissingScriptOnly && WrongScriptOnly && ValidAudioOnly && MissingAudioOnly &&
			WrongAudioOnly && WrongMeshType && WrongMaterialType && ValidMeshAndMaterial && MultipleReferences && Unrelated);
		if (!ValidScriptOnly || !MissingScriptOnly || !WrongScriptOnly || !ValidAudioOnly || !MissingAudioOnly ||
			!WrongAudioOnly || !WrongMeshType || !WrongMaterialType || !ValidMeshAndMaterial || !MultipleReferences || !Unrelated)
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
		PF_CHECK(Tests, AllMissing && AllMissing->size() == 15);
		PF_CHECK(Tests, AllMissing && HasIssue(*AllMissing, *ValidScriptOnly, AssetReferenceKind::Script,
			AssetReferenceIssueCode::MissingAsset, ScriptMetadata->ID));
		PF_CHECK(Tests, AllMissing && HasIssue(*AllMissing, *ValidAudioOnly, AssetReferenceKind::Audio,
			AssetReferenceIssueCode::MissingAsset, AudioMetadata->ID));

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Validation = AssetReferenceValidator::Validate(TestScene, Registry);
		PF_CHECK(Tests, Validation && Validation->size() == 10);
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
		PF_CHECK(Tests, ValidateMeshDescription(Imported->GetMeshDescription()).has_value());
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

		const std::filesystem::path InvalidImagePath = TextureDirectory / "invalid.bmp";
		{
			std::ofstream Output(InvalidImagePath, std::ios::binary | std::ios::trunc);
			Output.write("not an image", 12);
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto InvalidImageMetadata = AssetMetadataSerializer::CreateForNewAsset(InvalidImagePath);
		PF_CHECK(Tests, InvalidImageMetadata.has_value());
		if (InvalidImageMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto InvalidImage = ImageAssetImporter::ImportRGBA8(InvalidImageMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !InvalidImage && InvalidImage.error().Code == ImageAssetImportErrorCode::InvalidImage);
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
		const auto FirstAudioListener = FirstInstance->GetAudioListener();
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() &&
			FirstCamera->value().VerticalFieldOfViewRadians == RootCamera.VerticalFieldOfViewRadians);
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() && !FirstCamera->value().IsPrimary);
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

		TestBindingSet CreatedTestBindingSet(Layout);
		const std::array<const BindingSet*, 1> ValidSets = { &CreatedTestBindingSet };
		PF_CHECK(Tests, ValidateDrawBindingSets(Pipeline, ValidSets).has_value());
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, {}).has_value());

		BindingLayoutHandle DifferentLayout = std::make_shared<TestBindingLayout>(LayoutDescription);
		TestBindingSet MismatchedSet(DifferentLayout);
		const std::array<const BindingSet*, 1> MismatchedSets = { &MismatchedSet };
		PF_CHECK(Tests, !ValidateDrawBindingSets(Pipeline, MismatchedSets).has_value());
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

		auto InvalidDepthUsage = DepthDescription;
		InvalidDepthUsage.Usage = TextureUsage::DepthStencilAttachment | TextureUsage::ShaderResource;
		PF_CHECK(Tests, !ValidateTextureUpload(InvalidDepthUsage, 0).has_value());

		RenderTargetDesc TargetDescription;
		TargetDescription.Width = 800;
		TargetDescription.Height = 600;
		PF_CHECK(Tests, ValidateRenderTargetDescription(TargetDescription).has_value());
		auto ZeroTargetWidth = TargetDescription;
		ZeroTargetWidth.Width = 0;
		PF_CHECK(Tests, !ValidateRenderTargetDescription(ZeroTargetWidth).has_value());
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
		TestSampler Sampler;
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });

		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Textures.push_back({ 0, std::cref(static_cast<const PulseForge::Texture&>(FakeTexture)) });
		SetDescription.Samplers.push_back({ 0, std::cref(static_cast<const PulseForge::Sampler&>(Sampler)) });
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

		auto DepthTextureBinding = SetDescription;
		DepthTextureBinding.Textures[0].Resource = std::cref(static_cast<const Texture&>(FakeDepthTexture));
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
		PF_CHECK(Tests, SupportsRequiredDeviceFeatures({ true, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ false, true, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, false, true }));
		PF_CHECK(Tests, !SupportsRequiredDeviceFeatures({ true, true, false }));
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
	TestSceneSerializationRoundTrip(Tests);
	TestSceneSerializerClone(Tests);
	TestSceneAssetsByUUID(Tests);
	TestPrefabSerialization(Tests);
	TestPrefabAssetsByUUID(Tests);
	TestAssetMetadataAndRegistry(Tests);
	TestAssetOperations(Tests);
	TestAssetImportOperation(Tests);
	TestMaterialAssets(Tests);
	TestProjectFiles(Tests);
	TestAssetReferenceValidation(Tests);
	TestGltfMeshImport(Tests);
	TestImageAssetImport(Tests);
	TestAudioEngineAndAssetCache(Tests);
	TestScriptRuntime(Tests);
	TestGraphicsPipelineAndDrawValidation(Tests);
	TestConstantBufferBindingValidation(Tests);
	TestTextureAndSamplerValidation(Tests);
	TestTextureSamplerAndBufferBindingValidation(Tests);
	TestVulkanQueueFamilySelection(Tests);
	TestVulkanRequiredDeviceFeatures(Tests);
	TestVulkanSurfaceAndPresentationSelection(Tests);
	TestVulkanExtentAndImageCountSelection(Tests);
	return Tests.Finish();
}
