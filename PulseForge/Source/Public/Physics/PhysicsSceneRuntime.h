#pragma once

#include "Core/Core.h"
#include "Core/Timestep.h"
#include "Scene/UUID.h"

#include <expected>
#include <glm/glm.hpp>
#include <memory>
#include <string>

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
		SceneSynchronizationFailed
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

	private:
		friend struct PhysicsSceneRuntimeTestAccess;
		[[nodiscard]] uint64_t GetFullSceneReconciliationCountForTesting() const noexcept;

		struct Impl;
		PhysicsSceneRuntimeDesc m_Description;
		std::unique_ptr<Impl> m_Impl;
	};
}
