#include "Assets/PrefabInstanceReconciler.h"

#include "Assets/AssetImportProvenance.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <set>
#include <span>
#include <unordered_map>
#include <unordered_set>

namespace PulseForge
{
	namespace
	{
		using EntityMap = std::unordered_map<UUID, UUID, UUIDHash>;

#if defined(PF_DEBUG)
		thread_local std::vector<PrefabInstanceReconciler::CommitFailurePoint> CommitFailureSequence;

		bool ConsumeCommitFailurePoint(PrefabInstanceReconciler::CommitFailurePoint Point)
		{
			if (CommitFailureSequence.empty() || CommitFailureSequence.front() != Point) return false;
			CommitFailureSequence.erase(CommitFailureSequence.begin());
			return true;
		}
#endif

		PrefabReconcileError MakeError(PrefabReconcileErrorCode Code, std::string Message,
			std::vector<PrefabConflictRecord> Conflicts = {})
		{
			return { Code, std::move(Message), std::move(Conflicts) };
		}

		PrefabReconcileError ToReconcileError(const PulseForge::SceneError& Error)
		{
			return MakeError(PrefabReconcileErrorCode::SceneOperationFailed, Error.Message);
		}

		PrefabReconcileError SerializationError(const SceneSerializationError& Error)
		{
			return MakeError(PrefabReconcileErrorCode::SceneOperationFailed, Error.Message);
		}

		std::string FingerprintText(const AssetContentFingerprint& Fingerprint)
		{
			return Fingerprint.Algorithm + ":" + Fingerprint.DigestHex;
		}

		std::string FingerprintText(std::string_view Bytes)
		{
			const auto ByteSpan = std::as_bytes(std::span(Bytes.data(), Bytes.size()));
			return FingerprintText(ComputeAssetContentFingerprint(ByteSpan));
		}

		PrefabConflictRecord Conflict(
			PrefabEntityAddress Address,
			PrefabConflictKind Kind,
			std::string Message,
			std::string Component = {},
			std::string Property = {})
		{
			return { Address, Kind, std::move(Component), std::move(Property), std::move(Message) };
		}

