#include "TestCoreSupport.h"
#include "Input/GameplayInput.h"
#include "Core/BoundedInput.h"

#include <algorithm>
#include <functional>
#include <istream>
#include <sstream>
#include <streambuf>
#include <string>
#include <utility>

namespace
{
	class GrowingInputBuffer final : public std::streambuf
	{
	public:
		GrowingInputBuffer(std::string InitialData, std::string GrowthData)
			: m_Data(std::move(InitialData)), m_GrowthData(std::move(GrowthData))
		{
		}

		[[nodiscard]] size_t GetBytesRead() const noexcept { return m_Position; }

	protected:
		std::streamsize xsgetn(char* Destination, std::streamsize Count) override
		{
			const size_t Requested = static_cast<size_t>(Count);
			if (!m_GrowthAdded && Requested > m_Data.size() - m_Position)
			{
				m_Data += m_GrowthData;
				m_GrowthAdded = true;
			}

			const size_t Available = m_Data.size() - m_Position;
			const size_t BytesToCopy = std::min(Requested, Available);
			std::copy_n(m_Data.data() + m_Position, BytesToCopy, Destination);
			m_Position += BytesToCopy;
			return static_cast<std::streamsize>(BytesToCopy);
		}

	private:
		std::string m_Data;
		std::string m_GrowthData;
		size_t m_Position = 0;
		bool m_GrowthAdded = false;
	};

	class FailingInputBuffer final : public std::streambuf
	{
	protected:
		std::streamsize xsgetn(char*, std::streamsize) override { throw std::ios_base::failure("injected input failure"); }
	};

	void TestBoundedInputReader(TestRunner& Tests)
	{
		using namespace PulseForge::Detail;
		std::istringstream ExactInput("1234");
		const auto Exact = ReadBoundedInput(ExactInput, 4);
		PF_CHECK(Tests, Exact && *Exact == "1234");

		std::istringstream OversizedInput("12345");
		const auto Oversized = ReadBoundedInput(OversizedInput, 4);
		PF_CHECK(Tests, !Oversized && Oversized.error().Code == BoundedInputErrorCode::TooLarge);

		GrowingInputBuffer GrowingBuffer("1234", "5");
		std::istream GrowingInput(&GrowingBuffer);
		const auto GrewDuringRead = ReadBoundedInput(GrowingInput, 4);
		PF_CHECK(Tests, !GrewDuringRead && GrewDuringRead.error().Code == BoundedInputErrorCode::TooLarge);
		PF_CHECK(Tests, GrowingBuffer.GetBytesRead() == 5);

		FailingInputBuffer FailingBuffer;
		std::istream FailingInput(&FailingBuffer);
		const auto FailedRead = ReadBoundedInput(FailingInput, 4);
		PF_CHECK(Tests, !FailedRead && FailedRead.error().Code == BoundedInputErrorCode::ReadFailed);
	}

	struct LayerLifecycle
	{
		int AttachCount = 0;
		int DetachCount = 0;
		int DestroyCount = 0;
	};

	class TrackingLayer final : public PulseForge::Layer
	{
	public:
		explicit TrackingLayer(LayerLifecycle& Lifecycle) : Layer("TrackingLayer"), m_Lifecycle(Lifecycle) {}

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

	void TestLayerStackRawInputDispatch(TestRunner& Tests)
	{
		int Observed = 0;
		PulseForge::InputEvent LastInput;
		class InputObservingLayer final : public PulseForge::Layer
		{
		public:
			InputObservingLayer(int& Count, PulseForge::InputEvent& Last) : Layer("InputObservingLayer"), m_Count(Count), m_Last(Last) {}
			void OnInputEvent(const PulseForge::InputEvent& Event) override
			{
				++m_Count;
				m_Last = Event;
			}

		private:
			int& m_Count;
			PulseForge::InputEvent& m_Last;
		};

		PulseForge::LayerStack Stack;
		Stack.PushLayer(std::make_unique<InputObservingLayer>(Observed, LastInput));
		const PulseForge::InputEvent GamepadPress{
			.Kind = PulseForge::InputEventKind::Pressed,
			.Control = { PulseForge::InputDevice::GamepadButton,
				static_cast<uint16_t>(PulseForge::GamepadButton::A) },
			.DeviceInstance = 29
		};
		Stack.DispatchInputEvent(GamepadPress);
		PF_CHECK(Tests, Observed == 1);
		PF_CHECK(Tests, LastInput.Kind == GamepadPress.Kind && LastInput.Control == GamepadPress.Control);
		PF_CHECK(Tests, LastInput.DeviceInstance == GamepadPress.DeviceInstance);
	}

