#include "Core/Log.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Scene.h"

#include <Jolt/Jolt.h>
JPH_SUPPRESS_WARNINGS
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/MotionProperties.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <mutex>
#include <optional>
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

		struct PhysicsComponents
		{
			std::optional<RigidbodyComponent> Rigidbody;
			std::optional<BoxColliderComponent> Collider;
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
		uint32_t BodyCapacity = 16384;
		uint64_t FullSceneReconciliationCount = 0;

		~Impl()
		{
			ClearBodies();
		}

		void Initialize(const PhysicsSceneRuntimeDesc& Description)
		{
			BodyCapacity = Description.MaxBodies;
			System.Init(
				Description.MaxBodies, 0, 32768, 8192, BroadPhaseLayerInterface, ObjectVsBroadPhaseLayerFilter, ObjectLayerPairFilter);
			SystemInitialized = true;
			System.SetGravity(JPH::Vec3(Description.Gravity.x, Description.Gravity.y, Description.Gravity.z));
		}

		std::expected<PhysicsComponents, PhysicsSceneRuntimeError> ReadPhysicsComponents(const Entity& Current) const
		{
			const UUID Identifier = Current.GetUUID();
			auto Rigidbody = Current.GetRigidbody();
			auto Collider = Current.GetBoxCollider();
			if (!Rigidbody || !Collider)
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Could not inspect physics components for entity " + Identifier.ToString()));

			if (Rigidbody->has_value() != Collider->has_value())
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Physics entities require both a rigidbody and box collider: " + Identifier.ToString()));

			return PhysicsComponents{ std::move(*Rigidbody), std::move(*Collider) };
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
			const Entity& Current, const RigidbodyComponent& Rigidbody, const BoxColliderComponent& Collider)
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
			if (!IsFinite(EffectiveHalfExtents) || EffectiveHalfExtents.x <= 0.0f || EffectiveHalfExtents.y <= 0.0f ||
				EffectiveHalfExtents.z <= 0.0f)
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Box collider half-extents must remain finite and positive after applying entity scale: " +
					Current.GetUUID().ToString()));
			}
			return ValidatedBodySettings{ *Transform, EffectiveHalfExtents };
		}

		std::expected<JPH::Ref<JPH::Shape>, PhysicsSceneRuntimeError> CreateShape(UUID EntityIdentifier, const glm::vec3& HalfExtents)
		{
			JPH::BoxShapeSettings ShapeSettings(
				JPH::Vec3(HalfExtents.x, HalfExtents.y, HalfExtents.z),
				0.0f);
			ShapeSettings.SetEmbedded();
			auto ShapeResult = ShapeSettings.Create();
			if (ShapeResult.HasError())
			{
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Jolt could not create a box shape for entity " + EntityIdentifier.ToString() + ": " +
						std::string(ShapeResult.GetError().c_str())));
			}
			return ShapeResult.Get();
		}

		JPH::BodyCreationSettings MakeBodySettings(
			const JPH::Shape* Shape, const RigidbodyComponent& Rigidbody, const TransformComponent& Transform)
		{
			const bool Dynamic = Rigidbody.MotionType == RigidbodyMotionType::Dynamic;
			JPH::BodyCreationSettings Settings(Shape, JPH::RVec3(Transform.Translation.x, Transform.Translation.y, Transform.Translation.z),
				ToJolt(Transform.Rotation), Dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
				Dynamic ? DynamicObjectLayer : StaticObjectLayer);
			Settings.mAllowDynamicOrKinematic = true;
			Settings.mFriction = Rigidbody.Friction;
			Settings.mRestitution = Rigidbody.Restitution;
			Settings.mAllowSleeping = Rigidbody.AllowSleeping;
			if (Dynamic)
			{
				Settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
				Settings.mMassPropertiesOverride.mMass = Rigidbody.Mass;
			}
			return Settings;
		}

		std::expected<BodyRecord, PhysicsSceneRuntimeError> CreateBody(const Entity& Current, const RigidbodyComponent& Rigidbody,
			const BoxColliderComponent& Collider, const ValidatedBodySettings& ValidatedSettings)
		{
			auto Shape = CreateShape(Current.GetUUID(), ValidatedSettings.EffectiveHalfExtents);
			if (!Shape) return std::unexpected(Shape.error());

			const JPH::BodyCreationSettings Settings = MakeBodySettings(Shape->GetPtr(), Rigidbody, ValidatedSettings.Transform);
			const bool Dynamic = Rigidbody.MotionType == RigidbodyMotionType::Dynamic;
			const JPH::EActivation Activation = Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
			const JPH::BodyID Body = System.GetBodyInterface().CreateAndAddBody(Settings, Activation);
			if (Body.IsInvalid())
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Jolt could not allocate a body for entity " + Current.GetUUID().ToString()));
			}
			return BodyRecord{ Body, Current, Rigidbody, Collider, ValidatedSettings.EffectiveHalfExtents, ValidatedSettings.Transform };
		}

		void RemoveBody(JPH::BodyID Body) noexcept
		{
			JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
			BodyInterface.RemoveBody(Body);
			BodyInterface.DestroyBody(Body);
		}

		std::expected<void, PhysicsSceneRuntimeError> AddBody(const Entity& Current, const RigidbodyComponent& Rigidbody,
			const BoxColliderComponent& Collider, const ValidatedBodySettings& Settings)
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

		std::expected<bool, PhysicsSceneRuntimeError> ReconcileBody(const Entity& Current)
		{
			try
			{
				const UUID Identifier = Current.GetUUID();
				auto Components = ReadPhysicsComponents(Current);
				if (!Components) return std::unexpected(Components.error());
				auto ExistingBody = Bodies.find(Identifier);
				if (!Components->Rigidbody)
				{
					if (ExistingBody != Bodies.end())
					{
						RemoveBody(ExistingBody->second.ID);
						Bodies.erase(ExistingBody);
					}
					return false;
				}

				const RigidbodyComponent& CurrentRigidbody = *Components->Rigidbody;
				const BoxColliderComponent& CurrentCollider = *Components->Collider;
				auto Settings = ValidateBody(Current, CurrentRigidbody, CurrentCollider);
				if (!Settings) return std::unexpected(Settings.error());

				if (ExistingBody == Bodies.end())
				{
					if (auto Result = AddBody(Current, CurrentRigidbody, CurrentCollider, *Settings); !Result)
						return std::unexpected(Result.error());
					return true;
				}

				BodyRecord& Existing = ExistingBody->second;
				const bool SameIncarnation = Existing.BoundEntity == Current;
				const RigidbodyComponent& NewRigidbody = CurrentRigidbody;
				const BoxColliderComponent& NewCollider = CurrentCollider;
				const bool SameRigidbody =
					Existing.Rigidbody.MotionType == NewRigidbody.MotionType && Existing.Rigidbody.Mass == NewRigidbody.Mass &&
					Existing.Rigidbody.Friction == NewRigidbody.Friction && Existing.Rigidbody.Restitution == NewRigidbody.Restitution &&
					Existing.Rigidbody.AllowSleeping == NewRigidbody.AllowSleeping;
				const bool SameCollider = SameVector(Existing.Collider.HalfExtents, NewCollider.HalfExtents);
				const bool SameShape = SameVector(Existing.EffectiveHalfExtents, Settings->EffectiveHalfExtents);
				const bool ScenePoseChanged = !SameScenePose(Settings->Transform, Existing.LastSceneTransform);
				const bool SettingsChanged = !SameIncarnation || !SameRigidbody || !SameCollider || !SameShape;
				const bool WasDynamic = Existing.Rigidbody.MotionType == RigidbodyMotionType::Dynamic;
				const bool IsDynamic = NewRigidbody.MotionType == RigidbodyMotionType::Dynamic;

				JPH::BodyInterface& BodyInterface = System.GetBodyInterface();
				if (!BodyInterface.IsAdded(Existing.ID))
					return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
						"Tracked physics body is no longer added to Jolt for entity " + Identifier.ToString()));
				JPH::Ref<JPH::Shape> UpdatedShape;
				const JPH::Shape* EffectiveShape = nullptr;
				if (!SameShape)
				{
					auto Shape = CreateShape(Identifier, Settings->EffectiveHalfExtents);
					if (!Shape) return std::unexpected(Shape.error());
					UpdatedShape = std::move(*Shape);
					EffectiveShape = UpdatedShape.GetPtr();
				}
				const bool NeedsMassUpdate =
					IsDynamic && (!WasDynamic || !SameIncarnation || !SameShape || Existing.Rigidbody.Mass != NewRigidbody.Mass);
				const bool FrictionChanged = Existing.Rigidbody.Friction != NewRigidbody.Friction;
				const bool RestitutionChanged = Existing.Rigidbody.Restitution != NewRigidbody.Restitution;
				const bool UpdateDynamicProperties =
					IsDynamic &&
					(NeedsMassUpdate || Existing.Rigidbody.AllowSleeping != NewRigidbody.AllowSleeping || !SameIncarnation || !WasDynamic);
				std::optional<JPH::MassProperties> NewMassProperties;
				if (NeedsMassUpdate)
				{
					if (EffectiveShape == nullptr)
					{
						const JPH::RefConst<JPH::Shape> CurrentShape = BodyInterface.GetShape(Existing.ID);
						EffectiveShape = CurrentShape.GetPtr();
					}
					const JPH::BodyCreationSettings NewBodySettings = MakeBodySettings(EffectiveShape, NewRigidbody, Settings->Transform);
					NewMassProperties = NewBodySettings.GetMassProperties();
				}
				const JPH::EActivation Activation =
					IsDynamic && (SettingsChanged || ScenePoseChanged) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;

				// Shape creation and settings validation finish before mutating the live body. Jolt's
				// BodyInterface performs the required body locks and broadphase/contact-cache updates.
				if (!SameShape) BodyInterface.SetShape(Existing.ID, EffectiveShape, true, Activation);
				if (WasDynamic != IsDynamic)
				{
					BodyInterface.SetMotionType(Existing.ID, IsDynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static, Activation);
					BodyInterface.SetObjectLayer(Existing.ID, IsDynamic ? DynamicObjectLayer : StaticObjectLayer);
				}
				if (FrictionChanged || RestitutionChanged || UpdateDynamicProperties)
				{
					JPH::BodyLockWrite Lock(System.GetBodyLockInterface(), Existing.ID);
					if (!Lock.Succeeded())
						return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
							"Could not lock physics body while applying settings for entity " + Identifier.ToString()));
					JPH::Body& Body = Lock.GetBody();
					if (FrictionChanged) Body.SetFriction(NewRigidbody.Friction);
					if (RestitutionChanged) Body.SetRestitution(NewRigidbody.Restitution);
					if (UpdateDynamicProperties)
					{
						JPH::MotionProperties* MotionProperties = Body.GetMotionProperties();
						if (NeedsMassUpdate) MotionProperties->SetMassProperties(MotionProperties->GetAllowedDOFs(), *NewMassProperties);
						if (Existing.Rigidbody.AllowSleeping != NewRigidbody.AllowSleeping || !WasDynamic || !SameIncarnation)
							Body.SetAllowSleeping(NewRigidbody.AllowSleeping);
						if (!SameIncarnation)
						{
							Body.SetLinearVelocity(JPH::Vec3::sZero());
							Body.SetAngularVelocity(JPH::Vec3::sZero());
							MotionProperties->ResetForce();
							MotionProperties->ResetTorque();
						}
					}
				}
				if (SettingsChanged && IsDynamic) BodyInterface.ActivateBody(Existing.ID);

				if (!SameIncarnation || ScenePoseChanged)
				{
					// A scene-authored transform change teleports without resetting same-incarnation velocity.
					// A new entity incarnation starts at its own transform and never inherits old motion.
					BodyInterface.SetPositionAndRotation(Existing.ID,
						JPH::RVec3(Settings->Transform.Translation.x, Settings->Transform.Translation.y, Settings->Transform.Translation.z),
						ToJolt(Settings->Transform.Rotation), IsDynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate);
				}

				Existing.BoundEntity = Current;
				Existing.Rigidbody = NewRigidbody;
				Existing.Collider = NewCollider;
				Existing.EffectiveHalfExtents = Settings->EffectiveHalfExtents;
				Existing.LastSceneTransform = Settings->Transform;
				return true;
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Could not synchronize physics body for entity " + Current.GetUUID().ToString() + ": " + Exception.what()));
			}
			catch (...)
			{
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Could not synchronize physics body for entity " + Current.GetUUID().ToString()));
			}
		}

		std::expected<bool, PhysicsSceneRuntimeError> ReconcileEntity(Scene& Source, UUID Identifier)
		{
			const auto Current = Source.FindEntity(Identifier);
			if (!Current)
			{
				const auto ExistingBody = Bodies.find(Identifier);
				if (ExistingBody != Bodies.end())
				{
					RemoveBody(ExistingBody->second.ID);
					Bodies.erase(ExistingBody);
				}
				return false;
			}
			if (!Bodies.contains(Identifier) && Bodies.size() >= BodyCapacity)
			{
				PruneDestroyedBodies(Source);
				if (auto Result = PruneBodiesWithoutPhysicsComponents(Source); !Result) return std::unexpected(Result.error());
			}
			return ReconcileBody(*Current);
		}

		void PruneDestroyedBodies(Scene& Source)
		{
			for (auto Iterator = Bodies.begin(); Iterator != Bodies.end();)
			{
				if (!Source.FindEntity(Iterator->first))
				{
					RemoveBody(Iterator->second.ID);
					Iterator = Bodies.erase(Iterator);
				}
				else ++Iterator;
			}
		}

		std::expected<void, PhysicsSceneRuntimeError> PruneBodiesWithoutPhysicsComponents(Scene& Source)
		{
			// Validate all tracked entities first so a malformed component pair does not leave a
			// partially reclaimed pool when targeted synchronization is trying to free a slot.
			for (const auto& Entry : Bodies)
			{
				const UUID& Identifier = Entry.first;
				const auto Current = Source.FindEntity(Identifier);
				if (!Current) continue;
				auto Components = ReadPhysicsComponents(*Current);
				if (!Components) return std::unexpected(Components.error());
			}

			for (auto Iterator = Bodies.begin(); Iterator != Bodies.end();)
			{
				const auto Current = Source.FindEntity(Iterator->first);
				if (!Current)
				{
					RemoveBody(Iterator->second.ID);
					Iterator = Bodies.erase(Iterator);
				}
				else
				{
					auto Components = ReadPhysicsComponents(*Current);
					if (!Components) return std::unexpected(Components.error());
					if (!Components->Rigidbody)
					{
						RemoveBody(Iterator->second.ID);
						Iterator = Bodies.erase(Iterator);
					}
					else ++Iterator;
				}
			}
			return {};
		}

		std::expected<void, PhysicsSceneRuntimeError> ReconcileBodies(Scene& Source)
		{
			try
			{
				++FullSceneReconciliationCount;
				Bodies.reserve(Source.GetEntityCount());
				const std::vector<Entity> Entities = Source.GetEntities();
				PruneDestroyedBodies(Source);

				// Preserve the partial-component error contract before mutating the pool. Then reclaim
				// every body that no longer has physics components before any UUID-ordered additions.
				for (const Entity& Current : Entities)
					if (auto Components = ReadPhysicsComponents(Current); !Components) return std::unexpected(Components.error());

				if (auto Result = PruneBodiesWithoutPhysicsComponents(Source); !Result) return std::unexpected(Result.error());

				for (const Entity& Current : Entities)
				{
					if (auto Result = ReconcileBody(Current); !Result) return std::unexpected(Result.error());
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
					return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed,
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
						return std::unexpected(
							MakeError(PhysicsSceneRuntimeErrorCode::SceneSynchronizationFailed, SynchronizedTransform.error().Message));
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
			!std::isfinite(m_Description.MaxFrameDeltaSeconds) || m_Description.MaxFrameDeltaSeconds < m_Description.FixedStepSeconds ||
			m_Description.MaxSubsteps == 0 || m_Description.MaxBodies == 0 || !IsFinite(m_Description.Gravity))
		{
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidSettings,
				"Physics fixed step, frame-delta cap, substep limit, body capacity, or gravity is invalid"));
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

	uint64_t PhysicsSceneRuntime::GetFullSceneReconciliationCountForTesting() const noexcept
	{
		return m_Impl ? m_Impl->FullSceneReconciliationCount : 0;
	}

	std::expected<void, PhysicsSceneRuntimeError> PhysicsSceneRuntime::ApplyForce(UUID Entity, const glm::vec3& Force)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (!IsFinite(Force))
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidForce, "Applied force must be finite"));
		auto Reconciled = m_Impl->ReconcileEntity(*m_Impl->SourceScene, Entity);
		if (!Reconciled) return std::unexpected(Reconciled.error());
		if (!*Reconciled) return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::MissingBody, "Entity has no active physics body"));

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
