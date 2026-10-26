#include "Assets/AssetPreparationService.h"
#include "TestSupport.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
	using namespace PulseForge;
	using namespace std::chrono_literals;

	class MetricsGate final
	{
	public:
		~MetricsGate() { Release(); }

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
			return m_Changed.wait_for(Lock, 5s, [this] { return m_Entered; });
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

	bool WaitForCpuReady(const AssetPreparationService::Subscription& Subscription)
	{
		const auto Deadline = std::chrono::steady_clock::now() + 5s;
		while (std::chrono::steady_clock::now() < Deadline)
		{
			if (Subscription.Poll().Status == AssetPreparationStatus::CpuReady)
				return true;
			std::this_thread::yield();
		}
		return Subscription.Poll().Status == AssetPreparationStatus::CpuReady;
	}

	AssetPreparationService::Processor MakePayloadProcessor(size_t ByteCount)
	{
		return [ByteCount](std::stop_token)
		{
			auto Payload = MakeAssetPreparationValue(std::vector<std::byte>(ByteCount), ByteCount);
			return std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>(std::move(Payload));
		};
	}

	void TestPreparationServiceStats(TestRunner& Tests)
	{
		AssetPreparationService Service({ .MaximumWorkers = 1, .MaximumOutstandingJobs = 4,
			.MaximumSubscribers = 8, .MaximumReservedWorkingBytes = 64, .MaximumCompletedPayloadBytes = 64 });
		MetricsGate Gate;
		auto Active = Service.Request({ "stats-session", "active" }, AssetPreparationPriority::Normal, 7,
			[&](std::stop_token)
			{
				Gate.EnterAndWait();
				auto Payload = MakeAssetPreparationValue(std::vector<std::byte>(5), 5);
				return std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>(std::move(Payload));
			});
		PF_CHECK(Tests, Active.has_value());
		if (!Active) return;
		PF_CHECK(Tests, Gate.WaitUntilEntered());

		auto Shared = Service.Request({ "stats-session", "active" }, AssetPreparationPriority::Interactive, 7,
			MakePayloadProcessor(99));
		auto Queued = Service.Request({ "stats-session", "queued" }, AssetPreparationPriority::Background, 11,
			MakePayloadProcessor(7));
		PF_CHECK(Tests, Shared.has_value() && Queued.has_value());
		if (!Shared || !Queued) return;

		const AssetPreparationServiceStats DuringWork = Service.GetStats();
		PF_CHECK(Tests, DuringWork.QueuedJobs == 1);
		PF_CHECK(Tests, DuringWork.ActiveJobs == 1);
		PF_CHECK(Tests, DuringWork.Subscribers == 3);
		PF_CHECK(Tests, DuringWork.ReservedEstimatedWorkingBytes == 18);
		PF_CHECK(Tests, DuringWork.CompletedRetainedPayloadBytes == 0);
		PF_CHECK(Tests, DuringWork.HighWaterQueuedJobs >= 1);
		PF_CHECK(Tests, DuringWork.HighWaterActiveJobs >= 1);
		PF_CHECK(Tests, DuringWork.HighWaterOutstandingJobs >= 2);
		PF_CHECK(Tests, DuringWork.HighWaterSubscribers >= 3);
		PF_CHECK(Tests, DuringWork.HighWaterReservedEstimatedWorkingBytes >= 18);

		Gate.Release();
		PF_CHECK(Tests, WaitForCpuReady(*Active));
		PF_CHECK(Tests, WaitForCpuReady(*Shared));
		PF_CHECK(Tests, WaitForCpuReady(*Queued));
		const AssetPreparationServiceStats Completed = Service.GetStats();
		PF_CHECK(Tests, Completed.QueuedJobs == 0 && Completed.ActiveJobs == 0);
		PF_CHECK(Tests, Completed.CpuReadyJobs == 2);
		PF_CHECK(Tests, Completed.Subscribers == 3 && Completed.ReservedEstimatedWorkingBytes == 0);
		PF_CHECK(Tests, Completed.CompletedRetainedPayloadBytes == 12);
		PF_CHECK(Tests, Completed.HighWaterCompletedRetainedPayloadBytes >= 12);
		PF_CHECK(Tests, Completed.HighWaterCpuReadyJobs >= 2);

		Active->Cancel();
		Shared->Cancel();
		Queued->Cancel();
		const AssetPreparationServiceStats Released = Service.GetStats();
		PF_CHECK(Tests, Released.Subscribers == 0);
		PF_CHECK(Tests, Released.CompletedRetainedPayloadBytes == 0);
		Service.Shutdown();
	}
} // namespace

void RunAssetPreparationMetricsTests(TestRunner& Tests)
{
	TestPreparationServiceStats(Tests);
}