		template <typename T, typename Getter, typename Setter, typename Remover>
		std::expected<void, PrefabReconcileError> SyncOptional(
			const Entity& Source,
			const Entity& Destination,
			Getter Get,
			Setter Set,
			Remover Remove)
		{
			const auto SourceValue = (Source.*Get)();
			const auto DestinationValue = (Destination.*Get)();
			if (!SourceValue || !DestinationValue)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					"Could not read optional component during prefab reconciliation"));
			if (SourceValue->has_value())
			{
				if (auto Result = (Destination.*Set)(SourceValue->value()); !Result)
					return std::unexpected(ToReconcileError(Result.error()));
			}
			else if (DestinationValue->has_value())
			{
				if (auto Result = (Destination.*Remove)(); !Result)
					return std::unexpected(ToReconcileError(Result.error()));
			}
			return {};
		}

		std::expected<void, PrefabReconcileError> CopySourceComponents(
			const Entity& Source,
			const Entity& Destination,
			bool PreserveTransform,
			bool PreservePrimaryRoles)
		{
			const auto SourceTag = Source.GetTag();
			if (!SourceTag) return std::unexpected(ToReconcileError(SourceTag.error()));
			if (auto Result = Destination.SetTag(*SourceTag); !Result) return std::unexpected(ToReconcileError(Result.error()));

			if (!PreserveTransform)
			{
				const auto Transform = Source.GetTransform();
				if (!Transform) return std::unexpected(ToReconcileError(Transform.error()));
				if (auto Result = Destination.SetTransform(*Transform); !Result) return std::unexpected(ToReconcileError(Result.error()));
			}

			const auto SourceCamera = Source.GetCamera();
			const auto DestinationCamera = Destination.GetCamera();
			if (!SourceCamera || !DestinationCamera)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, "Could not read camera during prefab reconciliation"));
			if (SourceCamera->has_value())
			{
				CameraComponent Value = SourceCamera->value();
				Value.IsPrimary = PreservePrimaryRoles && DestinationCamera->has_value()
					? DestinationCamera->value().IsPrimary
					: false;
				if (auto Result = Destination.SetCamera(Value); !Result) return std::unexpected(ToReconcileError(Result.error()));
			}
			else if (DestinationCamera->has_value())
			{
				if (auto Result = Destination.RemoveCamera(); !Result) return std::unexpected(ToReconcileError(Result.error()));
			}

			if (auto Result = SyncOptional<DirectionalLightComponent>(Source, Destination,
				&Entity::GetDirectionalLight, &Entity::SetDirectionalLight, &Entity::RemoveDirectionalLight); !Result) return Result;
			if (auto Result = SyncOptional<EnvironmentLightComponent>(Source, Destination,
				&Entity::GetEnvironmentLight, &Entity::SetEnvironmentLight, &Entity::RemoveEnvironmentLight); !Result) return Result;
			if (auto Result = SyncOptional<PointLightComponent>(Source, Destination,
				&Entity::GetPointLight, &Entity::SetPointLight, &Entity::RemovePointLight); !Result) return Result;
			if (auto Result = SyncOptional<SpotLightComponent>(Source, Destination,
				&Entity::GetSpotLight, &Entity::SetSpotLight, &Entity::RemoveSpotLight); !Result) return Result;
			if (auto Result = SyncOptional<MeshRendererComponent>(Source, Destination,
				&Entity::GetMeshRenderer, &Entity::SetMeshRenderer, &Entity::RemoveMeshRenderer); !Result) return Result;
			if (auto Result = SyncOptional<RigidbodyComponent>(Source, Destination,
				&Entity::GetRigidbody, &Entity::SetRigidbody, &Entity::RemoveRigidbody); !Result) return Result;
			if (auto Result = SyncOptional<BoxColliderComponent>(Source, Destination,
				&Entity::GetBoxCollider, &Entity::SetBoxCollider, &Entity::RemoveBoxCollider); !Result) return Result;
			if (auto Result = SyncOptional<AudioSourceComponent>(Source, Destination,
				&Entity::GetAudioSource, &Entity::SetAudioSource, &Entity::RemoveAudioSource); !Result) return Result;

			const auto SourceListener = Source.GetAudioListener();
			const auto DestinationListener = Destination.GetAudioListener();
			if (!SourceListener || !DestinationListener)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, "Could not read listener during prefab reconciliation"));
			if (SourceListener->has_value())
			{
				AudioListenerComponent Value = SourceListener->value();
				Value.IsPrimary = PreservePrimaryRoles && DestinationListener->has_value()
					? DestinationListener->value().IsPrimary
					: false;
				if (auto Result = Destination.SetAudioListener(Value); !Result) return std::unexpected(ToReconcileError(Result.error()));
			}
			else if (DestinationListener->has_value())
			{
				if (auto Result = Destination.RemoveAudioListener(); !Result) return std::unexpected(ToReconcileError(Result.error()));
			}

			return SyncOptional<ScriptComponent>(Source, Destination, &Entity::GetScript, &Entity::SetScript, &Entity::RemoveScript);
		}

		std::expected<void, PrefabReconcileError> SyncFullEntity(const Entity& Source, const Entity& Destination)
		{
			std::optional<PrefabReconcileError> FirstFailure;
			const auto Remember = [&FirstFailure](std::expected<void, PrefabReconcileError> Result)
			{
				if (!Result && !FirstFailure) FirstFailure = Result.error();
			};
			const auto RememberSceneOperation = [&FirstFailure](std::expected<void, SceneError> Result)
			{
				if (!Result && !FirstFailure) FirstFailure = ToReconcileError(Result.error());
			};
			const auto Tag = Source.GetTag();
			const auto Transform = Source.GetTransform();
			if (!Tag || !Transform)
				FirstFailure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					"Could not read entity state during reconcile commit");
			else
			{
				RememberSceneOperation(Destination.SetTag(*Tag));
				RememberSceneOperation(Destination.SetTransform(*Transform));
			}
			Remember(SyncOptional<CameraComponent>(Source, Destination, &Entity::GetCamera, &Entity::SetCamera, &Entity::RemoveCamera));
			Remember(SyncOptional<DirectionalLightComponent>(Source, Destination, &Entity::GetDirectionalLight, &Entity::SetDirectionalLight, &Entity::RemoveDirectionalLight));
			Remember(SyncOptional<EnvironmentLightComponent>(Source, Destination, &Entity::GetEnvironmentLight, &Entity::SetEnvironmentLight, &Entity::RemoveEnvironmentLight));
			Remember(SyncOptional<PointLightComponent>(Source, Destination, &Entity::GetPointLight, &Entity::SetPointLight, &Entity::RemovePointLight));
			Remember(SyncOptional<SpotLightComponent>(Source, Destination, &Entity::GetSpotLight, &Entity::SetSpotLight, &Entity::RemoveSpotLight));
			Remember(SyncOptional<MeshRendererComponent>(Source, Destination, &Entity::GetMeshRenderer, &Entity::SetMeshRenderer, &Entity::RemoveMeshRenderer));
			Remember(SyncOptional<RigidbodyComponent>(Source, Destination, &Entity::GetRigidbody, &Entity::SetRigidbody, &Entity::RemoveRigidbody));
			Remember(SyncOptional<BoxColliderComponent>(Source, Destination, &Entity::GetBoxCollider, &Entity::SetBoxCollider, &Entity::RemoveBoxCollider));
			Remember(SyncOptional<AudioSourceComponent>(Source, Destination, &Entity::GetAudioSource, &Entity::SetAudioSource, &Entity::RemoveAudioSource));
			Remember(SyncOptional<AudioListenerComponent>(Source, Destination, &Entity::GetAudioListener, &Entity::SetAudioListener, &Entity::RemoveAudioListener));
			Remember(SyncOptional<ScriptComponent>(Source, Destination, &Entity::GetScript, &Entity::SetScript, &Entity::RemoveScript));

			const auto SourcePrefab = Source.GetPrefabInstance();
			const auto DestinationPrefab = Destination.GetPrefabInstance();
			if (!SourcePrefab || !DestinationPrefab)
				Remember(std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					"Could not read prefab metadata during reconcile commit")));
			else if (SourcePrefab->has_value())
			{
				RememberSceneOperation(Destination.SetPrefabInstance(SourcePrefab->value()));
			}
			else if (DestinationPrefab->has_value())
			{
				RememberSceneOperation(Destination.RemovePrefabInstance());
			}
			if (FirstFailure) return std::unexpected(std::move(*FirstFailure));
			return {};
		}

		std::expected<void, PrefabReconcileError> SyncParent(
			const Entity& Source,
			const Scene& DestinationScene,
			const Entity& Destination)
		{
			const auto SourceParent = Source.GetParent();
			const auto DestinationParent = Destination.GetParent();
			if (!SourceParent || !DestinationParent)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, "Could not read entity parent during reconcile commit"));
			const std::optional<UUID> SourceParentID = SourceParent->has_value()
				? std::optional<UUID>{ (**SourceParent).GetUUID() } : std::nullopt;
			const std::optional<UUID> DestinationParentID = DestinationParent->has_value()
				? std::optional<UUID>{ (**DestinationParent).GetUUID() } : std::nullopt;
			if (SourceParentID == DestinationParentID) return {};
			if (!SourceParent->has_value())
			{
				if (auto Result = Destination.ClearParent(); !Result) return std::unexpected(ToReconcileError(Result.error()));
				return {};
			}
			const auto Parent = DestinationScene.FindEntity(SourceParentID.value());
			if (!Parent) return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, "Target parent is missing during hierarchy commit"));
			if (auto Result = Destination.SetParent(*Parent); !Result) return std::unexpected(ToReconcileError(Result.error()));
			return {};
		}

		std::expected<std::vector<Entity>, PrefabReconcileError> GetParentFirstOrder(const Scene& Source)
		{
			const std::vector<Entity> Entities = Source.GetEntities();
			std::unordered_map<UUID, Entity, UUIDHash> ByID;
			std::unordered_map<UUID, size_t, UUIDHash> PendingParentCount;
			std::unordered_map<UUID, std::vector<UUID>, UUIDHash> ChildrenByParent;
			ByID.reserve(Entities.size());
			PendingParentCount.reserve(Entities.size());
			ChildrenByParent.reserve(Entities.size());
			for (const Entity& Current : Entities)
			{
				if (!ByID.emplace(Current.GetUUID(), Current).second)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
						"Scene hierarchy contains duplicate entity identities"));
				PendingParentCount.emplace(Current.GetUUID(), 0);
			}

			for (const Entity& Current : Entities)
			{
				const auto Parent = Current.GetParent();
				const auto Children = Current.GetChildren();
				if (!Parent || !Children)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
						"Could not inspect scene hierarchy before prefab commit"));
				if (Parent->has_value())
				{
					const UUID ParentID = (**Parent).GetUUID();
					if (!ByID.contains(ParentID))
						return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
							"Scene hierarchy references a missing parent"));
					PendingParentCount[Current.GetUUID()] = 1;
					ChildrenByParent[ParentID].push_back(Current.GetUUID());
				}

				std::unordered_set<UUID, UUIDHash> UniqueChildren;
				UniqueChildren.reserve(Children->size());
				for (const Entity& Child : *Children)
				{
					const auto ChildInScene = ByID.find(Child.GetUUID());
					const auto ChildParent = Child.GetParent();
					if (ChildInScene == ByID.end() || !ChildParent || !ChildParent->has_value() ||
						(**ChildParent).GetUUID() != Current.GetUUID() || !UniqueChildren.insert(Child.GetUUID()).second)
						return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
							"Scene hierarchy has inconsistent parent and child links"));
				}
			}
			for (const Entity& Current : Entities)
			{
				const auto ActualChildren = Current.GetChildren();
				if (!ActualChildren) return std::unexpected(ToReconcileError(ActualChildren.error()));
				std::vector<UUID> ActualIDs;
				ActualIDs.reserve(ActualChildren->size());
				for (const Entity& Child : *ActualChildren) ActualIDs.push_back(Child.GetUUID());
				std::vector<UUID> ExpectedIDs = ChildrenByParent[Current.GetUUID()];
				std::sort(ActualIDs.begin(), ActualIDs.end());
				std::sort(ExpectedIDs.begin(), ExpectedIDs.end());
				if (ActualIDs != ExpectedIDs)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
						"Scene hierarchy has inconsistent parent and child links"));
			}

			std::set<UUID> Ready;
			for (const auto& [Identifier, ParentTotal] : PendingParentCount)
				if (ParentTotal == 0) Ready.insert(Identifier);

			std::vector<Entity> Ordered;
			Ordered.reserve(Entities.size());
			while (!Ready.empty())
			{
				const UUID CurrentID = *Ready.begin();
				Ready.erase(Ready.begin());
				Ordered.push_back(ByID.at(CurrentID));
				auto Children = ChildrenByParent.find(CurrentID);
				if (Children == ChildrenByParent.end()) continue;
				std::sort(Children->second.begin(), Children->second.end());
				for (const UUID ChildID : Children->second)
				{
					PendingParentCount[ChildID] = 0;
					Ready.insert(ChildID);
				}
			}
			if (Ordered.size() != Entities.size())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
					"Scene hierarchy contains a parent cycle"));
			return Ordered;
		}

		std::expected<std::unique_ptr<Scene>, PrefabReconcileError> DeserializeScene(std::string_view Data)
		{
			auto Result = std::make_unique<Scene>();
			if (auto Deserialized = SceneSerializer::Deserialize(Data, *Result); !Deserialized)
				return std::unexpected(SerializationError(Deserialized.error()));
			return Result;
		}

		std::expected<std::vector<Entity>, PrefabReconcileError> GetSubtree(const Entity& Root)
		{
			std::vector<Entity> Result{ Root };
			std::unordered_set<UUID, UUIDHash> Visited{ Root.GetUUID() };
			for (size_t Index = 0; Index < Result.size(); ++Index)
			{
				const auto Children = Result[Index].GetChildren();
				if (!Children) return std::unexpected(ToReconcileError(Children.error()));
				for (const Entity& Child : *Children)
				{
					if (!Visited.insert(Child.GetUUID()).second)
						return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Linked hierarchy contains a cycle"));
					Result.push_back(Child);
				}
			}
			return Result;
		}

		bool HasOverrideFor(const PrefabInstanceComponent& Instance, UUID SourceKey)
		{
			return std::any_of(Instance.Overrides.begin(), Instance.Overrides.end(), [SourceKey](const PrefabOverrideRecord& Override)
			{
				return !Override.Entity.IsLocal && Override.Entity.Key == SourceKey &&
					Override.Operation != PrefabOverrideOperation::RemoveSourceEntity;
			});
		}

		const PrefabConflictRecord* FindRemovedSourceRecoveryConflict(
			const PrefabInstanceComponent& Instance,
			UUID SourceKey)
		{
			const auto It = std::find_if(Instance.Conflicts.begin(), Instance.Conflicts.end(), [SourceKey](const PrefabConflictRecord& ConflictValue)
			{
				return !ConflictValue.Entity.IsLocal && ConflictValue.Entity.Key == SourceKey &&
					ConflictValue.Kind == PrefabConflictKind::RemovedSourceEntity;
			});
			return It == Instance.Conflicts.end() ? nullptr : &*It;
		}

		PrefabInstanceComponent MakeConflictOnlyMetadata(
			const PrefabInstanceComponent& Current,
			const PrefabInstanceComponent& Planned,
			const std::vector<PrefabConflictRecord>& Conflicts)
		{
			PrefabInstanceComponent Result = Current;
			Result.LocalEntities = Planned.LocalEntities;
			std::unordered_set<UUID, UUIDHash> LocalKeys;
			for (const PrefabLocalEntityMapping& Mapping : Result.LocalEntities) LocalKeys.insert(Mapping.LocalKey);
			std::erase_if(Result.Overrides, [&LocalKeys](const PrefabOverrideRecord& Override)
			{
				return Override.Entity.IsLocal && !LocalKeys.contains(Override.Entity.Key);
			});
			Result.Conflicts = Conflicts;
			return Result;
		}

		bool IsDescendantOf(const Entity& Current, UUID Ancestor)
		{
			auto Parent = Current.GetParent();
			for (size_t Steps = 0; Parent && Parent->has_value() && Steps <= 250'000; ++Steps)
			{
				if ((**Parent).GetUUID() == Ancestor) return true;
				Parent = (**Parent).GetParent();
			}
			return false;
		}

		std::optional<UUID> ResolveAddress(const PrefabInstanceComponent& Instance, const PrefabEntityAddress& Address)
		{
			if (Address.IsLocal)
			{
				const auto It = std::find_if(Instance.LocalEntities.begin(), Instance.LocalEntities.end(),
					[&Address](const PrefabLocalEntityMapping& Mapping) { return Mapping.LocalKey == Address.Key; });
				return It == Instance.LocalEntities.end() ? std::nullopt : std::optional<UUID>{ It->SceneEntity };
			}
			const auto It = std::find_if(Instance.SourceEntities.begin(), Instance.SourceEntities.end(),
				[&Address](const PrefabEntityMapping& Mapping) { return Mapping.SourceKey == Address.Key; });
			return It == Instance.SourceEntities.end() ? std::nullopt : std::optional<UUID>{ It->SceneEntity };
		}

		std::expected<void, PrefabReconcileError> ApplyOverride(
			Scene& Staged,
			PrefabInstanceComponent& Instance,
			const PrefabOverrideRecord& Override,
			std::vector<PrefabConflictRecord>& Conflicts)
		{
			if (Override.Operation == PrefabOverrideOperation::ResetProperty ||
				Override.Operation == PrefabOverrideOperation::AddLocalEntity)
				return {};
			if (Override.Operation == PrefabOverrideOperation::AddComponent || Override.Operation == PrefabOverrideOperation::RemoveComponent)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
					"Component presence overrides are not supported by the current typed property adapter"));

			const auto SceneEntityID = ResolveAddress(Instance, Override.Entity);
			if (!SceneEntityID)
			{
				if (Override.Operation == PrefabOverrideOperation::RemoveSourceEntity) return {};
				Conflicts.push_back(Conflict(Override.Entity, PrefabConflictKind::RemovedSourceEntity,
					"The entity referenced by this prefab override no longer exists"));
				return {};
			}
			const auto Target = Staged.FindEntity(*SceneEntityID);
			if (!Target)
			{
				if (Override.Operation == PrefabOverrideOperation::RemoveSourceEntity) return {};
				Conflicts.push_back(Conflict(Override.Entity, PrefabConflictKind::RemovedSourceEntity,
					"The mapped entity for this prefab override no longer exists"));
				return {};
			}

			if (Override.Operation == PrefabOverrideOperation::Rename)
			{
				auto Tag = Target->GetTag();
				if (!Tag) return std::unexpected(ToReconcileError(Tag.error()));
				Tag->Name = Override.Name;
				if (auto Result = Target->SetTag(*Tag); !Result) return std::unexpected(ToReconcileError(Result.error()));
				return {};
			}
			if (Override.Operation == PrefabOverrideOperation::Reparent)
			{
				if (Override.Entity.Key == Instance.SourceRootKey && !Override.Entity.IsLocal)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
						"The linked prefab root parent belongs to the destination scene"));
				if (!Override.Parent)
				{
					if (auto Result = Target->ClearParent(); !Result) return std::unexpected(ToReconcileError(Result.error()));
					return {};
				}
				const auto ParentID = ResolveAddress(Instance, *Override.Parent);
				const auto Parent = ParentID ? Staged.FindEntity(*ParentID) : std::nullopt;
				if (!Parent)
				{
					Conflicts.push_back(Conflict(Override.Entity, PrefabConflictKind::ConflictingReparent,
						"The local parent override no longer resolves"));
					return {};
				}
				if (auto Result = Target->SetParent(*Parent); !Result) return std::unexpected(ToReconcileError(Result.error()));
				return {};
			}
			if (Override.Operation == PrefabOverrideOperation::SetProperty)
			{
				if (!Override.Entity.IsLocal && Override.Entity.Key == Instance.SourceRootKey &&
					Override.ComponentID == PropertyComponentIds::Transform)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "Root transform is destination placement"));
				auto Current = ReadProperty(*Target, Override.ComponentID, Override.PropertyID);
				if (!Current)
				{
					const bool Missing = Current.error().Code == PropertyAccessErrorCode::UnknownProperty ||
						Current.error().Code == PropertyAccessErrorCode::UnknownComponent ||
						Current.error().Code == PropertyAccessErrorCode::MissingComponent;
					Conflicts.push_back(Conflict(Override.Entity,
						Missing ? PrefabConflictKind::MissingProperty : PrefabConflictKind::ChangedPropertyType,
						Current.error().Message, Override.ComponentID, Override.PropertyID));
					return {};
				}
				if (Current->index() != Override.Value.index())
				{
					Conflicts.push_back(Conflict(Override.Entity, PrefabConflictKind::ChangedPropertyType,
						"The overridden source property changed to a different value type", Override.ComponentID, Override.PropertyID));
					return {};
				}
				if (auto Result = WriteProperty(*Target, Override.ComponentID, Override.PropertyID, Override.Value); !Result)
				{
					Conflicts.push_back(Conflict(Override.Entity, PrefabConflictKind::ChangedPropertyType,
						Result.error().Message, Override.ComponentID, Override.PropertyID));
				}
				return {};
			}
			if (Override.Operation == PrefabOverrideOperation::RemoveSourceEntity)
			{
				if (!Override.Entity.IsLocal && Override.Entity.Key == Instance.SourceRootKey)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "The linked prefab root cannot be removed"));
				const auto Children = GetSubtree(*Target);
				if (!Children) return std::unexpected(Children.error());
				if (Children->size() > 1)
				{
					Conflicts.push_back(Conflict(Override.Entity, PrefabConflictKind::RemovedEntityHasLocalChildren,
						"The source entity has descendants that must be preserved before removal"));
					return {};
				}
				if (auto Result = Staged.DestroyEntity(*Target); !Result) return std::unexpected(ToReconcileError(Result.error()));
			}
			return {};
		}

		std::expected<void, PrefabReconcileError> RestoreScene(
			Scene& Destination,
			const Scene& Backup,
			const std::vector<Entity>& ParentFirst)
		{
			std::optional<PrefabReconcileError> FirstFailure;
			const auto Remember = [&FirstFailure](std::expected<void, PrefabReconcileError> Result)
			{
				if (!Result && !FirstFailure) FirstFailure = Result.error();
			};
#if defined(PF_DEBUG)
			bool SkippedInjectedRestore = false;
#endif
			for (const Entity& Original : ParentFirst)
			{
				const auto Current = Destination.FindEntity(Original.GetUUID());
				if (!Current)
				{
					if (!FirstFailure) FirstFailure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
						"An original entity is missing while restoring a prefab commit");
					continue;
				}
#if defined(PF_DEBUG)
				const auto OriginalTag = Original.GetTag();
				const auto CurrentTag = Current->GetTag();
				const bool HasRestorableChange = OriginalTag && CurrentTag && OriginalTag->Name != CurrentTag->Name;
				if (!SkippedInjectedRestore && HasRestorableChange && ConsumeCommitFailurePoint(
					PrefabInstanceReconciler::CommitFailurePoint::DuringRollbackRestore))
				{
					SkippedInjectedRestore = true;
					if (!FirstFailure) FirstFailure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
						"Injected failure while restoring an original prefab entity");
					continue;
				}
