#include "TestSceneSupport.h"

#include <algorithm>
#include <array>

namespace
{
	void TestUUIDBehavior(TestRunner& Tests)
	{
		using namespace PulseForge;
		const auto Parsed = UUID::Parse("00112233-4455-6677-8899-aabbccddeeff");
		PF_CHECK(Tests, Parsed.has_value());
		PF_CHECK(Tests, Parsed && Parsed->ToString() == "00112233-4455-6677-8899-aabbccddeeff");
		PF_CHECK(Tests, Parsed && Parsed->GetHigh() == 0x0011223344556677ull);
		PF_CHECK(Tests, Parsed && Parsed->GetLow() == 0x8899aabbccddeeffull);
		PF_CHECK(Tests, !UUID::Parse("00112233-4455-6677-8899-aabbccddeefg").has_value());
		PF_CHECK(Tests, !UUID::Parse("00112233445566778899aabbccddeeff").has_value());

		const auto GeneratedFirst = UUID::Generate();
		const auto GeneratedSecond = UUID::Generate();
		PF_CHECK(Tests, GeneratedFirst.has_value() && GeneratedSecond.has_value());
		if (GeneratedFirst && GeneratedSecond)
		{
			PF_CHECK(Tests, !GeneratedFirst->IsNil());
			PF_CHECK(Tests, *GeneratedFirst != *GeneratedSecond);
			const auto ParsedGenerated = UUID::Parse(GeneratedFirst->ToString());
			PF_CHECK(Tests, ParsedGenerated && *ParsedGenerated == *GeneratedFirst);
			PF_CHECK(Tests, ((GeneratedFirst->GetHigh() >> 12) & 0xf) == 4);
			PF_CHECK(Tests, (GeneratedFirst->GetLow() >> 62) == 2);
		}
	}

	void TestSceneEntityAndHierarchy(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene TestScene;
		const UUID ParentId{ 0x1000000000000000ull, 1 };
		const UUID ChildId{ 0x2000000000000000ull, 2 };
		const UUID GrandchildId{ 0x3000000000000000ull, 3 };
		auto ParentResult = TestScene.CreateEntityWithUUID(ParentId, "Root");
		auto ChildResult = TestScene.CreateEntityWithUUID(ChildId, "Child");
		auto GrandchildResult = TestScene.CreateEntityWithUUID(GrandchildId, "Grandchild");
		PF_CHECK(Tests, ParentResult.has_value() && ChildResult.has_value() && GrandchildResult.has_value());
		if (!ParentResult || !ChildResult || !GrandchildResult) return;

		Entity Parent = *ParentResult;
		Entity Child = *ChildResult;
		Entity Grandchild = *GrandchildResult;
		PF_CHECK(Tests, Parent.IsValid() && Child.IsValid() && Grandchild.IsValid());
		PF_CHECK(Tests, TestScene.GetEntityCount() == 3);
		PF_CHECK(Tests, TestScene.FindEntity(ChildId) == Child);
		PF_CHECK(Tests, !TestScene.FindEntity(UUID{ 0x4000000000000000ull, 4 }).has_value());
		PF_CHECK(Tests, !TestScene.CreateEntityWithUUID(ChildId, "Duplicate UUID").has_value());
		PF_CHECK(Tests, !TestScene.CreateEntityWithUUID(UUID{}, "Nil UUID").has_value());

		auto ChildTag = Child.GetTag();
		PF_CHECK(Tests, ChildTag && ChildTag->Name == "Child");
		if (ChildTag)
		{
			ChildTag->Name = "Renamed child";
			PF_CHECK(Tests, Child.SetTag(*ChildTag).has_value());
		}

		TransformComponent ParentTransform;
		ParentTransform.Translation = { 10.0f, 0.0f, 0.0f };
		TransformComponent ChildTransform;
		ChildTransform.Translation = { 2.0f, 0.0f, 0.0f };
		TransformComponent GrandchildTransform;
		GrandchildTransform.Translation = { 1.0f, 0.0f, 0.0f };
		PF_CHECK(Tests, Parent.SetTransform(ParentTransform).has_value());
		PF_CHECK(Tests, Child.SetTransform(ChildTransform).has_value());
		PF_CHECK(Tests, Grandchild.SetTransform(GrandchildTransform).has_value());
		auto InvalidTransform = ChildTransform;
		InvalidTransform.Translation.x = std::numeric_limits<float>::infinity();
		const auto InvalidTransformResult = Child.SetTransform(InvalidTransform);
		PF_CHECK(Tests, !InvalidTransformResult.has_value());
		PF_CHECK(Tests, !InvalidTransformResult && InvalidTransformResult.error().Code == SceneErrorCode::InvalidTransform);
		InvalidTransform = ChildTransform;
		InvalidTransform.Rotation = glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
		PF_CHECK(Tests, !Child.SetTransform(InvalidTransform).has_value());
		InvalidTransform = ChildTransform;
		InvalidTransform.Rotation = glm::quat(2.0f, 0.0f, 0.0f, 0.0f);
		PF_CHECK(Tests, Child.SetTransform(InvalidTransform).has_value());
		const auto NormalizedTransform = Child.GetTransform();
		PF_CHECK(Tests, NormalizedTransform && glm::abs(glm::length(NormalizedTransform->Rotation) - 1.0f) < 0.0001f);
		PF_CHECK(Tests, Child.SetTransform(ChildTransform).has_value());
		PF_CHECK(Tests, Child.SetParent(Parent).has_value());
		PF_CHECK(Tests, Grandchild.SetParent(Child).has_value());

		const auto WorldMatrix = Grandchild.GetWorldMatrix();
		PF_CHECK(Tests, WorldMatrix.has_value());
		if (WorldMatrix)
		{
			const glm::vec4 WorldOrigin = *WorldMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
			PF_CHECK(Tests, glm::abs(WorldOrigin.x - 13.0f) < 0.0001f);
		}

		const auto ParentChildren = Parent.GetChildren();
		const auto ChildParent = Child.GetParent();
		PF_CHECK(Tests, ParentChildren && ParentChildren->size() == 1 && ParentChildren->front() == Child);
		PF_CHECK(Tests, ChildParent && ChildParent->has_value() && **ChildParent == Parent);
		const auto CycleResult = Parent.SetParent(Grandchild);
		PF_CHECK(Tests, !CycleResult.has_value());
		PF_CHECK(Tests, !CycleResult && CycleResult.error().Code == SceneErrorCode::ParentCycle);

		PF_CHECK(Tests, Grandchild.SetParent(Parent).has_value());
		const auto ChildChildrenAfterReparent = Child.GetChildren();
		const auto ParentChildrenAfterReparent = Parent.GetChildren();
		PF_CHECK(Tests, ChildChildrenAfterReparent && ChildChildrenAfterReparent->empty());
		PF_CHECK(Tests, ParentChildrenAfterReparent && ParentChildrenAfterReparent->size() == 2);
		CameraComponent DuplicateCamera;
		DuplicateCamera.VerticalFieldOfViewRadians = 1.0f;
		DuplicateCamera.IsPrimary = true;
		PF_CHECK(Tests, Child.SetCamera(DuplicateCamera).has_value());
		PF_CHECK(Tests, Child.SetAudioListener(AudioListenerComponent{ true }).has_value());
		const AssetID DuplicateMeshAsset{ 0x4000000000000000ull, 4 };
		PF_CHECK(Tests, Child.SetMeshRenderer(MeshRendererComponent{ DuplicateMeshAsset }).has_value());
		const auto InvalidMeshRenderer = Child.SetMeshRenderer(MeshRendererComponent{ UUID{} });
		PF_CHECK(Tests, !InvalidMeshRenderer.has_value());
		PF_CHECK(Tests, !InvalidMeshRenderer && InvalidMeshRenderer.error().Code == SceneErrorCode::InvalidAssetReference);

		auto DuplicateResult = TestScene.DuplicateEntity(Child);
		PF_CHECK(Tests, DuplicateResult.has_value());
		if (DuplicateResult)
		{
			const Entity Duplicate = *DuplicateResult;
			const auto DuplicateTag = Duplicate.GetTag();
			const auto DuplicateTransform = Duplicate.GetTransform();
			const auto DuplicatedCamera = Duplicate.GetCamera();
			const auto DuplicatedListener = Duplicate.GetAudioListener();
			const auto OriginalCamera = Child.GetCamera();
			const auto OriginalListener = Child.GetAudioListener();
			const auto DuplicatedMeshRenderer = Duplicate.GetMeshRenderer();
			const auto DuplicateParent = Duplicate.GetParent();
			PF_CHECK(Tests, Duplicate.GetUUID() != Child.GetUUID());
			PF_CHECK(Tests, DuplicateTag && DuplicateTag->Name == "Renamed child Copy");
			PF_CHECK(Tests, DuplicateTransform && glm::all(glm::equal(DuplicateTransform->Translation, ChildTransform.Translation)));
			PF_CHECK(Tests, DuplicatedCamera && DuplicatedCamera->has_value() &&
								DuplicatedCamera->value().VerticalFieldOfViewRadians == DuplicateCamera.VerticalFieldOfViewRadians &&
								!DuplicatedCamera->value().IsPrimary);
			PF_CHECK(Tests, OriginalCamera && OriginalCamera->has_value() && OriginalCamera->value().IsPrimary);
			PF_CHECK(Tests, DuplicatedListener && DuplicatedListener->has_value() && !DuplicatedListener->value().IsPrimary);
			PF_CHECK(Tests, OriginalListener && OriginalListener->has_value() && OriginalListener->value().IsPrimary);
			PF_CHECK(Tests, DuplicatedMeshRenderer && DuplicatedMeshRenderer->has_value() &&
								DuplicatedMeshRenderer->value().MeshAsset == DuplicateMeshAsset);
			PF_CHECK(Tests, DuplicateParent && DuplicateParent->has_value() && **DuplicateParent == Parent);
			PF_CHECK(Tests, Duplicate.RemoveMeshRenderer().has_value());
			const auto RemovedMeshRenderer = Duplicate.GetMeshRenderer();
			PF_CHECK(Tests, RemovedMeshRenderer && !RemovedMeshRenderer->has_value());
			const auto MissingMeshRendererRemoval = Duplicate.RemoveMeshRenderer();
			PF_CHECK(Tests, !MissingMeshRendererRemoval.has_value());
			PF_CHECK(Tests, !MissingMeshRendererRemoval && MissingMeshRendererRemoval.error().Code == SceneErrorCode::MissingComponent);
		}

		Scene NonPrimaryScene;
		auto NonPrimarySource = NonPrimaryScene.CreateEntity("Non-primary camera");
		PF_CHECK(Tests, NonPrimarySource.has_value());
		if (NonPrimarySource)
		{
			CameraComponent NonPrimaryCamera;
			PF_CHECK(Tests, NonPrimarySource->SetCamera(NonPrimaryCamera).has_value());
			PF_CHECK(Tests, NonPrimarySource->SetAudioListener(AudioListenerComponent{ false }).has_value());
			auto NonPrimaryDuplicate = NonPrimaryScene.DuplicateEntity(*NonPrimarySource);
			PF_CHECK(Tests, NonPrimaryDuplicate.has_value());
			if (NonPrimaryDuplicate)
			{
				const auto CopiedCamera = NonPrimaryDuplicate->GetCamera();
				const auto CopiedListener = NonPrimaryDuplicate->GetAudioListener();
				PF_CHECK(Tests, CopiedCamera && CopiedCamera->has_value() && !CopiedCamera->value().IsPrimary);
				PF_CHECK(Tests, CopiedListener && CopiedListener->has_value() && !CopiedListener->value().IsPrimary);
			}
		}

		Scene OtherScene;
		auto OtherEntityResult = OtherScene.CreateEntity("Other scene");
		PF_CHECK(Tests, OtherEntityResult.has_value());
		if (OtherEntityResult)
		{
			PF_CHECK(Tests, !Child.SetParent(*OtherEntityResult).has_value());
			PF_CHECK(Tests, !OtherScene.DestroyEntity(Child).has_value());
		}

		PF_CHECK(Tests, TestScene.DestroyEntity(Parent).has_value());
		PF_CHECK(Tests, !Parent.IsValid());
		PF_CHECK(Tests, Child.IsValid() && Grandchild.IsValid());
		const auto DetachedGrandchild = Grandchild.GetParent();
		PF_CHECK(Tests, DetachedGrandchild && !DetachedGrandchild->has_value());
		PF_CHECK(Tests, TestScene.GetEntityCount() == 3);
		PF_CHECK(Tests, !TestScene.DestroyEntity(Parent).has_value());

		const auto OrderedEntities = TestScene.GetEntities();
		PF_CHECK(Tests, std::is_sorted(OrderedEntities.begin(), OrderedEntities.end(),
							[](const Entity& First, const Entity& Second) { return First.GetUUID() < Second.GetUUID(); }));
	}

