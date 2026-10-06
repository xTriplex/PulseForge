#include <algorithm>
#include <iostream>
#include <string_view>

#include "Assets/AssetMetadata.h"
#include "Assets/AssetOperations.h"
#include "Assets/AssetReferenceValidator.h"
#include "Assets/AssetRegistry.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/PrefabSerializer.h"
#include "Core/Input.h"
#include "Core/LayerStack.h"
#include "Core/Timestep.h"
#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/Mesh.h"
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
		PF_CHECK(Tests, Child.SetCamera(DuplicateCamera).has_value());
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
			const auto DuplicatedMeshRenderer = Duplicate.GetMeshRenderer();
			const auto DuplicateParent = Duplicate.GetParent();
			PF_CHECK(Tests, Duplicate.GetUUID() != Child.GetUUID());
			PF_CHECK(Tests, DuplicateTag && DuplicateTag->Name == "Renamed child Copy");
			PF_CHECK(Tests, DuplicateTransform && glm::all(glm::equal(DuplicateTransform->Translation, ChildTransform.Translation)));
			PF_CHECK(Tests, DuplicatedCamera && DuplicatedCamera->has_value() &&
				DuplicatedCamera->value().VerticalFieldOfViewRadians == DuplicateCamera.VerticalFieldOfViewRadians);
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

	void TestEntityHandlesExpireWithScene(TestRunner& Tests)
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
		PF_CHECK(Tests, !StaleHandle.GetTag().has_value());
	}

	void TestSceneRenderSnapshot(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene TestScene;
		const UUID CameraID{ 0x1000000000000000ull, 1 };
		const UUID ParentID{ 0x2000000000000000ull, 2 };
		const UUID FirstMeshID{ 0x3000000000000000ull, 3 };
		const UUID SecondMeshID{ 0x4000000000000000ull, 4 };
		const AssetID FirstMeshAssetID{ 0x5000000000000000ull, 5 };
		const AssetID SecondMeshAssetID{ 0x6000000000000000ull, 6 };
		auto Camera = TestScene.CreateEntityWithUUID(CameraID, "Camera");
		auto Parent = TestScene.CreateEntityWithUUID(ParentID, "Parent");
		auto FirstMesh = TestScene.CreateEntityWithUUID(FirstMeshID, "First mesh");
		auto SecondMesh = TestScene.CreateEntityWithUUID(SecondMeshID, "Second mesh");
		PF_CHECK(Tests, Camera && Parent && FirstMesh && SecondMesh);
		if (!Camera || !Parent || !FirstMesh || !SecondMesh)
			return;

		TransformComponent CameraTransform;
		CameraTransform.Translation = { 0.0f, 0.0f, 5.0f };
		PF_CHECK(Tests, Camera->SetTransform(CameraTransform).has_value());
		PF_CHECK(Tests, Camera->SetCamera(CameraComponent{}).has_value());

		TransformComponent ParentTransform;
		ParentTransform.Translation = { 2.0f, 1.0f, 0.0f };
		TransformComponent FirstMeshTransform;
		FirstMeshTransform.Translation = { -1.0f, 0.0f, -2.0f };
		TransformComponent SecondMeshTransform;
		SecondMeshTransform.Translation = { 0.0f, -1.0f, -4.0f };
		PF_CHECK(Tests, Parent->SetTransform(ParentTransform).has_value());
		PF_CHECK(Tests, FirstMesh->SetTransform(FirstMeshTransform).has_value());
		PF_CHECK(Tests, SecondMesh->SetTransform(SecondMeshTransform).has_value());
		PF_CHECK(Tests, FirstMesh->SetParent(*Parent).has_value());
		PF_CHECK(Tests, FirstMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID }).has_value());
		PF_CHECK(Tests, SecondMesh->SetMeshRenderer(MeshRendererComponent{ SecondMeshAssetID }).has_value());

		const auto Snapshot = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 16.0f / 9.0f);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot)
			return;
		PF_CHECK(Tests, Snapshot->CameraEntity == CameraID);
		PF_CHECK(Tests, Snapshot->Meshes.size() == 2);
		PF_CHECK(Tests, Snapshot->Meshes[0].Entity == FirstMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[1].Entity == SecondMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[0].MeshAsset == FirstMeshAssetID);
		PF_CHECK(Tests, Snapshot->Meshes[1].MeshAsset == SecondMeshAssetID);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].x - 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].y - 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].z + 2.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[1].WorldTransform[3].y + 1.0f) < 0.0001f);
		const glm::vec4 CameraPositionClip = Snapshot->ViewProjection * glm::vec4(CameraTransform.Translation, 1.0f);
		PF_CHECK(Tests, glm::abs(CameraPositionClip.x) < 0.0001f && glm::abs(CameraPositionClip.y) < 0.0001f);

		const auto NilCamera = SceneRenderSnapshotBuilder::Build(TestScene, UUID{}, 1.0f);
		PF_CHECK(Tests, !NilCamera && NilCamera.error().Code == SceneRenderSnapshotErrorCode::InvalidCameraEntity);
		const auto MissingCamera = SceneRenderSnapshotBuilder::Build(TestScene, FirstMeshID, 1.0f);
		PF_CHECK(Tests, !MissingCamera && MissingCamera.error().Code == SceneRenderSnapshotErrorCode::MissingCameraComponent);
		const auto InvalidAspect = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 0.0f);
		PF_CHECK(Tests, !InvalidAspect && InvalidAspect.error().Code == SceneRenderSnapshotErrorCode::InvalidCamera);

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
		PF_CHECK(Tests, Child.SetMeshRenderer(MeshRendererComponent{ MeshAssetIdentifier }).has_value());
		const auto RootCamera = Root.GetCamera();
		PF_CHECK(Tests, RootCamera.has_value() && !RootCamera->has_value());

		const auto Serialized = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		PF_CHECK(Tests, Serialized->find("\"format\": \"PulseForgeScene\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"version\": 2") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"meshAsset\": \"" + MeshAssetIdentifier.ToString() + "\"") != std::string::npos);
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
		const auto LoadedParent = LoadedChild->GetParent();
		PF_CHECK(Tests, LoadedTag && LoadedTag->Name == "Child \"one\"");
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Translation, ChildTransform.Translation)));
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Scale, ChildTransform.Scale)));
		PF_CHECK(Tests, LoadedTransform && glm::abs(glm::length(LoadedTransform->Rotation) - 1.0f) < 0.0001f);
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value());
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value() &&
			glm::abs(LoadedCamera->value().VerticalFieldOfViewRadians - SourceCamera.VerticalFieldOfViewRadians) < 0.0001f);
		PF_CHECK(Tests, LoadedMeshRenderer && LoadedMeshRenderer->has_value() &&
			LoadedMeshRenderer->value().MeshAsset == MeshAssetIdentifier);
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
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 2");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 2").size(), "\"version\": 99");
		const auto UnsupportedVersionResult = SceneSerializer::Deserialize(UnsupportedVersion, Destination);
		PF_CHECK(Tests, !UnsupportedVersionResult.has_value());
		PF_CHECK(Tests, !UnsupportedVersionResult && UnsupportedVersionResult.error().Code == SceneSerializationErrorCode::UnsupportedVersion);
		PF_CHECK(Tests, Destination.FindEntity(RootId) == DestinationRootBeforeFailure);

		Scene LegacySource;
		const auto LegacyEntity = LegacySource.CreateEntityWithUUID(UUID{ 0x6000000000000000ull, 6 }, "Version one");
		PF_CHECK(Tests, LegacyEntity.has_value());
		const auto LegacySerializedVersionTwo = SceneSerializer::Serialize(LegacySource);
		PF_CHECK(Tests, LegacySerializedVersionTwo.has_value());
		if (LegacySerializedVersionTwo)
		{
			std::string LegacyVersionOne = *LegacySerializedVersionTwo;
			const size_t LegacyVersionPosition = LegacyVersionOne.find("\"version\": 2");
			PF_CHECK(Tests, LegacyVersionPosition != std::string::npos);
			if (LegacyVersionPosition != std::string::npos)
				LegacyVersionOne.replace(LegacyVersionPosition, std::string("\"version\": 2").size(), "\"version\": 1");
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

		const std::filesystem::path MovedPath = "Assets/Models/ship-renamed.gltf";
		const auto MoveResult = AssetOperations::Move(InitialRegistry, ProjectRoot, "Assets/Models/ship.gltf", MovedPath);
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
		const auto DuplicateResult = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, MovedPath, DuplicatePath);
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

		const auto ExistingDestination = AssetOperations::Duplicate(InitialRegistry, ProjectRoot, MovedPath, DuplicatePath);
		PF_CHECK(Tests, !ExistingDestination.has_value());
		PF_CHECK(Tests, !ExistingDestination && ExistingDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		const auto ExistingMoveDestination = AssetOperations::Move(InitialRegistry, ProjectRoot, MovedPath, DuplicatePath);
		PF_CHECK(Tests, !ExistingMoveDestination.has_value());
		PF_CHECK(Tests, !ExistingMoveDestination &&
			ExistingMoveDestination.error().Code == AssetOperationErrorCode::DestinationExists);
		PF_CHECK(Tests, std::filesystem::exists(MovedSource));
		PF_CHECK(Tests, std::filesystem::exists(DuplicateSource));
		const auto InvalidTraversal = AssetOperations::Move(InitialRegistry, ProjectRoot, MovedPath, "Assets/../escaped.gltf");
		PF_CHECK(Tests, !InvalidTraversal.has_value());
		PF_CHECK(Tests, !InvalidTraversal && InvalidTraversal.error().Code == AssetOperationErrorCode::InvalidPath);

		PF_CHECK(Tests, AssetOperations::Delete(InitialRegistry, ProjectRoot, DuplicatePath).has_value());
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
			InitialRegistry, ProjectRoot, MovedPath, CollisionCopyPath);
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
			InitialRegistry, ProjectRoot, MovedPath, "Assets/Models/untracked.gltf");
		PF_CHECK(Tests, !MissingMetadataOperation.has_value());
		PF_CHECK(Tests, !MissingMetadataOperation &&
			MissingMetadataOperation.error().Code == AssetOperationErrorCode::InvalidProjectAssets);
		PF_CHECK(Tests, !std::filesystem::exists(ProjectRoot / "Assets/Models/untracked.gltf"));
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
		PF_CHECK(Tests, ProjectIdentifier.has_value() && MissingAssetIdentifier.has_value());
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

		const std::filesystem::path AssetsDirectory = ProjectRoot / "Assets" / "Meshes";
		std::filesystem::create_directories(AssetsDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError)
			return;

		Scene TestScene;
		const auto ResolvedEntity = TestScene.CreateEntity("Resolved mesh");
		const auto MissingEntity = TestScene.CreateEntity("Missing mesh");
		PF_CHECK(Tests, ResolvedEntity.has_value() && MissingEntity.has_value());
		if (!ResolvedEntity || !MissingEntity)
			return;

		const std::filesystem::path MeshSource = AssetsDirectory / "resolved.mesh";
		{
			std::ofstream Output(MeshSource, std::ios::binary | std::ios::trunc);
			Output << "mesh source";
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto MeshMetadata = AssetMetadataSerializer::CreateForNewAsset(MeshSource);
		PF_CHECK(Tests, MeshMetadata.has_value());
		if (!MeshMetadata)
			return;

		PF_CHECK(Tests, ResolvedEntity->SetMeshRenderer(MeshRendererComponent{ MeshMetadata->ID }).has_value());
		PF_CHECK(Tests, MissingEntity->SetMeshRenderer(MeshRendererComponent{ *MissingAssetIdentifier }).has_value());

		AssetRegistry EmptyRegistry;
		const auto AllMissing = AssetReferenceValidator::Validate(TestScene, EmptyRegistry);
		PF_CHECK(Tests, AllMissing.has_value());
		PF_CHECK(Tests, AllMissing && AllMissing->size() == 2);

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Validation = AssetReferenceValidator::Validate(TestScene, Registry);
		PF_CHECK(Tests, Validation.has_value());
		PF_CHECK(Tests, Validation && Validation->size() == 1);
		if (!Validation || Validation->size() != 1)
			return;

		const AssetReferenceIssue& Issue = Validation->front();
		PF_CHECK(Tests, Issue.Code == AssetReferenceIssueCode::MissingAsset);
		PF_CHECK(Tests, Issue.Kind == AssetReferenceKind::Mesh);
		PF_CHECK(Tests, Issue.Entity == MissingEntity->GetUUID());
		PF_CHECK(Tests, Issue.Asset == *MissingAssetIdentifier);
		const auto MissingReference = MissingEntity->GetMeshRenderer();
		PF_CHECK(Tests, MissingReference && MissingReference->has_value());
		PF_CHECK(Tests, MissingReference && MissingReference->value().MeshAsset == *MissingAssetIdentifier);
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
			"Assets/Models/Triangle.gltf",
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
			"Assets/Textures/checker.bmp",
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
		PF_CHECK(Tests, Root->SetCamera(RootCamera).has_value());
		const AssetID MeshAssetID{ 0x7300000000000000ull, 7 };
		PF_CHECK(Tests, Child->SetMeshRenderer(MeshRendererComponent{ MeshAssetID }).has_value());

		const auto PrefabData = PrefabSerializer::Serialize(Source, *Root);
		PF_CHECK(Tests, PrefabData.has_value());
		if (!PrefabData)
			return;
		PF_CHECK(Tests, PrefabData->find("\"format\": \"PulseForgePrefab\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"root\": \"" + Root->GetUUID().ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("\"meshAsset\": \"" + MeshAssetID.ToString() + "\"") != std::string::npos);
		PF_CHECK(Tests, PrefabData->find("Outside prefab") == std::string::npos);
		PF_CHECK(Tests, PrefabData->find(ExternalParent->GetUUID().ToString()) == std::string::npos);

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
		PF_CHECK(Tests, FirstCamera && FirstCamera->has_value() &&
			FirstCamera->value().VerticalFieldOfViewRadians == RootCamera.VerticalFieldOfViewRadians);
		const auto FirstChildren = FirstInstance->GetChildren();
		PF_CHECK(Tests, FirstChildren && FirstChildren->size() == 1);
		if (!FirstChildren || FirstChildren->size() != 1)
			return;
		const Entity FirstChild = FirstChildren->front();
		const auto FirstChildTag = FirstChild.GetTag();
		const auto FirstChildMesh = FirstChild.GetMeshRenderer();
		const auto FirstGrandchildren = FirstChild.GetChildren();
		PF_CHECK(Tests, FirstChildTag && FirstChildTag->Name == "Prefab child");
		PF_CHECK(Tests, FirstChildMesh && FirstChildMesh->has_value() && FirstChildMesh->value().MeshAsset == MeshAssetID);
		PF_CHECK(Tests, FirstGrandchildren && FirstGrandchildren->size() == 1);

		const auto SecondInstance = PrefabSerializer::Instantiate(*PrefabData, Destination);
		PF_CHECK(Tests, SecondInstance.has_value());
		PF_CHECK(Tests, Destination.GetEntityCount() == OriginalEntityCount + 6);
		PF_CHECK(Tests, SecondInstance && FirstInstance->GetUUID() != SecondInstance->GetUUID());

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
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 1");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 1").size(), "\"version\": 99");
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
		TestSampler Sampler;
		TestBuffer ConstantBuffer(BufferDesc{ 16, BufferUsage::Constant, "Test constants" });

		BindingSetDesc SetDescription;
		SetDescription.Layout = Layout;
		SetDescription.Textures.push_back({ 0, std::cref(static_cast<const PulseForge::Texture&>(FakeTexture)) });
		SetDescription.Samplers.push_back({ 0, std::cref(static_cast<const PulseForge::Sampler&>(Sampler)) });
		SetDescription.Buffers.push_back({ 0, std::cref(static_cast<const Buffer&>(ConstantBuffer)) });
		PF_CHECK(Tests, ValidateBindingSet(SetDescription).has_value());

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
	TestEntityHandlesExpireWithScene(Tests);
	TestSceneRenderSnapshot(Tests);
	TestSceneSerializationRoundTrip(Tests);
	TestPrefabSerialization(Tests);
	TestAssetMetadataAndRegistry(Tests);
	TestAssetOperations(Tests);
	TestAssetReferenceValidation(Tests);
	TestGltfMeshImport(Tests);
	TestImageAssetImport(Tests);
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
