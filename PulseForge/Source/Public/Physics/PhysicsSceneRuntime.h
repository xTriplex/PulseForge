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
		[[nodiscard]] std::expected<void, PhysicsSceneRuntimeError> ApplyForce(UUID Entity, const glm::vec3& Force);
		// Returns the number of fixed simulation steps advanced. Excess time beyond
		// MaxSubsteps is dropped to keep catch-up work bounded after a stall.
		[[nodiscard]] std::expected<uint32_t, PhysicsSceneRuntimeError> Advance(Scene& Source, Timestep FrameDelta);

	private:
		struct Impl;
		PhysicsSceneRuntimeDesc m_Description;
		std::unique_ptr<Impl> m_Impl;
	};
}