	void TestPhysicsComponentsAndPersistence(TestRunner& Tests)
	{
		using namespace PulseForge;
		RigidbodyComponent Rigidbody;
		PF_CHECK(Tests, Rigidbody.Validate().has_value());
		Rigidbody.Mass = std::numeric_limits<float>::infinity();
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());
		Rigidbody.Mass = 1.0f;
		Rigidbody.Friction = -0.1f;
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());
		Rigidbody.Friction = 0.2f;
		Rigidbody.Restitution = 1.1f;
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());
		Rigidbody.Restitution = 0.0f;
		Rigidbody.MotionType = static_cast<RigidbodyMotionType>(0xff);
		PF_CHECK(Tests, !Rigidbody.Validate().has_value());

		BoxColliderComponent Collider;
		PF_CHECK(Tests, Collider.Validate().has_value());
		Collider.HalfExtents.y = 0.0f;
		PF_CHECK(Tests, !Collider.Validate().has_value());
		Collider.HalfExtents = { 0.75f, 0.5f, 0.25f };

		Scene Source;
		const auto Body = Source.CreateEntity("Physics body");
		PF_CHECK(Tests, Body.has_value());
		if (!Body) return;

		Rigidbody = { RigidbodyMotionType::Dynamic, 3.0f, 0.6f, 0.15f, false };
		PF_CHECK(Tests, Body->SetRigidbody(Rigidbody).has_value());
		PF_CHECK(Tests, Body->SetBoxCollider(Collider).has_value());
		Rigidbody.Mass = std::numeric_limits<float>::quiet_NaN();
		const auto InvalidRigidbodySet = Body->SetRigidbody(Rigidbody);
		PF_CHECK(Tests, !InvalidRigidbodySet && InvalidRigidbodySet.error().Code == SceneErrorCode::InvalidPhysicsComponent);
		Collider.HalfExtents.y = 0.0f;
		const auto InvalidColliderSet = Body->SetBoxCollider(Collider);
		PF_CHECK(Tests, !InvalidColliderSet && InvalidColliderSet.error().Code == SceneErrorCode::InvalidPhysicsComponent);
		Collider.HalfExtents.y = 0.5f;

		const auto Duplicated = Source.DuplicateEntity(*Body);
		PF_CHECK(Tests, Duplicated.has_value());
		if (Duplicated)
		{
			const auto DuplicateRigidbody = Duplicated->GetRigidbody();
			const auto DuplicateCollider = Duplicated->GetBoxCollider();
			PF_CHECK(Tests, DuplicateRigidbody && DuplicateRigidbody->has_value() && DuplicateRigidbody->value().Mass == 3.0f);
			PF_CHECK(Tests,
				DuplicateCollider && DuplicateCollider->has_value() && DuplicateCollider->value().HalfExtents == Collider.HalfExtents);
		}

		const auto Serialized = SceneSerializer::Serialize(Source);
		PF_CHECK(Tests, Serialized.has_value());
		if (!Serialized) return;
		PF_CHECK(Tests, Serialized->find("\"version\": 7") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"motionType\": \"dynamic\"") != std::string::npos);
		PF_CHECK(Tests, Serialized->find("\"halfExtents\": [\n          0.75,") != std::string::npos);

		Scene Destination;
		PF_CHECK(Tests, SceneSerializer::Deserialize(*Serialized, Destination).has_value());
		const auto LoadedBody = Destination.FindEntity(Body->GetUUID());
		PF_CHECK(Tests, LoadedBody.has_value());
		if (LoadedBody)
		{
			const auto LoadedRigidbody = LoadedBody->GetRigidbody();
			const auto LoadedCollider = LoadedBody->GetBoxCollider();
			PF_CHECK(Tests, LoadedRigidbody && LoadedRigidbody->has_value() && LoadedRigidbody->value().Mass == 3.0f &&
								LoadedRigidbody->value().Friction == 0.6f && LoadedRigidbody->value().Restitution == 0.15f &&
								!LoadedRigidbody->value().AllowSleeping);
			PF_CHECK(Tests, LoadedCollider && LoadedCollider->has_value() && LoadedCollider->value().HalfExtents == Collider.HalfExtents);
		}
	}

	void TestPhysicsSceneRuntime(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene Source;
		const auto Floor = Source.CreateEntity("Floor");
		const auto FallingBody = Source.CreateEntity("Falling body");
		PF_CHECK(Tests, Floor.has_value() && FallingBody.has_value());
		if (!Floor || !FallingBody) return;

		TransformComponent FloorTransform;
		FloorTransform.Translation.y = -0.5f;
		PF_CHECK(Tests, Floor->SetTransform(FloorTransform).has_value());
		PF_CHECK(Tests, Floor->SetRigidbody({ RigidbodyMotionType::Static }).has_value());
		PF_CHECK(Tests, Floor->SetBoxCollider({ { 5.0f, 0.5f, 5.0f } }).has_value());

		TransformComponent FallingTransform;
		FallingTransform.Translation.y = 2.0f;
		PF_CHECK(Tests, FallingBody->SetTransform(FallingTransform).has_value());
		PF_CHECK(Tests, FallingBody->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
		PF_CHECK(Tests, FallingBody->SetBoxCollider({ { 0.5f, 0.5f, 0.5f } }).has_value());

		PhysicsSceneRuntimeDesc Description;
		Description.MaxSubsteps = 4;
		PhysicsSceneRuntime Runtime(Description);
		const auto BeforeStart = Runtime.Advance(Source, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, !BeforeStart && BeforeStart.error().Code == PhysicsSceneRuntimeErrorCode::NotRunning);
		const auto ForceBeforeStart = Runtime.ApplyForce(FallingBody->GetUUID(), { 0.0f, 10.0f, 0.0f });
		PF_CHECK(Tests, !ForceBeforeStart && ForceBeforeStart.error().Code == PhysicsSceneRuntimeErrorCode::NotRunning);
		PF_CHECK(Tests, Runtime.Start(Source).has_value());
		PF_CHECK(Tests, Runtime.IsRunning());
		const auto DuplicateStart = Runtime.Start(Source);
		PF_CHECK(Tests, !DuplicateStart && DuplicateStart.error().Code == PhysicsSceneRuntimeErrorCode::AlreadyRunning);

		Scene OtherScene;
		const auto DifferentScene = Runtime.Advance(OtherScene, Timestep(1.0 / 60.0));
		PF_CHECK(Tests, !DifferentScene && DifferentScene.error().Code == PhysicsSceneRuntimeErrorCode::DifferentScene);
		PhysicsSceneRuntime IndependentRuntime;
		PF_CHECK(Tests, IndependentRuntime.Start(OtherScene).has_value());
		const auto NegativeDelta = Runtime.Advance(Source, Timestep(-0.1));
		PF_CHECK(Tests, !NegativeDelta && NegativeDelta.error().Code == PhysicsSceneRuntimeErrorCode::InvalidDeltaTime);
		const auto NonFiniteDelta = Runtime.Advance(Source, Timestep(std::numeric_limits<double>::quiet_NaN()));
		PF_CHECK(Tests, !NonFiniteDelta && NonFiniteDelta.error().Code == PhysicsSceneRuntimeErrorCode::InvalidDeltaTime);

		constexpr double FixedStep = 1.0 / 60.0;
		const auto PartialStep = Runtime.Advance(Source, Timestep(FixedStep * 0.5));
		PF_CHECK(Tests, PartialStep && *PartialStep == 0);
		const auto FirstStep = Runtime.Advance(Source, Timestep(FixedStep * 0.5));
		PF_CHECK(Tests, FirstStep && *FirstStep == 1);
		const auto FirstUpdatedTransform = FallingBody->GetTransform();
		PF_CHECK(Tests, FirstUpdatedTransform && FirstUpdatedTransform->Translation.y < 2.0f);

		uint32_t TotalSteps = 1;
		for (uint32_t Index = 0; Index < 179; ++Index)
		{
			const auto Step = Runtime.Advance(Source, Timestep(FixedStep));
			if (!Step)
			{
				PF_CHECK(Tests, false);
				break;
			}
			TotalSteps += *Step;
		}
		PF_CHECK(Tests, TotalSteps == 180);
		const auto SettledTransform = FallingBody->GetTransform();
		PF_CHECK(Tests, SettledTransform && SettledTransform->Translation.y > 0.4f && SettledTransform->Translation.y < 0.7f);
		PF_CHECK(Tests, Runtime.ApplyForce(FallingBody->GetUUID(), { 0.0f, 100.0f, 0.0f }).has_value());
		const auto StaticForce = Runtime.ApplyForce(Floor->GetUUID(), { 0.0f, 100.0f, 0.0f });
		PF_CHECK(Tests, !StaticForce && StaticForce.error().Code == PhysicsSceneRuntimeErrorCode::StaticBody);
		const auto MissingBodyForce = Runtime.ApplyForce(UUID{ 0x5300000000000000ull, 3 }, { 0.0f, 1.0f, 0.0f });
		PF_CHECK(Tests, !MissingBodyForce && MissingBodyForce.error().Code == PhysicsSceneRuntimeErrorCode::MissingBody);
		const auto InvalidForce = Runtime.ApplyForce(FallingBody->GetUUID(), { std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f });
		PF_CHECK(Tests, !InvalidForce && InvalidForce.error().Code == PhysicsSceneRuntimeErrorCode::InvalidForce);
		PF_CHECK(Tests, Runtime.Advance(Source, Timestep(FixedStep)).has_value());
		const auto ForcedTransform = FallingBody->GetTransform();
		PF_CHECK(Tests, ForcedTransform && SettledTransform && ForcedTransform->Translation.y > SettledTransform->Translation.y);

		const auto CappedCatchup = Runtime.Advance(Source, Timestep(0.25));
		PF_CHECK(Tests, CappedCatchup && *CappedCatchup == Description.MaxSubsteps);

		const auto RuntimeAddedBody = Source.CreateEntity("Runtime-added body");
		PF_CHECK(Tests, RuntimeAddedBody.has_value());
		if (RuntimeAddedBody)
		{
			TransformComponent RuntimeAddedTransform;
			RuntimeAddedTransform.Translation.y = 4.0f;
			PF_CHECK(Tests, RuntimeAddedBody->SetTransform(RuntimeAddedTransform).has_value());
			PF_CHECK(Tests, RuntimeAddedBody->SetRigidbody({ RigidbodyMotionType::Dynamic, 1.0f, 0.5f, 0.0f, true }).has_value());
			PF_CHECK(Tests, RuntimeAddedBody->SetBoxCollider({ { 0.5f, 0.5f, 0.5f } }).has_value());

			const auto ReconciledWithoutStep = Runtime.Advance(Source, Timestep(0.0));
			PF_CHECK(Tests, ReconciledWithoutStep && *ReconciledWithoutStep == 0);
			PF_CHECK(Tests, Runtime.ApplyForce(RuntimeAddedBody->GetUUID(), { 0.0f, 120.0f, 0.0f }).has_value());
			PF_CHECK(Tests, Runtime.Advance(Source, Timestep(FixedStep)).has_value());
			const auto RuntimeAddedTransformAfterStep = RuntimeAddedBody->GetTransform();
			PF_CHECK(Tests, RuntimeAddedTransformAfterStep && RuntimeAddedTransformAfterStep->Translation.y > 4.0f);

			PF_CHECK(Tests, RuntimeAddedBody->RemoveRigidbody().has_value());
			const auto RejectedPartialPair = Runtime.Advance(Source, Timestep(0.0));
			PF_CHECK(Tests, !RejectedPartialPair && RejectedPartialPair.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			const auto RemovedPartialBodyForce = Runtime.ApplyForce(RuntimeAddedBody->GetUUID(), { 0.0f, 1.0f, 0.0f });
			PF_CHECK(Tests,
				!RemovedPartialBodyForce && RemovedPartialBodyForce.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			PF_CHECK(Tests, RuntimeAddedBody->RemoveBoxCollider().has_value());
			PF_CHECK(Tests, Runtime.Advance(Source, Timestep(0.0)).has_value());
		}

		const UUID RemovedEntityIdentifier = FallingBody->GetUUID();
		PF_CHECK(Tests, Source.DestroyEntity(*FallingBody).has_value());
		PF_CHECK(Tests, Runtime.Advance(Source, Timestep(0.0)).has_value());
		const auto RemovedEntityForce = Runtime.ApplyForce(RemovedEntityIdentifier, { 0.0f, 1.0f, 0.0f });
		PF_CHECK(Tests, !RemovedEntityForce && RemovedEntityForce.error().Code == PhysicsSceneRuntimeErrorCode::MissingBody);
		Runtime.Stop();
		PF_CHECK(Tests, !Runtime.IsRunning());
		const auto IndependentStep = IndependentRuntime.Advance(OtherScene, Timestep(FixedStep));
		PF_CHECK(Tests, IndependentStep && *IndependentStep == 1);
		IndependentRuntime.Stop();

		Scene InvalidScene;
		const auto Parent = InvalidScene.CreateEntity("Physics parent");
		const auto Child = InvalidScene.CreateEntity("Parented physics body");
		PF_CHECK(Tests, Parent.has_value() && Child.has_value());
		if (Parent && Child)
		{
			PF_CHECK(Tests, Child->SetParent(*Parent).has_value());
			PF_CHECK(Tests, Child->SetRigidbody({}).has_value());
			PF_CHECK(Tests, Child->SetBoxCollider({}).has_value());
			PhysicsSceneRuntime InvalidRuntime;
			const auto RejectedHierarchy = InvalidRuntime.Start(InvalidScene);
			PF_CHECK(Tests, !RejectedHierarchy && RejectedHierarchy.error().Code == PhysicsSceneRuntimeErrorCode::UnsupportedHierarchy);
			PF_CHECK(Tests, !InvalidRuntime.IsRunning());
		}

		Scene IncompleteScene;
		const auto IncompleteBody = IncompleteScene.CreateEntity("Incomplete physics body");
		PF_CHECK(Tests, IncompleteBody.has_value());
		if (IncompleteBody)
		{
			PF_CHECK(Tests, IncompleteBody->SetRigidbody({}).has_value());
			PhysicsSceneRuntime IncompleteRuntime;
			const auto RejectedIncompletePair = IncompleteRuntime.Start(IncompleteScene);
			PF_CHECK(Tests,
				!RejectedIncompletePair && RejectedIncompletePair.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			PF_CHECK(Tests, !IncompleteRuntime.IsRunning());
		}

		PhysicsSceneRuntime InvalidSettingsRuntime(PhysicsSceneRuntimeDesc{ 0.0, 0, 0.0, glm::vec3(0.0f) });
		const auto RejectedSettings = InvalidSettingsRuntime.Start(Source);
		PF_CHECK(Tests, !RejectedSettings && RejectedSettings.error().Code == PhysicsSceneRuntimeErrorCode::InvalidSettings);
		PhysicsSceneRuntime UnrepresentableStepRuntime(PhysicsSceneRuntimeDesc{ 1.0e100, 1, 1.0e101, glm::vec3(0.0f) });
		const auto RejectedStep = UnrepresentableStepRuntime.Start(Source);
		PF_CHECK(Tests, !RejectedStep && RejectedStep.error().Code == PhysicsSceneRuntimeErrorCode::InvalidSettings);
		PhysicsSceneRuntime UnderflowingStepRuntime(
			PhysicsSceneRuntimeDesc{ std::numeric_limits<double>::denorm_min(), 1, 1.0, glm::vec3(0.0f) });
		const auto RejectedUnderflowingStep = UnderflowingStepRuntime.Start(Source);
		PF_CHECK(
			Tests, !RejectedUnderflowingStep && RejectedUnderflowingStep.error().Code == PhysicsSceneRuntimeErrorCode::InvalidSettings);

		Scene ScaledExtentsOverflowScene;
		const auto OverflowingBody = ScaledExtentsOverflowScene.CreateEntity("Overflowing collider");
		PF_CHECK(Tests, OverflowingBody.has_value());
		if (OverflowingBody)
		{
			TransformComponent OverflowingTransform;
			OverflowingTransform.Scale = glm::vec3(std::numeric_limits<float>::max());
			PF_CHECK(Tests, OverflowingBody->SetTransform(OverflowingTransform).has_value());
			PF_CHECK(Tests, OverflowingBody->SetRigidbody({}).has_value());
			PF_CHECK(Tests, OverflowingBody->SetBoxCollider({ glm::vec3(std::numeric_limits<float>::max()) }).has_value());
			PhysicsSceneRuntime OverflowRuntime;
			const auto RejectedOverflow = OverflowRuntime.Start(ScaledExtentsOverflowScene);
			PF_CHECK(Tests, !RejectedOverflow && RejectedOverflow.error().Code == PhysicsSceneRuntimeErrorCode::InvalidPhysicsEntity);
			PF_CHECK(Tests, !OverflowRuntime.IsRunning());
		}
	}

	void TestPhysicsLiveReconciliation(TestRunner& Tests)
	{
		using namespace PulseForge;
		constexpr double FixedStep = 1.0 / 60.0;

		Scene TeleportScene;
		const auto TeleportedBody = TeleportScene.CreateEntity("Teleported body");
		PF_CHECK(Tests, TeleportedBody.has_value());
		if (!TeleportedBody) return;
		TransformComponent InitialTransform;
		InitialTransform.Translation.y = 4.0f;
		PF_CHECK(Tests, TeleportedBody->SetTransform(InitialTransform).has_value());
		PF_CHECK(Tests, TeleportedBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, TeleportedBody->SetBoxCollider(BoxColliderComponent{}).has_value());

		PhysicsSceneRuntimeDesc ZeroGravity;
		ZeroGravity.Gravity = glm::vec3(0.0f);
		PhysicsSceneRuntime TeleportRuntime(ZeroGravity);
		PF_CHECK(Tests, TeleportRuntime.Start(TeleportScene).has_value());
		for (int Index = 0; Index < 10; ++Index)
		{
			PF_CHECK(Tests, TeleportRuntime.ApplyForce(TeleportedBody->GetUUID(), { 60.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, TeleportRuntime.Advance(TeleportScene, Timestep(FixedStep)).has_value());
		}
		TransformComponent Teleport;
		Teleport.Translation = { 10.0f, 4.0f, 0.0f };
		PF_CHECK(Tests, TeleportedBody->SetTransform(Teleport).has_value());
		PF_CHECK(Tests, TeleportRuntime.Advance(TeleportScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, TeleportRuntime.Advance(TeleportScene, Timestep(FixedStep)).has_value());
		const auto TeleportedTransform = TeleportedBody->GetTransform();
		PF_CHECK(Tests, TeleportedTransform && TeleportedTransform->Translation.x > 10.1f);
		TeleportRuntime.Stop();

		Scene MassScene;
		const auto LightBody = MassScene.CreateEntity("Light body");
		const auto HeavyBody = MassScene.CreateEntity("Heavy body");
		PF_CHECK(Tests, LightBody && HeavyBody);
		if (!LightBody || !HeavyBody) return;
		TransformComponent HeavyTransform;
		HeavyTransform.Translation.x = 10.0f;
		PF_CHECK(Tests, HeavyBody->SetTransform(HeavyTransform).has_value());
		for (const Entity& Current : { *LightBody, *HeavyBody })
		{
			PF_CHECK(Tests, Current.SetRigidbody(RigidbodyComponent{}).has_value());
			PF_CHECK(Tests, Current.SetBoxCollider(BoxColliderComponent{}).has_value());
		}
		PhysicsSceneRuntimeDesc MassDescription;
		MassDescription.FixedStepSeconds = 0.1;
		MassDescription.MaxFrameDeltaSeconds = 0.2;
		MassDescription.Gravity = glm::vec3(0.0f);
		PhysicsSceneRuntime MassRuntime(MassDescription);
		PF_CHECK(Tests, MassRuntime.Start(MassScene).has_value());
		PF_CHECK(Tests, MassRuntime.ApplyForce(LightBody->GetUUID(), { 10.0f, 0.0f, 0.0f }).has_value());
		PF_CHECK(Tests, MassRuntime.ApplyForce(HeavyBody->GetUUID(), { 10.0f, 0.0f, 0.0f }).has_value());
		PF_CHECK(Tests, MassRuntime.Advance(MassScene, Timestep(0.1)).has_value());
		const auto LightBeforeMassEdit = LightBody->GetTransform();
		const auto HeavyBeforeMassEdit = HeavyBody->GetTransform();
		PF_CHECK(Tests, LightBeforeMassEdit && HeavyBeforeMassEdit);
		RigidbodyComponent HeavierConfiguration;
		HeavierConfiguration.Mass = 10.0f;
		PF_CHECK(Tests, HeavyBody->SetRigidbody(HeavierConfiguration).has_value());
		const auto MassReconciliation = MassRuntime.Advance(MassScene, Timestep(0.0));
		PF_CHECK(Tests, MassReconciliation && *MassReconciliation == 0);
		PF_CHECK(Tests, MassRuntime.ApplyForce(LightBody->GetUUID(), { 10.0f, 0.0f, 0.0f }).has_value());
		PF_CHECK(Tests, MassRuntime.ApplyForce(HeavyBody->GetUUID(), { 10.0f, 0.0f, 0.0f }).has_value());
		PF_CHECK(Tests, MassRuntime.Advance(MassScene, Timestep(0.1)).has_value());
		const auto LightAfterMassEdit = LightBody->GetTransform();
		const auto HeavyAfterMassEdit = HeavyBody->GetTransform();
		PF_CHECK(Tests, LightAfterMassEdit && HeavyAfterMassEdit && LightBeforeMassEdit && HeavyBeforeMassEdit);
		if (LightAfterMassEdit && HeavyAfterMassEdit && LightBeforeMassEdit && HeavyBeforeMassEdit)
		{
			const float LightDisplacement = LightAfterMassEdit->Translation.x - LightBeforeMassEdit->Translation.x;
			const float HeavyDisplacement = HeavyAfterMassEdit->Translation.x - HeavyBeforeMassEdit->Translation.x;
			PF_CHECK(Tests, LightDisplacement > HeavyDisplacement + 0.02f);
		}
		PF_CHECK(Tests, HeavyBody->SetRigidbody(RigidbodyComponent{ RigidbodyMotionType::Static }).has_value());
		PF_CHECK(Tests, MassRuntime.Advance(MassScene, Timestep(0.0)).has_value());
		const auto StaticAfterMotionChange = MassRuntime.ApplyForce(HeavyBody->GetUUID(), { 1.0f, 0.0f, 0.0f });
		PF_CHECK(Tests, !StaticAfterMotionChange && StaticAfterMotionChange.error().Code == PhysicsSceneRuntimeErrorCode::StaticBody);
		PF_CHECK(Tests, HeavyBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, MassRuntime.Advance(MassScene, Timestep(0.0)).has_value());
		PF_CHECK(Tests, MassRuntime.ApplyForce(HeavyBody->GetUUID(), { 1.0f, 0.0f, 0.0f }).has_value());

		const auto Parent = MassScene.CreateEntity("Physics parent");
		PF_CHECK(Tests, Parent.has_value());
		if (Parent)
		{
			PF_CHECK(Tests, HeavyBody->SetParent(*Parent).has_value());
			const auto RejectedParentedBody = MassRuntime.Advance(MassScene, Timestep(0.0));
			PF_CHECK(Tests,
				!RejectedParentedBody && RejectedParentedBody.error().Code == PhysicsSceneRuntimeErrorCode::UnsupportedHierarchy);
			PF_CHECK(Tests, HeavyBody->ClearParent().has_value());
			PF_CHECK(Tests, MassRuntime.Advance(MassScene, Timestep(0.0)).has_value());
			PF_CHECK(Tests, MassRuntime.ApplyForce(HeavyBody->GetUUID(), { 1.0f, 0.0f, 0.0f }).has_value());
		}
		MassRuntime.Stop();

		Scene FrictionScene;
		const auto FrictionFloor = FrictionScene.CreateEntity("Friction floor");
		const auto LowFrictionBody = FrictionScene.CreateEntity("Low friction body");
		const auto UpdatedFrictionBody = FrictionScene.CreateEntity("Updated friction body");
		PF_CHECK(Tests, FrictionFloor && LowFrictionBody && UpdatedFrictionBody);
		if (!FrictionFloor || !LowFrictionBody || !UpdatedFrictionBody) return;
		TransformComponent FrictionFloorTransform;
		FrictionFloorTransform.Translation.y = -0.5f;
		PF_CHECK(Tests, FrictionFloor->SetTransform(FrictionFloorTransform).has_value());
		PF_CHECK(Tests, FrictionFloor->SetRigidbody(RigidbodyComponent{ RigidbodyMotionType::Static, 1.0f, 1.0f }).has_value());
		PF_CHECK(Tests, FrictionFloor->SetBoxCollider(BoxColliderComponent{ { 100.0f, 0.5f, 2.0f } }).has_value());
		TransformComponent LowFrictionTransform;
		LowFrictionTransform.Translation = { -5.0f, 0.5f, 0.0f };
		PF_CHECK(Tests, LowFrictionBody->SetTransform(LowFrictionTransform).has_value());
		TransformComponent UpdatedFrictionTransform;
		UpdatedFrictionTransform.Translation = { 5.0f, 0.5f, 0.0f };
		PF_CHECK(Tests, UpdatedFrictionBody->SetTransform(UpdatedFrictionTransform).has_value());
		const RigidbodyComponent NoFriction{ RigidbodyMotionType::Dynamic, 1.0f, 0.0f, 0.0f, false };
		PF_CHECK(Tests, LowFrictionBody->SetRigidbody(NoFriction).has_value());
		PF_CHECK(Tests, UpdatedFrictionBody->SetRigidbody(NoFriction).has_value());
		PF_CHECK(Tests, LowFrictionBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, UpdatedFrictionBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PhysicsSceneRuntime FrictionRuntime;
		PF_CHECK(Tests, FrictionRuntime.Start(FrictionScene).has_value());
		PF_CHECK(Tests, FrictionRuntime.Advance(FrictionScene, Timestep(FixedStep * 2.0)).has_value());
		for (int Index = 0; Index < 10; ++Index)
		{
			PF_CHECK(Tests, FrictionRuntime.ApplyForce(LowFrictionBody->GetUUID(), { 20.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, FrictionRuntime.ApplyForce(UpdatedFrictionBody->GetUUID(), { 20.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, FrictionRuntime.Advance(FrictionScene, Timestep(FixedStep)).has_value());
		}
		const auto LowFrictionBeforeEdit = LowFrictionBody->GetTransform();
		const auto UpdatedFrictionBeforeEdit = UpdatedFrictionBody->GetTransform();
		PF_CHECK(Tests, LowFrictionBeforeEdit && UpdatedFrictionBeforeEdit);
		RigidbodyComponent HighFriction = NoFriction;
		HighFriction.Friction = 1.0f;
		PF_CHECK(Tests, UpdatedFrictionBody->SetRigidbody(HighFriction).has_value());
		PF_CHECK(Tests, FrictionRuntime.Advance(FrictionScene, Timestep(0.0)).has_value());
		for (int Index = 0; Index < 60; ++Index)
			PF_CHECK(Tests, FrictionRuntime.Advance(FrictionScene, Timestep(FixedStep)).has_value());
		const auto LowFrictionAfterSlide = LowFrictionBody->GetTransform();
		const auto UpdatedFrictionAfterSlide = UpdatedFrictionBody->GetTransform();
		PF_CHECK(Tests,
			LowFrictionBeforeEdit && UpdatedFrictionBeforeEdit && LowFrictionAfterSlide && UpdatedFrictionAfterSlide);
		if (LowFrictionBeforeEdit && UpdatedFrictionBeforeEdit && LowFrictionAfterSlide && UpdatedFrictionAfterSlide)
		{
			const float LowFrictionTravel = LowFrictionAfterSlide->Translation.x - LowFrictionBeforeEdit->Translation.x;
			const float UpdatedFrictionTravel =
				UpdatedFrictionAfterSlide->Translation.x - UpdatedFrictionBeforeEdit->Translation.x;
			PF_CHECK(Tests, LowFrictionTravel > UpdatedFrictionTravel + 0.5f);
		}
		FrictionRuntime.Stop();

		Scene RestitutionScene;
		const auto RestitutionFloor = RestitutionScene.CreateEntity("Restitution floor");
		const auto BouncingBody = RestitutionScene.CreateEntity("Bouncing body");
		PF_CHECK(Tests, RestitutionFloor && BouncingBody);
		if (!RestitutionFloor || !BouncingBody) return;
		TransformComponent RestitutionFloorTransform;
		RestitutionFloorTransform.Translation.y = -0.5f;
		PF_CHECK(Tests, RestitutionFloor->SetTransform(RestitutionFloorTransform).has_value());
		PF_CHECK(Tests, RestitutionFloor->SetRigidbody(RigidbodyComponent{ RigidbodyMotionType::Static, 1.0f, 0.0f }).has_value());
		PF_CHECK(Tests, RestitutionFloor->SetBoxCollider(BoxColliderComponent{ { 5.0f, 0.5f, 5.0f } }).has_value());
		TransformComponent BouncingTransform;
		BouncingTransform.Translation.y = 2.0f;
		PF_CHECK(Tests, BouncingBody->SetTransform(BouncingTransform).has_value());
		PF_CHECK(Tests, BouncingBody->SetRigidbody(RigidbodyComponent{ RigidbodyMotionType::Dynamic, 1.0f, 0.0f, 0.0f }).has_value());
		PF_CHECK(Tests, BouncingBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PhysicsSceneRuntime RestitutionRuntime;
		PF_CHECK(Tests, RestitutionRuntime.Start(RestitutionScene).has_value());
		for (int Index = 0; Index < 20; ++Index)
			PF_CHECK(Tests, RestitutionRuntime.Advance(RestitutionScene, Timestep(FixedStep)).has_value());
		RigidbodyComponent BouncyConfiguration{ RigidbodyMotionType::Dynamic, 1.0f, 0.0f, 1.0f, false };
		PF_CHECK(Tests, BouncingBody->SetRigidbody(BouncyConfiguration).has_value());
		PF_CHECK(Tests, RestitutionRuntime.Advance(RestitutionScene, Timestep(0.0)).has_value());
		for (int Index = 0; Index < 28; ++Index)
			PF_CHECK(Tests, RestitutionRuntime.Advance(RestitutionScene, Timestep(FixedStep)).has_value());
		const auto BouncedTransform = BouncingBody->GetTransform();
		PF_CHECK(Tests, BouncedTransform && BouncedTransform->Translation.y > 0.6f);
		RestitutionRuntime.Stop();

		Scene StaticEditScene;
		const auto Floor = StaticEditScene.CreateEntity("Movable floor");
		const auto FallingBody = StaticEditScene.CreateEntity("Floor test body");
		PF_CHECK(Tests, Floor && FallingBody);
		if (!Floor || !FallingBody) return;
		TransformComponent InitialFloor;
		InitialFloor.Translation = { 2.0f, -10.0f, 0.0f };
		PF_CHECK(Tests, Floor->SetTransform(InitialFloor).has_value());
		PF_CHECK(Tests, Floor->SetRigidbody(RigidbodyComponent{ RigidbodyMotionType::Static }).has_value());
		PF_CHECK(Tests, Floor->SetBoxCollider(BoxColliderComponent{ { 0.25f, 0.5f, 2.0f } }).has_value());
		TransformComponent InitialFallingBody;
		InitialFallingBody.Translation = { 2.0f, 2.0f, 0.0f };
		PF_CHECK(Tests, FallingBody->SetTransform(InitialFallingBody).has_value());
		PF_CHECK(Tests, FallingBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, FallingBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PhysicsSceneRuntime StaticEditRuntime;
		PF_CHECK(Tests, StaticEditRuntime.Start(StaticEditScene).has_value());
		for (int Index = 0; Index < 60; ++Index)
			PF_CHECK(Tests, StaticEditRuntime.Advance(StaticEditScene, Timestep(FixedStep)).has_value());
		const auto FallenBeforeFloorMove = FallingBody->GetTransform();
		PF_CHECK(Tests, FallenBeforeFloorMove && FallenBeforeFloorMove->Translation.y < -1.0f);
		TransformComponent MovedFloor = InitialFloor;
		MovedFloor.Translation.y = 0.0f;
		PF_CHECK(Tests, Floor->SetTransform(MovedFloor).has_value());
		TransformComponent ResetAboveMovedFloor;
		ResetAboveMovedFloor.Translation = { 2.0f, 2.0f, 0.0f };
		PF_CHECK(Tests, StaticEditScene.DestroyEntity(*FallingBody).has_value());
		const auto BodyAboveMovedFloor = StaticEditScene.CreateEntity("Body above moved floor");
		PF_CHECK(Tests, BodyAboveMovedFloor.has_value());
		if (!BodyAboveMovedFloor) return;
		PF_CHECK(Tests, BodyAboveMovedFloor->SetTransform(ResetAboveMovedFloor).has_value());
		PF_CHECK(Tests, BodyAboveMovedFloor->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, BodyAboveMovedFloor->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, StaticEditRuntime.Advance(StaticEditScene, Timestep(0.0)).has_value());
		for (int Index = 0; Index < 60; ++Index)
			PF_CHECK(Tests, StaticEditRuntime.Advance(StaticEditScene, Timestep(FixedStep)).has_value());
		const auto SettledAfterFloorMove = BodyAboveMovedFloor->GetTransform();
		PF_CHECK(Tests,
			SettledAfterFloorMove && SettledAfterFloorMove->Translation.y > 0.9f && SettledAfterFloorMove->Translation.y < 1.1f);
		StaticEditRuntime.Stop();

		Scene ShapeEditScene;
		const auto ShapeEditFloor = ShapeEditScene.CreateEntity("Shape edited floor");
		const auto BodyOutsideFloor = ShapeEditScene.CreateEntity("Body outside floor");
		PF_CHECK(Tests, ShapeEditFloor && BodyOutsideFloor);
		if (!ShapeEditFloor || !BodyOutsideFloor) return;
		TransformComponent ShapeEditFloorTransform;
		ShapeEditFloorTransform.Translation = { 2.0f, 0.0f, 0.0f };
		PF_CHECK(Tests, ShapeEditFloor->SetTransform(ShapeEditFloorTransform).has_value());
		PF_CHECK(Tests, ShapeEditFloor->SetRigidbody(RigidbodyComponent{ RigidbodyMotionType::Static }).has_value());
		PF_CHECK(Tests, ShapeEditFloor->SetBoxCollider(BoxColliderComponent{ { 0.25f, 0.5f, 2.0f } }).has_value());
		TransformComponent OutsideSmallFloor;
		OutsideSmallFloor.Translation = { 3.0f, 2.0f, 0.0f };
		PF_CHECK(Tests, BodyOutsideFloor->SetTransform(OutsideSmallFloor).has_value());
		PF_CHECK(Tests, BodyOutsideFloor->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, BodyOutsideFloor->SetBoxCollider(BoxColliderComponent{ { 0.4f, 0.5f, 2.0f } }).has_value());
		PhysicsSceneRuntime ShapeEditRuntime;
		PF_CHECK(Tests, ShapeEditRuntime.Start(ShapeEditScene).has_value());
		for (int Index = 0; Index < 60; ++Index)
			PF_CHECK(Tests, ShapeEditRuntime.Advance(ShapeEditScene, Timestep(FixedStep)).has_value());
		const auto FallenOutsideSmallFloor = BodyOutsideFloor->GetTransform();
		PF_CHECK(Tests, FallenOutsideSmallFloor && FallenOutsideSmallFloor->Translation.y < -1.0f);

		TransformComponent ScaledFloor = ShapeEditFloorTransform;
		ScaledFloor.Scale.x = 2.0f;
		PF_CHECK(Tests, ShapeEditFloor->SetTransform(ScaledFloor).has_value());
		PF_CHECK(Tests, ShapeEditFloor->SetBoxCollider(BoxColliderComponent{ { 0.4f, 0.5f, 2.0f } }).has_value());
		TransformComponent ResetFallingBody;
		ResetFallingBody.Translation = { 2.7f, 2.0f, 0.0f };
		const auto BodyAboveEditedFloor = ShapeEditScene.CreateEntity("Body above edited floor");
		PF_CHECK(Tests, BodyAboveEditedFloor.has_value());
		if (!BodyAboveEditedFloor) return;
		PF_CHECK(Tests, BodyAboveEditedFloor->SetTransform(ResetFallingBody).has_value());
		PF_CHECK(Tests, BodyAboveEditedFloor->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, BodyAboveEditedFloor->SetBoxCollider(BoxColliderComponent{ { 0.4f, 0.5f, 2.0f } }).has_value());
		PF_CHECK(Tests, ShapeEditRuntime.Advance(ShapeEditScene, Timestep(0.0)).has_value());
		for (int Index = 0; Index < 90; ++Index)
			PF_CHECK(Tests, ShapeEditRuntime.Advance(ShapeEditScene, Timestep(FixedStep)).has_value());
		const auto SettledAfterShapeEdit = BodyAboveEditedFloor->GetTransform();
		PF_CHECK(Tests,
			SettledAfterShapeEdit && SettledAfterShapeEdit->Translation.y > 0.9f && SettledAfterShapeEdit->Translation.y < 1.1f);
		ShapeEditRuntime.Stop();

		Scene IdentityScene;
		const UUID ReusedIdentifier{ 0x5100000000000000ull, 99 };
		const auto OriginalBody = IdentityScene.CreateEntityWithUUID(ReusedIdentifier, "Original physics incarnation");
		PF_CHECK(Tests, OriginalBody.has_value());
		if (!OriginalBody) return;
		PF_CHECK(Tests, OriginalBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, OriginalBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PhysicsSceneRuntime IdentityRuntime(ZeroGravity);
		PF_CHECK(Tests, IdentityRuntime.Start(IdentityScene).has_value());
		for (int Index = 0; Index < 10; ++Index)
		{
			PF_CHECK(Tests, IdentityRuntime.ApplyForce(ReusedIdentifier, { 60.0f, 0.0f, 0.0f }).has_value());
			PF_CHECK(Tests, IdentityRuntime.Advance(IdentityScene, Timestep(FixedStep)).has_value());
		}
		const auto OriginalMoved = OriginalBody->GetTransform();
		PF_CHECK(Tests, OriginalMoved && OriginalMoved->Translation.x > 0.5f);
		PF_CHECK(Tests, IdentityScene.DestroyEntity(*OriginalBody).has_value());
		const auto ReplacementBody = IdentityScene.CreateEntityWithUUID(ReusedIdentifier, "Replacement physics incarnation");
		PF_CHECK(Tests, ReplacementBody.has_value());
		if (!ReplacementBody) return;
		TransformComponent ReplacementTransform;
		ReplacementTransform.Translation.x = 10.0f;
		PF_CHECK(Tests, ReplacementBody->SetTransform(ReplacementTransform).has_value());
		PF_CHECK(Tests, ReplacementBody->SetRigidbody(RigidbodyComponent{}).has_value());
		PF_CHECK(Tests, ReplacementBody->SetBoxCollider(BoxColliderComponent{}).has_value());
		PF_CHECK(Tests, IdentityRuntime.Advance(IdentityScene, Timestep(0.1)).has_value());
		const auto ReplacementAfterStep = ReplacementBody->GetTransform();
		PF_CHECK(Tests, ReplacementAfterStep && std::abs(ReplacementAfterStep->Translation.x - 10.0f) < 0.001f);
		IdentityRuntime.Stop();
	}

	void TestEntityHandleIdentityAndLifetime(TestRunner& Tests)
	{
		using namespace PulseForge;
		Entity StaleHandle;
		{
			Scene TemporaryScene;
			auto Created = TemporaryScene.CreateEntity("Temporary");
			PF_CHECK(Tests, Created.has_value());
			if (Created) StaleHandle = *Created;
			PF_CHECK(Tests, StaleHandle.IsValid());
		}
		PF_CHECK(Tests, !StaleHandle.IsValid());
		const auto DestroyedSceneTag = StaleHandle.GetTag();
		PF_CHECK(Tests, !DestroyedSceneTag && DestroyedSceneTag.error().Code == SceneErrorCode::InvalidEntity);

		Scene ReuseScene;
		const UUID ReusedIdentifier{ 0x6f00000000000000ull, 42 };
		auto Original = ReuseScene.CreateEntityWithUUID(ReusedIdentifier, "Original incarnation");
		PF_CHECK(Tests, Original.has_value());
		if (!Original) return;

		const Entity OldHandle = *Original;
		PF_CHECK(Tests, OldHandle.IsValid());
		PF_CHECK(Tests, ReuseScene.DestroyEntity(OldHandle).has_value());
		PF_CHECK(Tests, !OldHandle.IsValid());
		PF_CHECK(Tests, !ReuseScene.FindEntity(ReusedIdentifier).has_value());
		const auto DestroyedTag = OldHandle.GetTag();
		PF_CHECK(Tests, !DestroyedTag && DestroyedTag.error().Code == SceneErrorCode::InvalidEntity);
		const auto DestroyedTransform = OldHandle.GetTransform();
		PF_CHECK(Tests, !DestroyedTransform && DestroyedTransform.error().Code == SceneErrorCode::InvalidEntity);
		const auto StaleTagSet = OldHandle.SetTag(TagComponent{ "Must not reach replacement" });
		PF_CHECK(Tests, !StaleTagSet && StaleTagSet.error().Code == SceneErrorCode::InvalidEntity);
		const auto StaleTransformSet = OldHandle.SetTransform(TransformComponent{});
		PF_CHECK(Tests, !StaleTransformSet && StaleTransformSet.error().Code == SceneErrorCode::InvalidEntity);
		const auto StaleDestroy = ReuseScene.DestroyEntity(OldHandle);
		PF_CHECK(Tests, !StaleDestroy && StaleDestroy.error().Code == SceneErrorCode::InvalidEntity);

		auto Replacement = ReuseScene.CreateEntityWithUUID(ReusedIdentifier, "Replacement incarnation");
		PF_CHECK(Tests, Replacement.has_value());
		if (!Replacement) return;
		const Entity NewHandle = *Replacement;
		PF_CHECK(Tests, NewHandle.IsValid());
		PF_CHECK(Tests, OldHandle.GetUUID() == NewHandle.GetUUID());
		PF_CHECK(Tests, OldHandle != NewHandle);
		PF_CHECK(Tests, !OldHandle.IsValid());
		PF_CHECK(Tests, ReuseScene.FindEntity(ReusedIdentifier) == NewHandle);
		const auto ReplacementTag = NewHandle.GetTag();
		PF_CHECK(Tests, ReplacementTag && ReplacementTag->Name == "Replacement incarnation");
		const auto StillStaleTag = OldHandle.GetTag();
		PF_CHECK(Tests, !StillStaleTag && StillStaleTag.error().Code == SceneErrorCode::InvalidEntity);
		PF_CHECK(Tests, NewHandle.SetTag(TagComponent{ "Replacement works" }).has_value());
		const auto UpdatedReplacementTag = NewHandle.GetTag();
		PF_CHECK(Tests, UpdatedReplacementTag && UpdatedReplacementTag->Name == "Replacement works");

		PF_CHECK(Tests, ReuseScene.DestroyEntity(NewHandle).has_value());
		bool ReusePreservedStaleHandle = true;
		// Exercise beyond EnTT's current packed-entity generation cycle for one repeatedly reused slot.
		for (uint32_t Reuse = 0; Reuse < 5000; ++Reuse)
		{
			auto RecycledEntity = ReuseScene.CreateEntityWithUUID(ReusedIdentifier, "Recycled incarnation");
			if (!RecycledEntity)
			{
				ReusePreservedStaleHandle = false;
				break;
			}
			if (!RecycledEntity->IsValid() || OldHandle.IsValid() || !ReuseScene.DestroyEntity(*RecycledEntity))
			{
				ReusePreservedStaleHandle = false;
				break;
			}
		}
		PF_CHECK(Tests, ReusePreservedStaleHandle);
	}

	void TestSceneRenderSnapshot(TestRunner& Tests)
	{
		using namespace PulseForge;
		Scene TestScene;
		const UUID CameraID{ 0x1000000000000000ull, 1 };
		const UUID ParentID{ 0x2000000000000000ull, 2 };
		const UUID FirstMeshID{ 0x3000000000000000ull, 3 };
		const UUID SharedMeshID{ 0x3800000000000000ull, 38 };
		const UUID SecondMeshID{ 0x4000000000000000ull, 4 };
		const UUID LightID{ 0x7000000000000000ull, 7 };
		const AssetID FirstMeshAssetID{ 0x5000000000000000ull, 5 };
		const AssetID SecondMeshAssetID{ 0x6000000000000000ull, 6 };
		const AssetID MaterialAssetID{ 0x6100000000000000ull, 61 };
		const AssetID EnvironmentAssetID{ 0x6200000000000000ull, 62 };
		auto Camera = TestScene.CreateEntityWithUUID(CameraID, "Camera");
		auto Parent = TestScene.CreateEntityWithUUID(ParentID, "Parent");
		auto FirstMesh = TestScene.CreateEntityWithUUID(FirstMeshID, "First mesh");
		auto SharedMesh = TestScene.CreateEntityWithUUID(SharedMeshID, "Shared mesh instance");
		auto SecondMesh = TestScene.CreateEntityWithUUID(SecondMeshID, "Second mesh");
		auto Light = TestScene.CreateEntityWithUUID(LightID, "Sun");
		PF_CHECK(Tests, Camera && Parent && FirstMesh && SharedMesh && SecondMesh && Light);
		if (!Camera || !Parent || !FirstMesh || !SharedMesh || !SecondMesh || !Light) return;

		TransformComponent CameraTransform;
		CameraTransform.Translation = { 0.0f, 0.0f, 5.0f };
		CameraComponent PrimaryCameraComponent;
		PrimaryCameraComponent.IsPrimary = true;
		PF_CHECK(Tests, Camera->SetTransform(CameraTransform).has_value());
		PF_CHECK(Tests, Camera->SetCamera(PrimaryCameraComponent).has_value());
		TransformComponent LightTransform;
		LightTransform.Rotation = glm::angleAxis(glm::half_pi<float>(), glm::vec3(0.0f, 1.0f, 0.0f));
		PF_CHECK(Tests, Light->SetTransform(LightTransform).has_value());
		PF_CHECK(Tests, Light->SetDirectionalLight(DirectionalLightComponent{ glm::vec3(0.5f, 0.6f, 0.7f), 2.0f }).has_value());
		PF_CHECK(Tests, Light->SetPointLight(PointLightComponent{ glm::vec3(1.0f, 0.5f, 0.25f), 4.0f, 12.0f }).has_value());
		SpotLightComponent SnapshotSpotLight{ glm::vec3(0.2f, 0.4f, 1.0f), 8.0f, 20.0f, 10.0f, 25.0f };
		SnapshotSpotLight.CastShadows = true;
		SnapshotSpotLight.ShadowBias = 0.004f;
		PF_CHECK(Tests, Light->SetSpotLight(SnapshotSpotLight).has_value());
		PointLightComponent InvalidPointLight;
		InvalidPointLight.Intensity = -1.0f;
		const auto InvalidPointLightSet = Light->SetPointLight(InvalidPointLight);
		PF_CHECK(Tests, !InvalidPointLightSet && InvalidPointLightSet.error().Code == SceneErrorCode::InvalidPointLight);
		SpotLightComponent InvalidSpotLight;
		InvalidSpotLight.OuterConeAngleDegrees = InvalidSpotLight.InnerConeAngleDegrees;
		const auto InvalidSpotLightSet = Light->SetSpotLight(InvalidSpotLight);
		PF_CHECK(Tests, !InvalidSpotLightSet && InvalidSpotLightSet.error().Code == SceneErrorCode::InvalidSpotLight);
		const auto ExistingPointLight = Light->GetPointLight();
		const auto ExistingSpotLight = Light->GetSpotLight();
		PF_CHECK(Tests, ExistingPointLight && ExistingPointLight->has_value() && ExistingPointLight->value().Intensity == 4.0f);
		PF_CHECK(Tests, ExistingSpotLight && ExistingSpotLight->has_value() && ExistingSpotLight->value().Intensity == 8.0f);
		auto DuplicateLightEntity = TestScene.DuplicateEntity(*Light);
		PF_CHECK(Tests, DuplicateLightEntity.has_value());
		if (DuplicateLightEntity)
		{
			const auto CopiedLight = DuplicateLightEntity->GetDirectionalLight();
			PF_CHECK(Tests, CopiedLight && CopiedLight->has_value() && CopiedLight->value().Intensity == 2.0f);
			const auto CopiedPoint = DuplicateLightEntity->GetPointLight();
			const auto CopiedSpot = DuplicateLightEntity->GetSpotLight();
			PF_CHECK(Tests, CopiedPoint && CopiedPoint->has_value() && CopiedPoint->value().Range == 12.0f);
			PF_CHECK(Tests, CopiedSpot && CopiedSpot->has_value() && CopiedSpot->value().OuterConeAngleDegrees == 25.0f);
			(void)TestScene.DestroyEntity(*DuplicateLightEntity);
		}
		DirectionalLightComponent InvalidLight;
		InvalidLight.Intensity = -1.0f;
		const auto InvalidLightSet = Light->SetDirectionalLight(InvalidLight);
		PF_CHECK(Tests, !InvalidLightSet && InvalidLightSet.error().Code == SceneErrorCode::InvalidDirectionalLight);

		TransformComponent ParentTransform;
		ParentTransform.Translation = { 2.0f, 1.0f, 0.0f };
		TransformComponent FirstMeshTransform;
		FirstMeshTransform.Translation = { -1.0f, 0.0f, -2.0f };
		TransformComponent SharedMeshTransform;
		SharedMeshTransform.Translation = { 3.0f, 0.0f, -1.0f };
		TransformComponent SecondMeshTransform;
		SecondMeshTransform.Translation = { 0.0f, -1.0f, -4.0f };
		PF_CHECK(Tests, Parent->SetTransform(ParentTransform).has_value());
		PF_CHECK(Tests, Light->SetParent(*Parent).has_value());
		PF_CHECK(Tests, Light->SetEnvironmentLight(EnvironmentLightComponent{ EnvironmentAssetID, 1.5f }).has_value());
		PF_CHECK(Tests, FirstMesh->SetTransform(FirstMeshTransform).has_value());
		PF_CHECK(Tests, SharedMesh->SetTransform(SharedMeshTransform).has_value());
		PF_CHECK(Tests, SecondMesh->SetTransform(SecondMeshTransform).has_value());
		PF_CHECK(Tests, FirstMesh->SetParent(*Parent).has_value());
		PF_CHECK(Tests, FirstMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID, MaterialAssetID }).has_value());
		PF_CHECK(Tests, FirstMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID, std::nullopt }).has_value());
		PF_CHECK(Tests, SharedMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID, MaterialAssetID }).has_value());
		PF_CHECK(Tests, SecondMesh->SetMeshRenderer(MeshRendererComponent{ SecondMeshAssetID }).has_value());

		const auto Snapshot = SceneRenderSnapshotBuilder::Build(TestScene, 16.0f / 9.0f);
		PF_CHECK(Tests, Snapshot.has_value());
		if (!Snapshot) return;
		PF_CHECK(Tests, Snapshot->CameraEntity == CameraID);
		PF_CHECK(Tests, glm::all(glm::equal(Snapshot->CameraWorldPosition, glm::vec3(0.0f, 0.0f, 5.0f))));
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->Entity == LightID);
		PF_CHECK(Tests, Snapshot->DirectionalLight && glm::abs(Snapshot->DirectionalLight->RayDirection.x + 1.0f) < 0.0001f);
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->Intensity == 2.0f);
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->CastShadows);
		PF_CHECK(Tests, Snapshot->DirectionalLight && Snapshot->DirectionalLight->ShadowDistance == 100.0f);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && Snapshot->EnvironmentLight->Entity == LightID);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && Snapshot->EnvironmentLight->HdrImage == EnvironmentAssetID);
		PF_CHECK(Tests, Snapshot->EnvironmentLight && Snapshot->EnvironmentLight->Intensity == 1.5f);
		PF_CHECK(
			Tests, Snapshot->EnvironmentLight &&
					   glm::abs((glm::mat3(Snapshot->EnvironmentLight->WorldRotation) * glm::vec3(0.0f, 0.0f, -1.0f)).x + 1.0f) < 0.0001f);
		PF_CHECK(Tests, Snapshot->PointLights.size() == 1 && Snapshot->PointLights[0].Entity == LightID);
		PF_CHECK(Tests,
			Snapshot->PointLights.size() == 1 && glm::all(glm::equal(Snapshot->PointLights[0].WorldPosition, glm::vec3(2.0f, 1.0f, 0.0f))));
		PF_CHECK(Tests, Snapshot->SpotLights.size() == 1 && Snapshot->SpotLights[0].Entity == LightID);
		PF_CHECK(Tests, Snapshot->SpotLights.size() == 1 && glm::abs(Snapshot->SpotLights[0].WorldDirection.x + 1.0f) < 0.0001f);
		PF_CHECK(Tests, Snapshot->SpotLights.size() == 1 && Snapshot->SpotLights[0].CastShadows &&
							Snapshot->SpotLights[0].ShadowBias == 0.004f && Snapshot->SpotLights[0].ShadowSoftness == 1.5f);
		PF_CHECK(Tests, Snapshot->Meshes.size() == 3);
		PF_CHECK(Tests, Snapshot->Meshes[0].Entity == FirstMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[1].Entity == SharedMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[2].Entity == SecondMeshID);
		PF_CHECK(Tests, Snapshot->Meshes[0].MeshAsset == FirstMeshAssetID);
		PF_CHECK(Tests, Snapshot->Meshes[1].MeshAsset == FirstMeshAssetID);
		PF_CHECK(Tests, Snapshot->Meshes[2].MeshAsset == SecondMeshAssetID);
		PF_CHECK(Tests, !Snapshot->Meshes[0].MaterialAsset.has_value());
		PF_CHECK(Tests, Snapshot->Meshes[1].MaterialAsset == MaterialAssetID);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].x - 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].y - 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[0].WorldTransform[3].z + 2.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[1].WorldTransform[3].x - 3.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[1].WorldTransform[3].z + 1.0f) < 0.0001f);
		PF_CHECK(Tests, glm::abs(Snapshot->Meshes[2].WorldTransform[3].y + 1.0f) < 0.0001f);
		const glm::vec4 CameraPositionClip = Snapshot->ViewProjection * glm::vec4(CameraTransform.Translation, 1.0f);
		PF_CHECK(Tests, glm::abs(CameraPositionClip.x) < 0.0001f && glm::abs(CameraPositionClip.y) < 0.0001f);
		auto DuplicatedCamera = TestScene.DuplicateEntity(*Camera);
		PF_CHECK(Tests, DuplicatedCamera.has_value());
		if (DuplicatedCamera)
		{
			const auto DuplicateCameraComponent = DuplicatedCamera->GetCamera();
			PF_CHECK(
				Tests, DuplicateCameraComponent && DuplicateCameraComponent->has_value() && !DuplicateCameraComponent->value().IsPrimary);
			const auto SnapshotAfterCameraDuplication = SceneRenderSnapshotBuilder::Build(TestScene, 16.0f / 9.0f);
			PF_CHECK(Tests, SnapshotAfterCameraDuplication && SnapshotAfterCameraDuplication->CameraEntity == CameraID);
		}
		auto SecondLight = TestScene.CreateEntity("Second sun");
		PF_CHECK(Tests, SecondLight.has_value());
		if (SecondLight)
		{
			PF_CHECK(Tests, SecondLight->SetDirectionalLight(DirectionalLightComponent{}).has_value());
			const auto MultipleLights = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 16.0f / 9.0f);
			PF_CHECK(Tests, !MultipleLights && MultipleLights.error().Code == SceneRenderSnapshotErrorCode::MultipleDirectionalLights);
			const std::array ExcludedDirectional = { SecondLight->GetUUID() };
			const auto FilteredLights = SceneRenderSnapshotBuilder::BuildForViewFiltered(
				TestScene, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.0f), 0.1f, 1000.0f, ExcludedDirectional);
			PF_CHECK(Tests, FilteredLights && FilteredLights->DirectionalLight && FilteredLights->DirectionalLight->Entity == LightID);
			(void)TestScene.DestroyEntity(*SecondLight);
		}
		auto SecondEnvironment = TestScene.CreateEntity("Second environment");
		PF_CHECK(Tests, SecondEnvironment.has_value());
		if (SecondEnvironment)
		{
			PF_CHECK(Tests, SecondEnvironment->SetEnvironmentLight(EnvironmentLightComponent{ EnvironmentAssetID, 0.5f }).has_value());
			const auto MultipleEnvironments = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 16.0f / 9.0f);
			PF_CHECK(Tests,
				!MultipleEnvironments && MultipleEnvironments.error().Code == SceneRenderSnapshotErrorCode::MultipleEnvironmentLights);
			const std::array ExcludedEnvironment = { SecondEnvironment->GetUUID() };
			const auto FilteredEnvironments = SceneRenderSnapshotBuilder::BuildForViewFiltered(
				TestScene, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.0f), 0.1f, 1000.0f, ExcludedEnvironment);
			PF_CHECK(Tests, FilteredEnvironments && FilteredEnvironments->EnvironmentLight &&
								FilteredEnvironments->EnvironmentLight->Entity == LightID);
			(void)TestScene.DestroyEntity(*SecondEnvironment);
		}
		PF_CHECK(Tests, Parent->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID }).has_value());
		const std::array ExcludedParent = { Parent->GetUUID() };
		const auto FilteredHierarchy = SceneRenderSnapshotBuilder::BuildForViewFiltered(
			TestScene, glm::mat4(1.0f), glm::mat4(1.0f), glm::vec3(0.0f), 0.1f, 1000.0f, ExcludedParent);
		PF_CHECK(Tests, FilteredHierarchy && FilteredHierarchy->Meshes.size() == 3);
		if (FilteredHierarchy)
		{
			const auto ParentMesh = std::find_if(FilteredHierarchy->Meshes.begin(), FilteredHierarchy->Meshes.end(),
				[&Parent](const SceneMeshInstance& Instance) { return Instance.Entity == Parent->GetUUID(); });
			const auto VisibleChildMesh = std::find_if(FilteredHierarchy->Meshes.begin(), FilteredHierarchy->Meshes.end(),
				[&FirstMeshID](const SceneMeshInstance& Instance) { return Instance.Entity == FirstMeshID; });
			PF_CHECK(Tests, ParentMesh == FilteredHierarchy->Meshes.end());
			PF_CHECK(Tests, VisibleChildMesh != FilteredHierarchy->Meshes.end());
			if (VisibleChildMesh != FilteredHierarchy->Meshes.end())
				PF_CHECK(Tests, glm::abs(VisibleChildMesh->WorldTransform[3].x - 1.0f) < 0.0001f &&
									glm::abs(VisibleChildMesh->WorldTransform[3].y - 1.0f) < 0.0001f &&
									glm::abs(VisibleChildMesh->WorldTransform[3].z + 2.0f) < 0.0001f);
		}
		TransformComponent SingularMeshTransform;
		SingularMeshTransform.Scale.x = 0.0f;
		PF_CHECK(Tests, FirstMesh->SetTransform(SingularMeshTransform).has_value());
		const auto SingularMeshWorld = FirstMesh->GetWorldMatrix();
		PF_CHECK(Tests, SingularMeshWorld.has_value());
		PF_CHECK(Tests, SingularMeshWorld && !BuildNormalTransform(*SingularMeshWorld));
		PF_CHECK(Tests, FirstMesh->SetTransform(FirstMeshTransform).has_value());
		const auto NonUniformNormal = BuildNormalTransform(glm::scale(glm::mat4(1.0f), glm::vec3(2.0f, 1.0f, 0.5f)));
		PF_CHECK(Tests, NonUniformNormal.has_value());
		if (NonUniformNormal)
			PF_CHECK(Tests, glm::abs((*NonUniformNormal)[0][0] - 0.5f) < 1.0e-6f && glm::abs((*NonUniformNormal)[1][1] - 1.0f) < 1.0e-6f &&
								glm::abs((*NonUniformNormal)[2][2] - 2.0f) < 1.0e-6f);
		PF_CHECK(Tests, !BuildNormalTransform(glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, 0.0f, 1.0f))));

		const auto NilCamera = SceneRenderSnapshotBuilder::Build(TestScene, UUID{}, 1.0f);
		PF_CHECK(Tests, !NilCamera && NilCamera.error().Code == SceneRenderSnapshotErrorCode::InvalidCameraEntity);
		const auto MissingCamera = SceneRenderSnapshotBuilder::Build(TestScene, FirstMeshID, 1.0f);
		PF_CHECK(Tests, !MissingCamera && MissingCamera.error().Code == SceneRenderSnapshotErrorCode::MissingCameraComponent);
		const auto InvalidAspect = SceneRenderSnapshotBuilder::Build(TestScene, 0.0f);
		PF_CHECK(Tests, !InvalidAspect && InvalidAspect.error().Code == SceneRenderSnapshotErrorCode::InvalidCamera);
		CameraComponent NonPrimaryCameraComponent;
		PF_CHECK(Tests, Camera->SetCamera(NonPrimaryCameraComponent).has_value());
		const auto NoPrimaryCamera = SceneRenderSnapshotBuilder::Build(TestScene, 1.0f);
		PF_CHECK(Tests, !NoPrimaryCamera && NoPrimaryCamera.error().Code == SceneRenderSnapshotErrorCode::MissingPrimaryCamera);
		PF_CHECK(Tests, Camera->SetCamera(PrimaryCameraComponent).has_value());
		auto AdditionalCamera = TestScene.CreateEntityWithUUID(UUID{ 0x1800000000000000ull, 18 }, "Second camera");
		PF_CHECK(Tests, AdditionalCamera.has_value());
		if (AdditionalCamera)
		{
			PF_CHECK(Tests, AdditionalCamera->SetCamera(PrimaryCameraComponent).has_value());
			const auto MultiplePrimaryCameras = SceneRenderSnapshotBuilder::Build(TestScene, 1.0f);
			PF_CHECK(Tests,
				!MultiplePrimaryCameras && MultiplePrimaryCameras.error().Code == SceneRenderSnapshotErrorCode::MultiplePrimaryCameras);
		}

		TransformComponent SingularCameraTransform = CameraTransform;
		SingularCameraTransform.Scale.x = 0.0f;
		PF_CHECK(Tests, Camera->SetTransform(SingularCameraTransform).has_value());
		const auto SingularCamera = SceneRenderSnapshotBuilder::Build(TestScene, CameraID, 1.0f);
		PF_CHECK(Tests, !SingularCamera && SingularCamera.error().Code == SceneRenderSnapshotErrorCode::InvalidCameraTransform);

		Scene CameraLessScene;
		auto CameraLessMesh = CameraLessScene.CreateEntity("Editor view mesh");
		PF_CHECK(Tests, CameraLessMesh.has_value());
		if (CameraLessMesh) PF_CHECK(Tests, CameraLessMesh->SetMeshRenderer(MeshRendererComponent{ FirstMeshAssetID }).has_value());

		const glm::mat4 TransientViewProjection(1.0f);
		const auto TransientView =
			SceneRenderSnapshotBuilder::BuildForView(CameraLessScene, TransientViewProjection, glm::vec3(3.0f, 4.0f, 5.0f));
		PF_CHECK(Tests, TransientView.has_value());
		PF_CHECK(Tests, TransientView && !TransientView->HasCameraFrustum);
		if (TransientView)
		{
			PF_CHECK(Tests, !TransientView->CameraEntity.has_value());
			PF_CHECK(Tests, glm::all(glm::equal(TransientView->CameraWorldPosition, glm::vec3(3.0f, 4.0f, 5.0f))));
			PF_CHECK(Tests, TransientView->Meshes.size() == 1);
			PF_CHECK(Tests, !TransientView->DirectionalLight.has_value());
			PF_CHECK(Tests, TransientView->ViewProjection[0][0] == 1.0f);
		}

		CameraComponent CascadeCamera;
		CascadeCamera.NearClipPlane = 0.1f;
		CascadeCamera.FarClipPlane = 100.0f;
		const auto CascadeProjection = CascadeCamera.GetProjectionMatrix(16.0f / 9.0f);
		PF_CHECK(Tests, CascadeProjection.has_value());
		if (CascadeProjection)
		{
			const auto ExplicitTransientView = SceneRenderSnapshotBuilder::BuildForView(
				CameraLessScene, glm::mat4(1.0f), *CascadeProjection, glm::vec3(3.0f, 4.0f, 5.0f), 0.1f, 100.0f);
			PF_CHECK(Tests, ExplicitTransientView && ExplicitTransientView->HasCameraFrustum);
			const auto Cascades =
				BuildDirectionalShadowCascades(glm::mat4(1.0f), *CascadeProjection, 0.1f, 100.0f, 60.0f, glm::vec3(0.0f, 1.0f, 0.0f));
			PF_CHECK(Tests, Cascades.has_value());
			if (Cascades)
			{
				PF_CHECK(Tests, Cascades->ShadowDistance == 60.0f);
				PF_CHECK(Tests, Cascades->Cascades.front().NearDistance == 0.1f);
				PF_CHECK(Tests, Cascades->Cascades.back().FarDistance == 60.0f);
				for (size_t Index = 0; Index < Cascades->Cascades.size(); ++Index)
				{
					PF_CHECK(Tests, Cascades->Cascades[Index].FarDistance > Cascades->Cascades[Index].NearDistance);
					if (Index > 0) PF_CHECK(Tests, Cascades->Cascades[Index].NearDistance == Cascades->Cascades[Index - 1].FarDistance);
					for (int Column = 0; Column < 4; ++Column)
						for (int Row = 0; Row < 4; ++Row)
							PF_CHECK(Tests, std::isfinite(Cascades->Cascades[Index].ViewProjection[Column][Row]));

					for (const float Distance : { Cascades->Cascades[Index].NearDistance, Cascades->Cascades[Index].FarDistance })
					{
						for (const float Y : { -1.0f, 1.0f })
						{
							for (const float X : { -1.0f, 1.0f })
							{
								glm::vec4 ViewCorner = glm::inverse(*CascadeProjection) * glm::vec4(X, Y, 1.0f, 1.0f);
								ViewCorner /= ViewCorner.w;
								ViewCorner = glm::vec4(glm::vec3(ViewCorner) * (Distance / -ViewCorner.z), 1.0f);
								const glm::vec4 ShadowClip = Cascades->Cascades[Index].ViewProjection * ViewCorner;
								const glm::vec3 ShadowNdc = glm::vec3(ShadowClip) / ShadowClip.w;
								PF_CHECK(Tests, ShadowNdc.x >= -1.001f && ShadowNdc.x <= 1.001f);
								PF_CHECK(Tests, ShadowNdc.y >= -1.001f && ShadowNdc.y <= 1.001f);
								PF_CHECK(Tests, ShadowNdc.z >= 0.0f && ShadowNdc.z <= 1.0f);
							}
						}
					}
				}
			}
			const glm::mat4 SlightCameraTranslation = glm::translate(glm::mat4(1.0f), glm::vec3(0.001f, 0.0f, 0.0f));
			const auto StableCascades = BuildDirectionalShadowCascades(
				SlightCameraTranslation, *CascadeProjection, 0.1f, 100.0f, 60.0f, glm::vec3(0.0f, 1.0f, 0.0f));
			PF_CHECK(Tests, StableCascades.has_value());
			if (Cascades && StableCascades)
				for (int Column = 0; Column < 4; ++Column)
					for (int Row = 0; Row < 4; ++Row)
						PF_CHECK(Tests, glm::abs(Cascades->Cascades[0].ViewProjection[Column][Row] -
												 StableCascades->Cascades[0].ViewProjection[Column][Row]) < 0.001f);
			PF_CHECK(Tests,
				!BuildDirectionalShadowCascades(glm::mat4(1.0f), *CascadeProjection, 0.1f, 100.0f, 10.0f, glm::vec3(0.0f)).has_value());
			PF_CHECK(Tests,
				!BuildDirectionalShadowCascades(glm::mat4(1.0f), *CascadeProjection, 0.1f, 100.0f, 0.05f, glm::vec3(0.0f, 1.0f, 0.0f))
					.has_value());
		}

		glm::mat4 InvalidTransientView(1.0f);
		InvalidTransientView[2][1] = std::numeric_limits<float>::quiet_NaN();
		const auto InvalidView = SceneRenderSnapshotBuilder::BuildForView(CameraLessScene, InvalidTransientView, glm::vec3(0.0f));
		PF_CHECK(Tests, !InvalidView && InvalidView.error().Code == SceneRenderSnapshotErrorCode::InvalidViewProjection);
	}
} // namespace

void RunSceneTests(TestRunner& Tests)
{
	TestUUIDBehavior(Tests);
	TestSceneEntityAndHierarchy(Tests);
	TestPhysicsComponentsAndPersistence(Tests);
	TestPhysicsSceneRuntime(Tests);
	TestPhysicsLiveReconciliation(Tests);
	TestEntityHandleIdentityAndLifetime(Tests);
	TestSceneRenderSnapshot(Tests);
}
