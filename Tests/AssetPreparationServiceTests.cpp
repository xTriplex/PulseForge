#include "Assets/AssetPreparationService.h"
#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace
{
	using namespace PulseForge;
	using namespace std::chrono_literals;
	using CpuPayload = AssetPreparationService::CpuPayload;
	using Processor = AssetPreparationService::Processor;
	using TestPayloadValue = AssetPreparationValue<std::vector<std::byte>>;

	constexpr auto TestTimeout = 5s;

	AssetPreparationRequestIdentity Identity(std::string Recipe)
	{
		return { "asset-preparation-test-session", std::move(Recipe) };
	}

	CpuPayload Payload(size_t Size, std::byte Value = std::byte{ 0x2a })
	{
		return MakeAssetPreparationValue(std::vector<std::byte>(Size, Value), Size);
	}

	size_t PayloadSize(const CpuPayload& Value)
	{
		const auto Typed = std::dynamic_pointer_cast<const TestPayloadValue>(Value);
		return Typed ? Typed->Get().size() : 0;
	}

	class ProcessorGate final
	{
	public:
		~ProcessorGate() { Release(); }

		void EnterAndWait()
		{
			std::unique_lock Lock(m_Mutex);
			m_Entered = true;
			m_Changed.notify_all();
			m_Changed.wait(Lock, [this] { return m_Released; });
		}

		bool WaitUntilEntered()
		{
			std::unique_lock Lock(m_Mutex);
			return m_Changed.wait_for(Lock, TestTimeout, [this] { return m_Entered; });
		}

		void Release()
		{
			{
				std::lock_guard Lock(m_Mutex);
				m_Released = true;
			}
			m_Changed.notify_all();
		}

	private:
		std::mutex m_Mutex;
		std::condition_variable m_Changed;
		bool m_Entered = false;
		bool m_Released = false;
	};

	bool WaitForStatus(
		const AssetPreparationService::Subscription& Subscription,
		AssetPreparationStatus Expected,
		AssetPreparationSnapshot* Result = nullptr)
	{
		const auto Deadline = std::chrono::steady_clock::now() + TestTimeout;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			AssetPreparationSnapshot Snapshot = Subscription.Poll();
			if (Snapshot.Status == Expected)
			{
				if (Result) *Result = std::move(Snapshot);
				return true;
			}
			std::this_thread::yield();
		}
		if (Result) *Result = Subscription.Poll();
		return false;
	}

	void TestEquivalentSubscribersAndIndependentCancellation(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		std::atomic_size_t Calls = 0;
		Processor Work = [&](std::stop_token)
		{
			++Calls;
			Gate.EnterAndWait();
			return std::expected<CpuPayload, AssetPreparationError>(Payload(3));
		};
		auto First = Service.Request(Identity("same-recipe"), AssetPreparationPriority::Normal, 16, Work);
		PF_CHECK(Tests, First.has_value());
		if (!First) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());

		auto Second = Service.Request(Identity("same-recipe"), AssetPreparationPriority::Background, 16,
			[&](std::stop_token)
			{
				++Calls;
				return std::expected<CpuPayload, AssetPreparationError>(Payload(99));
			});
		PF_CHECK(Tests, Second.has_value());
		if (!Second) return;
		First->Cancel();
		PF_CHECK(Tests, First->Poll().Status == AssetPreparationStatus::Cancelled);
		Gate.Release();

		AssetPreparationSnapshot Ready;
		PF_CHECK(Tests, WaitForStatus(*Second, AssetPreparationStatus::CpuReady, &Ready));
		PF_CHECK(Tests, Ready.CpuPayload && PayloadSize(Ready.CpuPayload) == 3 && Ready.CpuPayload->GetRetainedBytes() == 3);
		PF_CHECK(Tests, Calls.load() == 1);
		Service.Shutdown();
	}

	void TestFinalCancellationSuppressesLateResult(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		std::atomic_bool Returned = false;
		auto Request = Service.Request(Identity("cancel-final"), AssetPreparationPriority::Normal, 16,
			[&](std::stop_token)
			{
				Gate.EnterAndWait(); // Deliberately completes even after cancellation was requested.
				Returned = true;
				return std::expected<CpuPayload, AssetPreparationError>(Payload(2));
			});
		PF_CHECK(Tests, Request.has_value());
		if (!Request) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());
		Request->Cancel();
		PF_CHECK(Tests, Request->Poll().Status == AssetPreparationStatus::Cancelled);
		Gate.Release();

		const auto Deadline = std::chrono::steady_clock::now() + TestTimeout;
		while (!Returned.load() && std::chrono::steady_clock::now() < Deadline) std::this_thread::yield();
		PF_CHECK(Tests, Returned.load());
		PF_CHECK(Tests, Request->Poll().Status == AssetPreparationStatus::Cancelled);
		Service.Shutdown();
	}

	void TestStopCallbackCanQueryStatsDuringCancellation(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		std::atomic_bool CallbackEntered = false;
		std::atomic_bool CallbackReturned = false;
		auto Request = Service.Request(Identity("cancel-callback-stats"), AssetPreparationPriority::Normal, 1,
			[&](std::stop_token Token)
			{
				std::stop_callback Callback(Token, [&]
				{
					CallbackEntered = true;
					(void)Service.GetStats();
					CallbackReturned = true;
				});
				Gate.EnterAndWait();
				return std::expected<CpuPayload, AssetPreparationError>(Payload(1));
			});
		PF_CHECK(Tests, Request.has_value());
		if (!Request) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());

		Request->Cancel();
		PF_CHECK(Tests, CallbackEntered.load());
		PF_CHECK(Tests, CallbackReturned.load());
		PF_CHECK(Tests, Request->Poll().Status == AssetPreparationStatus::Cancelled);
		Gate.Release();
		Service.Shutdown();
	}

	void TestCallerCanAdvanceWhilePreparationIsBlocked(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		auto Request = Service.Request(Identity("responsive-caller"), AssetPreparationPriority::Interactive, 1,
			[&](std::stop_token)
			{
				Gate.EnterAndWait();
				return std::expected<CpuPayload, AssetPreparationError>(Payload(1));
			});
		PF_CHECK(Tests, Request.has_value());
		if (!Request) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());

		// A deterministic caller-side frame/status loop keeps advancing while the worker is held at the gate.
		size_t MainLoopAdvances = 0;
		for (size_t Frame = 0; Frame < 1000; ++Frame)
		{
			(void)Request->Poll();
			++MainLoopAdvances;
		}
		PF_CHECK(Tests, MainLoopAdvances == 1000);
		PF_CHECK(Tests, Request->Poll().Status == AssetPreparationStatus::Processing);
		Gate.Release();
		PF_CHECK(Tests, WaitForStatus(*Request, AssetPreparationStatus::CpuReady));
		Service.Shutdown();
	}

	void TestQueuedPriorityPromotionDoesNotDuplicateWork(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		std::mutex OrderMutex;
		std::vector<std::string> Order;
		std::atomic_size_t PromotedCalls = 0;
		auto First = Service.Request(Identity("active-gate"), AssetPreparationPriority::Normal, 1,
			[&](std::stop_token)
			{
				Gate.EnterAndWait();
				return std::expected<CpuPayload, AssetPreparationError>(Payload(1));
			});
		PF_CHECK(Tests, First.has_value());
		if (!First) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());

		auto Promoted = Service.Request(Identity("to-promote"), AssetPreparationPriority::Background, 1,
			[&](std::stop_token)
			{
				++PromotedCalls;
				std::lock_guard Lock(OrderMutex);
				Order.emplace_back("promoted");
				return std::expected<CpuPayload, AssetPreparationError>(Payload(1));
			});
		auto Later = Service.Request(Identity("normal"), AssetPreparationPriority::Normal, 1,
			[&](std::stop_token)
			{
				std::lock_guard Lock(OrderMutex);
				Order.emplace_back("normal");
				return std::expected<CpuPayload, AssetPreparationError>(Payload(1));
			});
		PF_CHECK(Tests, Promoted.has_value() && Later.has_value());
		if (!Promoted || !Later) return;
		auto PromotedSubscriber = Service.Request(Identity("to-promote"), AssetPreparationPriority::Interactive, 1,
			[&](std::stop_token)
			{
				++PromotedCalls;
				return std::expected<CpuPayload, AssetPreparationError>(Payload(7));
			});
		PF_CHECK(Tests, PromotedSubscriber.has_value());
		if (!PromotedSubscriber) return;
		Gate.Release();

		PF_CHECK(Tests, WaitForStatus(*Promoted, AssetPreparationStatus::CpuReady));
		PF_CHECK(Tests, WaitForStatus(*PromotedSubscriber, AssetPreparationStatus::CpuReady));
		PF_CHECK(Tests, WaitForStatus(*Later, AssetPreparationStatus::CpuReady));
		{
			std::lock_guard Lock(OrderMutex);
			PF_CHECK(Tests, Order == (std::vector<std::string>{ "promoted", "normal" }));
		}
		PF_CHECK(Tests, PromotedCalls.load() == 1);
		Service.Shutdown();
	}

	void TestExplicitFailureRetryAndExceptionRecovery(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		std::atomic_size_t FailureCalls = 0;
		auto Failed = Service.Request(Identity("retry"), AssetPreparationPriority::Normal, 1,
			[&](std::stop_token)
			{
				++FailureCalls;
				return std::expected<CpuPayload, AssetPreparationError>(std::unexpected(
					AssetPreparationError{ AssetPreparationErrorCode::ProcessingFailed, "injected failure" }));
			});
		PF_CHECK(Tests, Failed.has_value());
		if (!Failed) return;
		AssetPreparationSnapshot Failure;
		PF_CHECK(Tests, WaitForStatus(*Failed, AssetPreparationStatus::Failed, &Failure));
		PF_CHECK(Tests, Failure.Error && Failure.Error->Message == "injected failure");
		auto RetainedFailure = Service.Request(Identity("retry"), AssetPreparationPriority::Interactive, 1,
			[&](std::stop_token)
			{
				++FailureCalls;
				return std::expected<CpuPayload, AssetPreparationError>(Payload(100));
			});
		PF_CHECK(Tests, RetainedFailure.has_value());
		if (!RetainedFailure) return;
		PF_CHECK(Tests, RetainedFailure->Poll().Status == AssetPreparationStatus::Failed);
		PF_CHECK(Tests, FailureCalls.load() == 1);
		Failed->Cancel();
		RetainedFailure->Cancel();

		auto Retry = Service.Request(Identity("retry"), AssetPreparationPriority::Normal, 1,
			[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(5)); });
		PF_CHECK(Tests, Retry.has_value());
		if (!Retry) return;
		PF_CHECK(Tests, WaitForStatus(*Retry, AssetPreparationStatus::CpuReady));
		PF_CHECK(Tests, Retry->Poll().CpuPayload && PayloadSize(Retry->Poll().CpuPayload) == 5);

		auto Threw = Service.Request(Identity("throw"), AssetPreparationPriority::Normal, 1,
			[](std::stop_token) -> std::expected<CpuPayload, AssetPreparationError> { throw std::runtime_error("worker exception"); });
		PF_CHECK(Tests, Threw.has_value());
		if (!Threw) return;
		AssetPreparationSnapshot ExceptionFailure;
		PF_CHECK(Tests, WaitForStatus(*Threw, AssetPreparationStatus::Failed, &ExceptionFailure));
		PF_CHECK(Tests, ExceptionFailure.Error && ExceptionFailure.Error->Message == "worker exception");

		auto Recovered = Service.Request(Identity("after-throw"), AssetPreparationPriority::Normal, 1,
			[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
		PF_CHECK(Tests, Recovered.has_value());
		if (!Recovered) return;
		PF_CHECK(Tests, WaitForStatus(*Recovered, AssetPreparationStatus::CpuReady));
		Service.Shutdown();
	}

	void TestAdmissionAndCompletedPayloadBounds(TestRunner& Tests)
	{
		{
			AssetPreparationServiceLimits Limits;
			Limits.MaximumWorkers = 1;
			Limits.MaximumOutstandingJobs = 4;
			Limits.MaximumSubscribers = 2;
			Limits.MaximumReservedWorkingBytes = 10;
			AssetPreparationService Service(Limits);
			ProcessorGate Gate;
			auto Active = Service.Request(Identity("active"), AssetPreparationPriority::Normal, 7,
				[&](std::stop_token) { Gate.EnterAndWait(); return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
			PF_CHECK(Tests, Active.has_value());
			if (!Active) return;
			PF_CHECK(Tests, Gate.WaitUntilEntered());
			auto SecondSubscriber = Service.Request(Identity("active"), AssetPreparationPriority::Normal, 1,
				[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
			PF_CHECK(Tests, SecondSubscriber.has_value());
			auto TooManySubscribers = Service.Request(Identity("active"), AssetPreparationPriority::Normal, 1,
				[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
			PF_CHECK(Tests, !TooManySubscribers && TooManySubscribers.error().Code == AssetPreparationErrorCode::SubscriberLimit);
			if (SecondSubscriber) SecondSubscriber->Cancel();
			auto TooMuchWorkingSet = Service.Request(Identity("working-limit"), AssetPreparationPriority::Normal, 4,
				[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
			PF_CHECK(Tests, !TooMuchWorkingSet && TooMuchWorkingSet.error().Code == AssetPreparationErrorCode::WorkingSetLimit);
			Gate.Release();
			PF_CHECK(Tests, WaitForStatus(*Active, AssetPreparationStatus::CpuReady));
			Service.Shutdown();
		}
		{
			AssetPreparationServiceLimits Limits;
			Limits.MaximumWorkers = 1;
			Limits.MaximumOutstandingJobs = 1;
			Limits.MaximumReservedWorkingBytes = 8;
			AssetPreparationService Service(Limits);
			ProcessorGate Gate;
			auto Active = Service.Request(Identity("active"), AssetPreparationPriority::Normal, 1,
				[&](std::stop_token) { Gate.EnterAndWait(); return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
			PF_CHECK(Tests, Active.has_value());
			if (!Active) return;
			PF_CHECK(Tests, Gate.WaitUntilEntered());
			auto QueueFull = Service.Request(Identity("queue-full"), AssetPreparationPriority::Normal, 1,
				[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
			PF_CHECK(Tests, !QueueFull && QueueFull.error().Code == AssetPreparationErrorCode::QueueFull);
			Gate.Release();
			PF_CHECK(Tests, WaitForStatus(*Active, AssetPreparationStatus::CpuReady));
			Service.Shutdown();
		}
		{
			AssetPreparationServiceLimits Limits;
			Limits.MaximumWorkers = 1;
			Limits.MaximumCompletedPayloadBytes = 2;
			AssetPreparationService Service(Limits);
			auto Oversized = Service.Request(Identity("oversized-result"), AssetPreparationPriority::Normal, 1,
				[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(3)); });
			PF_CHECK(Tests, Oversized.has_value());
			if (!Oversized) return;
			AssetPreparationSnapshot Result;
			PF_CHECK(Tests, WaitForStatus(*Oversized, AssetPreparationStatus::Failed, &Result));
			PF_CHECK(Tests, Result.Error && Result.Error->Code == AssetPreparationErrorCode::ResultLimit);
			Service.Shutdown();
		}
	}

	void TestShutdownJoinsAndSuppressesLatePublication(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		std::atomic_bool ShutdownFinished = false;
		auto Active = Service.Request(Identity("shutdown-active"), AssetPreparationPriority::Normal, 1,
			[&](std::stop_token)
			{
				Gate.EnterAndWait();
				return std::expected<CpuPayload, AssetPreparationError>(Payload(4));
			});
		PF_CHECK(Tests, Active.has_value());
		if (!Active) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());
		std::thread ShutdownThread([&]
		{
			Service.Shutdown();
			ShutdownFinished = true;
		});
		// This processor deliberately ignores cancellation. Shutdown must wait for the bounded operation to return.
		const auto Deadline = std::chrono::steady_clock::now() + TestTimeout;
		while (!ShutdownFinished.load() && std::chrono::steady_clock::now() < Deadline)
		{
			if (Active->Poll().Status == AssetPreparationStatus::Cancelled) break;
			std::this_thread::yield();
		}
		PF_CHECK(Tests, Active->Poll().Status == AssetPreparationStatus::Cancelled);
		Gate.Release();
		ShutdownThread.join();
		PF_CHECK(Tests, ShutdownFinished.load());
		PF_CHECK(Tests, Active->Poll().Status == AssetPreparationStatus::Cancelled);
		auto Rejected = Service.Request(Identity("after-shutdown"), AssetPreparationPriority::Normal, 1,
			[](std::stop_token) { return std::expected<CpuPayload, AssetPreparationError>(Payload(1)); });
		PF_CHECK(Tests, !Rejected && Rejected.error().Code == AssetPreparationErrorCode::ServiceStopping);
	}

	void TestStopCallbackCanQueryStatsDuringShutdown(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1 });
		ProcessorGate Gate;
		std::atomic_bool CallbackEntered = false;
		std::atomic_bool CallbackReturned = false;
		auto Active = Service.Request(Identity("shutdown-callback-stats"), AssetPreparationPriority::Normal, 1,
			[&](std::stop_token Token)
			{
				std::stop_callback Callback(Token, [&]
				{
					CallbackEntered = true;
					(void)Service.GetStats();
					CallbackReturned = true;
					Gate.Release();
				});
				Gate.EnterAndWait();
				return std::expected<CpuPayload, AssetPreparationError>(Payload(1));
			});
		PF_CHECK(Tests, Active.has_value());
		if (!Active) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());

		Service.Shutdown();
		PF_CHECK(Tests, CallbackEntered.load());
		PF_CHECK(Tests, CallbackReturned.load());
		PF_CHECK(Tests, Active->Poll().Status == AssetPreparationStatus::Cancelled);
	}
} // namespace

void RunAssetPreparationServiceTests(TestRunner& Tests)
{
	TestEquivalentSubscribersAndIndependentCancellation(Tests);
	TestFinalCancellationSuppressesLateResult(Tests);
	TestStopCallbackCanQueryStatsDuringCancellation(Tests);
	TestCallerCanAdvanceWhilePreparationIsBlocked(Tests);
	TestQueuedPriorityPromotionDoesNotDuplicateWork(Tests);
	TestExplicitFailureRetryAndExceptionRecovery(Tests);
	TestAdmissionAndCompletedPayloadBounds(Tests);
	TestShutdownJoinsAndSuppressesLatePublication(Tests);
	TestStopCallbackCanQueryStatsDuringShutdown(Tests);
}
