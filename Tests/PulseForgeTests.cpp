#include <iostream>
#include <string_view>

#include "Core/Input.h"
#include "Core/LayerStack.h"
#include "Core/Timestep.h"
#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/Vulkan/VulkanSupport.h"
#include "Events/ApplicationEvent.h"
#include "Events/Event.h"
#include "Events/KeyEvent.h"
#include "Events/MouseEvent.h"

#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
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
		LayoutDescription.Visibility = ShaderStage::Fragment;
		LayoutDescription.Items = { { BindingResourceType::ConstantBuffer, 0 } };
		LayoutDescription.DebugName = "Test fragment constants";
		PF_CHECK(Tests, ValidateBindingLayout(LayoutDescription).has_value());

		auto DuplicateSlot = LayoutDescription;
		DuplicateSlot.Items.push_back({ BindingResourceType::ConstantBuffer, 0 });
		PF_CHECK(Tests, !ValidateBindingLayout(DuplicateSlot).has_value());

		auto InvalidVisibility = LayoutDescription;
		InvalidVisibility.Visibility = static_cast<ShaderStage>(0xff);
		PF_CHECK(Tests, !ValidateBindingLayout(InvalidVisibility).has_value());

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
		LayoutDescription.Visibility = ShaderStage::Fragment;
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
