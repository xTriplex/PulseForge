#pragma once

#include "Core/Core.h"
#include "Core/Timestep.h"
#include "Scene/Components/BoxColliderComponent.h"
#include "Scene/Entity.h"
#include "Scene/UUID.h"

#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	class Scene;

	struct PhysicsSceneRuntimeDesc
	{
		double FixedStepSeconds = 1.0 / 60.0;
		uint32_t MaxSubsteps = 8;
		double MaxFrameDeltaSeconds = 0.25;
		glm::vec3 Gravity{ 0.0f, -9.81f, 0.0f };
		// Jolt's live rigid-body pool. Exposed for bounded runtime configurations and tests.
		uint32_t MaxBodies = 16384;
		uint64_t SessionGeneration = 1;
	};

	struct PhysicsQueryFilter
	{
		uint32_t CategoryMask = 0xffffffffu;
		bool IncludeSensors = false;
		std::span<const UUID> ExcludedEntities;
	};

	struct PhysicsQueryHit
	{
		Entity Target;
		UUID EntityIdentifier;
		uint64_t SessionGeneration = 0;
		uint64_t BodyGeneration = 0;
		float Fraction = 0.0f;
		glm::vec3 Position{ 0.0f };
		glm::vec3 Normal{ 0.0f };
	};

	struct PhysicsOverlapResult
	{
		std::vector<PhysicsQueryHit> Hits;
		bool Truncated = false;
	};

	enum class PhysicsContactPhase : uint8_t
	{
		Begin,
		Stay,
		End
	};

	// Entity-pair events are sorted by canonical UUID pair. End means Jolt removed the pair or runtime
	// reconciliation invalidated it; it does not claim geometric separation or provide a solver impulse.
	// Jolt may remove pairs during sleep, so a later contact after wake can produce a new Begin.
	struct PhysicsContactEvent
	{
		PhysicsContactPhase Phase = PhysicsContactPhase::Begin;
		Entity First;
		Entity Second;
		UUID FirstIdentifier;
		UUID SecondIdentifier;
		uint64_t SessionGeneration = 0;
		uint64_t FirstBodyGeneration = 0;
		uint64_t SecondBodyGeneration = 0;
		bool IsSensorPair = false;
	};

	struct PhysicsSceneRuntimeStats
	{
		// Cumulative counters for the active runtime session.
		uint64_t FixedSteps = 0;
		uint64_t CappedFrames = 0;
		double DroppedSimulationSeconds = 0.0;
		uint64_t ContactQueueOverflows = 0;
	};

	enum class PhysicsSceneRuntimeErrorCode : uint8_t
	{
		InvalidSettings,
		AlreadyRunning,
		NotRunning,
		DifferentScene,
		InvalidDeltaTime,
		InvalidPhysicsEntity,
		UnsupportedHierarchy,
		MissingBody,
		StaticBody,
		InvalidForce,
		InitializationFailed,
		BodyCreationFailed,
		SimulationFailed,
		SceneSynchronizationFailed,
		InvalidQuery,
		WrongThread,
		ContactQueueOverflow
	};

	struct PhysicsSceneRuntimeError
	{
		PhysicsSceneRuntimeErrorCode Code;
		std::string Message;
	};

	class PULSEFORGE_API PhysicsSceneRuntime final
	{
	public:
		explicit PhysicsSceneRuntime(PhysicsSceneRuntimeDesc Description = {});
		~PhysicsSceneRuntime();

		PhysicsSceneRuntime(const PhysicsSceneRuntime&) = delete;
		PhysicsSceneRuntime& operator=(const PhysicsSceneRuntime&) = delete;

		[[nodiscard]] std::expected<void, PhysicsSceneRuntimeError> Start(Scene& Source);
		void Stop() noexcept;
		[[nodiscard]] bool IsRunning() const noexcept;
		// Synchronizes this entity's settings immediately and queues force for the next fixed step.
		[[nodiscard]] std::expected<void, PhysicsSceneRuntimeError> ApplyForce(UUID Entity, const glm::vec3& Force);
		// Reconciles the complete scene once before stepping. Excess time beyond MaxSubsteps is dropped.
		[[nodiscard]] std::expected<uint32_t, PhysicsSceneRuntimeError> Advance(Scene& Source, Timestep FrameDelta);
		// Queries use the last synchronized PhysicsSceneRuntime state. They never reconcile the scene.
		// Ray direction is normalized internally; MaxDistance and returned Position use world units.
		// The nearest hit wins, with UUID ordering breaking exact fraction ties. Jolt reports a start-inside hit at fraction zero.
		[[nodiscard]] std::expected<std::optional<PhysicsQueryHit>, PhysicsSceneRuntimeError> Raycast(
			const glm::vec3& Origin, const glm::vec3& Direction, float MaxDistance, PhysicsQueryFilter Filter = {}) const;
		// Results are owned, UUID-sorted and bounded to 64 entries. If truncated, which entities fit depends on Jolt traversal order.
		[[nodiscard]] std::expected<PhysicsOverlapResult, PhysicsSceneRuntimeError> OverlapShape(
			const BoxColliderComponent& Shape, const glm::vec3& Position, const glm::quat& Rotation,
			PhysicsQueryFilter Filter = {}, uint32_t MaxResults = 32) const;
		[[nodiscard]] std::expected<std::optional<PhysicsQueryHit>, PhysicsSceneRuntimeError> CastShape(
			const BoxColliderComponent& Shape, const glm::vec3& Position, const glm::quat& Rotation,
			const glm::vec3& Direction, float MaxDistance, PhysicsQueryFilter Filter = {}) const;
		// Owner-thread only. Call after Advance; events carry incarnation/session checked Entity values.
		[[nodiscard]] std::vector<PhysicsContactEvent> TakeContactEvents();
		[[nodiscard]] PhysicsSceneRuntimeStats GetStats() const noexcept;

	private:
		friend struct PhysicsSceneRuntimeTestAccess;
		[[nodiscard]] uint64_t GetFullSceneReconciliationCountForTesting() const noexcept;
		[[nodiscard]] bool IsBodyActiveForTesting(UUID Entity) const noexcept;
	#if defined(PF_DEBUG)
		[[nodiscard]] bool OverflowContactRemovalQueueForTesting(UUID First, UUID Second) noexcept;
	#endif

		struct Impl;
		PhysicsSceneRuntimeDesc m_Description;
		std::unique_ptr<Impl> m_Impl;
	};
}