	void TestLayerStackClearDetachesInReverseOrder(TestRunner& Tests)
	{
		std::vector<int> DetachOrder;
		class OrderedLayer final : public PulseForge::Layer
		{
		public:
			OrderedLayer(int Id, std::vector<int>& Order) : Layer("OrderedLayer"), m_Id(Id), m_Order(Order) {}
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
			VisitingLayer(int Id, std::vector<int>& Order) : Layer("VisitingLayer"), m_Id(Id), m_Order(Order) {}
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
		Stack.UpdateLayers(PulseForge::Timestep{});

		PF_CHECK(Tests, UpdateOrder == std::vector<int>({ 1, 2, 3, 4 }));
	}

	void TestLayerStackRejectsMutationDuringDispatch(TestRunner& Tests)
	{
		std::vector<int> UpdateOrder;
		std::vector<int> EventOrder;
		LayerLifecycle RejectedLayerLifecycle;
		int RejectedMutationCount = 0;
		class MutationAttemptingLayer final : public PulseForge::Layer
		{
		public:
			MutationAttemptingLayer(PulseForge::LayerStack& Stack, LayerLifecycle& RejectedLifecycle, std::vector<int>& UpdateOrder,
				std::vector<int>& EventOrder, int& RejectedMutationCount)
				: Layer("MutationAttemptingLayer"), m_Stack(Stack), m_RejectedLifecycle(RejectedLifecycle), m_UpdateOrder(UpdateOrder),
				  m_EventOrder(EventOrder), m_RejectedMutationCount(RejectedMutationCount)
			{
			}

			void OnUpdate(PulseForge::Timestep) override
			{
				m_UpdateOrder.push_back(2);
				TryMutation([this]() { m_Stack.PushOverlay(std::make_unique<TrackingLayer>(m_RejectedLifecycle)); });
				TryMutation([this]() { (void)m_Stack.PopLayer(*this); });
			}

			void OnEvent(PulseForge::Event&) override
			{
				m_EventOrder.push_back(2);
				TryMutation([this]() { m_Stack.PushLayer(std::make_unique<TrackingLayer>(m_RejectedLifecycle)); });
				TryMutation([this]() { m_Stack.Clear(); });
			}

		private:
			void TryMutation(const std::function<void()>& Attempt)
			{
				try
				{
					Attempt();
				}
				catch (const std::logic_error&)
				{
					++m_RejectedMutationCount;
				}
			}

			PulseForge::LayerStack& m_Stack;
			LayerLifecycle& m_RejectedLifecycle;
			std::vector<int>& m_UpdateOrder;
			std::vector<int>& m_EventOrder;
			int& m_RejectedMutationCount;
		};

		class VisitingLayer final : public PulseForge::Layer
		{
		public:
			VisitingLayer(int Id, std::vector<int>& UpdateOrder, std::vector<int>& EventOrder)
				: Layer("VisitingLayer"), m_Id(Id), m_UpdateOrder(UpdateOrder), m_EventOrder(EventOrder)
			{
			}

			void OnUpdate(PulseForge::Timestep) override { m_UpdateOrder.push_back(m_Id); }
			void OnEvent(PulseForge::Event&) override { m_EventOrder.push_back(m_Id); }

		private:
			int m_Id;
			std::vector<int>& m_UpdateOrder;
			std::vector<int>& m_EventOrder;
		};

		PulseForge::LayerStack Stack;
		Stack.PushLayer(std::make_unique<VisitingLayer>(1, UpdateOrder, EventOrder));
		Stack.PushLayer(
			std::make_unique<MutationAttemptingLayer>(Stack, RejectedLayerLifecycle, UpdateOrder, EventOrder, RejectedMutationCount));
		Stack.PushOverlay(std::make_unique<VisitingLayer>(3, UpdateOrder, EventOrder));
		Stack.UpdateLayers(PulseForge::Timestep{});
		PulseForge::KeyPressedEvent Event(65, 0);
		Stack.DispatchEvent(Event);

		PF_CHECK(Tests, UpdateOrder == std::vector<int>({ 1, 2, 3 }));
		PF_CHECK(Tests, EventOrder == std::vector<int>({ 3, 2, 1 }));
		PF_CHECK(Tests, RejectedMutationCount == 4);
		PF_CHECK(Tests, RejectedLayerLifecycle.AttachCount == 0);
		PF_CHECK(Tests, RejectedLayerLifecycle.DetachCount == 0);
		PF_CHECK(Tests, RejectedLayerLifecycle.DestroyCount == 2);
		PF_CHECK(Tests, Stack.GetLayerCount() == 3);
	}

	void TestEventDispatchAndHandledState(TestRunner& Tests)
	{
		PulseForge::KeyPressedEvent KeyEvent(65, 0);
		PulseForge::EventDispatcher Dispatcher(KeyEvent);
		int CallbackCount = 0;
		const bool WasDispatched = Dispatcher.Dispatch<PulseForge::KeyPressedEvent>(
			[&CallbackCount](PulseForge::KeyPressedEvent&)
			{
				++CallbackCount;
				return true;
			});
		PF_CHECK(Tests, WasDispatched);
		PF_CHECK(Tests, CallbackCount == 1);
		PF_CHECK(Tests, KeyEvent.bHandled);

		PulseForge::MouseMovedEvent MouseEvent(3.0f, 4.0f);
		PulseForge::EventDispatcher MismatchedDispatcher(MouseEvent);
		const bool MismatchedDispatch = MismatchedDispatcher.Dispatch<PulseForge::KeyPressedEvent>(
			[&CallbackCount](PulseForge::KeyPressedEvent&)
			{
				++CallbackCount;
				return true;
			});
		PF_CHECK(Tests, !MismatchedDispatch);
		PF_CHECK(Tests, CallbackCount == 1);
		PF_CHECK(Tests, !MouseEvent.bHandled);

		KeyEvent.bHandled = true;
		PulseForge::EventDispatcher AlreadyHandledDispatcher(KeyEvent);
		AlreadyHandledDispatcher.Dispatch<PulseForge::KeyPressedEvent>([](PulseForge::KeyPressedEvent&) { return false; });
		PF_CHECK(Tests, KeyEvent.bHandled);
	}

	void TestLayerAttachFailureRollsBackOwnership(TestRunner& Tests)
	{
		class FailingLayer final : public PulseForge::Layer
		{
		public:
			FailingLayer(int& DetachCount, int& DestroyCount)
				: Layer("FailingLayer"), m_DetachCount(DetachCount), m_DestroyCount(DestroyCount)
			{
			}
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
		PF_CHECK(Tests, Stack.GetLayerCount() == 0);
		PF_CHECK(Tests, DetachCount == 1);
		PF_CHECK(Tests, DestroyCount == 1);

		LayerLifecycle NestedLayerLifecycle;
		int FailedAttachDetachCount = 0;
		int FailedAttachDestroyCount = 0;
		bool NestedMutationRejected = false;
		class ReentrantFailingLayer final : public PulseForge::Layer
		{
		public:
			ReentrantFailingLayer(PulseForge::LayerStack& Stack, LayerLifecycle& NestedLayerLifecycle, int& DetachCount, int& DestroyCount,
				bool& NestedMutationRejected)
				: Layer("ReentrantFailingLayer"), m_Stack(Stack), m_NestedLayerLifecycle(NestedLayerLifecycle), m_DetachCount(DetachCount),
				  m_DestroyCount(DestroyCount), m_NestedMutationRejected(NestedMutationRejected)
			{
			}

			~ReentrantFailingLayer() override { ++m_DestroyCount; }
			void OnAttach() override
			{
				try
				{
					m_Stack.PushLayer(std::make_unique<TrackingLayer>(m_NestedLayerLifecycle));
				}
				catch (const std::logic_error&)
				{
					m_NestedMutationRejected = true;
				}
				throw std::runtime_error("expected failure after nested attach request");
			}
			void OnDetach() override { ++m_DetachCount; }

		private:
			PulseForge::LayerStack& m_Stack;
			LayerLifecycle& m_NestedLayerLifecycle;
			int& m_DetachCount;
			int& m_DestroyCount;
			bool& m_NestedMutationRejected;
		};

		PulseForge::LayerStack ReentrantStack;
		bool ReentrantAttachThrew = false;
		try
		{
			ReentrantStack.PushLayer(std::make_unique<ReentrantFailingLayer>(
				ReentrantStack, NestedLayerLifecycle, FailedAttachDetachCount, FailedAttachDestroyCount, NestedMutationRejected));
		}
		catch (const std::runtime_error&)
		{
			ReentrantAttachThrew = true;
		}
		PF_CHECK(Tests, ReentrantAttachThrew);
		PF_CHECK(Tests, NestedMutationRejected);
		PF_CHECK(Tests, ReentrantStack.GetLayerCount() == 0);
		PF_CHECK(Tests, FailedAttachDetachCount == 1);
		PF_CHECK(Tests, FailedAttachDestroyCount == 1);
		PF_CHECK(Tests, NestedLayerLifecycle.AttachCount == 0);
		PF_CHECK(Tests, NestedLayerLifecycle.DetachCount == 0);
		PF_CHECK(Tests, NestedLayerLifecycle.DestroyCount == 1);
	}

	void TestLayerPopDetachesAndDestroysWhenDetachThrows(TestRunner& Tests)
	{
		class ThrowingDetachLayer final : public PulseForge::Layer
		{
		public:
			ThrowingDetachLayer(std::string Name, int& DetachCount, int& DestroyCount)
				: Layer(Name), m_DetachCount(DetachCount), m_DestroyCount(DestroyCount)
			{
			}
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
			PulseForge::Layer& Layer =
				Stack.PushLayer(std::make_unique<ThrowingDetachLayer>("ThrowingLayer", LayerDetachCount, LayerDestroyCount));
			PulseForge::Layer& Overlay =
				Stack.PushOverlay(std::make_unique<ThrowingDetachLayer>("ThrowingOverlay", OverlayDetachCount, OverlayDestroyCount));

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
				: Layer("DetachingLayer"), m_DetachCount(DetachCount), m_DestroyCount(DestroyCount), m_ShouldThrow(ShouldThrow)
			{
			}
			~DetachingLayer() override { ++m_DestroyCount; }
			void OnDetach() override
			{
				++m_DetachCount;
				if (m_ShouldThrow) throw std::runtime_error("expected detach failure");
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
		PF_CHECK(Tests, Stack.GetLayerCount() == 0);
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

} // namespace

void RunCoreTests(TestRunner& Tests)
{
	TestBoundedInputReader(Tests);
	TestLayerStackOwnership(Tests);
	TestLayerStackRawInputDispatch(Tests);
	TestLayerStackClearDetachesInReverseOrder(Tests);
	TestLayerAndOverlayIterationOrder(Tests);
	TestLayerStackRejectsMutationDuringDispatch(Tests);
	TestEventDispatchAndHandledState(Tests);
	TestLayerAttachFailureRollsBackOwnership(Tests);
	TestLayerPopDetachesAndDestroysWhenDetachThrows(Tests);
	TestLayerClearSurvivesDetachFailure(Tests);
	TestInputState(Tests);
	TestTimestepConversions(Tests);
}
