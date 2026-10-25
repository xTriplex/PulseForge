#include "Assets/AssetPreparationService.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace PulseForge::Detail
{
	struct AssetPreparationSubscriber
	{
		uint64_t ID = 0;
		AssetPreparationStatus Status = AssetPreparationStatus::Queued;
		std::shared_ptr<const AssetPreparationPayload> Payload;
		std::shared_ptr<const AssetPreparationError> Error;
	};

	struct AssetPreparationJob
	{
		AssetPreparationRequestIdentity Identity;
		AssetPreparationPriority Priority = AssetPreparationPriority::Background;
		size_t EstimatedPeakWorkingBytes = 0;
		uint64_t Sequence = 0;
		AssetPreparationStatus Status = AssetPreparationStatus::Queued;
		AssetPreparationService::Processor Process;
		std::stop_source StopSource;
		std::unordered_map<uint64_t, std::shared_ptr<AssetPreparationSubscriber>> Subscribers;
		std::shared_ptr<const AssetPreparationPayload> Payload;
		std::shared_ptr<const AssetPreparationError> Error;
		bool IsActive = false;
		bool CompletedBytesCounted = false;
	};

	struct AssetPreparationIdentityHash
	{
		size_t operator()(const AssetPreparationRequestIdentity& Identity) const noexcept
		{
			const size_t SessionHash = std::hash<std::string>{}(Identity.SessionKey);
			const size_t RecipeHash = std::hash<std::string>{}(Identity.RecipeKey);
			return SessionHash ^ (RecipeHash + static_cast<size_t>(0x9e3779b9u) + (SessionHash << 6) + (SessionHash >> 2));
		}
	};

	struct AssetPreparationServiceState
	{
		explicit AssetPreparationServiceState(AssetPreparationServiceLimits ServiceLimits)
			: Limits(ServiceLimits)
		{
		}

		std::mutex Mutex;
		std::condition_variable_any Changed;
		AssetPreparationServiceLimits Limits;
		std::unordered_map<AssetPreparationRequestIdentity, std::shared_ptr<AssetPreparationJob>,
			AssetPreparationIdentityHash> Jobs;
		std::vector<std::shared_ptr<AssetPreparationJob>> Queue;
		size_t ActiveJobs = 0;
		size_t SubscriberCount = 0;
		size_t ReservedWorkingBytes = 0;
		size_t CompletedPayloadBytes = 0;
		AssetPreparationServiceStats HighWater;
		uint64_t NextSubscriberID = 1;
		uint64_t NextSequence = 1;
		size_t DispatchesSinceFairness = 0;
		bool Accepting = true;
	};

	namespace
	{
		uint8_t PriorityValue(AssetPreparationPriority Priority) noexcept
		{
			return static_cast<uint8_t>(Priority);
		}

		void RecordHighWater(AssetPreparationServiceState& State) noexcept
		{
			const size_t CpuReadyJobs = static_cast<size_t>(std::count_if(State.Jobs.begin(), State.Jobs.end(),
				[](const auto& Entry) { return Entry.second->Status == AssetPreparationStatus::CpuReady; }));
			State.HighWater.HighWaterQueuedJobs = (std::max)(State.HighWater.HighWaterQueuedJobs, State.Queue.size());
			State.HighWater.HighWaterActiveJobs = (std::max)(State.HighWater.HighWaterActiveJobs, State.ActiveJobs);
			State.HighWater.HighWaterCpuReadyJobs = (std::max)(State.HighWater.HighWaterCpuReadyJobs, CpuReadyJobs);
			State.HighWater.HighWaterOutstandingJobs = (std::max)(State.HighWater.HighWaterOutstandingJobs,
				State.Queue.size() + State.ActiveJobs);
			State.HighWater.HighWaterSubscribers = (std::max)(State.HighWater.HighWaterSubscribers, State.SubscriberCount);
			State.HighWater.HighWaterReservedEstimatedWorkingBytes = (std::max)(
				State.HighWater.HighWaterReservedEstimatedWorkingBytes, State.ReservedWorkingBytes);
			State.HighWater.HighWaterCompletedRetainedPayloadBytes = (std::max)(
				State.HighWater.HighWaterCompletedRetainedPayloadBytes, State.CompletedPayloadBytes);
		}

		void EraseQueuedJob(AssetPreparationServiceState& State, const std::shared_ptr<AssetPreparationJob>& Job)
		{
			std::erase(State.Queue, Job);
		}

		void EraseJobMapEntry(
			AssetPreparationServiceState& State,
			const std::shared_ptr<AssetPreparationJob>& Job)
		{
			const auto Found = State.Jobs.find(Job->Identity);
			if (Found != State.Jobs.end() && Found->second == Job)
				State.Jobs.erase(Found);
		}

		void SetSubscriberStatus(
			const std::shared_ptr<AssetPreparationJob>& Job,
			AssetPreparationStatus Status)
		{
			for (auto& [ID, Subscriber] : Job->Subscribers)
			{
				(void)ID;
				Subscriber->Status = Status;
				Subscriber->Payload = Status == AssetPreparationStatus::CpuReady ? Job->Payload : nullptr;
				Subscriber->Error = Status == AssetPreparationStatus::Failed ? Job->Error : nullptr;
			}
		}

		void TrySetProcessingError(const std::shared_ptr<AssetPreparationJob>& Job, AssetPreparationError&& Error) noexcept
		{
			try
			{
				Job->Error = std::make_shared<const AssetPreparationError>(std::move(Error));
			}
			catch (...)
			{
				// The terminal state remains observable even if diagnostic allocation itself fails.
				Job->Error.reset();
			}
		}

		void TrySetProcessingError(
			const std::shared_ptr<AssetPreparationJob>& Job,
			AssetPreparationErrorCode Code,
			const char* Message) noexcept
		{
			try
			{
				TrySetProcessingError(Job, AssetPreparationError{ Code, Message });
			}
			catch (...)
			{
				Job->Error.reset();
			}
		}

		void FinishJobLocked(
			AssetPreparationServiceState& State,
			const std::shared_ptr<AssetPreparationJob>& Job,
			AssetPreparationStatus Status)
		{
			Job->Status = Status;
			Job->IsActive = false;
			if (State.ActiveJobs > 0)
				--State.ActiveJobs;
			State.ReservedWorkingBytes -= (std::min)(State.ReservedWorkingBytes, Job->EstimatedPeakWorkingBytes);
			SetSubscriberStatus(Job, Status);
			State.Changed.notify_all();
		}

		std::shared_ptr<AssetPreparationJob> TakeNextJob(AssetPreparationServiceState& State)
		{
			if (State.Queue.empty())
				return {};
			auto Selected = State.Queue.begin();
			if (State.DispatchesSinceFairness >= 8)
			{
				Selected = std::min_element(State.Queue.begin(), State.Queue.end(),
					[](const auto& First, const auto& Second) { return First->Sequence < Second->Sequence; });
				State.DispatchesSinceFairness = 0;
			}
			else
			{
				Selected = std::max_element(State.Queue.begin(), State.Queue.end(),
					[](const auto& First, const auto& Second)
					{
						if (First->Priority != Second->Priority)
							return PriorityValue(First->Priority) < PriorityValue(Second->Priority);
						return First->Sequence > Second->Sequence;
					});
				++State.DispatchesSinceFairness;
			}
			auto Job = std::move(*Selected);
			State.Queue.erase(Selected);
			Job->Status = AssetPreparationStatus::Processing;
			Job->IsActive = true;
			++State.ActiveJobs;
			RecordHighWater(State);
			for (auto& [ID, Subscriber] : Job->Subscribers)
			{
				(void)ID;
				Subscriber->Status = AssetPreparationStatus::Processing;
			}
			return Job;
		}

		void WorkerMain(
			const std::shared_ptr<AssetPreparationServiceState>& State,
			std::stop_token WorkerStopToken) noexcept
		{
			while (!WorkerStopToken.stop_requested())
			{
				std::shared_ptr<AssetPreparationJob> Job;
				{
					std::unique_lock Lock(State->Mutex);
					State->Changed.wait(Lock, WorkerStopToken,
						[&] { return !State->Accepting || !State->Queue.empty(); });
					if (WorkerStopToken.stop_requested() || !State->Accepting)
						return;
					Job = TakeNextJob(*State);
				}

				std::optional<std::expected<AssetPreparationService::CpuPayload, AssetPreparationError>> Processed;
				std::shared_ptr<const AssetPreparationError> ExceptionError;
				bool ProcessorThrew = false;
				if (Job->StopSource.stop_requested())
				{
					std::lock_guard Lock(State->Mutex);
					Job->Status = AssetPreparationStatus::Cancelled;
					Job->IsActive = false;
					if (State->ActiveJobs > 0)
						--State->ActiveJobs;
					State->ReservedWorkingBytes -= (std::min)(State->ReservedWorkingBytes,
						Job->EstimatedPeakWorkingBytes);
					EraseJobMapEntry(*State, Job);
					State->Changed.notify_all();
					continue;
				}
				try
				{
					Processed.emplace(Job->Process(Job->StopSource.get_token()));
				}
				catch (const std::exception& Exception)
				{
					ProcessorThrew = true;
					try
					{
						ExceptionError = std::make_shared<const AssetPreparationError>(AssetPreparationError{
							AssetPreparationErrorCode::ProcessingFailed, Exception.what() });
					}
					catch (...) { }
				}
				catch (...)
				{
					ProcessorThrew = true;
					try
					{
						ExceptionError = std::make_shared<const AssetPreparationError>(AssetPreparationError{
							AssetPreparationErrorCode::ProcessingFailed, "Asset preparation threw an unknown exception" });
					}
					catch (...) { }
				}

				std::unique_lock Lock(State->Mutex);
				if (!State->Accepting || Job->StopSource.stop_requested() || Job->Subscribers.empty())
				{
					Job->Status = AssetPreparationStatus::Cancelled;
					Job->IsActive = false;
					if (State->ActiveJobs > 0)
						--State->ActiveJobs;
					State->ReservedWorkingBytes -= (std::min)(State->ReservedWorkingBytes, Job->EstimatedPeakWorkingBytes);
					EraseJobMapEntry(*State, Job);
					State->Changed.notify_all();
					continue;
				}

				if (ProcessorThrew)
				{
					Job->Error = std::move(ExceptionError);
					FinishJobLocked(*State, Job, AssetPreparationStatus::Failed);
					continue;
				}
				if (!Processed)
				{
					TrySetProcessingError(Job, AssetPreparationErrorCode::ProcessingFailed,
						"Asset preparation produced no terminal result");
					FinishJobLocked(*State, Job, AssetPreparationStatus::Failed);
					continue;
				}
				if (!*Processed)
				{
					TrySetProcessingError(Job, std::move(Processed->error()));
					FinishJobLocked(*State, Job, AssetPreparationStatus::Failed);
					continue;
				}
				if (!**Processed)
				{
					TrySetProcessingError(Job, AssetPreparationErrorCode::ProcessingFailed,
						"Asset preparation returned a null CPU payload");
					FinishJobLocked(*State, Job, AssetPreparationStatus::Failed);
					continue;
				}

				const size_t PayloadBytes = (**Processed)->GetRetainedBytes();
				if (PayloadBytes > State->Limits.MaximumCompletedPayloadBytes)
				{
					TrySetProcessingError(Job, AssetPreparationErrorCode::ResultLimit,
						"CPU payload exceeds the completed-payload byte limit");
					FinishJobLocked(*State, Job, AssetPreparationStatus::Failed);
					continue;
				}

				State->Changed.wait(Lock, WorkerStopToken,
					[&]
					{
						return !State->Accepting || WorkerStopToken.stop_requested() || Job->StopSource.stop_requested() ||
							Job->Subscribers.empty() || State->CompletedPayloadBytes <=
								State->Limits.MaximumCompletedPayloadBytes - PayloadBytes;
					});
				if (!State->Accepting || WorkerStopToken.stop_requested() || Job->StopSource.stop_requested() ||
					Job->Subscribers.empty())
				{
					Job->Status = AssetPreparationStatus::Cancelled;
					Job->IsActive = false;
					if (State->ActiveJobs > 0)
						--State->ActiveJobs;
					State->ReservedWorkingBytes -= (std::min)(State->ReservedWorkingBytes, Job->EstimatedPeakWorkingBytes);
					EraseJobMapEntry(*State, Job);
					State->Changed.notify_all();
					continue;
				}

				Job->Payload = std::move(**Processed);
				Job->CompletedBytesCounted = true;
				State->CompletedPayloadBytes += PayloadBytes;
				FinishJobLocked(*State, Job, AssetPreparationStatus::CpuReady);
				RecordHighWater(*State);
			}
		}
	}

	void CancelSubscription(
		const std::shared_ptr<AssetPreparationServiceState>& State,
		const std::shared_ptr<AssetPreparationSubscriber>& Subscriber) noexcept
	{
		if (!State || !Subscriber)
			return;
		std::shared_ptr<AssetPreparationJob> RetiredJob;
		std::shared_ptr<const AssetPreparationPayload> RetiredSubscriberPayload;
		std::shared_ptr<const AssetPreparationError> RetiredSubscriberError;
		std::shared_ptr<const AssetPreparationPayload> RetiredJobPayload;
		std::shared_ptr<const AssetPreparationError> RetiredJobError;
		bool RequestStop = false;
		try
		{
			{
				std::lock_guard Lock(State->Mutex);
				if (Subscriber->Status == AssetPreparationStatus::Cancelled)
					return;
				Subscriber->Status = AssetPreparationStatus::Cancelled;
				RetiredSubscriberPayload = std::move(Subscriber->Payload);
				RetiredSubscriberError = std::move(Subscriber->Error);
				if (State->SubscriberCount > 0)
					--State->SubscriberCount;

				for (auto& [Identity, Job] : State->Jobs)
				{
					(void)Identity;
					const auto Found = Job->Subscribers.find(Subscriber->ID);
					if (Found == Job->Subscribers.end())
						continue;
					RetiredJob = Job;
					Job->Subscribers.erase(Found);
					if (!Job->Subscribers.empty())
						break;

					if (Job->Status == AssetPreparationStatus::Queued)
					{
						EraseQueuedJob(*State, Job);
						State->ReservedWorkingBytes -= (std::min)(State->ReservedWorkingBytes,
							Job->EstimatedPeakWorkingBytes);
						Job->Status = AssetPreparationStatus::Cancelled;
					}
					else if (Job->IsActive)
					{
						RequestStop = true;
					}
					if (Job->CompletedBytesCounted && Job->Payload)
					{
						State->CompletedPayloadBytes -= (std::min)(State->CompletedPayloadBytes,
							Job->Payload->GetRetainedBytes());
						Job->CompletedBytesCounted = false;
					}
					RetiredJobPayload = std::move(Job->Payload);
					RetiredJobError = std::move(Job->Error);
					EraseJobMapEntry(*State, Job);
					break;
				}
				State->Changed.notify_all();
			}

			// Stop callbacks are user supplied and may call back into the service. Keep the job alive,
			// but never invoke them while holding the state mutex.
			if (RequestStop && RetiredJob)
				RetiredJob->StopSource.request_stop();
		}
		catch (...)
		{
			// Destruction is non-throwing; shutdown still cancels subscriptions if synchronization fails here.
		}
	}
} // namespace PulseForge::Detail