#endif
				Remember(SyncFullEntity(Original, *Current));
			}
			for (const Entity& Original : ParentFirst)
			{
				const auto Current = Destination.FindEntity(Original.GetUUID());
				if (!Current)
				{
					if (!FirstFailure) FirstFailure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
						"An original entity is missing while restoring prefab hierarchy");
					continue;
				}
				Remember(SyncParent(Original, Destination, *Current));
			}
			if (FirstFailure) return std::unexpected(std::move(*FirstFailure));
			return {};
		}

		PrefabReconcileError MakeRecoveryRequired(
			const PrefabReconcileError& OriginalFailure,
			const std::vector<std::string>& RecoveryFailures,
			std::string_view BackupSnapshot)
		{
			std::string Message = "Prefab commit failed: " + OriginalFailure.Message + ". Rollback could not be verified";
			for (const std::string& Failure : RecoveryFailures) Message += "; " + Failure;
			Message += ". Retain the supplied RecoverySnapshot and scene state for recovery.";
			PrefabReconcileError Result = MakeError(PrefabReconcileErrorCode::RecoveryRequired, std::move(Message));
			Result.RecoverySnapshot = BackupSnapshot;
			return Result;
		}

		std::expected<void, PrefabReconcileError> CommitScene(Scene& Destination, const Scene& Target, const Scene& Backup)
		{
			// All allocation-heavy planning and snapshot capture happens before the destination is mutated.
			auto TargetOrderResult = GetParentFirstOrder(Target);
			if (!TargetOrderResult) return std::unexpected(TargetOrderResult.error());
			auto BackupOrderResult = GetParentFirstOrder(Backup);
			if (!BackupOrderResult) return std::unexpected(BackupOrderResult.error());
			auto BackupBytes = SceneSerializer::Serialize(Backup);
			if (!BackupBytes) return std::unexpected(SerializationError(BackupBytes.error()));
			auto CurrentBytes = SceneSerializer::Serialize(Destination);
			if (!CurrentBytes) return std::unexpected(SerializationError(CurrentBytes.error()));
			if (*CurrentBytes != *BackupBytes)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan,
					"Destination changed before the prefab commit began"));

			const std::vector<Entity>& TargetOrder = *TargetOrderResult;
			const std::vector<Entity>& BackupOrder = *BackupOrderResult;
			std::unordered_set<UUID, UUIDHash> TargetIDs;
			TargetIDs.reserve(TargetOrder.size());
			std::vector<Entity> Additions;
			Additions.reserve(TargetOrder.size());
			for (const Entity& TargetEntity : TargetOrder)
			{
				TargetIDs.insert(TargetEntity.GetUUID());
				if (!Destination.FindEntity(TargetEntity.GetUUID())) Additions.push_back(TargetEntity);
			}

			std::unordered_map<UUID, size_t, UUIDHash> DepthByID;
			DepthByID.reserve(BackupOrder.size());
			for (const Entity& Original : BackupOrder)
			{
				const auto Parent = Original.GetParent();
				if (!Parent) return std::unexpected(ToReconcileError(Parent.error()));
				DepthByID[Original.GetUUID()] = Parent->has_value()
					? DepthByID.at((**Parent).GetUUID()) + 1 : 0;
			}
			std::vector<Entity> Removed;
			for (const Entity& Original : BackupOrder)
			{
				if (TargetIDs.contains(Original.GetUUID())) continue;
				const auto Current = Destination.FindEntity(Original.GetUUID());
				if (!Current)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan,
						"An entity selected for prefab removal is missing before commit"));
				Removed.push_back(*Current);
			}
			std::sort(Removed.begin(), Removed.end(), [&](const Entity& First, const Entity& Second)
			{
				const size_t FirstDepth = DepthByID.at(First.GetUUID());
				const size_t SecondDepth = DepthByID.at(Second.GetUUID());
				return FirstDepth != SecondDepth ? FirstDepth > SecondDepth : First.GetUUID() < Second.GetUUID();
			});
			std::unordered_set<UUID, UUIDHash> RemovedIDs;
			RemovedIDs.reserve(Removed.size());
			for (const Entity& Current : Removed) RemovedIDs.insert(Current.GetUUID());
			for (const Entity& Current : Removed)
				if (!Current.IsValid())
					return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan,
						"An entity selected for prefab removal is invalid before commit"));

			const std::vector<Entity> OriginalHandles = Destination.GetEntities();
			std::vector<Entity> Created;
			Created.reserve(Additions.size());
			std::vector<std::string> RecoveryFailures;
			RecoveryFailures.reserve(OriginalHandles.size() + Additions.size() + 4);
			const auto Rollback = [&](const PrefabReconcileError& OriginalFailure) -> PrefabReconcileError
			{
				RecoveryFailures.clear();
				if (auto Restored = RestoreScene(Destination, Backup, BackupOrder); !Restored)
					RecoveryFailures.push_back(Restored.error().Message);
				for (const Entity& Original : OriginalHandles)
				{
					const auto Current = Destination.FindEntity(Original.GetUUID());
					if (!Current || *Current != Original)
						RecoveryFailures.push_back("An original entity identity or incarnation was not preserved");
				}

				if (RecoveryFailures.empty())
				{
					for (auto It = Created.rbegin(); It != Created.rend(); ++It)
					{
						if (!It->IsValid()) continue;
						const auto Children = It->GetChildren();
						if (!Children || !Children->empty())
						{
							RecoveryFailures.push_back("An operation-created entity still owns children after rollback");
							continue;
						}
						if (auto Destroyed = Destination.DestroyEntity(*It); !Destroyed)
							RecoveryFailures.push_back(Destroyed.error().Message);
					}
				}

				if (RecoveryFailures.empty())
				{
					auto RestoredBytes = SceneSerializer::Serialize(Destination);
					if (!RestoredBytes) RecoveryFailures.push_back(RestoredBytes.error().Message);
					else if (*RestoredBytes != *BackupBytes)
						RecoveryFailures.push_back("Restored scene contents do not match the original scene snapshot");
				}
				if (!RecoveryFailures.empty()) return MakeRecoveryRequired(OriginalFailure, RecoveryFailures, *BackupBytes);
				return OriginalFailure;
			};

			std::optional<PrefabReconcileError> Failure;
			size_t ExistingSynchronized = 0;
			try
			{
				// Create all identities first; the reserved rollback list is updated immediately after each creation.
				for (const Entity& TargetEntity : Additions)
				{
					const auto Tag = TargetEntity.GetTag();
					if (!Tag) { Failure = ToReconcileError(Tag.error()); break; }
					auto NewEntity = Destination.CreateEntityWithUUID(TargetEntity.GetUUID(), Tag->Name);
					if (!NewEntity) { Failure = ToReconcileError(NewEntity.error()); break; }
					Created.push_back(*NewEntity);
#if defined(PF_DEBUG)
					if (ConsumeCommitFailurePoint(PrefabInstanceReconciler::CommitFailurePoint::AfterAdditionCreated))
					{
						Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
							"Injected failure after creating a prefab entity");
						break;
					}
#endif
				}

				if (!Failure)
				{
					for (const Entity& TargetEntity : TargetOrder)
					{
						const auto Current = Destination.FindEntity(TargetEntity.GetUUID());
						if (!Current)
						{
							Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
								"Entity disappeared during prefab component synchronization");
							break;
						}
						const auto TargetTag = TargetEntity.GetTag();
						const auto CurrentTag = Current->GetTag();
						const bool HasTagChange = TargetTag && CurrentTag && TargetTag->Name != CurrentTag->Name;
						if (auto Result = SyncFullEntity(TargetEntity, *Current); !Result)
						{
							Failure = Result.error();
							break;
						}
						if (DepthByID.contains(TargetEntity.GetUUID()) && HasTagChange)
						{
							++ExistingSynchronized;
#if defined(PF_DEBUG)
							if (ExistingSynchronized == 1 && ConsumeCommitFailurePoint(
								PrefabInstanceReconciler::CommitFailurePoint::AfterExistingEntitySynchronized))
							{
								Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
									"Injected failure after synchronizing an existing prefab entity");
								break;
							}
#endif
						}
					}
				}

				if (!Failure)
				{
					for (const Entity& TargetEntity : TargetOrder)
					{
						const auto Current = Destination.FindEntity(TargetEntity.GetUUID());
						if (!Current)
						{
							Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
								"Entity disappeared during prefab hierarchy synchronization");
							break;
						}
						const auto PreviousParent = Current->GetParent();
						const auto TargetParent = TargetEntity.GetParent();
						if (!PreviousParent || !TargetParent)
						{
							Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
								"Could not inspect parent before prefab hierarchy synchronization");
							break;
						}
						const std::optional<UUID> PreviousParentID = PreviousParent->has_value()
							? std::optional<UUID>{ (**PreviousParent).GetUUID() } : std::nullopt;
						const std::optional<UUID> TargetParentID = TargetParent->has_value()
							? std::optional<UUID>{ (**TargetParent).GetUUID() } : std::nullopt;
						if (auto Result = SyncParent(TargetEntity, Destination, *Current); !Result)
						{
							Failure = Result.error();
							break;
						}
						if (PreviousParentID != TargetParentID)
						{
#if defined(PF_DEBUG)
							if (ConsumeCommitFailurePoint(PrefabInstanceReconciler::CommitFailurePoint::AfterHierarchyTransition))
							{
								Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
									"Injected failure after a prefab hierarchy transition");
								break;
							}
#endif
						}
					}
				}

				if (!Failure)
				{
					for (const Entity& Current : Removed)
					{
						const auto Children = Current.GetChildren();
						if (!Children)
						{
							Failure = ToReconcileError(Children.error());
							break;
						}
						if (std::any_of(Children->begin(), Children->end(), [&](const Entity& Child)
							{ return !RemovedIDs.contains(Child.GetUUID()); }))
						{
							Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
								"An obsolete entity still has a surviving child after hierarchy synchronization");
							break;
						}
					}
				}
			}
			catch (const std::exception& Exception)
			{
				Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					std::string("Prefab commit raised an exception before removal: ") + Exception.what());
			}
			catch (...)
			{
				Failure = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					"Prefab commit raised an unknown exception before removal");
			}
			if (Failure) return std::unexpected(Rollback(*Failure));

#if defined(PF_DEBUG)
			if (!Removed.empty() && ConsumeCommitFailurePoint(PrefabInstanceReconciler::CommitFailurePoint::BeforeRemoval))
				return std::unexpected(Rollback(MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					"Injected failure before destructive prefab removals")));
#endif
			// Destruction is the commit boundary: all handles were verified against the unchanged backup,
			// all target parents are installed, and Scene::DestroyEntity has no allocation or callback path.
			size_t RemovedCount = 0;
			for (const Entity& Current : Removed)
			{
				if (!Current.IsValid())
				{
					const PrefabReconcileError FailureValue = MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
						"An entity selected for prefab removal became invalid before the commit boundary");
					if (RemovedCount == 0) return std::unexpected(Rollback(FailureValue));
					return std::unexpected(MakeRecoveryRequired(FailureValue,
						{ "Some obsolete entities were already removed and their original incarnations cannot be restored" }, *BackupBytes));
				}
				if (auto Result = Destination.DestroyEntity(Current); !Result)
				{
					const PrefabReconcileError FailureValue = ToReconcileError(Result.error());
					if (RemovedCount == 0) return std::unexpected(Rollback(FailureValue));
					return std::unexpected(MakeRecoveryRequired(FailureValue,
						{ "Some obsolete entities were already removed and their original incarnations cannot be restored" }, *BackupBytes));
				}
				++RemovedCount;
			}
			return {};
		}
	}

#if defined(PF_DEBUG)
	void PrefabInstanceReconciler::SetCommitFailureSequenceForTesting(
		std::initializer_list<CommitFailurePoint> Sequence)
	{
		CommitFailureSequence.assign(Sequence.begin(), Sequence.end());
	}

	void PrefabInstanceReconciler::ClearCommitFailureSequenceForTesting() noexcept
	{
		CommitFailureSequence.clear();
	}
