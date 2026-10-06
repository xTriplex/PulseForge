#include <algorithm>
#include <iostream>
#include <string_view>

#include "Core/Input.h"
#include "Core/LayerStack.h"
#include "Core/Timestep.h"
#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/Mesh.h"
#include "Renderer/Vulkan/VulkanSupport.h"
#include "Scene/Scene.h"
#include "Scene/SceneSerializer.h"
#include "Scene/UUID.h"
#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
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

		auto DuplicateResult = TestScene.DuplicateEntity(Child);
		PF_CHECK(Tests, DuplicateResult.has_value());
		if (DuplicateResult)
		{
			const Entity Duplicate = *DuplicateResult;
			const auto DuplicateTag = Duplicate.GetTag();
			const auto DuplicateTransform = Duplicate.GetTransform();
			const auto DuplicateParent = Duplicate.GetParent();
			PF_CHECK(Tests, Duplicate.GetUUID() != Child.GetUUID());
			PF_CHECK(Tests, DuplicateTag && DuplicateTag->Name == "Renamed child Copy");
			PF_CHECK(Tests, DuplicateTransform && glm::all(glm::equal(DuplicateTransform->Translation, ChildTransform.Translation)));
			PF_CHECK(Tests, DuplicateParent && DuplicateParent->has_value() && **DuplicateParent == Parent);
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

	void TestSceneSerializationRoundTrip(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const UUID RootId{ 0x2000000000000000ull, 2 };
		const UUID ChildId{ 0x1000000000000000ull, 1 };
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
		const auto RootCamera = Root.GetCamera();
		PF_CHECK(Tests, RootCamera.has_value() && !RootCamera->has_value());

		const auto Serialized = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized)
			return;
		PF_CHECK(Tests, Serialized->find("\"format\": \"PulseForgeScene\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"version\": 1") != std::string::npos);
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
		const auto LoadedParent = LoadedChild->GetParent();
		PF_CHECK(Tests, LoadedTag && LoadedTag->Name == "Child \"one\"");
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Translation, ChildTransform.Translation)));
		PF_CHECK(Tests, LoadedTransform && glm::all(glm::equal(LoadedTransform->Scale, ChildTransform.Scale)));
		PF_CHECK(Tests, LoadedTransform && glm::abs(glm::length(LoadedTransform->Rotation) - 1.0f) < 0.0001f);
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value());
		PF_CHECK(Tests, LoadedCamera && LoadedCamera->has_value() &&
			glm::abs(LoadedCamera->value().VerticalFieldOfViewRadians - SourceCamera.VerticalFieldOfViewRadians) < 0.0001f);
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
		const size_t VersionPosition = UnsupportedVersion.find("\"version\": 1");
		PF_CHECK(Tests, VersionPosition != std::string::npos);
		if (VersionPosition != std::string::npos)
			UnsupportedVersion.replace(VersionPosition, std::string("\"version\": 1").size(), "\"version\": 99");
		const auto UnsupportedVersionResult = SceneSerializer::Deserialize(UnsupportedVersion, Destination);
		PF_CHECK(Tests, !UnsupportedVersionResult.has_value());
		PF_CHECK(Tests, !UnsupportedVersionResult && UnsupportedVersionResult.error().Code == SceneSerializationErrorCode::UnsupportedVersion);
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
	TestSceneSerializationRoundTrip(Tests);
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