namespace PulseForge
{
	AssetPreparationService::Subscription::Subscription(
		std::shared_ptr<Detail::AssetPreparationServiceState> State,
		std::shared_ptr<Detail::AssetPreparationSubscriber> Subscriber)
		: m_State(std::move(State)), m_Subscriber(std::move(Subscriber))
	{
	}

	AssetPreparationService::Subscription::Subscription(Subscription&& Other) noexcept
		: m_State(std::move(Other.m_State)), m_Subscriber(std::move(Other.m_Subscriber))
	{
	}

	AssetPreparationService::Subscription& AssetPreparationService::Subscription::operator=(Subscription&& Other) noexcept
	{
		if (this != &Other)
		{
			Cancel();
			m_State = std::move(Other.m_State);
			m_Subscriber = std::move(Other.m_Subscriber);
		}
		return *this;
	}

	AssetPreparationService::Subscription::~Subscription()
	{
		Cancel();
	}

	AssetPreparationSnapshot AssetPreparationService::Subscription::Poll() const
	{
		if (!m_State || !m_Subscriber)
			return {};
		std::lock_guard Lock(m_State->Mutex);
		return { m_Subscriber->Status, m_Subscriber->Payload, m_Subscriber->Error };
	}

	void AssetPreparationService::Subscription::Cancel() noexcept
	{
		Detail::CancelSubscription(m_State, m_Subscriber);
		m_Subscriber.reset();
		m_State.reset();
	}