#endif

	struct PrefabReconcilePlan::Detail
	{
		UUID InstanceID;
		AssetID SourceAsset;
		std::string SourceFingerprint;
		std::string SourceSnapshotFingerprint;
		std::string DestinationSnapshot;
		std::string TargetSnapshot;
		UUID RootSceneEntity;
		PrefabInstanceComponent ConflictOnlyMetadata;
		PrefabReconcileSummary Summary;
		std::vector<PrefabConflictRecord> Conflicts;
	};

	PrefabReconcilePlan::PrefabReconcilePlan(std::unique_ptr<Detail> Data) : m_Data(std::move(Data)) {}
	PrefabReconcilePlan::PrefabReconcilePlan(PrefabReconcilePlan&&) noexcept = default;
	PrefabReconcilePlan& PrefabReconcilePlan::operator=(PrefabReconcilePlan&&) noexcept = default;
	PrefabReconcilePlan::~PrefabReconcilePlan() = default;
	UUID PrefabReconcilePlan::GetInstanceID() const noexcept { return m_Data ? m_Data->InstanceID : UUID{}; }
	AssetID PrefabReconcilePlan::GetSourceAsset() const noexcept { return m_Data ? m_Data->SourceAsset : AssetID{}; }
	std::string_view PrefabReconcilePlan::GetSourceFingerprint() const noexcept { return m_Data ? m_Data->SourceFingerprint : std::string_view{}; }
	std::string_view PrefabReconcilePlan::GetSourceSnapshotFingerprint() const noexcept { return m_Data ? m_Data->SourceSnapshotFingerprint : std::string_view{}; }
	const PrefabReconcileSummary& PrefabReconcilePlan::GetSummary() const noexcept
	{
		static const PrefabReconcileSummary Empty;
		return m_Data ? m_Data->Summary : Empty;
	}
	const std::vector<PrefabConflictRecord>& PrefabReconcilePlan::GetConflicts() const noexcept
	{
		static const std::vector<PrefabConflictRecord> Empty;
		return m_Data ? m_Data->Conflicts : Empty;
	}
	bool PrefabReconcilePlan::CanApply() const noexcept { return m_Data && m_Data->Conflicts.empty(); }

	std::expected<PrefabInstantiationResult, PrefabReconcileError> PrefabInstanceReconciler::InstantiateLinked(
		const PrefabSourceDocument& Source,
		Scene& Destination,
		const PrefabPlacement& Placement)
	{
		if (Source.GetSourceAsset().IsNil())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "A linked prefab requires a managed source asset UUID"));
		if (Source.GetRootKey().IsNil())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Prefab source root UUID must not be nil"));
		auto SerializedSource = PrefabSerializer::Serialize(Source);
		if (!SerializedSource)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, SerializedSource.error().Message));
		const std::string SourceSnapshotFingerprint = FingerprintText(*SerializedSource);

		if (Placement.Parent)
		{
			const auto OwnedParent = Destination.FindEntity(Placement.Parent->GetUUID());
			if (!Placement.Parent->IsValid() || !OwnedParent || *OwnedParent != *Placement.Parent)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Prefab placement parent must be a valid entity owned by the destination scene"));
			Entity Parent = *Placement.Parent;
			while (true)
			{
				const auto Linked = Parent.GetPrefabInstance();
				if (!Linked) return std::unexpected(ToReconcileError(Linked.error()));
				if (Linked->has_value())
					return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Nested linked prefab instances are not supported"));
				const auto ParentResult = Parent.GetParent();
				if (!ParentResult) return std::unexpected(ToReconcileError(ParentResult.error()));
				if (!ParentResult->has_value()) break;
				Parent = **ParentResult;
			}
		}

		auto Instantiated = PrefabSerializer::Instantiate(Source, Destination, true);
		if (!Instantiated) return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, Instantiated.error().Message));
		const auto Rollback = [&]()
		{
			std::unordered_map<UUID, Entity, UUIDHash> Created;
			for (const PrefabEntityMapping& Mapping : Instantiated->SourceEntities)
				if (const auto Current = Destination.FindEntity(Mapping.SceneEntity)) Created.emplace(Mapping.SceneEntity, *Current);
			for (const auto& [ID, EntityValue] : Created) { (void)ID; (void)EntityValue.ClearParent(); }
			for (auto It = Created.begin(); It != Created.end(); ++It) (void)Destination.DestroyEntity(It->second);
		};

		if (Placement.RootTransform)
		{
			if (auto Result = Instantiated->Root.SetTransform(*Placement.RootTransform); !Result)
			{
				Rollback();
				return std::unexpected(ToReconcileError(Result.error()));
			}
		}
		if (Placement.Parent)
		{
			if (auto Result = Instantiated->Root.SetParent(*Placement.Parent); !Result)
			{
				Rollback();
				return std::unexpected(ToReconcileError(Result.error()));
			}
		}
		auto InstanceID = UUID::Generate();
		if (!InstanceID)
		{
			Rollback();
			return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, InstanceID.error().Message));
		}
		PrefabInstanceComponent Metadata;
		Metadata.InstanceID = *InstanceID;
		Metadata.SourceAsset = Source.GetSourceAsset();
		Metadata.SourceRootKey = Source.GetRootKey();
		Metadata.AppliedSourceFingerprint = SourceSnapshotFingerprint;
		Metadata.SourceEntities = Instantiated->SourceEntities;
		if (auto Validation = Metadata.Validate(Instantiated->Root.GetUUID()); !Validation)
		{
			Rollback();
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, Validation.error()));
		}
		if (auto Result = Instantiated->Root.SetPrefabInstance(Metadata); !Result)
		{
			Rollback();
			return std::unexpected(ToReconcileError(Result.error()));
		}
		return std::move(*Instantiated);
	}

	std::expected<PrefabReconcilePlan, PrefabReconcileError> PrefabInstanceReconciler::PlanReconcile(
		const Scene& Destination,
		const Entity& LinkedRoot,
		const PrefabSourceDocument& Source)
	{
		const auto OwnedRoot = Destination.FindEntity(LinkedRoot.GetUUID());
		if (!LinkedRoot.IsValid() || !OwnedRoot || *OwnedRoot != LinkedRoot)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Linked root must be a valid destination entity"));
		const auto CurrentMetadata = LinkedRoot.GetPrefabInstance();
		if (!CurrentMetadata || !CurrentMetadata->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Entity is not a linked prefab root"));
		const PrefabInstanceComponent& Current = CurrentMetadata->value();
		if (auto Validation = Current.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (Current.SourceAsset != Source.GetSourceAsset() || Current.SourceRootKey != Source.GetRootKey())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::SourceMismatch, "Prefab source does not match the linked instance identity"));

		const auto SerializedSource = PrefabSerializer::Serialize(Source);
		if (!SerializedSource)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, SerializedSource.error().Message));
		const auto SourceSnapshotFingerprint = FingerprintText(*SerializedSource);
		const auto SerializedDestination = SceneSerializer::Serialize(Destination);
		if (!SerializedDestination)
			return std::unexpected(SerializationError(SerializedDestination.error()));
		auto StagedResult = DeserializeScene(*SerializedDestination);
		if (!StagedResult) return std::unexpected(StagedResult.error());
		Scene& Staged = **StagedResult;
		const auto StagedRoot = Staged.FindEntity(LinkedRoot.GetUUID());
		if (!StagedRoot) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Linked root disappeared from destination snapshot"));
		const auto StagedMetadataResult = StagedRoot->GetPrefabInstance();
		if (!StagedMetadataResult || !StagedMetadataResult->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Linked root metadata is missing from destination snapshot"));
		PrefabInstanceComponent Metadata = StagedMetadataResult->value();

		const auto RootParent = StagedRoot->GetParent();
		if (!RootParent) return std::unexpected(ToReconcileError(RootParent.error()));
		if (RootParent->has_value())
		{
			Entity Parent = **RootParent;
			while (true)
			{
				const auto ParentLink = Parent.GetPrefabInstance();
				if (!ParentLink) return std::unexpected(ToReconcileError(ParentLink.error()));
				if (ParentLink->has_value())
					return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Nested linked prefab instances are not supported"));
				const auto ParentResult = Parent.GetParent();
				if (!ParentResult) return std::unexpected(ToReconcileError(ParentResult.error()));
				if (!ParentResult->has_value()) break;
				Parent = **ParentResult;
			}
		}

		auto StagedSubtree = GetSubtree(*StagedRoot);
		if (!StagedSubtree) return std::unexpected(StagedSubtree.error());
		EntityMap LiveBySource;
		std::unordered_map<UUID, UUID, UUIDHash> SourceByLive;
		for (const PrefabEntityMapping& Mapping : Metadata.SourceEntities)
		{
			LiveBySource.emplace(Mapping.SourceKey, Mapping.SceneEntity);
			SourceByLive.emplace(Mapping.SceneEntity, Mapping.SourceKey);
		}
		std::unordered_map<UUID, UUID, UUIDHash> LocalKeyByLive;
		for (const PrefabLocalEntityMapping& Mapping : Metadata.LocalEntities)
			LocalKeyByLive.emplace(Mapping.SceneEntity, Mapping.LocalKey);
		std::unordered_set<UUID, UUIDHash> SuppressedKeys;
		for (const PrefabOverrideRecord& Override : Metadata.Overrides)
			if (!Override.Entity.IsLocal && Override.Operation == PrefabOverrideOperation::RemoveSourceEntity)
				SuppressedKeys.insert(Override.Entity.Key);

		std::vector<PrefabConflictRecord> Conflicts;
		PrefabReconcileSummary Summary;
		std::unordered_set<UUID, UUIDHash> DescendantIDs;
		for (const Entity& CurrentEntity : *StagedSubtree)
		{
			if (CurrentEntity.GetUUID() == StagedRoot->GetUUID()) continue;
			DescendantIDs.insert(CurrentEntity.GetUUID());
			const auto Nested = CurrentEntity.GetPrefabInstance();
			if (!Nested) return std::unexpected(ToReconcileError(Nested.error()));
			if (Nested->has_value())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Nested linked prefab instances are not supported"));
			if (!SourceByLive.contains(CurrentEntity.GetUUID()) && !LocalKeyByLive.contains(CurrentEntity.GetUUID()))
			{
				auto LocalKey = UUID::Generate();
				if (!LocalKey) return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, LocalKey.error().Message));
				Metadata.LocalEntities.push_back({ *LocalKey, CurrentEntity.GetUUID() });
				LocalKeyByLive.emplace(CurrentEntity.GetUUID(), *LocalKey);
			}
		}
		std::erase_if(Metadata.LocalEntities, [&](const PrefabLocalEntityMapping& Mapping)
		{
			return !Staged.FindEntity(Mapping.SceneEntity) || !DescendantIDs.contains(Mapping.SceneEntity);
		});
		std::unordered_set<UUID, UUIDHash> ActiveLocalKeys;
		for (const PrefabLocalEntityMapping& Mapping : Metadata.LocalEntities) ActiveLocalKeys.insert(Mapping.LocalKey);
		std::erase_if(Metadata.Overrides, [&](const PrefabOverrideRecord& Override)
		{
			return Override.Entity.IsLocal && !ActiveLocalKeys.contains(Override.Entity.Key);
		});

		auto SourceData = PrefabSerializer::Parse(*SerializedSource, Source.GetSourceAsset(), std::string(Source.GetFingerprint()));
		if (!SourceData) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, SourceData.error().Message));
		const auto SourceRoot = SourceData->GetScene().FindEntity(SourceData->GetRootKey());
		if (!SourceRoot) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Prefab source root is missing"));
		auto SourceSubtree = GetSubtree(*SourceRoot);
		if (!SourceSubtree) return std::unexpected(SourceSubtree.error());
		std::unordered_set<UUID, UUIDHash> IncomingKeys;
		for (const Entity& SourceEntity : *SourceSubtree) IncomingKeys.insert(SourceEntity.GetUUID());
		for (const PrefabEntityMapping& Mapping : Metadata.SourceEntities)
		{
			if (Mapping.SourceKey == Metadata.SourceRootKey || SuppressedKeys.contains(Mapping.SourceKey) ||
				!IncomingKeys.contains(Mapping.SourceKey)) continue;
			if (!Staged.FindEntity(Mapping.SceneEntity) || !DescendantIDs.contains(Mapping.SceneEntity))
				Conflicts.push_back(Conflict({ Mapping.SourceKey, false }, PrefabConflictKind::RemovedSourceEntity,
					"A mapped source entity was deleted or detached from this instance; its mapping needs recovery"));
		}
		if (!Conflicts.empty())
		{
			auto Data = std::make_unique<PrefabReconcilePlan::Detail>();
			Data->InstanceID = Current.InstanceID;
			Data->SourceAsset = Source.GetSourceAsset();
			Data->SourceFingerprint = Source.GetFingerprint().empty() ? SourceSnapshotFingerprint : std::string(Source.GetFingerprint());
			Data->SourceSnapshotFingerprint = SourceSnapshotFingerprint;
			Data->DestinationSnapshot = *SerializedDestination;
			Data->RootSceneEntity = LinkedRoot.GetUUID();
			Data->ConflictOnlyMetadata = MakeConflictOnlyMetadata(Current, Metadata, Conflicts);
			Data->Summary.ConflictCount = Conflicts.size();
			Data->Conflicts = std::move(Conflicts);
			return PrefabReconcilePlan(std::move(Data));
		}

		std::vector<UUID> ToDestroy;
		std::erase_if(Metadata.SourceEntities, [&](const PrefabEntityMapping& Mapping)
		{
			if (IncomingKeys.contains(Mapping.SourceKey) || Mapping.SourceKey == Metadata.SourceRootKey) return false;
			if (const PrefabConflictRecord* RecoveryConflict = FindRemovedSourceRecoveryConflict(Metadata, Mapping.SourceKey))
			{
				Conflicts.push_back(*RecoveryConflict);
				return false;
			}
			const bool HasLocalDescendants = std::any_of(Metadata.LocalEntities.begin(), Metadata.LocalEntities.end(),
				[&](const PrefabLocalEntityMapping& Local)
				{
					const auto LocalEntity = Staged.FindEntity(Local.SceneEntity);
					return LocalEntity && IsDescendantOf(*LocalEntity, Mapping.SceneEntity);
				});
			if (HasOverrideFor(Metadata, Mapping.SourceKey))
				Conflicts.push_back(Conflict({ Mapping.SourceKey, false }, PrefabConflictKind::RemovedSourceEntity,
					"The source deleted an entity with local overrides; the mapped entity is retained for recovery"));
			if (HasLocalDescendants)
				Conflicts.push_back(Conflict({ Mapping.SourceKey, false }, PrefabConflictKind::RemovedEntityHasLocalChildren,
					"The source deleted an entity that contains local descendants; the entity is retained for recovery"));
			if (HasOverrideFor(Metadata, Mapping.SourceKey) || HasLocalDescendants) return false;
			ToDestroy.push_back(Mapping.SceneEntity);
			return true;
		});
		std::erase_if(Metadata.Overrides, [&](const PrefabOverrideRecord& Override)
		{
			return !Override.Entity.IsLocal && Override.Operation == PrefabOverrideOperation::RemoveSourceEntity &&
				!IncomingKeys.contains(Override.Entity.Key);
		});

		std::unordered_map<UUID, UUID, UUIDHash> TargetBySource;
		for (const PrefabEntityMapping& Mapping : Metadata.SourceEntities) TargetBySource.emplace(Mapping.SourceKey, Mapping.SceneEntity);
		for (const Entity& SourceEntity : *SourceSubtree)
		{
			if (SuppressedKeys.contains(SourceEntity.GetUUID())) continue;
			if (TargetBySource.contains(SourceEntity.GetUUID())) continue;
			const auto Tag = SourceEntity.GetTag();
			if (!Tag) return std::unexpected(ToReconcileError(Tag.error()));
			auto Created = Staged.CreateEntity(Tag->Name);
			if (!Created) return std::unexpected(ToReconcileError(Created.error()));
			Metadata.SourceEntities.push_back({ SourceEntity.GetUUID(), Created->GetUUID() });
			TargetBySource.emplace(SourceEntity.GetUUID(), Created->GetUUID());
		}

		std::sort(Metadata.SourceEntities.begin(), Metadata.SourceEntities.end(),
			[](const PrefabEntityMapping& First, const PrefabEntityMapping& Second) { return First.SourceKey < Second.SourceKey; });
		std::sort(Metadata.LocalEntities.begin(), Metadata.LocalEntities.end(),
			[](const PrefabLocalEntityMapping& First, const PrefabLocalEntityMapping& Second) { return First.LocalKey < Second.LocalKey; });
		Metadata.AppliedSourceFingerprint = SourceSnapshotFingerprint;
		Metadata.Conflicts.clear();
		if (!Conflicts.empty())
		{
			Metadata.Conflicts = Conflicts;
			auto Data = std::make_unique<PrefabReconcilePlan::Detail>();
			Data->InstanceID = Current.InstanceID;
			Data->SourceAsset = Source.GetSourceAsset();
			Data->SourceFingerprint = Source.GetFingerprint().empty() ? SourceSnapshotFingerprint : std::string(Source.GetFingerprint());
			Data->SourceSnapshotFingerprint = SourceSnapshotFingerprint;
			Data->DestinationSnapshot = *SerializedDestination;
			Data->RootSceneEntity = LinkedRoot.GetUUID();
			Data->ConflictOnlyMetadata = Current;
			Data->ConflictOnlyMetadata.LocalEntities = Metadata.LocalEntities;
			Summary.ConflictCount = Conflicts.size();
			Data->Summary = Summary;
			Data->Conflicts = std::move(Conflicts);
			return PrefabReconcilePlan(std::move(Data));
		}
		// SetParent validates membership against the staged root's persisted mapping. Publish the
		// complete candidate mapping before assigning any source hierarchy relationships.
		if (auto Result = StagedRoot->SetPrefabInstance(Metadata); !Result)
			return std::unexpected(ToReconcileError(Result.error()));

		for (const Entity& SourceEntity : *SourceSubtree)
		{
			const UUID SourceKey = SourceEntity.GetUUID();
			if (SuppressedKeys.contains(SourceKey)) continue;
			const auto LiveID = TargetBySource.find(SourceKey);
			if (LiveID == TargetBySource.end()) continue;
			const auto LiveEntity = Staged.FindEntity(LiveID->second);
			if (!LiveEntity) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Mapped instance entity is missing"));
			const bool IsRoot = SourceKey == Metadata.SourceRootKey;
			const bool ExistingSourceMapping = LiveBySource.contains(SourceKey);
			if (auto Copy = CopySourceComponents(SourceEntity, *LiveEntity, IsRoot, ExistingSourceMapping); !Copy)
				return std::unexpected(Copy.error());
			if (!IsRoot)
			{
				const auto SourceParent = SourceEntity.GetParent();
				if (!SourceParent) return std::unexpected(ToReconcileError(SourceParent.error()));
				if (SourceParent->has_value())
				{
					const auto ParentMapping = TargetBySource.find((**SourceParent).GetUUID());
					if (ParentMapping == TargetBySource.end())
						return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Source hierarchy parent is not mapped"));
					const auto Parent = Staged.FindEntity(ParentMapping->second);
					if (!Parent) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Mapped source parent is missing"));
					if (auto Result = LiveEntity->SetParent(*Parent); !Result) return std::unexpected(ToReconcileError(Result.error()));
				}
			}
			if (ExistingSourceMapping)
				++Summary.ExistingEntitiesProcessed;
			else ++Summary.Added;
		}

		const size_t EntityCountBeforeOverrides = Staged.GetEntityCount();
		for (const PrefabOverrideRecord& Override : Metadata.Overrides)
		{
			if (auto Result = ApplyOverride(Staged, Metadata, Override, Conflicts); !Result)
				return std::unexpected(Result.error());
		}
		Summary.Removed += EntityCountBeforeOverrides - Staged.GetEntityCount();
		if (!Conflicts.empty())
		{
			Metadata.Conflicts = Conflicts;
			const auto CurrentBytes = SceneSerializer::Serialize(Destination);
			if (!CurrentBytes) return std::unexpected(SerializationError(CurrentBytes.error()));
			auto Data = std::make_unique<PrefabReconcilePlan::Detail>();
			Data->InstanceID = Current.InstanceID;
			Data->SourceAsset = Source.GetSourceAsset();
			Data->SourceFingerprint = Source.GetFingerprint().empty() ? SourceSnapshotFingerprint : std::string(Source.GetFingerprint());
			Data->SourceSnapshotFingerprint = SourceSnapshotFingerprint;
			Data->DestinationSnapshot = *CurrentBytes;
			Data->RootSceneEntity = LinkedRoot.GetUUID();
			Data->ConflictOnlyMetadata = MakeConflictOnlyMetadata(Current, Metadata, Conflicts);
			Summary.ConflictCount = Conflicts.size();
			Data->Summary = Summary;
			Data->Conflicts = std::move(Conflicts);
			return PrefabReconcilePlan(std::move(Data));
		}

		auto Data = std::make_unique<PrefabReconcilePlan::Detail>();
		for (const UUID ID : ToDestroy)
		{
			if (const auto CurrentEntity = Staged.FindEntity(ID))
				if (auto Result = Staged.DestroyEntity(*CurrentEntity); !Result) return std::unexpected(ToReconcileError(Result.error()));
			++Summary.Removed;
		}
		Metadata.Conflicts.clear();
		Metadata.AppliedSourceFingerprint = SourceSnapshotFingerprint;
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Result = StagedRoot->SetPrefabInstance(Metadata); !Result) return std::unexpected(ToReconcileError(Result.error()));
		Data->Summary = Summary;
		if (auto TargetBytes = SceneSerializer::Serialize(Staged); !TargetBytes) return std::unexpected(SerializationError(TargetBytes.error()));
		else Data->TargetSnapshot = *TargetBytes;
		Data->InstanceID = Current.InstanceID;
		Data->SourceAsset = Source.GetSourceAsset();
		Data->SourceFingerprint = Source.GetFingerprint().empty() ? SourceSnapshotFingerprint : std::string(Source.GetFingerprint());
		Data->SourceSnapshotFingerprint = SourceSnapshotFingerprint;
		Data->DestinationSnapshot = *SerializedDestination;
		Data->RootSceneEntity = LinkedRoot.GetUUID();
		return PrefabReconcilePlan(std::move(Data));
	}

	std::expected<PrefabReconcileSummary, PrefabReconcileError> PrefabInstanceReconciler::ApplyReconcile(
		Scene& Destination,
		const PrefabSourceDocument& Source,
		const PrefabReconcilePlan& Plan)
	{
		if (!Plan.m_Data) return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan, "Reconcile plan is empty or moved from"));
		if (Source.GetSourceAsset() != Plan.m_Data->SourceAsset)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan, "Prefab source identity changed after planning"));
		const auto CurrentSource = PrefabSerializer::Serialize(Source);
		if (!CurrentSource) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, CurrentSource.error().Message));
		if (FingerprintText(*CurrentSource) != Plan.m_Data->SourceSnapshotFingerprint)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan, "Prefab source changed after the reconcile plan was built"));
		const auto CurrentBytes = SceneSerializer::Serialize(Destination);
		if (!CurrentBytes) return std::unexpected(SerializationError(CurrentBytes.error()));
		if (*CurrentBytes != Plan.m_Data->DestinationSnapshot)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan, "Destination scene changed after the reconcile plan was built"));
		if (!Plan.m_Data->Conflicts.empty())
		{
			const auto Root = Destination.FindEntity(Plan.m_Data->RootSceneEntity);
			if (!Root) return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan, "Linked root no longer exists"));
			const auto CurrentInstance = Root->GetPrefabInstance();
			if (!CurrentInstance || !CurrentInstance->has_value() || CurrentInstance->value().InstanceID != Plan.m_Data->InstanceID)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::StalePlan, "Linked instance identity changed after planning"));
			PrefabInstanceComponent Metadata = Plan.m_Data->ConflictOnlyMetadata;
			Metadata.Conflicts = Plan.m_Data->Conflicts;
			if (auto Result = Root->SetPrefabInstance(Metadata); !Result) return std::unexpected(ToReconcileError(Result.error()));
			return Plan.m_Data->Summary;
		}
		auto Target = DeserializeScene(Plan.m_Data->TargetSnapshot);
		if (!Target) return std::unexpected(Target.error());
		auto Backup = DeserializeScene(Plan.m_Data->DestinationSnapshot);
		if (!Backup) return std::unexpected(Backup.error());
		if (auto Result = CommitScene(Destination, **Target, **Backup); !Result) return std::unexpected(Result.error());
		return Plan.m_Data->Summary;
	}

	std::expected<PrefabOverrideEditSummary, PrefabReconcileError> PrefabInstanceReconciler::ApplyOverridesToSource(
		PrefabSourceDocument& Source,
		const PrefabInstanceComponent& Instance,
		std::optional<PrefabEntityAddress> OnlyEntity)
	{
		const auto RootMapping = std::find_if(Instance.SourceEntities.begin(), Instance.SourceEntities.end(),
			[&Instance](const PrefabEntityMapping& Mapping) { return Mapping.SourceKey == Instance.SourceRootKey; });
		if (RootMapping == Instance.SourceEntities.end() || Instance.SourceAsset != Source.GetSourceAsset() ||
			Instance.SourceRootKey != Source.GetRootKey())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::SourceMismatch,
				"The selected source document does not match this linked prefab instance"));
		if (auto Validation = Instance.Validate(RootMapping->SceneEntity); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (std::any_of(Instance.Conflicts.begin(), Instance.Conflicts.end(), [&](const PrefabConflictRecord& ConflictValue)
			{ return !OnlyEntity || ConflictValue.Entity == *OnlyEntity; }))
			return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
				"Resolve the selected linked-instance conflict before applying its overrides to the source draft",
				Instance.Conflicts));

		const auto Before = SceneSerializer::Serialize(Source.GetScene());
		if (!Before) return std::unexpected(SerializationError(Before.error()));
		auto StagedResult = DeserializeScene(*Before);
		if (!StagedResult) return std::unexpected(StagedResult.error());
		Scene& Staged = **StagedResult;
		const auto SourceRoot = Staged.FindEntity(Source.GetRootKey());
		if (!SourceRoot) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Prefab source root is missing"));
		auto SourceSubtree = GetSubtree(*SourceRoot);
		if (!SourceSubtree) return std::unexpected(SourceSubtree.error());
		std::unordered_set<UUID, UUIDHash> SourceKeys;
		for (const Entity& EntityValue : *SourceSubtree) SourceKeys.insert(EntityValue.GetUUID());

		PrefabOverrideEditSummary Summary;
		std::vector<const PrefabOverrideRecord*> Applicable;
		std::vector<const PrefabOverrideRecord*> RemovalOperations;
		for (const PrefabOverrideRecord& Override : Instance.Overrides)
		{
			if (OnlyEntity && Override.Entity != *OnlyEntity) continue;
			if (Override.Entity.IsLocal || Override.Operation == PrefabOverrideOperation::AddLocalEntity)
			{
				++Summary.RetainedInstanceLocal;
				continue;
			}
			if (Override.Operation == PrefabOverrideOperation::ResetProperty) continue;
			if (Override.Operation == PrefabOverrideOperation::AddComponent ||
				Override.Operation == PrefabOverrideOperation::RemoveComponent)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
					"Component presence overrides are not supported by the current typed property adapter"));
			if (!SourceKeys.contains(Override.Entity.Key))
				return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
					"An overridden source entity no longer exists; its override remains recoverable",
					{ Conflict(Override.Entity, PrefabConflictKind::RemovedSourceEntity,
						"The source entity was removed before this override could be applied") }));
			if (Override.Operation == PrefabOverrideOperation::RemoveSourceEntity)
				RemovalOperations.push_back(&Override);
			else
				Applicable.push_back(&Override);
		}

		for (const PrefabOverrideRecord* Override : Applicable)
		{
			const auto Target = Staged.FindEntity(Override->Entity.Key);
			if (!Target)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Overridden prefab source entity is missing"));
			if (Override->Operation == PrefabOverrideOperation::SetProperty)
			{
				if (Override->Entity.Key == Source.GetRootKey() && Override->ComponentID == PropertyComponentIds::Transform)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
						"The linked root transform is instance placement and cannot be applied to the source"));
				const auto Current = ReadProperty(*Target, Override->ComponentID, Override->PropertyID);
				if (!Current || Current->index() != Override->Value.index())
					return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
						Current ? "The source property type changed after the override was recorded" : Current.error().Message,
						{ Conflict(Override->Entity,
							Current ? PrefabConflictKind::ChangedPropertyType : PrefabConflictKind::MissingProperty,
							Current ? "The source property type changed after the override was recorded" : Current.error().Message,
							Override->ComponentID, Override->PropertyID) }));
				if (auto Written = WriteProperty(*Target, Override->ComponentID, Override->PropertyID, Override->Value); !Written)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, Written.error().Message));
			}
			else if (Override->Operation == PrefabOverrideOperation::Rename)
			{
				auto Tag = Target->GetTag();
				if (!Tag) return std::unexpected(ToReconcileError(Tag.error()));
				Tag->Name = Override->Name;
				if (auto Written = Target->SetTag(*Tag); !Written) return std::unexpected(ToReconcileError(Written.error()));
			}
			else if (Override->Operation == PrefabOverrideOperation::Reparent)
			{
				if (!Override->Parent || Override->Parent->IsLocal)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
						"A reparent to an instance-local entity remains instance-owned and cannot be applied to the source"));
				const auto Parent = Staged.FindEntity(Override->Parent->Key);
				if (!Parent || !SourceKeys.contains(Override->Parent->Key))
					return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
						"The source parent for this reparent no longer exists",
						{ Conflict(Override->Entity, PrefabConflictKind::ConflictingReparent,
							"The source parent for this reparent no longer exists") }));
				if (auto Parented = Target->SetParent(*Parent); !Parented)
					return std::unexpected(ToReconcileError(Parented.error()));
			}
			else
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
					"This linked-instance operation cannot be applied to the prefab source"));
			++Summary.Applied;
		}

		// Non-removal operations keep their authored order above. Their reparent changes are therefore
		// present when removal dependencies are computed: survivors can move away before old parents go.
		auto FinalHierarchy = GetParentFirstOrder(Staged);
		if (!FinalHierarchy) return std::unexpected(FinalHierarchy.error());
		std::unordered_map<UUID, size_t, UUIDHash> FinalDepth;
		FinalDepth.reserve(FinalHierarchy->size());
		for (const Entity& Current : *FinalHierarchy)
		{
			const auto Parent = Current.GetParent();
			if (!Parent) return std::unexpected(ToReconcileError(Parent.error()));
			FinalDepth[Current.GetUUID()] = Parent->has_value()
				? FinalDepth.at((**Parent).GetUUID()) + 1 : 0;
		}
		std::sort(RemovalOperations.begin(), RemovalOperations.end(), [&](const auto* First, const auto* Second)
		{
			const size_t FirstDepth = FinalDepth.at(First->Entity.Key);
			const size_t SecondDepth = FinalDepth.at(Second->Entity.Key);
			return FirstDepth != SecondDepth ? FirstDepth > SecondDepth : First->Entity.Key < Second->Entity.Key;
		});
		for (const PrefabOverrideRecord* Override : RemovalOperations)
		{
			if (Override->Entity.Key == Source.GetRootKey())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
					"The prefab source root cannot be removed"));
			const auto Target = Staged.FindEntity(Override->Entity.Key);
			if (!Target)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource,
					"Overridden prefab source entity is missing"));
			const auto Children = Target->GetChildren();
			if (!Children) return std::unexpected(ToReconcileError(Children.error()));
			if (!Children->empty())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
					"Only a source leaf can be removed by the current prefab operation",
					{ Conflict(Override->Entity, PrefabConflictKind::RemovedEntityHasLocalChildren,
						"Remove or reparent source descendants before applying this entity removal") }));
			if (auto Removed = Staged.DestroyEntity(*Target); !Removed)
				return std::unexpected(ToReconcileError(Removed.error()));
			++Summary.Applied;
		}

		if (Summary.Applied == 0) return Summary;
		if (auto TargetBytes = SceneSerializer::Serialize(Staged); !TargetBytes)
			return std::unexpected(SerializationError(TargetBytes.error()));
		auto Backup = DeserializeScene(*Before);
		if (!Backup) return std::unexpected(Backup.error());
		if (auto Committed = CommitScene(Source.GetScene(), Staged, **Backup); !Committed)
			return std::unexpected(Committed.error());
		return Summary;
	}

	std::expected<PrefabOverrideEditSummary, PrefabReconcileError> PrefabInstanceReconciler::RevertOverrides(
		Scene& Destination,
		const Entity& LinkedRoot,
		const PrefabSourceDocument& Source,
		std::optional<PrefabEntityAddress> OnlyEntity)
	{
		const auto OwnedRoot = Destination.FindEntity(LinkedRoot.GetUUID());
		const auto MetadataResult = LinkedRoot.GetPrefabInstance();
		if (!LinkedRoot.IsValid() || !OwnedRoot || *OwnedRoot != LinkedRoot || !MetadataResult || !MetadataResult->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override revert requires a valid linked root"));
		PrefabInstanceComponent Metadata = MetadataResult->value();
		if (Metadata.SourceAsset != Source.GetSourceAsset() || Metadata.SourceRootKey != Source.GetRootKey())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::SourceMismatch, "Prefab source does not match the linked instance"));
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		const auto Before = SceneSerializer::Serialize(Destination);
		if (!Before) return std::unexpected(SerializationError(Before.error()));
		auto StagedResult = DeserializeScene(*Before);
		if (!StagedResult) return std::unexpected(StagedResult.error());
		Scene& Staged = **StagedResult;
		const auto StagedRoot = Staged.FindEntity(LinkedRoot.GetUUID());
		if (!StagedRoot) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Linked root disappeared from scene staging"));
		auto StagedMetadataResult = StagedRoot->GetPrefabInstance();
		if (!StagedMetadataResult || !StagedMetadataResult->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Linked instance metadata disappeared from scene staging"));
		Metadata = StagedMetadataResult->value();
		const auto BeforeSource = PrefabSerializer::Serialize(Source);
		if (!BeforeSource) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, BeforeSource.error().Message));
		const auto SourceRoot = Source.GetScene().FindEntity(Source.GetRootKey());
		if (!SourceRoot) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Prefab source root is missing"));
		auto SourceSubtree = GetSubtree(*SourceRoot);
		if (!SourceSubtree) return std::unexpected(SourceSubtree.error());
		std::unordered_map<UUID, Entity, UUIDHash> SourceByKey;
		for (const Entity& SourceEntity : *SourceSubtree) SourceByKey.emplace(SourceEntity.GetUUID(), SourceEntity);

		std::vector<const PrefabOverrideRecord*> Reverted;
		for (const PrefabOverrideRecord& Override : Metadata.Overrides)
		{
			if (Override.Entity.IsLocal || (OnlyEntity && Override.Entity != *OnlyEntity)) continue;
			if (Override.Operation == PrefabOverrideOperation::AddLocalEntity) continue;
			Reverted.push_back(&Override);
		}
		if (Reverted.empty()) return PrefabOverrideEditSummary{};

		PrefabOverrideEditSummary Summary;
		std::unordered_map<UUID, UUID, UUIDHash> Resurrected;
		for (const PrefabOverrideRecord* Override : Reverted)
		{
			if (Override->Operation != PrefabOverrideOperation::RemoveSourceEntity) continue;
			const auto SourceEntity = SourceByKey.find(Override->Entity.Key);
			if (SourceEntity == SourceByKey.end())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
					"The removed source entity is still missing; restore the source before reverting this instance removal",
					{ Conflict(Override->Entity, PrefabConflictKind::RemovedSourceEntity,
						"The removed source entity is still missing") }));
			const auto Mapping = std::find_if(Metadata.SourceEntities.begin(), Metadata.SourceEntities.end(),
				[&Override](const PrefabEntityMapping& Item) { return Item.SourceKey == Override->Entity.Key; });
			if (Mapping == Metadata.SourceEntities.end())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Removed source entity mapping is missing"));
			if (Staged.FindEntity(Mapping->SceneEntity)) continue;
			const auto Tag = SourceEntity->second.GetTag();
			if (!Tag) return std::unexpected(ToReconcileError(Tag.error()));
			auto Created = Staged.CreateEntity(Tag->Name);
			if (!Created) return std::unexpected(ToReconcileError(Created.error()));
			Resurrected.emplace(Override->Entity.Key, Created->GetUUID());
			Mapping->SceneEntity = Created->GetUUID();
		}
		// Restored entities must be recognized as members before their original parent links
		// are rebuilt by Entity::SetParent.
		if (auto Updated = StagedRoot->SetPrefabInstance(Metadata); !Updated)
			return std::unexpected(ToReconcileError(Updated.error()));

		const auto ResolveMappedLive = [&](UUID SourceKey) -> std::optional<Entity>
		{
			if (const auto ResurrectedIt = Resurrected.find(SourceKey); ResurrectedIt != Resurrected.end())
				return Staged.FindEntity(ResurrectedIt->second);
			const auto Mapping = std::find_if(Metadata.SourceEntities.begin(), Metadata.SourceEntities.end(),
				[SourceKey](const PrefabEntityMapping& Item) { return Item.SourceKey == SourceKey; });
			return Mapping == Metadata.SourceEntities.end() ? std::nullopt : Staged.FindEntity(Mapping->SceneEntity);
		};
		const auto ResolveSourceParent = [&](const Entity& SourceEntity) -> std::expected<std::optional<Entity>, PrefabReconcileError>
		{
			const auto Parent = SourceEntity.GetParent();
			if (!Parent) return std::unexpected(ToReconcileError(Parent.error()));
			if (!Parent->has_value()) return std::optional<Entity>{};
			const auto ParentLive = ResolveMappedLive((**Parent).GetUUID());
			if (!ParentLive)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
					"The source parent has no live entity mapping; revert its removal first",
					{ Conflict({ SourceEntity.GetUUID(), false }, PrefabConflictKind::ConflictingReparent,
						"The source parent has no live entity mapping") }));
			return ParentLive;
		};

		for (const auto& [SourceKey, LiveID] : Resurrected)
		{
			const Entity SourceEntity = SourceByKey.at(SourceKey);
			const auto Live = Staged.FindEntity(LiveID);
			if (!Live) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Restored scene entity is unavailable"));
			if (auto Copied = CopySourceComponents(SourceEntity, *Live, false, false); !Copied)
				return std::unexpected(Copied.error());
			auto Parent = ResolveSourceParent(SourceEntity);
			if (!Parent) return std::unexpected(Parent.error());
			if (Parent->has_value())
				if (auto Parented = Live->SetParent(**Parent); !Parented) return std::unexpected(ToReconcileError(Parented.error()));
		}

		std::vector<const PrefabOverrideRecord*> Ordered = Reverted;
		std::stable_sort(Ordered.begin(), Ordered.end(), [](const auto* First, const auto* Second)
		{
			return (First->Operation == PrefabOverrideOperation::RemoveSourceEntity) <
				(Second->Operation == PrefabOverrideOperation::RemoveSourceEntity);
		});
		for (const PrefabOverrideRecord* Override : Ordered)
		{
			const auto SourceEntityIt = SourceByKey.find(Override->Entity.Key);
			if (SourceEntityIt == SourceByKey.end())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
					"The source entity for this override is missing; instance data was preserved",
					{ Conflict(Override->Entity, PrefabConflictKind::RemovedSourceEntity,
						"The source entity for this override is missing") }));
			const Entity SourceEntity = SourceEntityIt->second;
			auto Live = ResolveMappedLive(Override->Entity.Key);
			if (!Live)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
					"The mapped entity is missing; only a recorded source removal can recreate it",
					{ Conflict(Override->Entity, PrefabConflictKind::RemovedSourceEntity, "The mapped entity is missing") }));
			if (Override->Operation == PrefabOverrideOperation::SetProperty ||
				Override->Operation == PrefabOverrideOperation::ResetProperty)
			{
				auto SourceValue = ReadProperty(SourceEntity, Override->ComponentID, Override->PropertyID);
				if (!SourceValue)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected, SourceValue.error().Message,
						{ Conflict(Override->Entity, PrefabConflictKind::MissingProperty, SourceValue.error().Message,
							Override->ComponentID, Override->PropertyID) }));
				const auto Current = ReadProperty(*Live, Override->ComponentID, Override->PropertyID);
				if (!Current || Current->index() != SourceValue->index())
					return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
						Current ? "The source property type changed" : Current.error().Message,
						{ Conflict(Override->Entity, PrefabConflictKind::ChangedPropertyType,
							Current ? "The source property type changed" : Current.error().Message,
							Override->ComponentID, Override->PropertyID) }));
				if (auto Written = WriteProperty(*Live, Override->ComponentID, Override->PropertyID, *SourceValue); !Written)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, Written.error().Message));
			}
			else if (Override->Operation == PrefabOverrideOperation::Rename)
			{
				const auto Tag = SourceEntity.GetTag();
				if (!Tag) return std::unexpected(ToReconcileError(Tag.error()));
				if (auto Written = Live->SetTag(*Tag); !Written) return std::unexpected(ToReconcileError(Written.error()));
			}
			else if (Override->Operation == PrefabOverrideOperation::Reparent)
			{
				auto Parent = ResolveSourceParent(SourceEntity);
				if (!Parent) return std::unexpected(Parent.error());
				if (Parent->has_value())
				{
					if (auto Parented = Live->SetParent(**Parent); !Parented) return std::unexpected(ToReconcileError(Parented.error()));
				}
				else if (Override->Entity.Key != Metadata.SourceRootKey)
					return std::unexpected(MakeError(PrefabReconcileErrorCode::ConflictsDetected,
						"A non-root prefab source entity cannot be detached from its source hierarchy"));
			}
			else if (Override->Operation == PrefabOverrideOperation::RemoveSourceEntity)
			{
				if (!Resurrected.contains(Override->Entity.Key))
				{
					if (auto Copied = CopySourceComponents(SourceEntity, *Live, false, true); !Copied)
						return std::unexpected(Copied.error());
					auto Parent = ResolveSourceParent(SourceEntity);
					if (!Parent) return std::unexpected(Parent.error());
					if (Parent->has_value())
						if (auto Parented = Live->SetParent(**Parent); !Parented) return std::unexpected(ToReconcileError(Parented.error()));
				}
			}
			else
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
					"This override kind does not have a source-backed revert operation"));
			++Summary.Applied;
		}

		std::erase_if(Metadata.Overrides, [&](const PrefabOverrideRecord& Override)
		{
			return !Override.Entity.IsLocal && (!OnlyEntity || Override.Entity == *OnlyEntity);
		});
		std::erase_if(Metadata.Conflicts, [&](const PrefabConflictRecord& ConflictValue)
		{
			return !ConflictValue.Entity.IsLocal && (!OnlyEntity || ConflictValue.Entity == *OnlyEntity);
		});
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Updated = StagedRoot->SetPrefabInstance(Metadata); !Updated) return std::unexpected(ToReconcileError(Updated.error()));

		auto TargetBytes = SceneSerializer::Serialize(Staged);
		if (!TargetBytes) return std::unexpected(SerializationError(TargetBytes.error()));
		auto Backup = DeserializeScene(*Before);
		if (!Backup) return std::unexpected(Backup.error());
		if (auto Committed = CommitScene(Destination, Staged, **Backup); !Committed) return std::unexpected(Committed.error());
		return Summary;
	}

	std::expected<std::optional<Entity>, PrefabReconcileError> PrefabInstanceReconciler::FindLinkedRoot(
		const Scene& Destination,
		const Entity& Target)
	{
		const auto OwnedTarget = Destination.FindEntity(Target.GetUUID());
		if (!Target.IsValid() || !OwnedTarget || *OwnedTarget != Target)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Prefab target must be a valid entity in the destination scene"));
		Entity Current = Target;
		for (size_t Depth = 0; Depth <= Destination.GetEntityCount(); ++Depth)
		{
			const auto Metadata = Current.GetPrefabInstance();
			if (!Metadata) return std::unexpected(ToReconcileError(Metadata.error()));
			if (Metadata->has_value()) return std::optional<Entity>{ Current };
			const auto Parent = Current.GetParent();
			if (!Parent) return std::unexpected(ToReconcileError(Parent.error()));
			if (!Parent->has_value()) return std::optional<Entity>{};
			Current = **Parent;
		}
		return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
			"Prefab target hierarchy exceeded the scene entity bound"));
	}

	std::expected<PrefabEntityAddress, PrefabReconcileError> PrefabInstanceReconciler::ResolveOrRegisterEntityAddress(
		const Entity& LinkedRoot,
		const Entity& Target)
	{
		const auto MetadataResult = LinkedRoot.GetPrefabInstance();
		if (!LinkedRoot.IsValid() || !MetadataResult || !MetadataResult->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Entity is not a valid linked prefab root"));
		PrefabInstanceComponent Metadata = MetadataResult->value();
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		auto Subtree = GetSubtree(LinkedRoot);
		if (!Subtree) return std::unexpected(Subtree.error());
		const auto TargetIt = std::find_if(Subtree->begin(), Subtree->end(), [&](const Entity& Current)
		{
			return Current == Target;
		});
		if (TargetIt == Subtree->end())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Prefab edit target is outside the linked instance subtree"));
		for (const PrefabEntityMapping& Mapping : Metadata.SourceEntities)
			if (Mapping.SceneEntity == Target.GetUUID()) return PrefabEntityAddress{ Mapping.SourceKey, false };
		for (const PrefabLocalEntityMapping& Mapping : Metadata.LocalEntities)
			if (Mapping.SceneEntity == Target.GetUUID()) return PrefabEntityAddress{ Mapping.LocalKey, true };
		auto LocalKey = UUID::Generate();
		if (!LocalKey)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed, LocalKey.error().Message));
		Metadata.LocalEntities.push_back({ *LocalKey, Target.GetUUID() });
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Result = LinkedRoot.SetPrefabInstance(Metadata); !Result)
			return std::unexpected(ToReconcileError(Result.error()));
		return PrefabEntityAddress{ *LocalKey, true };
	}

	std::expected<PrefabEntityAddress, PrefabReconcileError> PrefabInstanceReconciler::AttachSubtreeAsLocal(
		Scene& Destination,
		const Entity& LinkedRoot,
		const Entity& Target,
		const Entity& Parent)
	{
		const auto OwnedRoot = Destination.FindEntity(LinkedRoot.GetUUID());
		const auto OwnedTarget = Destination.FindEntity(Target.GetUUID());
		const auto OwnedParent = Destination.FindEntity(Parent.GetUUID());
		if (!LinkedRoot.IsValid() || !OwnedRoot || *OwnedRoot != LinkedRoot || !OwnedTarget || *OwnedTarget != Target ||
			!OwnedParent || *OwnedParent != Parent)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Local subtree attachment requires live entities owned by the destination scene"));

		const auto CurrentMetadata = LinkedRoot.GetPrefabInstance();
		if (!CurrentMetadata || !CurrentMetadata->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Local subtree attachment requires a linked prefab root"));
		PrefabInstanceComponent Metadata = **CurrentMetadata;
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));

		const auto ParentRoot = FindLinkedRoot(Destination, Parent);
		if (!ParentRoot || !ParentRoot->has_value() || **ParentRoot != LinkedRoot)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Local content must be attached beneath its own linked prefab instance"));
		const auto TargetRoot = FindLinkedRoot(Destination, Target);
		if (!TargetRoot) return std::unexpected(TargetRoot.error());
		if (TargetRoot->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Only an unlinked scene subtree can be adopted as local prefab content"));

		auto Subtree = GetSubtree(Target);
		if (!Subtree) return std::unexpected(Subtree.error());
		if (std::any_of(Subtree->begin(), Subtree->end(), [&](const Entity& Current) { return Current == Parent; }))
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"A local subtree cannot be parented beneath one of its own descendants"));

		std::unordered_set<UUID, UUIDHash> SubtreeIDs;
		SubtreeIDs.reserve(Subtree->size());
		for (const Entity& Current : *Subtree)
		{
			const auto Owned = Destination.FindEntity(Current.GetUUID());
			const auto NestedRoot = Current.GetPrefabInstance();
			if (!Owned || *Owned != Current || !NestedRoot)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
					"The candidate local subtree contains an invalid or unreadable entity"));
			if (NestedRoot->has_value())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
					"Nested linked prefab instances cannot be adopted as local content"));
			SubtreeIDs.insert(Current.GetUUID());
		}
		for (const PrefabEntityMapping& Mapping : Metadata.SourceEntities)
			if (SubtreeIDs.contains(Mapping.SceneEntity))
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
					"The candidate subtree already contains an entity mapped by this prefab instance"));
		for (const PrefabLocalEntityMapping& Mapping : Metadata.LocalEntities)
			if (SubtreeIDs.contains(Mapping.SceneEntity))
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
					"The candidate subtree already contains an instance-local entity"));

		std::unordered_set<UUID, UUIDHash> LocalKeys;
		LocalKeys.reserve(Metadata.LocalEntities.size() + Subtree->size());
		for (const PrefabLocalEntityMapping& Mapping : Metadata.LocalEntities) LocalKeys.insert(Mapping.LocalKey);
		std::optional<PrefabEntityAddress> TargetAddress;
		for (const Entity& Current : *Subtree)
		{
			auto LocalKey = UUID::Generate();
			if (!LocalKey || !LocalKeys.insert(*LocalKey).second)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::SceneOperationFailed,
					LocalKey ? "Could not allocate a unique local prefab identity" : LocalKey.error().Message));
			Metadata.LocalEntities.push_back({ *LocalKey, Current.GetUUID() });
			if (Current == Target) TargetAddress = PrefabEntityAddress{ *LocalKey, true };
		}
		if (!TargetAddress)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"The candidate local subtree did not contain its requested root"));
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));

		const auto Before = SceneSerializer::Serialize(Destination);
		if (!Before) return std::unexpected(SerializationError(Before.error()));
		if (auto Updated = LinkedRoot.SetPrefabInstance(Metadata); !Updated)
			return std::unexpected(ToReconcileError(Updated.error()));
		if (auto Parented = Target.SetParent(Parent); !Parented)
		{
			const auto RestoredMetadata = LinkedRoot.SetPrefabInstance(**CurrentMetadata);
			const auto RestoredScene = SceneSerializer::Serialize(Destination);
			if (!RestoredMetadata || !RestoredScene || *RestoredScene != *Before)
			{
				PrefabReconcileError Recovery = MakeError(PrefabReconcileErrorCode::RecoveryRequired,
					"Local subtree attachment failed and its previous scene state could not be verified; retain the recovery snapshot");
				Recovery.RecoverySnapshot = *Before;
				return std::unexpected(std::move(Recovery));
			}
			return std::unexpected(ToReconcileError(Parented.error()));
		}
		return *TargetAddress;
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::RecordPropertyOverride(
		const Entity& LinkedRoot,
		const Entity& Target,
		std::string ComponentID,
		std::string PropertyID,
		PropertyValue Value)
	{
		const auto Metadata = LinkedRoot.GetPrefabInstance();
		if (!Metadata || !Metadata->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Property target is not in a linked prefab instance"));
		const auto CurrentValue = ReadProperty(Target, ComponentID, PropertyID);
		if (!CurrentValue)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, CurrentValue.error().Message));
		if (CurrentValue->index() != Value.index())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
				"Override value type does not match the authored property type"));
		const auto Address = ResolveOrRegisterEntityAddress(LinkedRoot, Target);
		if (!Address) return std::unexpected(Address.error());
		if (Address->IsLocal) return {}; // Local entity state is stored in the scene, not as a prefab source override.
		if (Address->Key == (**Metadata).SourceRootKey && ComponentID == PropertyComponentIds::Transform)
			return {};
		PrefabOverrideRecord Override;
		Override.Entity = *Address;
		Override.Operation = PrefabOverrideOperation::SetProperty;
		Override.ComponentID = std::move(ComponentID);
		Override.PropertyID = std::move(PropertyID);
		Override.Value = std::move(Value);
		return RecordOverride(LinkedRoot, Override);
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::RecordRenameOverride(
		const Entity& LinkedRoot,
		const Entity& Target,
		std::string Name)
	{
		if (Name.empty() || Name.size() > 4096)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "Prefab rename is empty or exceeds its bound"));
		const auto Address = ResolveOrRegisterEntityAddress(LinkedRoot, Target);
		if (!Address) return std::unexpected(Address.error());
		if (Address->IsLocal) return {};
		PrefabOverrideRecord Override;
		Override.Entity = *Address;
		Override.Operation = PrefabOverrideOperation::Rename;
		Override.Name = std::move(Name);
		return RecordOverride(LinkedRoot, Override);
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::RecordReparentOverride(
		const Entity& LinkedRoot,
		const Entity& Target,
		const std::optional<Entity>& Parent)
	{
		const auto Metadata = LinkedRoot.GetPrefabInstance();
		if (!Metadata || !Metadata->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Reparent target is not in a linked prefab instance"));
		if (Target == LinkedRoot) return {}; // The linked root's parent is instance placement.
		if (!Parent)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
				"A linked child must remain parented inside its instance; detach is not a supported override"));
		const auto Address = ResolveOrRegisterEntityAddress(LinkedRoot, Target);
		const auto ParentAddress = ResolveOrRegisterEntityAddress(LinkedRoot, *Parent);
		if (!Address) return std::unexpected(Address.error());
		if (!ParentAddress) return std::unexpected(ParentAddress.error());
		if (Address->IsLocal) return {};
		PrefabOverrideRecord Override;
		Override.Entity = *Address;
		Override.Operation = PrefabOverrideOperation::Reparent;
		Override.Parent = *ParentAddress;
		return RecordOverride(LinkedRoot, Override);
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::RecordRemoveSourceEntityOverride(
		const Entity& LinkedRoot,
		const Entity& Target)
	{
		const auto MetadataResult = LinkedRoot.GetPrefabInstance();
		if (!MetadataResult || !MetadataResult->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Entity is not a linked prefab root"));
		const auto Mapping = std::find_if((**MetadataResult).SourceEntities.begin(), (**MetadataResult).SourceEntities.end(),
			[&Target](const PrefabEntityMapping& Current) { return Current.SceneEntity == Target.GetUUID(); });
		if (Mapping == (**MetadataResult).SourceEntities.end()) return {}; // Local entity state is removed with its scene subtree.
		if (Target == LinkedRoot)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "A linked prefab root cannot be removed"));
		const auto Children = Target.GetChildren();
		if (!Children) return std::unexpected(ToReconcileError(Children.error()));
		if (!Children->empty())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
				"Remove source descendants from the leaves inward so local child state is preserved"));
		PrefabOverrideRecord Override;
		Override.Entity = { Mapping->SourceKey, false };
		Override.Operation = PrefabOverrideOperation::RemoveSourceEntity;
		return RecordOverride(LinkedRoot, Override);
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::DiscardOverride(
		const Entity& LinkedRoot,
		PrefabEntityAddress Address,
		std::optional<std::pair<std::string_view, std::string_view>> Property)
	{
		const auto Current = LinkedRoot.GetPrefabInstance();
		if (!LinkedRoot.IsValid() || !Current || !Current->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override target is not a valid linked prefab root"));
		PrefabInstanceComponent Metadata = Current->value();
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		std::erase_if(Metadata.Overrides, [&](const PrefabOverrideRecord& Override)
		{
			if (Override.Entity != Address) return false;
			return !Property ||
				((Override.Operation == PrefabOverrideOperation::SetProperty || Override.Operation == PrefabOverrideOperation::ResetProperty) &&
					Override.ComponentID == Property->first && Override.PropertyID == Property->second);
		});
		std::erase_if(Metadata.Conflicts, [&](const PrefabConflictRecord& ConflictRecord)
		{
			if (ConflictRecord.Entity != Address) return false;
			return !Property || (ConflictRecord.ComponentID == Property->first && ConflictRecord.PropertyID == Property->second);
		});
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Result = LinkedRoot.SetPrefabInstance(Metadata); !Result) return std::unexpected(ToReconcileError(Result.error()));
		return {};
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::RecordMissingSourceAssetConflict(
		const Entity& LinkedRoot,
		std::string Message)
	{
		const auto Current = LinkedRoot.GetPrefabInstance();
		if (!LinkedRoot.IsValid() || !Current || !Current->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance,
				"Missing-source conflict requires a valid linked prefab root"));
		PrefabInstanceComponent Metadata = Current->value();
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (Message.empty()) Message = "The prefab source asset is unavailable; the materialized instance was preserved";
		std::erase_if(Metadata.Conflicts, [](const PrefabConflictRecord& ConflictValue)
		{
			return ConflictValue.Kind == PrefabConflictKind::MissingSourceAsset;
		});
		Metadata.Conflicts.push_back(Conflict(
			{ Metadata.SourceRootKey, false }, PrefabConflictKind::MissingSourceAsset, std::move(Message)));
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Result = LinkedRoot.SetPrefabInstance(Metadata); !Result)
			return std::unexpected(ToReconcileError(Result.error()));
		return {};
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::RecordOverride(
		const Entity& LinkedRoot,
		const PrefabOverrideRecord& Override)
	{
		const auto Current = LinkedRoot.GetPrefabInstance();
		if (!LinkedRoot.IsValid() || !Current || !Current->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override target is not a valid linked prefab root"));
		PrefabInstanceComponent Metadata = Current->value();
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (!ResolveAddress(Metadata, Override.Entity))
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override address is not part of this linked instance"));
		if (Override.Operation == PrefabOverrideOperation::AddComponent ||
			Override.Operation == PrefabOverrideOperation::RemoveComponent)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
				"Component presence overrides are not supported by the current typed property adapter"));
		const bool HasRemovalOverride = std::any_of(Metadata.Overrides.begin(), Metadata.Overrides.end(),
			[&Override](const PrefabOverrideRecord& Existing)
			{
				return Existing.Entity == Override.Entity && Existing.Operation == PrefabOverrideOperation::RemoveSourceEntity;
			});
		const bool HasOtherOverride = std::any_of(Metadata.Overrides.begin(), Metadata.Overrides.end(),
			[&Override](const PrefabOverrideRecord& Existing)
			{
				return Existing.Entity == Override.Entity && Existing.Operation != PrefabOverrideOperation::RemoveSourceEntity;
			});
		if ((Override.Operation == PrefabOverrideOperation::RemoveSourceEntity && HasOtherOverride) ||
			(Override.Operation != PrefabOverrideOperation::RemoveSourceEntity && HasRemovalOverride))
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride,
				"An entity removal cannot coexist with property, hierarchy or component overrides on that source entity"));
		if (Override.Operation == PrefabOverrideOperation::SetProperty)
		{
			const auto ID = ResolveAddress(Metadata, Override.Entity);
			if (!ID)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override address is not part of this linked instance"));
			if (Override.ComponentID == PropertyComponentIds::Transform && !Override.Entity.IsLocal &&
				Override.Entity.Key == Metadata.SourceRootKey)
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "Root transform is destination placement"));
			const auto Subtree = GetSubtree(LinkedRoot);
			if (!Subtree) return std::unexpected(Subtree.error());
			const auto Target = std::find_if(Subtree->begin(), Subtree->end(), [ID](const Entity& CurrentEntity)
			{
				return CurrentEntity.GetUUID() == *ID;
			});
			if (Target == Subtree->end())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override entity is outside the linked instance subtree"));
			const auto CurrentValue = ReadProperty(*Target, Override.ComponentID, Override.PropertyID);
			if (!CurrentValue) return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, CurrentValue.error().Message));
			if (CurrentValue->index() != Override.Value.index())
				return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "Override value type does not match the current property type"));
		}
		const auto Matches = [&Override](const PrefabOverrideRecord& CurrentOverride)
		{
			return CurrentOverride.Entity == Override.Entity && CurrentOverride.ComponentID == Override.ComponentID &&
				CurrentOverride.PropertyID == Override.PropertyID &&
				(CurrentOverride.Operation == Override.Operation ||
					((Override.Operation == PrefabOverrideOperation::SetProperty || Override.Operation == PrefabOverrideOperation::ResetProperty) &&
						(CurrentOverride.Operation == PrefabOverrideOperation::SetProperty ||
							CurrentOverride.Operation == PrefabOverrideOperation::ResetProperty)));
		};
		std::erase_if(Metadata.Overrides, Matches);
		if (Override.Operation != PrefabOverrideOperation::ResetProperty) Metadata.Overrides.push_back(Override);
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Result = LinkedRoot.SetPrefabInstance(Metadata); !Result) return std::unexpected(ToReconcileError(Result.error()));
		return {};
	}

	std::expected<void, PrefabReconcileError> PrefabInstanceReconciler::ResetOverride(
		const Entity& LinkedRoot,
		const PrefabSourceDocument& Source,
		PrefabEntityAddress Address,
		std::string_view ComponentID,
		std::string_view PropertyID)
	{
		const auto Current = LinkedRoot.GetPrefabInstance();
		if (!LinkedRoot.IsValid() || !Current || !Current->has_value())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override target is not a valid linked prefab root"));
		PrefabInstanceComponent Metadata = Current->value();
		if (Metadata.SourceAsset != Source.GetSourceAsset() || Metadata.SourceRootKey != Source.GetRootKey())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::SourceMismatch, "Prefab source does not match the linked instance"));
		if (Address.IsLocal)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Local entity properties have no prefab source value to reset to"));
		const auto SceneEntityID = ResolveAddress(Metadata, Address);
		if (!SceneEntityID)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Override address is not part of this linked instance"));
		const auto SourceEntity = Source.GetScene().FindEntity(Address.Key);
		if (!SourceEntity) return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidSource, "Prefab source entity no longer exists"));
		if (Address.Key == Metadata.SourceRootKey && ComponentID == PropertyComponentIds::Transform)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "Root transform is destination placement"));
		auto SourceValue = ReadProperty(*SourceEntity, ComponentID, PropertyID);
		if (!SourceValue) return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, SourceValue.error().Message));
		const auto Subtree = GetSubtree(LinkedRoot);
		if (!Subtree) return std::unexpected(Subtree.error());
		const auto LiveIt = std::find_if(Subtree->begin(), Subtree->end(), [SceneEntityID](const Entity& CurrentEntity)
		{
			return CurrentEntity.GetUUID() == *SceneEntityID;
		});
		if (LiveIt == Subtree->end())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, "Mapped entity is outside its linked instance subtree"));
		auto PreviousValue = ReadProperty(*LiveIt, ComponentID, PropertyID);
		if (!PreviousValue) return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, PreviousValue.error().Message));
		if (PreviousValue->index() != SourceValue->index())
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, "Source and instance property types differ"));
		const auto RemoveMatching = [&](const PrefabOverrideRecord& Override)
		{
			return Override.Entity == Address && Override.ComponentID == ComponentID && Override.PropertyID == PropertyID &&
				(Override.Operation == PrefabOverrideOperation::SetProperty || Override.Operation == PrefabOverrideOperation::ResetProperty);
		};
		std::erase_if(Metadata.Overrides, RemoveMatching);
		std::erase_if(Metadata.Conflicts, [&](const PrefabConflictRecord& ConflictRecord)
		{
			return ConflictRecord.Entity == Address && ConflictRecord.ComponentID == ComponentID &&
				ConflictRecord.PropertyID == PropertyID;
		});
		if (auto Validation = Metadata.Validate(LinkedRoot.GetUUID()); !Validation)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::InvalidInstance, Validation.error()));
		if (auto Result = WriteProperty(*LiveIt, ComponentID, PropertyID, *SourceValue); !Result)
			return std::unexpected(MakeError(PrefabReconcileErrorCode::UnsupportedOverride, Result.error().Message));
		if (auto Result = LinkedRoot.SetPrefabInstance(Metadata); !Result)
		{
			(void)WriteProperty(*LiveIt, ComponentID, PropertyID, *PreviousValue);
			return std::unexpected(ToReconcileError(Result.error()));
		}
		return {};
	}
} // namespace PulseForge
