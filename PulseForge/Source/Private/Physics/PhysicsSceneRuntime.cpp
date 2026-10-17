#include "Core/Log.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Scene.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>

namespace PulseForge
{
	namespace
	{
		using JPH::BroadPhaseLayer;
		using JPH::ObjectLayer;

		constexpr ObjectLayer StaticObjectLayer = 0;
		constexpr ObjectLayer DynamicObjectLayer = 1;
		constexpr uint32_t BroadPhaseLayerCount = 2;

		struct JoltRuntimeState
		{
			std::mutex Mutex;
			size_t Users = 0;
		};

		JoltRuntimeState& GetJoltRuntimeState()
		{
			static JoltRuntimeState State;
			return State;
		}

		void JoltTrace(const char* Format, ...)
		{
			char Buffer[1024]{};
			va_list Arguments;
			va_start(Arguments, Format);
			std::vsnprintf(Buffer, sizeof(Buffer), Format, Arguments);
			va_end(Arguments);
			Log::CoreTrace("Jolt: {}", Buffer);
		}

		class JoltRuntimeLease final
		{
		public:
			JoltRuntimeLease()
			{
				JoltRuntimeState& State = GetJoltRuntimeState();
				std::scoped_lock Lock(State.Mutex);
				if (State.Users == 0)
				{
					if (JPH::Factory::sInstance)
						throw std::runtime_error("Jolt's global Factory is already owned by another integration");
					JPH::RegisterDefaultAllocator();
					JPH::Trace = JoltTrace;
					JPH::Factory::sInstance = new JPH::Factory();
					try
					{
						JPH::RegisterTypes();
					}
					catch (...)
					{
						JPH::UnregisterTypes();
						delete JPH::Factory::sInstance;
						JPH::Factory::sInstance = nullptr;
						JPH::Trace = nullptr;
						throw;
					}
				}
				++State.Users;
				m_Active = true;
			}

			~JoltRuntimeLease()
			{
				if (!m_Active)
					return;
				JoltRuntimeState& State = GetJoltRuntimeState();
				std::scoped_lock Lock(State.Mutex);
				if (--State.Users == 0)
				{
					JPH::UnregisterTypes();
					delete JPH::Factory::sInstance;
					JPH::Factory::sInstance = nullptr;
					JPH::Trace = nullptr;
				}
			}

		private:
			bool m_Active = false;
		};

		class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface
		{
		public:
			uint32_t GetNumBroadPhaseLayers() const override { return BroadPhaseLayerCount; }

			BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer Layer) const override
			{
				return Layer == StaticObjectLayer ? BroadPhaseLayer(0) : BroadPhaseLayer(1);
			}

		#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
			const char* GetBroadPhaseLayerName(BroadPhaseLayer Layer) const override
			{
				return static_cast<uint8_t>(Layer) == 0 ? "Static" : "Dynamic";
			}
		#endif
		};

		class ObjectVsBroadPhaseFilter final : public JPH::ObjectVsBroadPhaseLayerFilter
		{
		public:
			bool ShouldCollide(ObjectLayer Object, BroadPhaseLayer BroadPhase) const override
			{
				return Object == DynamicObjectLayer || static_cast<uint8_t>(BroadPhase) == 1;
			}
		};

		class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
		{
		public:
			bool ShouldCollide(ObjectLayer First, ObjectLayer Second) const override
			{
				return First == DynamicObjectLayer || Second == DynamicObjectLayer;
			}
		};

		int GetWorkerThreadCount()
		{
			const unsigned int HardwareThreads = std::max(1u, std::thread::hardware_concurrency());
			return static_cast<int>(std::min(HardwareThreads, 8u)) - 1;
		}

		PhysicsSceneRuntimeError MakeError(PhysicsSceneRuntimeErrorCode Code, std::string Message)
		{
			return { Code, std::move(Message) };
		}