	AssetPreparationService::AssetPreparationService(AssetPreparationServiceLimits Limits)
		: m_State(std::make_shared<Detail::AssetPreparationServiceState>(Limits))
	{
		m_Workers.reserve(Limits.MaximumWorkers);
		for (size_t Index = 0; Index < Limits.MaximumWorkers; ++Index)
		{
			(void)Index;
			m_Workers.emplace_back([State = m_State](std::stop_token StopToken)
				{ Detail::WorkerMain(State, StopToken); });
		}
	}

	AssetPreparationService::~AssetPreparationService()
	{
		Shutdown();
	}

	std::expected<AssetPreparationService::Subscription, AssetPreparationError> AssetPreparationService::Request(
		AssetPreparationRequestIdentity Identity,
		AssetPreparationPriority Priority,
		size_t EstimatedPeakWorkingBytes,
		Processor Process)
	{
		if (Identity.SessionKey.empty() || Identity.RecipeKey.empty() || !Process)
			return std::unexpected(AssetPreparationError{
				AssetPreparationErrorCode::InvalidRequest, "Session key, recipe key and processor are required" });
		std::lock_guard Lock(m_State->Mutex);
		if (!m_State->Accepting)
			return std::unexpected(AssetPreparationError{
				AssetPreparationErrorCode::ServiceStopping, "Asset preparation service is stopping" });
		if (m_Workers.empty())
			return std::unexpected(AssetPreparationError{
				AssetPreparationErrorCode::NoWorkers, "Asset preparation service has no workers" });
		if (m_State->SubscriberCount >= m_State->Limits.MaximumSubscribers)
			return std::unexpected(AssetPreparationError{
				AssetPreparationErrorCode::SubscriberLimit, "Asset preparation subscriber limit is full" });

		try
		{
			auto Found = m_State->Jobs.find(Identity);
			std::shared_ptr<Detail::AssetPreparationJob> Job;
			auto Subscriber = std::make_shared<Detail::AssetPreparationSubscriber>();
			Subscriber->ID = m_State->NextSubscriberID;
			if (Found != m_State->Jobs.end())
			{
				Job = Found->second;
				if (Job->Status == AssetPreparationStatus::Queued &&
					static_cast<uint8_t>(Priority) > static_cast<uint8_t>(Job->Priority))
					Job->Priority = Priority;
				Subscriber->Status = Job->Status;
				Subscriber->Payload = Job->Payload;
				Subscriber->Error = Job->Error;
				Job->Subscribers.emplace(Subscriber->ID, Subscriber);
			}
			else
			{
				if (m_State->Queue.size() + m_State->ActiveJobs >= m_State->Limits.MaximumOutstandingJobs)
					return std::unexpected(AssetPreparationError{
						AssetPreparationErrorCode::QueueFull, "Asset preparation queued and active job limit is full" });
				if (EstimatedPeakWorkingBytes > m_State->Limits.MaximumReservedWorkingBytes ||
					m_State->ReservedWorkingBytes > m_State->Limits.MaximumReservedWorkingBytes - EstimatedPeakWorkingBytes)
					return std::unexpected(AssetPreparationError{
						AssetPreparationErrorCode::WorkingSetLimit, "Asset preparation working-set reservation limit is full" });

				Job = std::make_shared<Detail::AssetPreparationJob>();
				Job->Identity = std::move(Identity);
				Job->Priority = Priority;
				Job->EstimatedPeakWorkingBytes = EstimatedPeakWorkingBytes;
				Job->Sequence = m_State->NextSequence;
				Job->Status = AssetPreparationStatus::Queued;
				Job->Process = std::move(Process);
				Subscriber->Status = AssetPreparationStatus::Queued;
				Job->Subscribers.emplace(Subscriber->ID, Subscriber);

				auto [InsertedAt, Inserted] = m_State->Jobs.emplace(Job->Identity, Job);
				if (!Inserted)
				{
					// The service mutex should make this unreachable, but preserve deduplication if the key exists.
					return std::unexpected(AssetPreparationError{
						AssetPreparationErrorCode::InvalidRequest, "Asset preparation identity changed during admission" });
				}
				try
				{
					m_State->Queue.push_back(Job);
				}
				catch (...)
				{
					m_State->Jobs.erase(InsertedAt);
					throw;
				}
				m_State->ReservedWorkingBytes += EstimatedPeakWorkingBytes;
				++m_State->NextSequence;
			}

			++m_State->NextSubscriberID;
			++m_State->SubscriberCount;
			Detail::RecordHighWater(*m_State);
			m_State->Changed.notify_all();
			return Subscription(m_State, std::move(Subscriber));
		}
		catch (...)
		{
			return std::unexpected(AssetPreparationError{
				AssetPreparationErrorCode::AllocationFailed, "Could not allocate an asset preparation request" });
		}
	}

