#include "TestSupport.h"
#include "TestGraphicsSupport.h"

namespace
{
	using namespace PulseForgeTestSupport;
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
}

void RunCoreTests(TestRunner& Tests)
{
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
}