		bool IsFinite(const glm::vec3& Value)
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z);
		}

		bool SameVector(const glm::vec3& First, const glm::vec3& Second)
		{
			return First.x == Second.x && First.y == Second.y && First.z == Second.z;
		}

		bool SameRotation(const glm::quat& First, const glm::quat& Second)
		{
			return First.x == Second.x && First.y == Second.y && First.z == Second.z && First.w == Second.w;
		}

		bool SameScenePose(const TransformComponent& First, const TransformComponent& Second)
		{
			return SameVector(First.Translation, Second.Translation) && SameRotation(First.Rotation, Second.Rotation);
		}

		JPH::Quat ToJolt(const glm::quat& Rotation)
		{
			return { Rotation.x, Rotation.y, Rotation.z, Rotation.w };
		}
	}

	struct PhysicsSceneRuntime::Impl final
	{
		struct BodyRecord
		{
			JPH::BodyID ID;
			Entity BoundEntity;
			RigidbodyComponent Rigidbody;
			BoxColliderComponent Collider;
			glm::vec3 EffectiveHalfExtents{ 0.0f };
			TransformComponent LastSceneTransform;
		};

		struct ValidatedBodySettings
		{
			TransformComponent Transform;
			glm::vec3 EffectiveHalfExtents{ 0.0f };
		};

		JoltRuntimeLease Runtime;
		BroadPhaseLayers BroadPhaseLayerInterface;
		ObjectVsBroadPhaseFilter ObjectVsBroadPhaseLayerFilter;
		ObjectPairFilter ObjectLayerPairFilter;
		JPH::TempAllocatorImplWithMallocFallback TemporaryAllocator{ 32u * 1024u * 1024u };
		JPH::JobSystemThreadPool JobSystem{ JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, GetWorkerThreadCount() };
		JPH::PhysicsSystem System;
		bool SystemInitialized = false;
		std::unordered_map<UUID, BodyRecord, UUIDHash> Bodies;
		Scene* SourceScene = nullptr;
		double AccumulatorSeconds = 0.0;

		~Impl()
		{
			ClearBodies();
		}

		void Initialize(const PhysicsSceneRuntimeDesc& Description)
		{
			System.Init(
				16384,
				0,
				32768,
				8192,
				BroadPhaseLayerInterface,
				ObjectVsBroadPhaseLayerFilter,
				ObjectLayerPairFilter);
			SystemInitialized = true;
			System.SetGravity(JPH::Vec3(Description.Gravity.x, Description.Gravity.y, Description.Gravity.z));
		}

		void ClearBodies() noexcept
		{
			if (SystemInitialized)
			{
				JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
				for (const auto& [Identifier, Body] : Bodies)
				{
					(void)Identifier;
					BodyInterface.RemoveBody(Body.ID);
					BodyInterface.DestroyBody(Body.ID);
				}
			}
			Bodies.clear();
			SourceScene = nullptr;
		}

		std::expected<ValidatedBodySettings, PhysicsSceneRuntimeError> ValidateBody(
			const Entity& Current,
			const RigidbodyComponent& Rigidbody,
			const BoxColliderComponent& Collider)
		{
			const auto Parent = Current.GetParent();
			const auto Transform = Current.GetTransform();
			if (!Parent || !Transform)
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Could not read hierarchy or transform for physics entity " + Current.GetUUID().ToString()));
			}
			if (Parent->has_value())
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::UnsupportedHierarchy,
					"Physics entities must be scene roots in this initial integration: " + Current.GetUUID().ToString()));
			}
			if (auto Validation = Rigidbody.Validate(); !Validation)
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity, Validation.error().Message));
			if (auto Validation = Collider.Validate(); !Validation)
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity, Validation.error().Message));

			const glm::vec3 Scale = glm::abs(Transform->Scale);
			if (!IsFinite(Transform->Translation) || !IsFinite(Scale) ||
				Scale.x <= 0.00001f || Scale.y <= 0.00001f || Scale.z <= 0.00001f)
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Physics entity transform must have finite translation and non-zero scale: " + Current.GetUUID().ToString()));
			}

			const glm::vec3 EffectiveHalfExtents = Collider.HalfExtents * Scale;
			if (!IsFinite(EffectiveHalfExtents) || EffectiveHalfExtents.x <= 0.0f ||
				EffectiveHalfExtents.y <= 0.0f || EffectiveHalfExtents.z <= 0.0f)
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Box collider half-extents must remain finite and positive after applying entity scale: " +
					Current.GetUUID().ToString()));
			}
			return ValidatedBodySettings{ *Transform, EffectiveHalfExtents };
		}

		std::expected<BodyRecord, PhysicsSceneRuntimeError> CreateBody(
			const Entity& Current,
			const RigidbodyComponent& Rigidbody,
			const BoxColliderComponent& Collider,
			const ValidatedBodySettings& ValidatedSettings)
		{
			JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
			const TransformComponent& Transform = ValidatedSettings.Transform;
			const glm::vec3& HalfExtents = ValidatedSettings.EffectiveHalfExtents;
			JPH::BoxShapeSettings ShapeSettings(
				JPH::Vec3(HalfExtents.x, HalfExtents.y, HalfExtents.z),
				0.0f);
			ShapeSettings.SetEmbedded();
			auto ShapeResult = ShapeSettings.Create();
			if (ShapeResult.HasError())
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Jolt could not create a box shape for entity " + Current.GetUUID().ToString() + ": " +
					std::string(ShapeResult.GetError().c_str())));
			}
			const JPH::Ref<JPH::Shape> Shape = ShapeResult.Get();
			const bool Dynamic = Rigidbody.MotionType == RigidbodyMotionType::Dynamic;
			JPH::BodyCreationSettings BodySettings(
				Shape.GetPtr(),
				JPH::RVec3(Transform.Translation.x, Transform.Translation.y, Transform.Translation.z),
				ToJolt(Transform.Rotation),
				Dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
				Dynamic ? DynamicObjectLayer : StaticObjectLayer);
			BodySettings.mFriction = Rigidbody.Friction;
			BodySettings.mRestitution = Rigidbody.Restitution;
			BodySettings.mAllowSleeping = Rigidbody.AllowSleeping;
			if (Dynamic)
			{
				BodySettings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
				BodySettings.mMassPropertiesOverride.mMass = Rigidbody.Mass;
			}

			const JPH::EActivation Activation = Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
			const JPH::BodyID Body = BodyInterface.CreateAndAddBody(BodySettings, Activation);
			if (Body.IsInvalid())
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Jolt could not allocate a body for entity " + Current.GetUUID().ToString()));
			}
			return BodyRecord{ Body, Current, Rigidbody, Collider, HalfExtents, Transform };
		}

		void RemoveBody(JPH::BodyID Body) noexcept
		{
			JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
			BodyInterface.RemoveBody(Body);
			BodyInterface.DestroyBody(Body);
		}

		std::expected<void, PhysicsSceneRuntimeError> AddBody(
			const Entity& Current,
			const RigidbodyComponent& Rigidbody,
			const BoxColliderComponent& Collider,
			const ValidatedBodySettings& Settings)
		{
			auto CreatedBody = CreateBody(Current, Rigidbody, Collider, Settings);
			if (!CreatedBody)
				return std::unexpected(CreatedBody.error());

			try
			{
				if (!Bodies.emplace(Current.GetUUID(), *CreatedBody).second)
				{
					RemoveBody(CreatedBody->ID);
					return std::unexpected(MakeError(
						PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
						"Physics body already exists for entity " + Current.GetUUID().ToString()));
				}
			}
			catch (const std::exception& Exception)
			{
				RemoveBody(CreatedBody->ID);
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Could not track physics body for entity " + Current.GetUUID().ToString() + ": " + Exception.what()));
			}
			catch (...)
			{
				RemoveBody(CreatedBody->ID);
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Could not track physics body for entity " + Current.GetUUID().ToString()));
			}
			return {};
		}

		std::expected<void, PhysicsSceneRuntimeError> ReconcileBodies(Scene& Source)
		{
			try
			{
				Bodies.reserve(Source.GetEntityCount());
				for (auto Iterator = Bodies.begin(); Iterator != Bodies.end();)
				{
					if (!Source.FindEntity(Iterator->first))
					{
						RemoveBody(Iterator->second.ID);
						Iterator = Bodies.erase(Iterator);
					}
					else
						++Iterator;
				}

				for (const Entity& Current : Source.GetEntities())
				{
					const auto Rigidbody = Current.GetRigidbody();
					const auto Collider = Current.GetBoxCollider();
					if (!Rigidbody || !Collider)
					{
						return std::unexpected(MakeError(
							PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
							"Could not inspect physics components for entity " + Current.GetUUID().ToString()));
					}

					const bool HasRigidbody = Rigidbody->has_value();
					const bool HasCollider = Collider->has_value();
					auto ExistingBody = Bodies.find(Current.GetUUID());
					if (!HasRigidbody && !HasCollider)
					{
						if (ExistingBody != Bodies.end())
						{
							RemoveBody(ExistingBody->second.ID);
							Bodies.erase(ExistingBody);
						}
						continue;
					}
					if (HasRigidbody != HasCollider)
					{
						return std::unexpected(MakeError(
							PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
							"Physics entities require both a rigidbody and box collider: " + Current.GetUUID().ToString()));
					}

					auto Settings = ValidateBody(Current, Rigidbody->value(), Collider->value());
					if (!Settings)
						return std::unexpected(Settings.error());

					if (ExistingBody == Bodies.end())
					{
						if (auto Result = AddBody(Current, Rigidbody->value(), Collider->value(), *Settings); !Result)
							return std::unexpected(Result.error());
						continue;
					}

					BodyRecord& Existing = ExistingBody->second;
					const bool SameIncarnation = Existing.BoundEntity == Current;
					const bool SameRigidbody =
						Existing.Rigidbody.MotionType == Rigidbody->value().MotionType &&
						Existing.Rigidbody.Mass == Rigidbody->value().Mass &&
						Existing.Rigidbody.Friction == Rigidbody->value().Friction &&
						Existing.Rigidbody.Restitution == Rigidbody->value().Restitution &&
						Existing.Rigidbody.AllowSleeping == Rigidbody->value().AllowSleeping;
					const bool SameCollider = SameVector(Existing.Collider.HalfExtents, Collider->value().HalfExtents);
					const bool SameShape = SameVector(Existing.EffectiveHalfExtents, Settings->EffectiveHalfExtents);
					const bool NeedsReplacement = !SameIncarnation || !SameRigidbody || !SameCollider || !SameShape;
					if (NeedsReplacement)
					{
						auto Replacement = CreateBody(Current, Rigidbody->value(), Collider->value(), *Settings);
						if (!Replacement)
							return std::unexpected(Replacement.error());

						// Same-incarnation dynamic edits preserve velocity; motion-type changes and UUID reuse reset it.
						if (SameIncarnation && Existing.Rigidbody.MotionType == RigidbodyMotionType::Dynamic &&
							Rigidbody->value().MotionType == RigidbodyMotionType::Dynamic)
						{
							JPH::Vec3 LinearVelocity;
							JPH::Vec3 AngularVelocity;
							JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
							BodyInterface.GetLinearAndAngularVelocity(Existing.ID, LinearVelocity, AngularVelocity);
							BodyInterface.SetLinearAndAngularVelocity(Replacement->ID, LinearVelocity, AngularVelocity);
						}

						const JPH::BodyID PreviousBody = Existing.ID;
						Existing = std::move(*Replacement);
						RemoveBody(PreviousBody);
						continue;
					}

					if (!SameScenePose(Settings->Transform, Existing.LastSceneTransform))
					{
						// Scene-authored teleports retain velocity; physics writeback refreshes the comparison baseline.
						// This keeps simulation motion from being replayed as a new teleport on the next frame.
						const bool Dynamic = Rigidbody->value().MotionType == RigidbodyMotionType::Dynamic;
						System.GetBodyInterface().SetPositionAndRotation(
							Existing.ID,
							JPH::RVec3(
								Settings->Transform.Translation.x,
								Settings->Transform.Translation.y,
								Settings->Transform.Translation.z),
							ToJolt(Settings->Transform.Rotation),
							Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
					}
					Existing.LastSceneTransform = Settings->Transform;
				}
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					std::string("Could not reconcile physics bodies: ") + Exception.what()));
			}
			catch (...)
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Could not reconcile physics bodies"));
			}
			return {};
		}

		std::expected<void, PhysicsSceneRuntimeError> CreateBodies(Scene& Source)
		{
			if (auto Result = ReconcileBodies(Source); !Result)
				return std::unexpected(Result.error());
			SourceScene = &Source;
			return {};
		}

		std::expected<void, PhysicsSceneRuntimeError> SynchronizeScene()
		{
			JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
			for (auto Iterator = Bodies.begin(); Iterator != Bodies.end();)
			{
				const auto EntityValue = SourceScene->FindEntity(Iterator->first);
				if (!EntityValue || *EntityValue != Iterator->second.BoundEntity)
					return std::unexpected(MakeError(
						PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
						"Physics entity incarnation changed before scene synchronization"));
				if (Iterator->second.Rigidbody.MotionType == RigidbodyMotionType::Dynamic)
				{
					auto Transform = EntityValue->GetTransform();
					if (!Transform)
						return std::unexpected(MakeError(
							PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
							Transform.error().Message));

					JPH::RVec3 Position;
					JPH::Quat Rotation;
					BodyInterface.GetPositionAndRotation(Iterator->second.ID, Position, Rotation);
					Transform->Translation = {
						static_cast<float>(Position.GetX()),
						static_cast<float>(Position.GetY()),
						static_cast<float>(Position.GetZ()) };
					Transform->Rotation = { Rotation.GetW(), Rotation.GetX(), Rotation.GetY(), Rotation.GetZ() };
					if (auto Result = EntityValue->SetTransform(*Transform); !Result)
						return std::unexpected(MakeError(
							PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
							Result.error().Message));
					auto SynchronizedTransform = EntityValue->GetTransform();
					if (!SynchronizedTransform)
						return std::unexpected(MakeError(
							PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
							SynchronizedTransform.error().Message));
					Iterator->second.LastSceneTransform = *SynchronizedTransform;
				}
				++Iterator;
			}
			return {};
		}
	};

	PhysicsSceneRuntime::PhysicsSceneRuntime(PhysicsSceneRuntimeDesc Description)
		: m_Description(Description)
	{
	}

	PhysicsSceneRuntime::~PhysicsSceneRuntime() = default;

	std::expected<void, PhysicsSceneRuntimeError> PhysicsSceneRuntime::Start(Scene& Source)
	{
		if (m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::AlreadyRunning, "Physics scene runtime is already started"));
		if (!std::isfinite(m_Description.FixedStepSeconds) || m_Description.FixedStepSeconds <= 0.0 ||
			!std::isfinite(m_Description.MaxFrameDeltaSeconds) ||
			m_Description.MaxFrameDeltaSeconds < m_Description.FixedStepSeconds ||
			m_Description.MaxSubsteps == 0 || !IsFinite(m_Description.Gravity))
		{
			return std::unexpected(MakeError(
				PhysicsSceneRuntimeErrorCode::InvalidSettings,
				"Physics fixed step, frame-delta cap, substep limit, or gravity is invalid"));
		}
		const float JoltFixedStep = static_cast<float>(m_Description.FixedStepSeconds);
		if (!std::isfinite(JoltFixedStep) || JoltFixedStep <= 0.0f)
		{
			return std::unexpected(MakeError(
				PhysicsSceneRuntimeErrorCode::InvalidSettings,
				"Physics fixed step must be representable as a positive Jolt timestep"));
		}

		try
		{
			auto Candidate = std::make_unique<Impl>();
			Candidate->Initialize(m_Description);
			if (auto Result = Candidate->CreateBodies(Source); !Result)
				return std::unexpected(Result.error());
			m_Impl = std::move(Candidate);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				PhysicsSceneRuntimeErrorCode::InitializationFailed,
				std::string("Could not initialize Jolt physics scene: ") + Exception.what()));
		}
	}

	void PhysicsSceneRuntime::Stop() noexcept
	{
		m_Impl.reset();
	}

	bool PhysicsSceneRuntime::IsRunning() const noexcept
	{
		return m_Impl != nullptr;
	}

	std::expected<void, PhysicsSceneRuntimeError> PhysicsSceneRuntime::ApplyForce(UUID Entity, const glm::vec3& Force)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (!IsFinite(Force))
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidForce, "Applied force must be finite"));
		if (auto Result = m_Impl->ReconcileBodies(*m_Impl->SourceScene); !Result)
			return std::unexpected(Result.error());

		const auto Body = m_Impl->Bodies.find(Entity);
		if (Body == m_Impl->Bodies.end())
			return std::unexpected(MakeError(
				PhysicsSceneRuntimeErrorCode::MissingBody,
				"Entity has no active physics body"));
		if (Body->second.Rigidbody.MotionType != RigidbodyMotionType::Dynamic)
			return std::unexpected(MakeError(
				PhysicsSceneRuntimeErrorCode::StaticBody,
				"Forces can only be applied to dynamic physics bodies"));

		m_Impl->System.GetBodyInterface().AddForce(
			Body->second.ID,
			JPH::Vec3(Force.x, Force.y, Force.z),
			JPH::EActivation::Activate);
		return {};
	}

	std::expected<uint32_t, PhysicsSceneRuntimeError> PhysicsSceneRuntime::Advance(Scene& Source, Timestep FrameDelta)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (m_Impl->SourceScene != &Source)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::DifferentScene, "Physics runtime was started for a different Scene"));
		const double DeltaSeconds = FrameDelta.GetSeconds();
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidDeltaTime, "Physics frame delta must be finite and non-negative"));
		if (auto Result = m_Impl->ReconcileBodies(Source); !Result)
			return std::unexpected(Result.error());

		m_Impl->AccumulatorSeconds += std::min(DeltaSeconds, m_Description.MaxFrameDeltaSeconds);
		const double FixedStep = m_Description.FixedStepSeconds;
		uint32_t Steps = 0;
		while (m_Impl->AccumulatorSeconds >= FixedStep && Steps < m_Description.MaxSubsteps)
		{
			const JPH::EPhysicsUpdateError UpdateError = m_Impl->System.Update(
				static_cast<float>(FixedStep),
				1,
				&m_Impl->TemporaryAllocator,
				&m_Impl->JobSystem);
			m_Impl->AccumulatorSeconds -= FixedStep;
			++Steps;
			if (UpdateError != JPH::EPhysicsUpdateError::None)
			{
				(void)m_Impl->SynchronizeScene();
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::SimulationFailed,
					"Jolt physics step exhausted a body-pair or contact capacity; increase the physics scene limits"));
			}
		}
		if (m_Impl->AccumulatorSeconds >= FixedStep)
			m_Impl->AccumulatorSeconds = std::fmod(m_Impl->AccumulatorSeconds, FixedStep);
		if (Steps > 0)
		{
			if (auto Result = m_Impl->SynchronizeScene(); !Result)
				return std::unexpected(Result.error());
		}
		return Steps;
	}
}