	AssetPreparationServiceStats AssetPreparationService::GetStats() const
	{
		if (!m_State)
			return {};
		std::lock_guard Lock(m_State->Mutex);
		AssetPreparationServiceStats Stats = m_State->HighWater;
		Stats.QueuedJobs = m_State->Queue.size();
		Stats.ActiveJobs = m_State->ActiveJobs;
		Stats.CpuReadyJobs = static_cast<size_t>(std::count_if(m_State->Jobs.begin(), m_State->Jobs.end(),
			[](const auto& Entry) { return Entry.second->Status == AssetPreparationStatus::CpuReady; }));
		Stats.Subscribers = m_State->SubscriberCount;
		Stats.ReservedEstimatedWorkingBytes = m_State->ReservedWorkingBytes;
		Stats.CompletedRetainedPayloadBytes = m_State->CompletedPayloadBytes;
		return Stats;
	}

	void AssetPreparationService::Shutdown() noexcept
	{
		if (!m_State)
			return;
		std::unordered_map<AssetPreparationRequestIdentity, std::shared_ptr<Detail::AssetPreparationJob>,
			Detail::AssetPreparationIdentityHash> RetiredJobs;
		std::vector<std::shared_ptr<Detail::AssetPreparationJob>> RetiredQueue;
		{
			std::lock_guard Lock(m_State->Mutex);
			if (m_State->Accepting)
			{
				m_State->Accepting = false;
				for (auto& [Identity, Job] : m_State->Jobs)
				{
					(void)Identity;
					Job->Status = AssetPreparationStatus::Cancelled;
					if (Job->CompletedBytesCounted)
						Job->CompletedBytesCounted = false;
					for (auto& [ID, Subscriber] : Job->Subscribers)
					{
						(void)ID;
						Subscriber->Status = AssetPreparationStatus::Cancelled;
					}
				}
				for (const auto& Job : m_State->Queue)
					m_State->ReservedWorkingBytes -= (std::min)(m_State->ReservedWorkingBytes,
						Job->EstimatedPeakWorkingBytes);
				RetiredJobs.swap(m_State->Jobs);
				RetiredQueue.swap(m_State->Queue);
				m_State->SubscriberCount = 0;
				m_State->CompletedPayloadBytes = 0;
			}
		}
		// Stop callbacks are user code and may query this service. Job and processor-closure
		// destruction is also deferred until after the mutex is released.
		for (const auto& [Identity, Job] : RetiredJobs)
		{
			(void)Identity;
			Job->StopSource.request_stop();
		}
		m_State->Changed.notify_all();
		for (std::jthread& Worker : m_Workers)
			Worker.request_stop();
		m_State->Changed.notify_all();
		for (std::jthread& Worker : m_Workers)
			if (Worker.joinable())
				Worker.join();
		m_Workers.clear();
		if (!RetiredJobs.empty())
		{
			std::lock_guard Lock(m_State->Mutex);
			for (const auto& [Identity, Job] : RetiredJobs)
			{
				(void)Identity;
				for (const auto& [ID, Subscriber] : Job->Subscribers)
				{
					(void)ID;
					// RetiredJobs still owns the job's matching result, so these resets cannot destroy
					// a payload/error object while the service mutex is held.
					Subscriber->Payload.reset();
					Subscriber->Error.reset();
				}
			}
		}
	}
} // namespace PulseForge
