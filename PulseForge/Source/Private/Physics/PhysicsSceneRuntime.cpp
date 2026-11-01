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
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/CollisionCollectorImpl.h>
#include <Jolt/Physics/Collision/ShapeCast.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/CollideShape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <limits>
#include <mutex>
#include <atomic>
#include <map>
#include <optional>
#include <set>
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
		constexpr size_t MaximumContactRecords = 65536;
		constexpr uint32_t MaximumQueryResults = 64;

		bool UUIDLess(UUID First, UUID Second) noexcept
		{
			return First.GetHigh() != Second.GetHigh()
				? First.GetHigh() < Second.GetHigh()
				: First.GetLow() < Second.GetLow();
		}

		struct PhysicsBodyIdentity final
		{
			Entity BoundEntity;
			UUID Identifier;
			uint64_t SessionGeneration = 0;
			uint64_t BodyGeneration = 0;
			uint32_t Category = 1;
			uint32_t Mask = 0xffffffffu;
			bool IsSensor = false;
		};

		struct EffectiveColliderGeometry final
		{
			ColliderShape Shape = ColliderShape::Box;
			glm::vec3 BoxHalfExtents{ 0.0f };
			float SphereRadius = 0.0f;
			float CapsuleRadius = 0.0f;
			float CapsuleHalfCylinder = 0.0f;
		};

		PhysicsBodyIdentity* GetPhysicsBodyIdentity(const JPH::Body& Body) noexcept
		{
			return reinterpret_cast<PhysicsBodyIdentity*>(static_cast<uintptr_t>(Body.GetUserData()));
		}

		bool AcceptsQuery(const PhysicsBodyIdentity* Identity, const PhysicsQueryFilter& Filter)
		{
			if (!Identity || (!Filter.IncludeSensors && Identity->IsSensor) ||
				(Filter.CategoryMask & Identity->Category) == 0)
				return false;
			return std::find(Filter.ExcludedEntities.begin(), Filter.ExcludedEntities.end(), Identity->Identifier) ==
				Filter.ExcludedEntities.end();
		}

		bool SameCollider(const BoxColliderComponent& First, const BoxColliderComponent& Second)
		{
			return First.Shape == Second.Shape && First.HalfExtents == Second.HalfExtents &&
				First.SphereRadius == Second.SphereRadius && First.CapsuleRadius == Second.CapsuleRadius &&
				First.CapsuleHeight == Second.CapsuleHeight && First.IsSensor == Second.IsSensor &&
				First.CollisionCategory == Second.CollisionCategory && First.CollisionMask == Second.CollisionMask;
		}

		bool SameGeometry(const EffectiveColliderGeometry& First, const EffectiveColliderGeometry& Second)
		{
			return First.Shape == Second.Shape && First.BoxHalfExtents == Second.BoxHalfExtents &&
				First.SphereRadius == Second.SphereRadius && First.CapsuleRadius == Second.CapsuleRadius &&
				First.CapsuleHalfCylinder == Second.CapsuleHalfCylinder;
		}

		bool IsFinite(const glm::quat& Value)
		{
			return std::isfinite(Value.x) && std::isfinite(Value.y) && std::isfinite(Value.z) && std::isfinite(Value.w);
		}

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
			bool ShouldCollide(ObjectLayer, BroadPhaseLayer) const override
			{
				return true;
			}
		};

		class ObjectPairFilter final : public JPH::ObjectLayerPairFilter
		{
		public:
			bool ShouldCollide(ObjectLayer, ObjectLayer) const override
			{
				return true;
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

		class FilteredRayCollector final : public JPH::CastRayCollector
		{
		public:
			explicit FilteredRayCollector(const PhysicsQueryFilter& Filter) : m_Filter(Filter) { }

			void OnBody(const JPH::Body& Body) override
			{
				m_Identity = GetPhysicsBodyIdentity(Body);
				m_Accept = AcceptsQuery(m_Identity, m_Filter);
			}

			void AddHit(const JPH::RayCastResult& Result) override
			{
				if (!m_Accept) return;
				if (m_HadHit && (Result.mFraction > m_Hit.mFraction ||
					(Result.mFraction == m_Hit.mFraction && !UUIDLess(m_Identity->Identifier, m_IdentityValue.Identifier))))
					return;
				UpdateEarlyOutFraction(std::nextafter(Result.mFraction, std::numeric_limits<float>::infinity()));
				m_Hit = Result;
				m_IdentityValue = *m_Identity;
				m_HadHit = true;
			}

			[[nodiscard]] bool HadHit() const noexcept { return m_HadHit; }
			[[nodiscard]] const JPH::RayCastResult& GetHit() const noexcept { return m_Hit; }
			[[nodiscard]] const PhysicsBodyIdentity& GetIdentity() const noexcept { return m_IdentityValue; }

		private:
			const PhysicsQueryFilter& m_Filter;
			PhysicsBodyIdentity* m_Identity = nullptr;
			PhysicsBodyIdentity m_IdentityValue;
			JPH::RayCastResult m_Hit;
			bool m_Accept = false;
			bool m_HadHit = false;
		};

		class FilteredOverlapCollector final : public JPH::CollideShapeCollector
		{
		public:
			FilteredOverlapCollector(const PhysicsQueryFilter& Filter, uint32_t Limit, glm::vec3 BasePosition)
				: m_Filter(Filter), m_Limit(Limit), m_BasePosition(BasePosition)
			{
				m_Hits.reserve(Limit);
			}

			void OnBody(const JPH::Body& Body) override
			{
				m_Identity = GetPhysicsBodyIdentity(Body);
				m_Accept = AcceptsQuery(m_Identity, m_Filter);
			}

			void AddHit(const JPH::CollideShapeResult& Result) override
			{
				if (!m_Accept) return;
				if (m_Hits.size() == m_Limit)
				{
					m_Truncated = true;
					ForceEarlyOut();
					return;
				}
				const JPH::Vec3 JoltNormal = -Result.mPenetrationAxis.Normalized();
				m_Hits.push_back({ *m_Identity, Result.mBodyID2,
					m_BasePosition + glm::vec3(Result.mContactPointOn2.GetX(), Result.mContactPointOn2.GetY(), Result.mContactPointOn2.GetZ()),
					glm::vec3(JoltNormal.GetX(), JoltNormal.GetY(), JoltNormal.GetZ()) });
			}

			struct Hit final
			{
				PhysicsBodyIdentity Identity;
				JPH::BodyID Body;
				glm::vec3 Position{ 0.0f };
				glm::vec3 Normal{ 0.0f };
			};

			[[nodiscard]] std::vector<Hit>& Hits() noexcept { return m_Hits; }
			[[nodiscard]] bool Truncated() const noexcept { return m_Truncated; }

		private:
			const PhysicsQueryFilter& m_Filter;
			const uint32_t m_Limit;
			glm::vec3 m_BasePosition{ 0.0f };
			PhysicsBodyIdentity* m_Identity = nullptr;
			bool m_Accept = false;
			bool m_Truncated = false;
			std::vector<Hit> m_Hits;
		};

		class FilteredShapeCastCollector final : public JPH::CastShapeCollector
		{
		public:
			explicit FilteredShapeCastCollector(const PhysicsQueryFilter& Filter) : m_Filter(Filter) { }

			void OnBody(const JPH::Body& Body) override
			{
				m_Identity = GetPhysicsBodyIdentity(Body);
				m_Accept = AcceptsQuery(m_Identity, m_Filter);
			}

			void AddHit(const JPH::ShapeCastResult& Result) override
			{
				const float Fraction = Result.GetEarlyOutFraction();
				if (!m_Accept) return;
				if (m_HadHit)
				{
					const float ExistingFraction = m_Hit.GetEarlyOutFraction();
					if (Fraction > ExistingFraction ||
						(Fraction == ExistingFraction && !UUIDLess(m_Identity->Identifier, m_IdentityValue.Identifier)))
						return;
				}
				UpdateEarlyOutFraction(std::nextafter(Fraction, std::numeric_limits<float>::infinity()));
				m_Hit = Result;
				m_IdentityValue = *m_Identity;
				m_HadHit = true;
			}

			[[nodiscard]] bool HadHit() const noexcept { return m_HadHit; }
			[[nodiscard]] const JPH::ShapeCastResult& GetHit() const noexcept { return m_Hit; }
			[[nodiscard]] const PhysicsBodyIdentity& GetIdentity() const noexcept { return m_IdentityValue; }

		private:
			const PhysicsQueryFilter& m_Filter;
			PhysicsBodyIdentity* m_Identity = nullptr;
			PhysicsBodyIdentity m_IdentityValue;
			JPH::ShapeCastResult m_Hit;
			bool m_Accept = false;
			bool m_HadHit = false;
		};
	}

	struct PhysicsSceneRuntime::Impl final
	{
		struct ContactPairKey final
		{
			uint32_t FirstBody = 0;
			uint32_t SecondBody = 0;
			friend auto operator<=>(const ContactPairKey&, const ContactPairKey&) = default;
		};

		enum class ContactRecordKind : uint8_t
		{
			Added,
			Persisted,
			Removed
		};

		struct ContactRecord final
		{
			ContactRecordKind Kind = ContactRecordKind::Added;
			ContactPairKey Pair;
			PhysicsContactEvent Event;
		};

		struct BodyRecord
		{
			JPH::BodyID ID;
			Entity BoundEntity;
			RigidbodyComponent Rigidbody;
			BoxColliderComponent Collider;
			EffectiveColliderGeometry Geometry;
			TransformComponent LastSceneTransform;
			std::unique_ptr<PhysicsBodyIdentity> Identity;
		};

		struct ValidatedBodySettings
		{
			TransformComponent Transform;
			EffectiveColliderGeometry Geometry;
		};

		struct PhysicsComponents
		{
			std::optional<RigidbodyComponent> Rigidbody;
			std::optional<BoxColliderComponent> Collider;
		};

		struct ContactListener final : JPH::ContactListener
		{
			explicit ContactListener(Impl& Runtime) : Owner(Runtime) { }

			JPH::ValidateResult OnContactValidate(const JPH::Body& First, const JPH::Body& Second,
				JPH::RVec3Arg, const JPH::CollideShapeResult&) noexcept override
			{
				const PhysicsBodyIdentity* FirstIdentity = GetPhysicsBodyIdentity(First);
				const PhysicsBodyIdentity* SecondIdentity = GetPhysicsBodyIdentity(Second);
				if (!FirstIdentity || !SecondIdentity || FirstIdentity->Identifier == SecondIdentity->Identifier)
					return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
				if ((FirstIdentity->Category & SecondIdentity->Mask) == 0 ||
					(SecondIdentity->Category & FirstIdentity->Mask) == 0)
					return JPH::ValidateResult::RejectAllContactsForThisBodyPair;
				return JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
			}

			void OnContactAdded(const JPH::Body& First, const JPH::Body& Second,
				const JPH::ContactManifold&, JPH::ContactSettings& Settings) noexcept override
			{
				const auto* FirstIdentity = GetPhysicsBodyIdentity(First);
				const auto* SecondIdentity = GetPhysicsBodyIdentity(Second);
				Settings.mIsSensor = FirstIdentity && SecondIdentity && (FirstIdentity->IsSensor || SecondIdentity->IsSensor);
				Owner.EnqueueContact(ContactRecordKind::Added, First.GetID(), Second.GetID(), FirstIdentity, SecondIdentity);
			}

			void OnContactPersisted(const JPH::Body& First, const JPH::Body& Second,
				const JPH::ContactManifold&, JPH::ContactSettings& Settings) noexcept override
			{
				const auto* FirstIdentity = GetPhysicsBodyIdentity(First);
				const auto* SecondIdentity = GetPhysicsBodyIdentity(Second);
				Settings.mIsSensor = FirstIdentity && SecondIdentity && (FirstIdentity->IsSensor || SecondIdentity->IsSensor);
				Owner.EnqueueContact(ContactRecordKind::Persisted, First.GetID(), Second.GetID(), FirstIdentity, SecondIdentity);
			}

			void OnContactRemoved(const JPH::SubShapeIDPair& Pair) noexcept override
			{
				Owner.EnqueueRemoved(Pair.GetBody1ID(), Pair.GetBody2ID());
			}

			Impl& Owner;
		};

		JoltRuntimeLease Runtime;
		BroadPhaseLayers BroadPhaseLayerInterface;
		ObjectVsBroadPhaseFilter ObjectVsBroadPhaseLayerFilter;
		ObjectPairFilter ObjectLayerPairFilter;
		JPH::TempAllocatorImplWithMallocFallback TemporaryAllocator{ 32u * 1024u * 1024u };
		JPH::JobSystemThreadPool JobSystem{ JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, GetWorkerThreadCount() };
		JPH::PhysicsSystem System;
		ContactListener Listener;
		bool SystemInitialized = false;
		std::unordered_map<UUID, BodyRecord, UUIDHash> Bodies;
		std::vector<ContactRecord> ContactQueue;
		std::vector<ContactRecord> DrainedContactQueue;
		std::mutex ContactQueueMutex;
		std::atomic<bool> ContactOverflowed = false;
		std::atomic<uint64_t> ContactOverflowCount = 0;
		std::map<ContactPairKey, PhysicsContactEvent> ActiveContacts;
		std::set<ContactPairKey> BeganThisFrame;
		std::vector<PhysicsContactEvent> FrameEvents;
		Scene* SourceScene = nullptr;
		double AccumulatorSeconds = 0.0;
		uint32_t BodyCapacity = 16384;
		uint64_t FullSceneReconciliationCount = 0;
		uint64_t SessionGeneration = 1;
		uint64_t NextBodyGeneration = 0;
		uint64_t FixedSteps = 0;
		uint64_t CappedFrames = 0;
		double DroppedSimulationSeconds = 0.0;
		std::thread::id OwnerThread = std::this_thread::get_id();
		bool Unhealthy = false;

		Impl() : Listener(*this)
		{
			ContactQueue.reserve(MaximumContactRecords);
			DrainedContactQueue.reserve(MaximumContactRecords);
			FrameEvents.reserve(1024);
		}

		~Impl()
		{
			ClearBodies();
		}

		void Initialize(const PhysicsSceneRuntimeDesc& Description)
		{
			BodyCapacity = Description.MaxBodies;
			SessionGeneration = Description.SessionGeneration;
			System.Init(
				Description.MaxBodies, 0, 32768, 8192, BroadPhaseLayerInterface, ObjectVsBroadPhaseLayerFilter, ObjectLayerPairFilter);
			System.SetContactListener(&Listener);
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
					"Physics entities require both a rigidbody and collider: " + Identifier.ToString()));

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

		std::expected<EffectiveColliderGeometry, PhysicsSceneRuntimeError> GetEffectiveGeometry(
			const Entity& Current, const BoxColliderComponent& Collider, const TransformComponent& Transform) const
		{
			const glm::vec3 Scale = glm::abs(Transform.Scale);
			if (!IsFinite(Transform.Translation) || !IsFinite(Scale) || !IsFinite(Transform.Rotation) ||
				Scale.x <= 0.00001f || Scale.y <= 0.00001f || Scale.z <= 0.00001f ||
				std::abs(glm::dot(Transform.Rotation, Transform.Rotation) - 1.0f) > 0.001f)
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Physics transform must have finite position, normalized rotation, and non-zero scale: " + Current.GetUUID().ToString()));

			EffectiveColliderGeometry Geometry;
			Geometry.Shape = Collider.Shape;
			if (Collider.Shape == ColliderShape::Box)
				Geometry.BoxHalfExtents = Collider.HalfExtents * Scale;
			else if (Collider.Shape == ColliderShape::Sphere)
			{
				const float Tolerance = 0.0001f * (std::max)({ Scale.x, Scale.y, Scale.z, 1.0f });
				if (std::abs(Scale.x - Scale.y) > Tolerance || std::abs(Scale.x - Scale.z) > Tolerance)
					return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
						"Sphere collider requires uniform entity scale"));
				Geometry.SphereRadius = Collider.SphereRadius * Scale.x;
			}
			else
			{
				const float Tolerance = 0.0001f * (std::max)({ Scale.x, Scale.y, Scale.z, 1.0f });
				if (std::abs(Scale.x - Scale.y) > Tolerance || std::abs(Scale.x - Scale.z) > Tolerance)
					return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
						"Capsule collider requires uniform absolute entity scale so its spherical caps remain exact"));
				Geometry.CapsuleRadius = Collider.CapsuleRadius * Scale.x;
				Geometry.CapsuleHalfCylinder = ((Collider.CapsuleHeight - 2.0f * Collider.CapsuleRadius) * 0.5f) * Scale.x;
			}

			const bool ValidGeometry = Geometry.Shape == ColliderShape::Box
				? IsFinite(Geometry.BoxHalfExtents) && Geometry.BoxHalfExtents.x > 0.0f &&
					Geometry.BoxHalfExtents.y > 0.0f && Geometry.BoxHalfExtents.z > 0.0f
				: Geometry.Shape == ColliderShape::Sphere
					? std::isfinite(Geometry.SphereRadius) && Geometry.SphereRadius > 0.0f
					: std::isfinite(Geometry.CapsuleRadius) && Geometry.CapsuleRadius > 0.0f &&
						std::isfinite(Geometry.CapsuleHalfCylinder) && Geometry.CapsuleHalfCylinder >= 0.0f;
			if (!ValidGeometry)
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity,
					"Collider dimensions must remain finite and positive after applying supported entity scale"));
			return Geometry;
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

			auto Geometry = GetEffectiveGeometry(Current, Collider, *Transform);
			if (!Geometry) return std::unexpected(Geometry.error());
			return ValidatedBodySettings{ *Transform, *Geometry };
		}

		std::expected<JPH::Ref<JPH::Shape>, PhysicsSceneRuntimeError> CreateShape(
			UUID EntityIdentifier, const EffectiveColliderGeometry& Geometry)
		{
			JPH::ShapeSettings::ShapeResult ShapeResult;
			if (Geometry.Shape == ColliderShape::Box)
			{
				JPH::BoxShapeSettings ShapeSettings(
					JPH::Vec3(Geometry.BoxHalfExtents.x, Geometry.BoxHalfExtents.y, Geometry.BoxHalfExtents.z), 0.0f);
				ShapeSettings.SetEmbedded();
				ShapeResult = ShapeSettings.Create();
			}
			else if (Geometry.Shape == ColliderShape::Sphere)
			{
				JPH::SphereShapeSettings ShapeSettings(Geometry.SphereRadius);
				ShapeSettings.SetEmbedded();
				ShapeResult = ShapeSettings.Create();
			}
			else
			{
				JPH::CapsuleShapeSettings ShapeSettings(Geometry.CapsuleHalfCylinder, Geometry.CapsuleRadius);
				ShapeSettings.SetEmbedded();
				ShapeResult = ShapeSettings.Create();
			}
			if (ShapeResult.HasError())
			{
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Jolt could not create collider shape for entity " + EntityIdentifier.ToString() + ": " +
						std::string(ShapeResult.GetError().c_str())));
			}
			return ShapeResult.Get();
		}

		JPH::BodyCreationSettings MakeBodySettings(
			const JPH::Shape* Shape, const RigidbodyComponent& Rigidbody, const BoxColliderComponent& Collider,
			const TransformComponent& Transform, const PhysicsBodyIdentity& Identity)
		{
			const bool Dynamic = Rigidbody.MotionType == RigidbodyMotionType::Dynamic;
			JPH::BodyCreationSettings Settings(Shape, JPH::RVec3(Transform.Translation.x, Transform.Translation.y, Transform.Translation.z),
				ToJolt(Transform.Rotation), Dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
				Dynamic ? DynamicObjectLayer : StaticObjectLayer);
			Settings.mAllowDynamicOrKinematic = true;
			Settings.mFriction = Rigidbody.Friction;
			Settings.mRestitution = Rigidbody.Restitution;
			Settings.mAllowSleeping = Rigidbody.AllowSleeping;
			Settings.mIsSensor = Collider.IsSensor;
			Settings.mUserData = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&Identity));
			if (Dynamic)
			{
				Settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
				Settings.mMassPropertiesOverride.mMass = Rigidbody.Mass;
			}
			return Settings;
		}

		static ContactPairKey MakeContactPairKey(JPH::BodyID First, JPH::BodyID Second) noexcept
		{
			uint32_t FirstValue = First.GetIndexAndSequenceNumber();
			uint32_t SecondValue = Second.GetIndexAndSequenceNumber();
			if (SecondValue < FirstValue) std::swap(FirstValue, SecondValue);
			return { FirstValue, SecondValue };
		}

		static PhysicsContactEvent MakeContactEvent(
			const PhysicsBodyIdentity& First, const PhysicsBodyIdentity& Second) noexcept
		{
			const PhysicsBodyIdentity* Low = &First;
			const PhysicsBodyIdentity* High = &Second;
			if (UUIDLess(High->Identifier, Low->Identifier)) std::swap(Low, High);
			return { .Phase = PhysicsContactPhase::Begin,
				.First = Low->BoundEntity,
				.Second = High->BoundEntity,
				.FirstIdentifier = Low->Identifier,
				.SecondIdentifier = High->Identifier,
				.SessionGeneration = Low->SessionGeneration,
				.FirstBodyGeneration = Low->BodyGeneration,
				.SecondBodyGeneration = High->BodyGeneration,
				.IsSensorPair = First.IsSensor || Second.IsSensor };
		}

		void EnqueueContact(ContactRecordKind Kind, JPH::BodyID First, JPH::BodyID Second,
			const PhysicsBodyIdentity* FirstIdentity, const PhysicsBodyIdentity* SecondIdentity) noexcept
		{
			if (!FirstIdentity || !SecondIdentity || FirstIdentity->SessionGeneration != SessionGeneration ||
				SecondIdentity->SessionGeneration != SessionGeneration)
				return;
			try
			{
				std::scoped_lock Lock(ContactQueueMutex);
				if (ContactQueue.size() >= MaximumContactRecords)
				{
					ContactOverflowed.store(true, std::memory_order_relaxed);
					ContactOverflowCount.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				ContactQueue.push_back({ Kind, MakeContactPairKey(First, Second), MakeContactEvent(*FirstIdentity, *SecondIdentity) });
			}
			catch (...)
			{
				ContactOverflowed.store(true, std::memory_order_relaxed);
				ContactOverflowCount.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void EnqueueRemoved(JPH::BodyID First, JPH::BodyID Second) noexcept
		{
			try
			{
				std::scoped_lock Lock(ContactQueueMutex);
				if (ContactQueue.size() >= MaximumContactRecords)
				{
					ContactOverflowed.store(true, std::memory_order_relaxed);
					ContactOverflowCount.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				ContactQueue.push_back({ ContactRecordKind::Removed, MakeContactPairKey(First, Second), {} });
			}
			catch (...)
			{
				ContactOverflowed.store(true, std::memory_order_relaxed);
				ContactOverflowCount.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void EndContactsForBody(uint32_t BodyIdentifier)
		{
			for (auto Iterator = ActiveContacts.begin(); Iterator != ActiveContacts.end();)
			{
				if (Iterator->first.FirstBody != BodyIdentifier && Iterator->first.SecondBody != BodyIdentifier)
				{
					++Iterator;
					continue;
				}
				auto End = Iterator->second;
				End.Phase = PhysicsContactPhase::End;
				AppendFrameEvent(End);
				Iterator = ActiveContacts.erase(Iterator);
			}
		}

		void AppendFrameEvent(const PhysicsContactEvent& Event) noexcept
		{
			try
			{
				if (FrameEvents.size() >= MaximumContactRecords)
				{
					ContactOverflowed.store(true, std::memory_order_relaxed);
					ContactOverflowCount.fetch_add(1, std::memory_order_relaxed);
					return;
				}
				FrameEvents.push_back(Event);
			}
			catch (...)
			{
				ContactOverflowed.store(true, std::memory_order_relaxed);
				ContactOverflowCount.fetch_add(1, std::memory_order_relaxed);
			}
		}

		void ProcessContactRecords()
		{
			{
				std::scoped_lock Lock(ContactQueueMutex);
				DrainedContactQueue.clear();
				DrainedContactQueue.swap(ContactQueue);
			}
			for (const ContactRecord& Record : DrainedContactQueue)
			{
				if (Record.Kind == ContactRecordKind::Removed)
				{
					const auto Existing = ActiveContacts.find(Record.Pair);
					if (Existing == ActiveContacts.end()) continue;
					auto End = Existing->second;
					End.Phase = PhysicsContactPhase::End;
					AppendFrameEvent(End);
					ActiveContacts.erase(Existing);
					continue;
				}

				if (Record.Event.SessionGeneration != SessionGeneration) continue;
				const auto Existing = ActiveContacts.find(Record.Pair);
				if (Existing == ActiveContacts.end())
				{
					ActiveContacts.emplace(Record.Pair, Record.Event);
					BeganThisFrame.insert(Record.Pair);
					AppendFrameEvent(Record.Event);
				}
				else if (Existing->second.FirstBodyGeneration != Record.Event.FirstBodyGeneration ||
					Existing->second.SecondBodyGeneration != Record.Event.SecondBodyGeneration ||
					Existing->second.First != Record.Event.First || Existing->second.Second != Record.Event.Second)
				{
					auto End = Existing->second;
					End.Phase = PhysicsContactPhase::End;
					AppendFrameEvent(End);
					Existing->second = Record.Event;
					AppendFrameEvent(Record.Event);
				}
			}
			DrainedContactQueue.clear();
		}

		void AppendStays(bool HasStepped)
		{
			if (HasStepped)
			{
				for (const auto& [Pair, Contact] : ActiveContacts)
				{
					if (!BeganThisFrame.contains(Pair))
					{
						auto Stay = Contact;
						Stay.Phase = PhysicsContactPhase::Stay;
						AppendFrameEvent(Stay);
					}
				}
			}
			std::stable_sort(FrameEvents.begin(), FrameEvents.end(), [](const PhysicsContactEvent& First, const PhysicsContactEvent& Second)
			{
				if (First.FirstIdentifier != Second.FirstIdentifier) return UUIDLess(First.FirstIdentifier, Second.FirstIdentifier);
				if (First.SecondIdentifier != Second.SecondIdentifier) return UUIDLess(First.SecondIdentifier, Second.SecondIdentifier);
				return false;
			});
		}

		std::expected<BodyRecord, PhysicsSceneRuntimeError> CreateBody(const Entity& Current, const RigidbodyComponent& Rigidbody,
			const BoxColliderComponent& Collider, const ValidatedBodySettings& ValidatedSettings)
		{
			auto Identity = std::make_unique<PhysicsBodyIdentity>();
			Identity->BoundEntity = Current;
			Identity->Identifier = Current.GetUUID();
			Identity->SessionGeneration = SessionGeneration;
			Identity->BodyGeneration = ++NextBodyGeneration;
			Identity->Category = Collider.CollisionCategory;
			Identity->Mask = Collider.CollisionMask;
			Identity->IsSensor = Collider.IsSensor;
			auto Shape = CreateShape(Current.GetUUID(), ValidatedSettings.Geometry);
			if (!Shape) return std::unexpected(Shape.error());

			const JPH::BodyCreationSettings Settings = MakeBodySettings(
				Shape->GetPtr(), Rigidbody, Collider, ValidatedSettings.Transform, *Identity);
			const bool Dynamic = Rigidbody.MotionType == RigidbodyMotionType::Dynamic;
			const JPH::EActivation Activation = Dynamic ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;
			const JPH::BodyID Body = System.GetBodyInterface().CreateAndAddBody(Settings, Activation);
			if (Body.IsInvalid())
			{
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::BodyCreationFailed,
					"Jolt could not allocate a body for entity " + Current.GetUUID().ToString()));
			}
			return BodyRecord{ Body, Current, Rigidbody, Collider, ValidatedSettings.Geometry,
				ValidatedSettings.Transform, std::move(Identity) };
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
				const JPH::BodyID CreatedID = CreatedBody->ID;
				if (!Bodies.emplace(Current.GetUUID(), std::move(*CreatedBody)).second)
				{
					RemoveBody(CreatedID);
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
				const bool SameColliderSettings = SameCollider(Existing.Collider, NewCollider);
				const bool SameShape = SameGeometry(Existing.Geometry, Settings->Geometry);
				const bool ScenePoseChanged = !SameScenePose(Settings->Transform, Existing.LastSceneTransform);
				const bool SettingsChanged = !SameIncarnation || !SameRigidbody || !SameColliderSettings || !SameShape;
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
						auto Shape = CreateShape(Identifier, Settings->Geometry);
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
						const JPH::BodyCreationSettings NewBodySettings = MakeBodySettings(
							EffectiveShape, NewRigidbody, NewCollider, Settings->Transform, *Existing.Identity);
					NewMassProperties = NewBodySettings.GetMassProperties();
				}
				const JPH::EActivation Activation =
					IsDynamic && (SettingsChanged || ScenePoseChanged) ? JPH::EActivation::Activate : JPH::EActivation::DontActivate;

				const bool ContactSettingsChanged = !SameIncarnation ||
					Existing.Collider.CollisionCategory != NewCollider.CollisionCategory ||
					Existing.Collider.CollisionMask != NewCollider.CollisionMask ||
					Existing.Collider.IsSensor != NewCollider.IsSensor;
				if (ContactSettingsChanged) EndContactsForBody(Existing.ID.GetIndexAndSequenceNumber());

				// Shape creation and settings validation finish before mutating the live body. Re-applying
				// the shape when filter/sensor state changes invalidates Jolt's cached contact constraints.
				if (!SameShape || ContactSettingsChanged)
				{
					if (!UpdatedShape)
					{
						const JPH::RefConst<JPH::Shape> CurrentShape = BodyInterface.GetShape(Existing.ID);
						EffectiveShape = CurrentShape.GetPtr();
					}
					BodyInterface.SetShape(Existing.ID, EffectiveShape, !SameShape, Activation);
				}
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
				if (Existing.Collider.IsSensor != NewCollider.IsSensor)
					BodyInterface.SetIsSensor(Existing.ID, NewCollider.IsSensor);
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
				Existing.Geometry = Settings->Geometry;
				Existing.LastSceneTransform = Settings->Transform;
				Existing.Identity->BoundEntity = Current;
				Existing.Identity->Identifier = Identifier;
				Existing.Identity->Category = NewCollider.CollisionCategory;
				Existing.Identity->Mask = NewCollider.CollisionMask;
				Existing.Identity->IsSensor = NewCollider.IsSensor;
				if (SettingsChanged) Existing.Identity->BodyGeneration = ++NextBodyGeneration;
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
			m_Description.MaxSubsteps == 0 || m_Description.MaxBodies == 0 || m_Description.SessionGeneration == 0 ||
			!IsFinite(m_Description.Gravity))
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

	bool PhysicsSceneRuntime::IsBodyActiveForTesting(UUID Entity) const noexcept
	{
		if (!m_Impl) return false;
		const auto Body = m_Impl->Bodies.find(Entity);
		return Body != m_Impl->Bodies.end() && m_Impl->System.GetBodyInterface().IsActive(Body->second.ID);
	}

	#if defined(PF_DEBUG)
	bool PhysicsSceneRuntime::OverflowContactRemovalQueueForTesting(UUID First, UUID Second) noexcept
	{
		if (!m_Impl) return false;
		const auto FirstBody = m_Impl->Bodies.find(First);
		const auto SecondBody = m_Impl->Bodies.find(Second);
		if (FirstBody == m_Impl->Bodies.end() || SecondBody == m_Impl->Bodies.end()) return false;

		try
		{
			std::array<std::jthread, 4> Producers;
			for (std::jthread& Producer : Producers)
			{
				Producer = std::jthread([this, FirstBodyID = FirstBody->second.ID, SecondBodyID = SecondBody->second.ID]()
				{
					for (size_t Index = 0; Index <= MaximumContactRecords / 4; ++Index)
						m_Impl->EnqueueRemoved(FirstBodyID, SecondBodyID);
				});
			}
		}
		catch (...)
		{
			return false;
		}
		return m_Impl->ContactOverflowed.load(std::memory_order_relaxed);
	}
	#endif

	std::expected<void, PhysicsSceneRuntimeError> PhysicsSceneRuntime::ApplyForce(UUID Entity, const glm::vec3& Force)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (!IsFinite(Force))
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidForce, "Applied force must be finite"));
		if (std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::WrongThread,
				"Physics operations are restricted to the scene runtime owner thread"));
		if (m_Impl->Unhealthy)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics runtime entered a controlled failure state after contact event overflow; stop and restart the runtime"));
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
		if (std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::WrongThread,
				"Physics scene runtime must be advanced on its owner thread"));
		if (m_Impl->Unhealthy)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics runtime entered a controlled failure state after contact event overflow; stop and restart the runtime"));
		if (auto Result = m_Impl->ReconcileBodies(Source); !Result)
			return std::unexpected(Result.error());
		m_Impl->BeganThisFrame.clear();
		m_Impl->ProcessContactRecords();
		if (m_Impl->ContactOverflowed.load(std::memory_order_relaxed))
		{
			m_Impl->Unhealthy = true;
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics contact event capacity was exceeded; delivery stopped to avoid losing terminal contact events"));
		}

		const double AcceptedDelta = std::min(DeltaSeconds, m_Description.MaxFrameDeltaSeconds);
		m_Impl->DroppedSimulationSeconds += DeltaSeconds - AcceptedDelta;
		if (DeltaSeconds > AcceptedDelta) ++m_Impl->CappedFrames;
		m_Impl->AccumulatorSeconds += AcceptedDelta;
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
			++m_Impl->FixedSteps;
			m_Impl->ProcessContactRecords();
			if (m_Impl->ContactOverflowed.load(std::memory_order_relaxed))
			{
				m_Impl->Unhealthy = true;
				return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
					"Physics contact event capacity was exceeded; delivery stopped to avoid losing terminal contact events"));
			}
			if (UpdateError != JPH::EPhysicsUpdateError::None)
			{
				(void)m_Impl->SynchronizeScene();
				return std::unexpected(MakeError(
					PhysicsSceneRuntimeErrorCode::SimulationFailed,
					"Jolt physics step exhausted a body-pair or contact capacity; increase the physics scene limits"));
			}
		}
		if (m_Impl->AccumulatorSeconds >= FixedStep)
		{
			const double Remainder = std::fmod(m_Impl->AccumulatorSeconds, FixedStep);
			m_Impl->DroppedSimulationSeconds += m_Impl->AccumulatorSeconds - Remainder;
			m_Impl->AccumulatorSeconds = Remainder;
			++m_Impl->CappedFrames;
		}
		if (Steps > 0)
		{
			if (auto Result = m_Impl->SynchronizeScene(); !Result)
				return std::unexpected(Result.error());
		}
		m_Impl->ProcessContactRecords();
		m_Impl->AppendStays(Steps > 0);
		if (m_Impl->ContactOverflowed.load(std::memory_order_relaxed))
		{
			m_Impl->Unhealthy = true;
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics contact event capacity was exceeded; delivery stopped to avoid losing terminal contact events"));
		}
		return Steps;
	}

	std::expected<std::optional<PhysicsQueryHit>, PhysicsSceneRuntimeError> PhysicsSceneRuntime::Raycast(
		const glm::vec3& Origin, const glm::vec3& Direction, float MaxDistance, PhysicsQueryFilter Filter) const
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::WrongThread,
				"Physics queries are restricted to the scene runtime owner thread"));
		const float DirectionLength = glm::length(Direction);
		if (!IsFinite(Origin) || !IsFinite(Direction) || !std::isfinite(MaxDistance) || MaxDistance <= 0.0f ||
			!std::isfinite(DirectionLength) || DirectionLength <= 0.000001f || Filter.ExcludedEntities.size() > 256)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidQuery,
				"Ray query requires a finite origin, non-zero direction, positive distance, and at most 256 exclusions"));
		if (m_Impl->Unhealthy)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics runtime is unavailable after contact queue overflow"));

		const glm::vec3 UnitDirection = Direction / DirectionLength;
		JPH::RRayCast Ray(JPH::RVec3(Origin.x, Origin.y, Origin.z),
			JPH::Vec3(UnitDirection.x, UnitDirection.y, UnitDirection.z) * MaxDistance);
		JPH::RayCastSettings Settings;
		FilteredRayCollector Collector(Filter);
		m_Impl->System.GetNarrowPhaseQuery().CastRay(Ray, Settings, Collector);
		if (!Collector.HadHit()) return std::optional<PhysicsQueryHit>{};

		const JPH::RayCastResult& Result = Collector.GetHit();
		const float Fraction = Result.mFraction;
		const glm::vec3 Position = Origin + UnitDirection * (MaxDistance * Fraction);
		glm::vec3 Normal(0.0f);
		JPH::BodyLockRead Lock(m_Impl->System.GetBodyLockInterface(), Result.mBodyID);
		if (Lock.Succeeded())
		{
			const JPH::Vec3 JoltNormal = Lock.GetBody().GetWorldSpaceSurfaceNormal(
				Result.mSubShapeID2, JPH::RVec3(Position.x, Position.y, Position.z));
			Normal = { JoltNormal.GetX(), JoltNormal.GetY(), JoltNormal.GetZ() };
		}
		const PhysicsBodyIdentity& Identity = Collector.GetIdentity();
		return std::optional<PhysicsQueryHit>{ PhysicsQueryHit{ Identity.BoundEntity, Identity.Identifier,
			Identity.SessionGeneration, Identity.BodyGeneration, Fraction, Position, Normal } };
	}

	std::expected<PhysicsOverlapResult, PhysicsSceneRuntimeError> PhysicsSceneRuntime::OverlapShape(
		const BoxColliderComponent& QueryShape, const glm::vec3& Position, const glm::quat& Rotation,
		PhysicsQueryFilter Filter, uint32_t MaxResults) const
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::WrongThread,
				"Physics queries are restricted to the scene runtime owner thread"));
		if (auto Validation = QueryShape.Validate(); !Validation)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidQuery, Validation.error().Message));
		if (!IsFinite(Position) || !IsFinite(Rotation) || std::abs(glm::dot(Rotation, Rotation) - 1.0f) > 0.001f ||
			Filter.ExcludedEntities.size() > 256 || MaxResults == 0 || MaxResults > MaximumQueryResults)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidQuery,
				"Overlap query requires a finite pose, normalized rotation, bounded exclusions, and 1-64 results"));
		if (m_Impl->Unhealthy)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics runtime is unavailable after contact queue overflow"));

		EffectiveColliderGeometry Geometry;
		Geometry.Shape = QueryShape.Shape;
		Geometry.BoxHalfExtents = QueryShape.HalfExtents;
		Geometry.SphereRadius = QueryShape.SphereRadius;
		Geometry.CapsuleRadius = QueryShape.CapsuleRadius;
		Geometry.CapsuleHalfCylinder = (QueryShape.CapsuleHeight - 2.0f * QueryShape.CapsuleRadius) * 0.5f;
		auto Shape = m_Impl->CreateShape(UUID{}, Geometry);
		if (!Shape) return std::unexpected(Shape.error());
		const JPH::RVec3 Base(Position.x, Position.y, Position.z);
		const JPH::RMat44 Transform = JPH::RMat44::sRotationTranslation(
			ToJolt(Rotation), JPH::RVec3(Position.x, Position.y, Position.z));
		JPH::CollideShapeSettings Settings;
		FilteredOverlapCollector Collector(Filter, MaxResults, Position);
		m_Impl->System.GetNarrowPhaseQuery().CollideShape(Shape->GetPtr(), JPH::Vec3::sReplicate(1.0f),
			Transform, Settings, Base, Collector);

		PhysicsOverlapResult Output;
		Output.Truncated = Collector.Truncated();
		Output.Hits.reserve(Collector.Hits().size());
		for (const auto& Hit : Collector.Hits())
			Output.Hits.push_back({ Hit.Identity.BoundEntity, Hit.Identity.Identifier, Hit.Identity.SessionGeneration,
				Hit.Identity.BodyGeneration, 0.0f, Hit.Position, Hit.Normal });
		std::sort(Output.Hits.begin(), Output.Hits.end(), [](const PhysicsQueryHit& First, const PhysicsQueryHit& Second)
		{ return UUIDLess(First.EntityIdentifier, Second.EntityIdentifier); });
		return Output;
	}

	std::expected<std::optional<PhysicsQueryHit>, PhysicsSceneRuntimeError> PhysicsSceneRuntime::CastShape(
		const BoxColliderComponent& QueryShape, const glm::vec3& Position, const glm::quat& Rotation,
		const glm::vec3& Direction, float MaxDistance, PhysicsQueryFilter Filter) const
	{
		if (!m_Impl)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::NotRunning, "Physics scene runtime is not started"));
		if (std::this_thread::get_id() != m_Impl->OwnerThread)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::WrongThread,
				"Physics queries are restricted to the scene runtime owner thread"));
		if (auto Validation = QueryShape.Validate(); !Validation)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidQuery, Validation.error().Message));
		const float DirectionLength = glm::length(Direction);
		if (!IsFinite(Position) || !IsFinite(Rotation) || std::abs(glm::dot(Rotation, Rotation) - 1.0f) > 0.001f ||
			!IsFinite(Direction) || !std::isfinite(DirectionLength) || DirectionLength <= 0.000001f ||
			!std::isfinite(MaxDistance) || MaxDistance <= 0.0f || Filter.ExcludedEntities.size() > 256)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::InvalidQuery,
				"Shape cast requires a finite pose and direction, positive distance, normalized rotation, and bounded exclusions"));
		if (m_Impl->Unhealthy)
			return std::unexpected(MakeError(PhysicsSceneRuntimeErrorCode::ContactQueueOverflow,
				"Physics runtime is unavailable after contact queue overflow"));

		EffectiveColliderGeometry Geometry;
		Geometry.Shape = QueryShape.Shape;
		Geometry.BoxHalfExtents = QueryShape.HalfExtents;
		Geometry.SphereRadius = QueryShape.SphereRadius;
		Geometry.CapsuleRadius = QueryShape.CapsuleRadius;
		Geometry.CapsuleHalfCylinder = (QueryShape.CapsuleHeight - 2.0f * QueryShape.CapsuleRadius) * 0.5f;
		auto Shape = m_Impl->CreateShape(UUID{}, Geometry);
		if (!Shape) return std::unexpected(Shape.error());
		const JPH::RMat44 Transform = JPH::RMat44::sRotationTranslation(
			ToJolt(Rotation), JPH::RVec3(Position.x, Position.y, Position.z));
		const glm::vec3 UnitDirection = Direction / DirectionLength;
		const JPH::Vec3 Motion(UnitDirection.x * MaxDistance, UnitDirection.y * MaxDistance, UnitDirection.z * MaxDistance);
		const JPH::RShapeCast Cast = JPH::RShapeCast::sFromWorldTransform(
			Shape->GetPtr(), JPH::Vec3::sReplicate(1.0f), Transform, Motion);
		JPH::ShapeCastSettings Settings;
		Settings.mReturnDeepestPoint = true;
		FilteredShapeCastCollector Collector(Filter);
		m_Impl->System.GetNarrowPhaseQuery().CastShape(
			Cast, Settings, JPH::RVec3(Position.x, Position.y, Position.z), Collector);
		if (!Collector.HadHit()) return std::optional<PhysicsQueryHit>{};

		const auto& Result = Collector.GetHit();
		const PhysicsBodyIdentity& Identity = Collector.GetIdentity();
		const glm::vec3 HitPosition = Position + UnitDirection * (MaxDistance * Result.mFraction);
		const JPH::Vec3 JoltNormal = -Result.mPenetrationAxis.Normalized();
		const glm::vec3 Normal(JoltNormal.GetX(), JoltNormal.GetY(), JoltNormal.GetZ());
		return std::optional<PhysicsQueryHit>{ PhysicsQueryHit{ Identity.BoundEntity, Identity.Identifier,
			Identity.SessionGeneration, Identity.BodyGeneration, Result.mFraction, HitPosition, Normal } };
	}

	std::vector<PhysicsContactEvent> PhysicsSceneRuntime::TakeContactEvents()
	{
		if (!m_Impl || std::this_thread::get_id() != m_Impl->OwnerThread) return {};
		return std::exchange(m_Impl->FrameEvents, {});
	}

	PhysicsSceneRuntimeStats PhysicsSceneRuntime::GetStats() const noexcept
	{
		if (!m_Impl) return {};
		return { m_Impl->FixedSteps, m_Impl->CappedFrames, m_Impl->DroppedSimulationSeconds,
			m_Impl->ContactOverflowCount.load(std::memory_order_relaxed) };
	}
}
