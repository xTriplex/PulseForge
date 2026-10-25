#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace Detail
	{
		struct AssetPreparationServiceState;
		struct AssetPreparationSubscriber;
	}

	enum class AssetPreparationPriority : uint8_t
	{
		Background,
		Normal,
		Interactive
	};

	enum class AssetPreparationStatus : uint8_t
	{
		Queued,
		Processing,
		CpuReady,
		Failed,
		Cancelled
	};

	enum class AssetPreparationErrorCode : uint8_t
	{
		InvalidRequest,
		ServiceStopping,
		NoWorkers,
		QueueFull,
		SubscriberLimit,
		WorkingSetLimit,
		ResultLimit,
		AllocationFailed,
		ProcessingFailed
	};

	struct AssetPreparationError
	{
		AssetPreparationErrorCode Code = AssetPreparationErrorCode::ProcessingFailed;
		std::string Message;
	};

	class PULSEFORGE_API AssetPreparationPayload
	{
	public:
		virtual ~AssetPreparationPayload() = default;
		[[nodiscard]] virtual size_t GetRetainedBytes() const noexcept = 0;
	};

	template <typename T>
	class AssetPreparationValue final : public AssetPreparationPayload
	{
	public:
		AssetPreparationValue(T Value, size_t RetainedBytes)
			: m_Value(std::move(Value)), m_RetainedBytes(RetainedBytes)
		{
		}

		[[nodiscard]] const T& Get() const noexcept { return m_Value; }
		[[nodiscard]] size_t GetRetainedBytes() const noexcept override { return m_RetainedBytes; }

	private:
		T m_Value;
		size_t m_RetainedBytes = 0;
	};

	template <typename T>
	[[nodiscard]] std::shared_ptr<const AssetPreparationPayload> MakeAssetPreparationValue(T Value, size_t RetainedBytes)
	{
		return std::make_shared<const AssetPreparationValue<T>>(std::move(Value), RetainedBytes);
	}

	struct AssetPreparationRequestIdentity
	{
		// Include the project root and session incarnation. Reopening the same project is a new session.
		std::string SessionKey;
		// This is the complete canonical processing recipe identity, not an asset filename or UUID alone.
		std::string RecipeKey;

		friend bool operator==(const AssetPreparationRequestIdentity&, const AssetPreparationRequestIdentity&) = default;
	};

	struct AssetPreparationServiceLimits
	{
		size_t MaximumWorkers = 2;
		size_t MaximumOutstandingJobs = 32;
		size_t MaximumSubscribers = 128;
		// Reservation covers the caller's estimated peak working memory from enqueue through result publication.
		size_t MaximumReservedWorkingBytes = 512ull * 1024ull * 1024ull;
		// This bounds CPU payloads retained by completed service jobs. Callers retaining a copied shared_ptr after
		// releasing their subscription own that additional lifetime and it is outside this service accounting.
		size_t MaximumCompletedPayloadBytes = 256ull * 1024ull * 1024ull;
	};

	struct AssetPreparationSnapshot
	{
		AssetPreparationStatus Status = AssetPreparationStatus::Cancelled;
		std::shared_ptr<const AssetPreparationPayload> CpuPayload;
		std::shared_ptr<const AssetPreparationError> Error;
	};

	// Point-in-time service accounting. Working-set values are caller estimates; completed bytes use payload
	// retained-byte reports. Neither metric represents allocator overhead, total process RSS or GPU memory.
	struct AssetPreparationServiceStats
	{
		size_t QueuedJobs = 0;
		size_t ActiveJobs = 0;
		size_t CpuReadyJobs = 0;
		size_t Subscribers = 0;
		size_t ReservedEstimatedWorkingBytes = 0;
		size_t CompletedRetainedPayloadBytes = 0;
		size_t HighWaterQueuedJobs = 0;
		size_t HighWaterActiveJobs = 0;
		size_t HighWaterCpuReadyJobs = 0;
		size_t HighWaterOutstandingJobs = 0;
		size_t HighWaterSubscribers = 0;
		size_t HighWaterReservedEstimatedWorkingBytes = 0;
		size_t HighWaterCompletedRetainedPayloadBytes = 0;
	};

	// CPU-only, bounded preparation executor. It does not perform renderer calls or publish GPU resources.
	class PULSEFORGE_API AssetPreparationService final
	{
	public:
		using CpuPayload = std::shared_ptr<const AssetPreparationPayload>;
		using Processor = std::function<std::expected<CpuPayload, AssetPreparationError>(std::stop_token)>;

		class Subscription final
		{
		public:
			Subscription() = default;
			Subscription(const Subscription&) = delete;
			Subscription& operator=(const Subscription&) = delete;
			Subscription(Subscription&& Other) noexcept;
			Subscription& operator=(Subscription&& Other) noexcept;
			~Subscription();

			[[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(m_Subscriber); }
			[[nodiscard]] AssetPreparationSnapshot Poll() const;
			void Cancel() noexcept;

		private:
			friend class AssetPreparationService;
			Subscription(
				std::shared_ptr<Detail::AssetPreparationServiceState> State,
				std::shared_ptr<Detail::AssetPreparationSubscriber> Subscriber);

			std::shared_ptr<Detail::AssetPreparationServiceState> m_State;
			std::shared_ptr<Detail::AssetPreparationSubscriber> m_Subscriber;
		};

		explicit AssetPreparationService(AssetPreparationServiceLimits Limits = {});
		AssetPreparationService(const AssetPreparationService&) = delete;
		AssetPreparationService& operator=(const AssetPreparationService&) = delete;
		~AssetPreparationService();

		// Saturated bounds are reported immediately; Request never waits for queue or memory capacity. Equivalent
		// live identities share one processor. Attaching at a higher priority promotes queued work. A failed job is
		// not retried while any subscription still retains that terminal result; release them before retrying.
		[[nodiscard]] std::expected<Subscription, AssetPreparationError> Request(
			AssetPreparationRequestIdentity Identity,
			AssetPreparationPriority Priority,
			size_t EstimatedPeakWorkingBytes,
			Processor Process);

		// Returns a mutex-consistent point-in-time snapshot. Working memory is reserved from caller estimates;
		// completed bytes cover payloads retained by service jobs and exclude allocator/object overhead.
		[[nodiscard]] AssetPreparationServiceStats GetStats() const;

		// Stops admission, requests cooperative cancellation, wakes workers, then joins them without holding the
		// service lock. A decoder that cannot observe its token may finish its current bounded operation first.
		void Shutdown() noexcept;

	private:
		std::shared_ptr<Detail::AssetPreparationServiceState> m_State;
		std::vector<std::jthread> m_Workers;
	};
} // namespace PulseForge
