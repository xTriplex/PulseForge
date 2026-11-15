#include "Runtime/SceneRuntime.h"
#include "Runtime/CookedContentProvider.h"
#include "Animation/GltfAnimationPlayback.h"
#include "Assets/CookedProductCodec.h"
#include "Assets/GltfMeshImportCoordinator.h"

#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#if !defined(PF_COOKED_RUNTIME_ONLY)
#include "Assets/Project.h"
#endif
#include "Audio/AudioSceneRuntime.h"
#include "Audio/AudioSession.h"
#include "Core/Diagnostics.h"
#include "Physics/PhysicsSceneRuntime.h"
#include "Scene/Scene.h"
#include "Scripting/ScriptRuntime.h"
#include "UI/RuntimeUI.h"

#include <optional>
#include <algorithm>
#include <array>
#include <exception>
#include <atomic>
#include <deque>
#include <fstream>
#include <iterator>
#include <unordered_map>

namespace PulseForge
{
	namespace
	{
		SceneRuntimeError MakeError(
			SceneRuntimeErrorCode Code,
			SceneRuntimeSubsystem Subsystem,
			std::string Message)
		{
			return { Code, Subsystem, std::move(Message) };
		}

		std::string_view AudioDeviceStateName(AudioDeviceState State) noexcept
		{
			switch (State)
			{
				case AudioDeviceState::DefaultActive: return "default_active";
				case AudioDeviceState::ExplicitNull: return "explicit_null";
				case AudioDeviceState::Unavailable: return "unavailable";
				case AudioDeviceState::Recovering: return "recovering";
				case AudioDeviceState::RecoveryFailed: return "recovery_failed";
				case AudioDeviceState::OfflineTest: return "offline_test";
			}
			return "unknown";
		}

		uint64_t NextRuntimeSessionGeneration() noexcept
		{
			static std::atomic<uint64_t> Generation = 0;
			uint64_t Value = Generation.fetch_add(1, std::memory_order_relaxed) + 1;
			if (Value == 0) Value = Generation.fetch_add(1, std::memory_order_relaxed) + 1;
			return Value;
		}
		constexpr uint32_t MaximumBlueprintInstructionsPerFrame = 50'000;
		constexpr size_t MaximumQueuedEntityDestructionsPerFrame = 4096;
		constexpr size_t MaximumRuntimeSkeletalInstances = 256;
		constexpr size_t MaximumRuntimeSkinnedVerticesPerFrame = 2'000'000;
		constexpr size_t MaximumRuntimeSkeletalNodes = 4096;

		[[nodiscard]] std::expected<GltfAnimationNodePose, std::string> DecomposeSkeletalRestTransform(
			std::string NodeKey, const std::array<float, 16>& Matrix)
		{
			if (!std::all_of(Matrix.begin(), Matrix.end(), [](float Value) { return std::isfinite(Value); }) ||
				std::abs(Matrix[3]) > 1.0e-5f || std::abs(Matrix[7]) > 1.0e-5f ||
				std::abs(Matrix[11]) > 1.0e-5f || std::abs(Matrix[15] - 1.0f) > 1.0e-5f)
				return std::unexpected("Skeletal rest transform is not a finite affine matrix");

			double ScaleX = std::sqrt(static_cast<double>(Matrix[0]) * Matrix[0] +
				static_cast<double>(Matrix[1]) * Matrix[1] + static_cast<double>(Matrix[2]) * Matrix[2]);
			const double ScaleY = std::sqrt(static_cast<double>(Matrix[4]) * Matrix[4] +
				static_cast<double>(Matrix[5]) * Matrix[5] + static_cast<double>(Matrix[6]) * Matrix[6]);
			const double ScaleZ = std::sqrt(static_cast<double>(Matrix[8]) * Matrix[8] +
				static_cast<double>(Matrix[9]) * Matrix[9] + static_cast<double>(Matrix[10]) * Matrix[10]);
			const double Determinant = static_cast<double>(Matrix[0]) * (Matrix[5] * Matrix[10] - Matrix[9] * Matrix[6]) -
				static_cast<double>(Matrix[4]) * (Matrix[1] * Matrix[10] - Matrix[9] * Matrix[2]) +
				static_cast<double>(Matrix[8]) * (Matrix[1] * Matrix[6] - Matrix[5] * Matrix[2]);
			if (!std::isfinite(Determinant) || std::abs(Determinant) <= 1.0e-12 ||
				ScaleY <= 1.0e-8 || ScaleZ <= 1.0e-8 || ScaleX <= 1.0e-8)
				return std::unexpected("Skeletal rest transform has a singular scale or basis");
			if (Determinant < 0.0) ScaleX = -ScaleX;
			const double R00 = Matrix[0] / ScaleX, R10 = Matrix[1] / ScaleX, R20 = Matrix[2] / ScaleX;
			const double R01 = Matrix[4] / ScaleY, R11 = Matrix[5] / ScaleY, R21 = Matrix[6] / ScaleY;
			const double R02 = Matrix[8] / ScaleZ, R12 = Matrix[9] / ScaleZ, R22 = Matrix[10] / ScaleZ;
			const auto DotColumns = [](double AX, double AY, double AZ, double BX, double BY, double BZ)
				{ return AX * BX + AY * BY + AZ * BZ; };
			if (std::abs(DotColumns(R00, R10, R20, R01, R11, R21)) > 1.0e-4 ||
				std::abs(DotColumns(R00, R10, R20, R02, R12, R22)) > 1.0e-4 ||
				std::abs(DotColumns(R01, R11, R21, R02, R12, R22)) > 1.0e-4)
				return std::unexpected("Animated skeletal node has shear that cannot be represented by glTF TRS channels");

			GltfAnimationNodePose Pose;
			Pose.NodeKey = std::move(NodeKey);
			Pose.Translation = { Matrix[12], Matrix[13], Matrix[14], 0.0f };
			Pose.Scale = { static_cast<float>(ScaleX), static_cast<float>(ScaleY), static_cast<float>(ScaleZ), 0.0f };
			const double Trace = R00 + R11 + R22;
			if (Trace > 0.0)
			{
				const double S = std::sqrt(Trace + 1.0) * 2.0;
				Pose.Rotation = { static_cast<float>((R21 - R12) / S), static_cast<float>((R02 - R20) / S),
					static_cast<float>((R10 - R01) / S), static_cast<float>(0.25 * S) };
			}
			else if (R00 > R11 && R00 > R22)
			{
				const double S = std::sqrt(1.0 + R00 - R11 - R22) * 2.0;
				Pose.Rotation = { static_cast<float>(0.25 * S), static_cast<float>((R01 + R10) / S),
					static_cast<float>((R02 + R20) / S), static_cast<float>((R21 - R12) / S) };
			}
			else if (R11 > R22)
			{
				const double S = std::sqrt(1.0 + R11 - R00 - R22) * 2.0;
				Pose.Rotation = { static_cast<float>((R01 + R10) / S), static_cast<float>(0.25 * S),
					static_cast<float>((R12 + R21) / S), static_cast<float>((R02 - R20) / S) };
			}
			else
			{
				const double S = std::sqrt(1.0 + R22 - R00 - R11) * 2.0;
				Pose.Rotation = { static_cast<float>((R02 + R20) / S), static_cast<float>((R12 + R21) / S),
					static_cast<float>(0.25 * S), static_cast<float>((R10 - R01) / S) };
			}
			return Pose;
		}

		[[nodiscard]] std::array<float, 16> ComposeSkeletalTransform(const GltfAnimationNodePose& Pose)
		{
			const double X = Pose.Rotation[0], Y = Pose.Rotation[1], Z = Pose.Rotation[2], W = Pose.Rotation[3];
			const double XX = X * X, YY = Y * Y, ZZ = Z * Z;
			const double XY = X * Y, XZ = X * Z, YZ = Y * Z;
			const double XW = X * W, YW = Y * W, ZW = Z * W;
			const double SX = Pose.Scale[0], SY = Pose.Scale[1], SZ = Pose.Scale[2];
			return {
				static_cast<float>((1.0 - 2.0 * (YY + ZZ)) * SX), static_cast<float>((2.0 * (XY + ZW)) * SX),
				static_cast<float>((2.0 * (XZ - YW)) * SX), 0.0f,
				static_cast<float>((2.0 * (XY - ZW)) * SY), static_cast<float>((1.0 - 2.0 * (XX + ZZ)) * SY),
				static_cast<float>((2.0 * (YZ + XW)) * SY), 0.0f,
				static_cast<float>((2.0 * (XZ + YW)) * SZ), static_cast<float>((2.0 * (YZ - XW)) * SZ),
				static_cast<float>((1.0 - 2.0 * (XX + YY)) * SZ), 0.0f,
				Pose.Translation[0], Pose.Translation[1], Pose.Translation[2], 1.0f
			};
		}

		std::expected<std::string, std::string> ReadRuntimeUIDocumentBytes(
			const Project* SourceProject, const std::shared_ptr<const CookedContentProvider>& CookedAssets, const AssetID& Asset)
		{
			if (CookedAssets)
			{
				auto Bytes = CookedAssets->ReadRuntimeUI(Asset);
				if (!Bytes) return std::unexpected(Bytes.error().Message);
				return std::string(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
			}
			if (!SourceProject) return std::unexpected(std::string("Runtime UI requires a source project or cooked content provider"));
#if !defined(PF_COOKED_RUNTIME_ONLY)
			const auto Record = SourceProject->GetAssetRegistry().Find(Asset);
			if (!Record || !HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::RuntimeUI))
				return std::unexpected(std::string("Runtime UI document is missing from the managed project asset registry"));
			auto Path = AssetPathResolver::ResolveManagedSourcePath(SourceProject->GetRootPath(), Record->ProjectRelativePath);
			if (!Path) return std::unexpected(Path.error().Message);
			std::error_code Error;
			const uintmax_t Size = std::filesystem::file_size(*Path, Error);
			if (Error || Size == 0 || Size > 2u * 1024u * 1024u)
				return std::unexpected(std::string("Runtime UI document could not be read within the 2 MiB limit"));
			std::ifstream Input(*Path, std::ios::binary);
			if (!Input.is_open()) return std::unexpected(std::string("Runtime UI document could not be opened"));
			std::string Text(static_cast<size_t>(Size), '\0');
			Input.read(Text.data(), static_cast<std::streamsize>(Text.size()));
			if (!Input || Input.bad() || static_cast<size_t>(Input.gcount()) != Text.size())
				return std::unexpected(std::string("Runtime UI document could not be read completely"));
			return Text;
#else
			return std::unexpected(std::string("Cooked runtime cannot resolve source project UI documents"));
#endif
		}

		[[nodiscard]] std::expected<ImportedGltfSkeletalMesh, std::string> LoadRuntimeSkeletalProduct(
			const Project* SourceProject,
			const std::shared_ptr<const CookedContentProvider>& CookedAssets,
			const AssetID& Asset,
			std::string_view ProductKey)
		{
			if (CookedAssets)
			{
				auto Product = CookedAssets->LoadSkeletalMeshByProductKey(Asset, ProductKey);
				if (!Product) return std::unexpected("Cooked skeletal product load failed: " + Product.error().Message);
				return std::move(*Product);
			}
#if !defined(PF_COOKED_RUNTIME_ONLY)
			if (!SourceProject)
				return std::unexpected("Skeletal playback requires a source project or cooked content provider");
			const auto Record = SourceProject->GetAssetRegistry().Find(Asset);
			if (!Record)
				return std::unexpected("Skeletal source asset is not present in the active project registry");
			auto Imported = GltfMeshImportCoordinator::LoadLastSuccessfulSkeletalProduct(
				Asset, SourceProject->GetRootPath(), SourceProject->GetAssetRegistry(), ProductKey);
			if (!Imported) return std::unexpected("Skeletal source import failed: " + Imported.error().Message);
			return std::move(Imported->Mesh);
#else
			(void)SourceProject;
			(void)Asset;
			(void)ProductKey;
			return std::unexpected("Cooked runtime cannot resolve a skeletal product without its cooked provider");
#endif
		}
	}

	struct SceneRuntime::Impl final
	{
		struct BoundBlueprintEvent final
		{
			Entity BoundEntity;
			bool Resolved = false;
			AssetID Asset;
			uint64_t Revision = 0;
			std::shared_ptr<const VerifiedBlueprintProgram> Program;
			bool Faulted = false;
		};
		struct BlueprintInstanceState final
		{
			Entity BoundEntity;
			AssetID Asset;
			uint64_t Revision = 0;
			std::vector<std::string> PropertyIds;
			std::vector<float> ScalarState;
		};
		struct BlueprintInitialScalarDefaults final
		{
			Entity BoundEntity;
			std::unordered_map<std::string, float> Values;
		};
		struct PendingBlueprintPhysicsForce final
		{
			Entity Target;
			AssetID Asset;
			BlueprintEventKind Event = BlueprintEventKind::OnCreate;
			BlueprintPhysicsForceRequest Request;
			uint64_t SessionGeneration = 0;
			uint64_t ProgramGeneration = 0;
		};
		struct PendingBlueprintTimer final
		{
			Entity Target;
			AssetID Asset;
			std::string Name;
			double DueTimeSeconds = 0.0;
			UUID SourceNode;
			uint64_t SessionGeneration = 0;
			uint64_t ProgramGeneration = 0;
		};
		struct PendingBlueprintEntitySpawn final
		{
			Entity Target;
			AssetID Asset;
			BlueprintEventKind Event = BlueprintEventKind::OnCreate;
			BlueprintEntitySpawnRequest Request;
			uint64_t SessionGeneration = 0;
			uint64_t ProgramGeneration = 0;
		};
		static constexpr size_t BlueprintEventCount = static_cast<size_t>(BlueprintEventKind::OnTimer) + 1;
		using BlueprintEventStates = std::array<BoundBlueprintEvent, BlueprintEventCount>;

		struct RuntimeUICanvasState final
		{
			Entity Owner;
			AssetID Asset;
			RuntimeUIInputPolicy InputPolicy = RuntimeUIInputPolicy::Passive;
			RuntimeUIDocument Document;
			RuntimeUIController Controller;
			std::vector<RuntimeUILayoutEntry> Layout;
			std::vector<RuntimeUIDrawCommand> DrawCommands;
			uint64_t TreeGeneration = 0;
		};
		struct SkeletalInstanceState final
		{
			Entity BoundEntity;
			AssetID SourceAsset;
			std::string ProductKey;
			std::string ClipKey;
			ImportedGltfSkeletalMesh Product;
			std::vector<GltfAnimationNodePose> RestPose;
			GltfAnimationInstancePlayback Playback;
		};
		// Reverse destruction stops scripts and scene voices before the session-owned engine and cache.
		std::unique_ptr<AudioSession> OwnedAudioSession;
		AudioSession* AudioSessionState = nullptr;
		std::unique_ptr<AudioSceneRuntime> Audio;
		std::unique_ptr<PhysicsSceneRuntime> Physics;
		std::unique_ptr<ScriptRuntime> Scripting;
		SceneRuntimeServices Services;
		std::optional<RuntimeUICanvasState> RuntimeUI;
		std::unordered_map<UUID, SkeletalInstanceState, UUIDHash> SkeletalInstances;
		std::unordered_map<UUID, BlueprintEventStates, UUIDHash> BlueprintEvents;
		std::unordered_map<UUID, std::unordered_map<AssetID, BlueprintInstanceState, UUIDHash>, UUIDHash> BlueprintInstances;
		std::unordered_map<UUID, std::unordered_map<AssetID, BlueprintInitialScalarDefaults, UUIDHash>, UUIDHash>
			BlueprintInitialDefaults;
		std::unordered_map<UUID, Entity, UUIDHash> BlueprintKnownEntities;
		std::deque<Entity> PendingEntityDestructions;
		std::deque<PendingBlueprintPhysicsForce> PendingBlueprintPhysicsForces;
		std::vector<PendingBlueprintTimer> PendingBlueprintTimers;
		std::deque<PendingBlueprintEntitySpawn> PendingBlueprintEntitySpawns;
		std::deque<PhysicsContactEvent> PendingContactEvents;
		std::vector<BlueprintRuntimeDiagnostic> BlueprintDiagnostics;
		std::vector<BlueprintRuntimeTrace> BlueprintTraces;
		RuntimeUIViewport RuntimeUIView{};
		std::vector<RuntimeUIInputEvent> RuntimeUIEvents;
		std::vector<RuntimeUIDrawCommand> RuntimeUIDrawCommands;
		Scene* Source = nullptr;
		bool Paused = false;
		bool AudioActivationDeferred = false;
		enum class Lifecycle : uint8_t { Starting, Active, Stopping };
		enum class RequestStatus : uint8_t { None, Pending, Committed, Rejected, Cancelled, Superseded };
		Lifecycle State = Lifecycle::Starting;
		RequestStatus BoundaryRequestStatus = RequestStatus::None;
		std::optional<SceneRuntimeBoundaryRequest> BoundaryRequest;
		std::optional<bool> RequestedPause;
		uint64_t SessionGeneration = 0;
		uint64_t BlueprintProgramGeneration = 0;
		double BlueprintClockSeconds = 0.0;
		uint64_t NextBoundaryRequestGeneration = 0;
		uint32_t BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
		uint32_t BlueprintInstructionsExecuted = 0;
		uint32_t BlueprintDispatchesRemaining = BlueprintMaximumEventDispatchesPerFrame;
		bool BlueprintDispatchLimitReported = false;

		[[nodiscard]] std::expected<void, std::string> AdvanceSkeletalAnimations(
			Scene& SceneValue,
			float DeltaSeconds,
			const Project* SourceProject,
			const std::shared_ptr<const CookedContentProvider>& CookedAssets)
		{
			std::erase_if(SkeletalInstances, [&](const auto& Entry)
			{
				const auto Current = SceneValue.FindEntity(Entry.first);
				if (Current && *Current == Entry.second.BoundEntity) return false;
				SceneValue.ClearRuntimeSkeletalPose(Entry.first);
				return true;
			});
			std::vector<UUID> LiveSkeletalEntities;
			LiveSkeletalEntities.reserve(std::min(SceneValue.GetEntityCount(), MaximumRuntimeSkeletalInstances));
			size_t SkinnedVerticesThisFrame = 0;
			for (const Entity& EntityValue : SceneValue.GetEntities())
			{
				auto MeshRenderer = EntityValue.GetMeshRenderer();
				if (!MeshRenderer) return std::unexpected("Could not inspect skeletal mesh component: " + MeshRenderer.error().Message);
				if (!MeshRenderer->has_value() || !(**MeshRenderer).SkeletalProductKey)
				{
					SkeletalInstances.erase(EntityValue.GetUUID());
					SceneValue.ClearRuntimeSkeletalPose(EntityValue.GetUUID());
					continue;
				}

				const MeshRendererComponent& Component = **MeshRenderer;
				if (SkeletalInstances.size() >= MaximumRuntimeSkeletalInstances &&
					!SkeletalInstances.contains(EntityValue.GetUUID()))
					return std::unexpected("Scene exceeds the 256-instance skeletal animation runtime limit");
				auto Found = SkeletalInstances.find(EntityValue.GetUUID());
				if (Found != SkeletalInstances.end() &&
					(Found->second.BoundEntity != EntityValue || Found->second.SourceAsset != Component.MeshAsset ||
					Found->second.ProductKey != *Component.SkeletalProductKey))
				{
					SkeletalInstances.erase(Found);
					Found = SkeletalInstances.end();
				}
				if (Found == SkeletalInstances.end())
				{
					auto Product = LoadRuntimeSkeletalProduct(
						SourceProject, CookedAssets, Component.MeshAsset, *Component.SkeletalProductKey);
					if (!Product) return std::unexpected(Product.error());
					if (Product->SkeletonNodes.empty() || Product->SkeletonNodes.size() > MaximumRuntimeSkeletalNodes)
						return std::unexpected("Skeletal product node count is empty or exceeds the runtime limit");
					SkeletalInstanceState State;
					State.BoundEntity = EntityValue;
					State.SourceAsset = Component.MeshAsset;
					State.ProductKey = *Component.SkeletalProductKey;
					State.Product = std::move(*Product);
					State.RestPose.reserve(State.Product.SkeletonNodes.size());
					for (const ImportedGltfSkeletonNode& Node : State.Product.SkeletonNodes)
					{
						auto Pose = DecomposeSkeletalRestTransform(Node.StableKey, Node.LocalBindMatrix);
						if (!Pose) return std::unexpected("Invalid skeletal rest pose: " + Pose.error());
						State.RestPose.push_back(std::move(*Pose));
					}
					Found = SkeletalInstances.emplace(EntityValue.GetUUID(), std::move(State)).first;
				}

				SkeletalInstanceState& State = Found->second;
				const size_t VertexCount = State.Product.Geometry.Vertices.size();
				if (VertexCount > MaximumRuntimeSkinnedVerticesPerFrame - SkinnedVerticesThisFrame)
					return std::unexpected("Scene exceeds the 2,000,000 skinned-vertex per-frame CPU budget");
				SkinnedVerticesThisFrame += VertexCount;
				std::vector<GltfAnimationNodePose> Pose;
				if (Component.AnimationClipKey)
				{
					if (State.ClipKey != *Component.AnimationClipKey)
					{
					GltfAnimationInstancePlayback CandidatePlayback;
					if (auto Started = PlayGltfAnimationInstance(CandidatePlayback, State.Product,
							*Component.AnimationClipKey, true); !Started)
						return std::unexpected("Could not start skeletal animation clip: " + Started.error().Message);
					State.Playback = std::move(CandidatePlayback);
					State.ClipKey = *Component.AnimationClipKey;
				}
				auto Advanced = AdvanceGltfAnimationInstance(
					State.Playback, State.Product, State.RestPose, DeltaSeconds);
				if (!Advanced) return std::unexpected("Could not advance skeletal animation: " + Advanced.error().Message);
				Pose = std::move(*Advanced);
				}
				else
				{
					State.ClipKey.clear();
					State.Playback = {};
					Pose = State.RestPose;
				}

				std::vector<std::array<float, 16>> LocalTransforms;
				LocalTransforms.reserve(Pose.size());
				for (const GltfAnimationNodePose& NodePose : Pose)
					LocalTransforms.push_back(ComposeSkeletalTransform(NodePose));
				if (auto Published = SceneValue.SetRuntimeSkeletalPose(EntityValue.GetUUID(), std::move(LocalTransforms)); !Published)
					return std::unexpected("Could not publish runtime skeletal pose: " + Published.error().Message);
				LiveSkeletalEntities.push_back(EntityValue.GetUUID());
			}

			std::erase_if(SkeletalInstances, [&](const auto& Entry)
			{
				if (std::find(LiveSkeletalEntities.begin(), LiveSkeletalEntities.end(), Entry.first) != LiveSkeletalEntities.end())
					return false;
				SceneValue.ClearRuntimeSkeletalPose(Entry.first);
				return true;
			});
			return {};
		}

		void RecordBlueprintDiagnostic(
			const Entity& Target,
			const BlueprintProgramBinding* Binding,
			BlueprintEventKind Event,
			BlueprintProgramErrorCode Code,
			UUID SourceNode,
			std::string Message)
		{
			if (BlueprintDiagnostics.size() == 256) BlueprintDiagnostics.erase(BlueprintDiagnostics.begin());
			BlueprintDiagnostics.push_back({ Target.GetUUID(), Binding ? Binding->Asset : AssetID{}, Event, Code,
				SourceNode, std::move(Message) });
		}

		bool QueueEntityDestruction(const Entity& Target)
		{
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid()) return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			if (!Current || *Current != Target) return false;
			if (std::find(PendingEntityDestructions.begin(), PendingEntityDestructions.end(), Target) !=
				PendingEntityDestructions.end()) return true;
			if (PendingEntityDestructions.size() >= MaximumQueuedEntityDestructionsPerFrame) return false;
			PendingEntityDestructions.push_back(Target);
			return true;
		}

		bool QueueBlueprintPhysicsForces(
			const Entity& Target,
			const BlueprintProgramBinding& Binding,
			BlueprintEventKind Event,
			std::span<const BlueprintPhysicsForceRequest> Requests)
		{
			if (Requests.empty()) return true;
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid() ||
				Requests.size() > BlueprintMaximumDeferredPhysicsCallsPerEvent - PendingBlueprintPhysicsForces.size())
				return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			if (!Current || *Current != Target) return false;
			for (const BlueprintPhysicsForceRequest& Request : Requests)
				PendingBlueprintPhysicsForces.push_back({ Target, Binding.Asset, Event, Request, SessionGeneration,
					BlueprintProgramGeneration });
			return true;
		}

		bool CanQueueBlueprintPhysicsForces(
			const Entity& Target,
			std::span<const BlueprintPhysicsForceRequest> Requests) const
		{
			if (Requests.empty()) return true;
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid() ||
				Requests.size() > BlueprintMaximumDeferredPhysicsCallsPerEvent - PendingBlueprintPhysicsForces.size())
				return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			return Current && *Current == Target;
		}

		bool QueueBlueprintTimers(
			const Entity& Target,
			const BlueprintProgramBinding& Binding,
			std::span<const BlueprintTimerRequest> Requests)
		{
			if (Requests.empty()) return true;
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid() ||
				Requests.size() > BlueprintMaximumTimersPerEvent)
				return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			if (!Current || *Current != Target) return false;
			size_t AdditionalTimers = 0;
			for (size_t RequestIndex = 0; RequestIndex < Requests.size(); ++RequestIndex)
			{
				const BlueprintTimerRequest& Request = Requests[RequestIndex];
				const auto Existing = std::find_if(PendingBlueprintTimers.begin(), PendingBlueprintTimers.end(),
					[&](const PendingBlueprintTimer& Timer)
					{
						return Timer.Target == Target && Timer.Asset == Binding.Asset && Timer.Name == Request.Name;
					});
				const bool AlreadyCounted = std::any_of(Requests.begin(), Requests.begin() + RequestIndex,
					[&](const BlueprintTimerRequest& Earlier) { return Earlier.Name == Request.Name; });
				if (Existing == PendingBlueprintTimers.end() && !AlreadyCounted) ++AdditionalTimers;
			}
			if (AdditionalTimers > BlueprintMaximumScheduledTimers - PendingBlueprintTimers.size()) return false;
			for (const BlueprintTimerRequest& Request : Requests)
			{
				const double DueTime = BlueprintClockSeconds + Request.DelaySeconds;
				const auto Existing = std::find_if(PendingBlueprintTimers.begin(), PendingBlueprintTimers.end(),
					[&](const PendingBlueprintTimer& Timer)
					{
						return Timer.Target == Target && Timer.Asset == Binding.Asset && Timer.Name == Request.Name;
					});
				if (Existing != PendingBlueprintTimers.end())
					*Existing = { Target, Binding.Asset, Request.Name, DueTime, Request.SourceNode, SessionGeneration,
						BlueprintProgramGeneration };
				else
					PendingBlueprintTimers.push_back({ Target, Binding.Asset, Request.Name, DueTime, Request.SourceNode,
						SessionGeneration, BlueprintProgramGeneration });
			}
			return true;
		}

		bool CanQueueBlueprintTimers(
			const Entity& Target,
			const BlueprintProgramBinding& Binding,
			std::span<const BlueprintTimerRequest> Requests) const
		{
			if (Requests.empty()) return true;
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid() ||
				Requests.size() > BlueprintMaximumTimersPerEvent)
				return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			if (!Current || *Current != Target) return false;
			size_t AdditionalTimers = 0;
			for (size_t RequestIndex = 0; RequestIndex < Requests.size(); ++RequestIndex)
			{
				const BlueprintTimerRequest& Request = Requests[RequestIndex];
				const auto Existing = std::find_if(PendingBlueprintTimers.begin(), PendingBlueprintTimers.end(),
					[&](const PendingBlueprintTimer& Timer)
					{
						return Timer.Target == Target && Timer.Asset == Binding.Asset && Timer.Name == Request.Name;
					});
				const bool AlreadyCounted = std::any_of(Requests.begin(), Requests.begin() + RequestIndex,
					[&](const BlueprintTimerRequest& Earlier) { return Earlier.Name == Request.Name; });
				if (Existing == PendingBlueprintTimers.end() && !AlreadyCounted) ++AdditionalTimers;
			}
			return AdditionalTimers <= BlueprintMaximumScheduledTimers - PendingBlueprintTimers.size();
		}

		bool QueueBlueprintEntitySpawns(
			const Entity& Target,
			const BlueprintProgramBinding& Binding,
			BlueprintEventKind Event,
			std::span<const BlueprintEntitySpawnRequest> Requests)
		{
			constexpr size_t MaximumPendingSpawns = 256;
			if (Requests.empty()) return true;
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid() ||
				Requests.size() > BlueprintMaximumEntitySpawnsPerEvent ||
				Requests.size() > MaximumPendingSpawns - PendingBlueprintEntitySpawns.size())
				return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			if (!Current || *Current != Target) return false;
			for (const BlueprintEntitySpawnRequest& Request : Requests)
			{
				if (!std::isfinite(Request.X) || !std::isfinite(Request.Y) || !std::isfinite(Request.Z)) return false;
				PendingBlueprintEntitySpawns.push_back({ Target, Binding.Asset, Event, Request, SessionGeneration,
					BlueprintProgramGeneration });
			}
			return true;
		}

		bool CanQueueBlueprintEntitySpawns(
			const Entity& Target,
			std::span<const BlueprintEntitySpawnRequest> Requests) const
		{
			constexpr size_t MaximumPendingSpawns = 256;
			if (Requests.empty()) return true;
			if ((State != Lifecycle::Starting && State != Lifecycle::Active) || !Source || !Target.IsValid() ||
				Requests.size() > BlueprintMaximumEntitySpawnsPerEvent ||
				Requests.size() > MaximumPendingSpawns - PendingBlueprintEntitySpawns.size())
				return false;
			const auto Current = Source->FindEntity(Target.GetUUID());
			return Current && *Current == Target && std::all_of(Requests.begin(), Requests.end(), [](const auto& Request)
			{
				return std::isfinite(Request.X) && std::isfinite(Request.Y) && std::isfinite(Request.Z);
			});
		}

		void ApplyQueuedBlueprintEntitySpawns(Scene& SceneValue)
		{
			while (!PendingBlueprintEntitySpawns.empty())
			{
				const PendingBlueprintEntitySpawn Pending = PendingBlueprintEntitySpawns.front();
				PendingBlueprintEntitySpawns.pop_front();
				const auto Current = SceneValue.FindEntity(Pending.Target.GetUUID());
				if (Pending.SessionGeneration != SessionGeneration || Pending.ProgramGeneration != BlueprintProgramGeneration ||
					!Current || *Current != Pending.Target)
					continue;
				auto Spawned = SceneValue.CreateEntity("Blueprint Spawned Entity");
				TransformComponent Transform;
				Transform.Translation = { Pending.Request.X, Pending.Request.Y, Pending.Request.Z };
				if (!Spawned || !Spawned->SetTransform(Transform))
				{
					if (Spawned) (void)SceneValue.DestroyEntity(*Spawned);
					const BlueprintProgramBinding Binding{ Pending.Asset, 0, {} };
					RecordBlueprintDiagnostic(Pending.Target, &Binding, Pending.Event,
						BlueprintProgramErrorCode::HostRejectedDeferredCall, Pending.Request.SourceNode,
						"Scene runtime could not apply a deferred Blueprint entity spawn");
				}
			}
		}

		void DispatchDueBlueprintTimers(Scene& SceneValue)
		{
			const auto DueEnd = std::stable_partition(PendingBlueprintTimers.begin(), PendingBlueprintTimers.end(),
				[&](const PendingBlueprintTimer& Timer) { return Timer.DueTimeSeconds <= BlueprintClockSeconds; });
			std::vector<PendingBlueprintTimer> DueTimers(
				std::make_move_iterator(PendingBlueprintTimers.begin()), std::make_move_iterator(DueEnd));
			PendingBlueprintTimers.erase(PendingBlueprintTimers.begin(), DueEnd);
			std::stable_sort(DueTimers.begin(), DueTimers.end(), [](const auto& Left, const auto& Right)
			{
				return Left.DueTimeSeconds < Right.DueTimeSeconds;
			});
			for (size_t TimerIndex = 0; TimerIndex < DueTimers.size(); ++TimerIndex)
			{
				if (BlueprintDispatchesRemaining == 0)
				{
					PendingBlueprintTimers.insert(PendingBlueprintTimers.end(),
						std::make_move_iterator(DueTimers.begin() + TimerIndex), std::make_move_iterator(DueTimers.end()));
					if (!BlueprintDispatchLimitReported)
					{
						BlueprintDispatchLimitReported = true;
						RecordBlueprintDiagnostic({}, nullptr, BlueprintEventKind::OnTimer,
							BlueprintProgramErrorCode::EventBudgetExceeded, {},
							"Scene Blueprint event dispatch budget was exhausted before all due timers could run");
					}
					break;
				}
				const PendingBlueprintTimer& Timer = DueTimers[TimerIndex];
				if (Timer.SessionGeneration != SessionGeneration || Timer.ProgramGeneration != BlueprintProgramGeneration)
					continue;
				const auto Current = SceneValue.FindEntity(Timer.Target.GetUUID());
				if (!Current || *Current != Timer.Target) continue;
				const auto Instance = BlueprintInstances.find(Timer.Target.GetUUID());
				if (Instance == BlueprintInstances.end() || !Instance->second.contains(Timer.Asset)) continue;
				DispatchBlueprintEvent(SceneValue, Timer.Target, BlueprintEventKind::OnTimer, Services, Timer.Name);
				ApplyQueuedEntityDestructions(SceneValue);
				if (BoundaryRequestStatus == RequestStatus::Pending || BoundaryRequestStatus == RequestStatus::Committed)
					break;
			}
		}

		void ApplyQueuedBlueprintPhysicsForces(Scene& SceneValue)
		{
			while (!PendingBlueprintPhysicsForces.empty())
			{
				const PendingBlueprintPhysicsForce Pending = PendingBlueprintPhysicsForces.front();
				PendingBlueprintPhysicsForces.pop_front();
				const auto Current = SceneValue.FindEntity(Pending.Target.GetUUID());
				if (Pending.SessionGeneration != SessionGeneration || Pending.ProgramGeneration != BlueprintProgramGeneration ||
					!Current || *Current != Pending.Target)
					continue;
				const auto Applied = Physics->ApplyForce(Pending.Target.GetUUID(),
					glm::vec3(Pending.Request.X, Pending.Request.Y, Pending.Request.Z));
				if (!Applied)
				{
					const BlueprintProgramBinding Binding{ Pending.Asset, 0, {} };
					RecordBlueprintDiagnostic(Pending.Target, &Binding, Pending.Event,
						BlueprintProgramErrorCode::HostRejectedDeferredCall, Pending.Request.SourceNode,
						"Scene runtime rejected the Blueprint physics force request: " + Applied.error().Message);
				}
			}
		}

		void ApplyQueuedEntityDestructions(Scene& SceneValue)
		{
			while (!PendingEntityDestructions.empty())
			{
				const Entity Target = PendingEntityDestructions.front();
				PendingEntityDestructions.pop_front();
				const auto Current = SceneValue.FindEntity(Target.GetUUID());
				if (!Current || *Current != Target) continue;
				DispatchBlueprintEvent(SceneValue, Target, BlueprintEventKind::OnDestroy, Services);
				const auto Destroyed = SceneValue.DestroyEntity(Target);
				if (!Destroyed)
				{
					RecordBlueprintDiagnostic(Target, nullptr, BlueprintEventKind::OnDestroy,
						BlueprintProgramErrorCode::HostFailure, {}, "Could not apply the queued entity destruction: " +
							Destroyed.error().Message);
					continue;
				}
				BlueprintEvents.erase(Target.GetUUID());
				BlueprintInstances.erase(Target.GetUUID());
				BlueprintInitialDefaults.erase(Target.GetUUID());
				BlueprintKnownEntities.erase(Target.GetUUID());
				std::erase_if(PendingBlueprintTimers,
					[&](const PendingBlueprintTimer& Timer) { return Timer.Target == Target; });
			}
		}

		void DispatchBlueprintEvent(
			Scene& SceneValue,
			const Entity& Target,
			BlueprintEventKind Event,
			const SceneRuntimeServices& Services,
			std::string_view UIActionName = {})
		{
			if (!Services.ResolveBlueprintProgram || !Target.IsValid()) return;
			try
			{
				const auto Current = SceneValue.FindEntity(Target.GetUUID());
				if (!Current || *Current != Target) return;
				const size_t EventIndex = static_cast<size_t>(Event);
				if (EventIndex >= BlueprintEventCount)
				{
					RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::InvalidOperand, {},
						"Blueprint event binding uses an unknown event");
					return;
				}
				if (BlueprintDispatchesRemaining == 0)
				{
					if (!BlueprintDispatchLimitReported)
					{
						BlueprintDispatchLimitReported = true;
						RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::EventBudgetExceeded, {},
							"Scene Blueprint event dispatch budget was exhausted for this frame");
					}
					return;
				}
				--BlueprintDispatchesRemaining;

				BoundBlueprintEvent& Instance = BlueprintEvents[Target.GetUUID()][EventIndex];
				if (Instance.BoundEntity != Target)
				{
					Instance = {};
					Instance.BoundEntity = Target;
				}
				if (!Instance.Resolved)
				{
					const auto Binding = Services.ResolveBlueprintProgram(Target, Event);
					Instance.Resolved = true;
					if (!Binding || !Binding->Program) return;
					if (Binding->Asset.IsNil())
					{
						RecordBlueprintDiagnostic(Target, &*Binding, Event, BlueprintProgramErrorCode::InvalidOperand, {},
							"Blueprint event binding is missing a stable asset identity");
						Instance.Faulted = true;
						return;
					}
					Instance.Asset = Binding->Asset;
					Instance.Revision = Binding->Revision;
					Instance.Program = Binding->Program;
				}
				if (!Instance.Program) return;
				const BlueprintProgramBinding Binding{ Instance.Asset, Instance.Revision, Instance.Program };
				if (Instance.Faulted) return;
				if (BlueprintInstructionsRemaining == 0)
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::InstructionBudgetExceeded, {},
						"Scene Blueprint instruction budget was exhausted for this frame");
					Instance.Faulted = true;
					return;
				}
				BlueprintInstanceState& ProgramInstance = BlueprintInstances[Target.GetUUID()][Instance.Asset];
				const BlueprintProgramSource& ProgramSource = Instance.Program->GetSource();
				if (ProgramInstance.BoundEntity != Target || ProgramInstance.Asset != Instance.Asset ||
					ProgramInstance.Revision != Instance.Revision)
				{
					std::erase_if(BlueprintTraces, [&](const BlueprintRuntimeTrace& Trace)
						{ return Trace.Handle.Target == Target && Trace.Handle.Asset == Instance.Asset; });
					ProgramInstance = {};
					ProgramInstance.BoundEntity = Target;
					ProgramInstance.Asset = Instance.Asset;
					ProgramInstance.Revision = Instance.Revision;
					ProgramInstance.PropertyIds = ProgramSource.ScalarPropertyIds;
					ProgramInstance.ScalarState.resize(ProgramSource.ScalarPropertyIds.size(), 0.0f);
					BlueprintInitialScalarDefaults& InitialDefaults = BlueprintInitialDefaults[Target.GetUUID()][Instance.Asset];
					if (InitialDefaults.BoundEntity != Target)
					{
						InitialDefaults = {};
						InitialDefaults.BoundEntity = Target;
					}
					for (size_t PropertyIndex = 0; PropertyIndex < ProgramInstance.PropertyIds.size(); ++PropertyIndex)
					{
						const std::string& PropertyId = ProgramInstance.PropertyIds[PropertyIndex];
						auto [Default, Inserted] = InitialDefaults.Values.try_emplace(PropertyId, 0.0f);
						if (Inserted && Services.ReadBlueprintScalarProperty)
							if (const auto InitialValue = Services.ReadBlueprintScalarProperty(Target, PropertyId))
								Default->second = *InitialValue;
						ProgramInstance.ScalarState[PropertyIndex] = Default->second;
					}
				}
				else if (ProgramInstance.PropertyIds != ProgramSource.ScalarPropertyIds)
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::InvalidOperand, {},
						"Blueprint event programs for one asset disagree on their scalar property schema");
					Instance.Faulted = true;
					return;
				}

				std::vector<float> CandidateState = ProgramInstance.ScalarState;
				const uint32_t ExecutionBudget = std::min(BlueprintInstructionsRemaining, BlueprintDefaultInstructionBudget);
				auto Result = ExecuteBlueprintProgram(*Instance.Program, CandidateState, ExecutionBudget, UIActionName);
				if (!Result)
				{
					if (Result.error().Code == BlueprintProgramErrorCode::InstructionBudgetExceeded)
					{
						BlueprintInstructionsRemaining -= ExecutionBudget;
						BlueprintInstructionsExecuted += ExecutionBudget;
					}
					RecordBlueprintDiagnostic(Target, &Binding, Event, Result.error().Code, Result.error().SourceNode,
						std::move(Result.error().Message));
					Instance.Faulted = true;
					return;
				}
				BlueprintInstructionsRemaining -= std::min(BlueprintInstructionsRemaining, Result->InstructionsExecuted);
				BlueprintInstructionsExecuted += Result->InstructionsExecuted;
				if (!CanQueueBlueprintTimers(Target, Binding, Result->TimerRequests))
				{
					const UUID SourceNode = Result->TimerRequests.empty() ? UUID{} : Result->TimerRequests.front().SourceNode;
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						SourceNode, "Scene runtime rejected the Blueprint timer request batch");
					Instance.Faulted = true;
					return;
				}
				if (!CanQueueBlueprintPhysicsForces(Target, Result->PhysicsForceRequests))
				{
					const UUID SourceNode = Result->PhysicsForceRequests.empty()
						? UUID{} : Result->PhysicsForceRequests.front().SourceNode;
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						SourceNode, "Scene runtime rejected the Blueprint physics force request batch");
					Instance.Faulted = true;
					return;
				}
				if (!CanQueueBlueprintEntitySpawns(Target, Result->EntitySpawnRequests))
				{
					const UUID SourceNode = Result->EntitySpawnRequests.empty()
						? UUID{} : Result->EntitySpawnRequests.front().SourceNode;
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						SourceNode, "Scene runtime rejected the Blueprint entity spawn batch");
					Instance.Faulted = true;
					return;
				}
				if (!Result->PropertyWrites.empty() &&
					(!Services.ApplyBlueprintPropertyWrites || !Services.ApplyBlueprintPropertyWrites(Target, Result->PropertyWrites)))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedPropertyWrite,
						Result->PropertyWrites.front().SourceNode,
						"Scene runtime host rejected the Blueprint property transaction");
					Instance.Faulted = true;
					return;
				}
				if (!QueueBlueprintPhysicsForces(Target, Binding, Event, Result->PhysicsForceRequests))
				{
					const UUID SourceNode = Result->PhysicsForceRequests.empty()
						? UUID{} : Result->PhysicsForceRequests.front().SourceNode;
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						SourceNode, "Scene runtime rejected the Blueprint physics force request batch");
					Instance.Faulted = true;
					return;
				}
				if (Result->RequestsStop && (!Services.QueueStop || !Services.QueueStop()))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						Result->StopRequestSourceNode, "Scene runtime host rejected the Blueprint stop request");
					Instance.Faulted = true;
					return;
				}
				if (!Result->RequestsStop && Result->RequestedSceneTransition &&
					(!Services.QueueSceneTransition || !Services.QueueSceneTransition(*Result->RequestedSceneTransition)))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						Result->SceneTransitionSourceNode, "Scene runtime host rejected the Blueprint transition request");
					Instance.Faulted = true;
					return;
				}
				if (!Result->RequestsStop && !Result->RequestedSceneTransition && Result->RequestedPause &&
					(!Services.QueuePause || !Services.QueuePause(*Result->RequestedPause)))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						Result->PauseRequestSourceNode, "Scene runtime host rejected the Blueprint pause request");
					Instance.Faulted = true;
					return;
				}
				if (!Result->RequestsStop && !Result->RequestedSceneTransition && Result->RequestsEntityDestroy &&
					!QueueEntityDestruction(Target))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						Result->EntityDestroySourceNode, "Scene runtime host rejected the Blueprint entity destruction request");
					Instance.Faulted = true;
					return;
				}
				if (Result->AudioControlRequest)
				{
					std::optional<AudioControlOperation> Operation;
					switch (Result->AudioControlRequest->Operation)
					{
					case BlueprintAudioControl::Play: Operation = AudioControlOperation::Play; break;
					case BlueprintAudioControl::Pause: Operation = AudioControlOperation::Pause; break;
					case BlueprintAudioControl::Resume: Operation = AudioControlOperation::Resume; break;
					case BlueprintAudioControl::Stop: Operation = AudioControlOperation::Stop; break;
					}
					const auto Queued = Operation ? Audio->QueueControl(Target, *Operation)
						: std::expected<void, AudioSceneRuntimeError>(std::unexpected(AudioSceneRuntimeError{
							AudioSceneRuntimeErrorCode::InvalidCommand, "Unknown Blueprint audio control operation" }));
					if (!Queued)
					{
						RecordBlueprintDiagnostic(Target, &Binding, Event,
							BlueprintProgramErrorCode::HostRejectedDeferredCall, Result->AudioControlRequest->SourceNode,
							"Scene runtime rejected the Blueprint audio control request: " + Queued.error().Message);
						Instance.Faulted = true;
						return;
					}
				}
				if (!QueueBlueprintTimers(Target, Binding, Result->TimerRequests))
				{
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						Result->TimerRequests.empty() ? UUID{} : Result->TimerRequests.front().SourceNode,
						"Scene runtime could not queue the Blueprint timer request batch");
					Instance.Faulted = true;
					return;
				}
				if (!QueueBlueprintEntitySpawns(Target, Binding, Event, Result->EntitySpawnRequests))
				{
					const UUID SourceNode = Result->EntitySpawnRequests.empty()
						? UUID{} : Result->EntitySpawnRequests.front().SourceNode;
					RecordBlueprintDiagnostic(Target, &Binding, Event, BlueprintProgramErrorCode::HostRejectedDeferredCall,
						SourceNode, "Scene runtime could not queue the Blueprint entity spawn batch");
					Instance.Faulted = true;
					return;
				}
				ProgramInstance.ScalarState = std::move(CandidateState);
				BlueprintRuntimeTrace Trace;
				Trace.Handle = { Target, Instance.Asset, SessionGeneration, BlueprintProgramGeneration, Instance.Revision };
				Trace.Event = Event;
				Trace.ExecutedNodes = std::move(Result->ExecutedNodes);
				Trace.ExecutedNodesTruncated = Result->ExecutedNodeTraceTruncated;
				Trace.ScalarPropertyIds = ProgramInstance.PropertyIds;
				Trace.ScalarValues = ProgramInstance.ScalarState;
				const auto PreviousTrace = std::find_if(BlueprintTraces.begin(), BlueprintTraces.end(),
					[&](const BlueprintRuntimeTrace& Existing)
					{
						return Existing.Handle.Target == Target && Existing.Handle.Asset == Instance.Asset;
					});
				if (PreviousTrace != BlueprintTraces.end())
					*PreviousTrace = std::move(Trace);
				else
				{
					if (BlueprintTraces.size() == BlueprintMaximumRuntimeTraces) BlueprintTraces.erase(BlueprintTraces.begin());
					BlueprintTraces.push_back(std::move(Trace));
				}
			}
			catch (const std::exception& Exception)
			{
				RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::HostFailure, {}, Exception.what());
				const auto Found = BlueprintEvents.find(Target.GetUUID());
				if (Found != BlueprintEvents.end()) Found->second[static_cast<size_t>(Event)].Faulted = true;
			}
			catch (...)
			{
				RecordBlueprintDiagnostic(Target, nullptr, Event, BlueprintProgramErrorCode::HostFailure, {},
					"Blueprint host callback failed with a non-standard exception");
				const auto Found = BlueprintEvents.find(Target.GetUUID());
				if (Found != BlueprintEvents.end()) Found->second[static_cast<size_t>(Event)].Faulted = true;
			}
		}

		void ReconcileBlueprintEntities(Scene& SceneValue, const SceneRuntimeServices& Services)
		{
			std::unordered_map<UUID, Entity, UUIDHash> LiveEntities;
			const std::vector<Entity> Entities = SceneValue.GetEntities();
			LiveEntities.reserve(Entities.size());
			for (const Entity& EntityValue : Entities) LiveEntities.emplace(EntityValue.GetUUID(), EntityValue);

			for (auto Iterator = BlueprintKnownEntities.begin(); Iterator != BlueprintKnownEntities.end();)
			{
				const auto Live = LiveEntities.find(Iterator->first);
				if (Live == LiveEntities.end() || Live->second != Iterator->second)
				{
					BlueprintEvents.erase(Iterator->first);
					BlueprintInstances.erase(Iterator->first);
					BlueprintInitialDefaults.erase(Iterator->first);
					std::erase_if(BlueprintTraces,
						[&](const BlueprintRuntimeTrace& Trace) { return Trace.Handle.Target == Iterator->second; });
					std::erase_if(PendingBlueprintTimers,
						[&](const PendingBlueprintTimer& Timer) { return Timer.Target == Iterator->second; });
					Iterator = BlueprintKnownEntities.erase(Iterator);
				}
				else ++Iterator;
			}

			for (const Entity& EntityValue : Entities)
			{
				const UUID Identifier = EntityValue.GetUUID();
				const auto Known = BlueprintKnownEntities.find(Identifier);
				if (Known != BlueprintKnownEntities.end()) continue;
				BlueprintKnownEntities.emplace(Identifier, EntityValue);
				DispatchBlueprintEvent(SceneValue, EntityValue, BlueprintEventKind::OnCreate, Services);
			}
			ApplyQueuedEntityDestructions(SceneValue);
		}
	};

	#if !defined(PF_COOKED_RUNTIME_ONLY)
	SceneRuntime::SceneRuntime(
		const Project& SourceProject,
		SceneRuntimeDesc Description,
		SceneRuntimeServices Services)
		: m_Project(&SourceProject), m_Description(Description), m_Services(Services)
	{
	}
	#endif

	SceneRuntime::SceneRuntime(
		std::shared_ptr<const CookedContentProvider> CookedAssets,
		SceneRuntimeDesc Description,
		SceneRuntimeServices Services)
		: m_CookedAssets(std::move(CookedAssets)), m_Description(Description), m_Services(Services)
	{
	}

	SceneRuntime::~SceneRuntime()
	{
		Stop();
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::Start(Scene& Source, bool DeferAudioActivation)
	{
		if (m_Impl)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::AlreadyRunning,
				SceneRuntimeSubsystem::None,
				"Scene runtime is already started"));

		try
		{
			const uint64_t SessionGeneration = NextRuntimeSessionGeneration();
			m_Description.Scripting.SessionGeneration = SessionGeneration;
			m_Description.Physics.SessionGeneration = SessionGeneration;
			auto Candidate = std::make_unique<Impl>();
			Candidate->Source = &Source;
			Candidate->SessionGeneration = SessionGeneration;
			Candidate->AudioActivationDeferred = DeferAudioActivation;
			Source.ClearRuntimeSkeletalPoses();
			const Project* SourceProject = nullptr;
#if !defined(PF_COOKED_RUNTIME_ONLY)
			SourceProject = m_Project;
#endif
			for (const Entity& EntityValue : Source.GetEntities())
			{
				auto Canvas = EntityValue.GetRuntimeUICanvas();
				if (!Canvas)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"Could not inspect runtime UI canvas: " + Canvas.error().Message));
				if (!Canvas->has_value() || !(**Canvas).Enabled) continue;
				if (Candidate->RuntimeUI)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"A scene may contain only one enabled screen-space Runtime UI Canvas"));
				auto Text = ReadRuntimeUIDocumentBytes(SourceProject, m_CookedAssets, (**Canvas).DocumentAsset);
				if (!Text)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"Could not load Runtime UI document " + (**Canvas).DocumentAsset.ToString() + ": " + Text.error()));
				auto Document = ParseRuntimeUIDocument(*Text);
				if (!Document)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed, SceneRuntimeSubsystem::None,
						"Runtime UI document failed validation: " + Document.error().Message));
				Candidate->RuntimeUI.emplace(Impl::RuntimeUICanvasState{
					.Owner = EntityValue, .Asset = (**Canvas).DocumentAsset, .InputPolicy = (**Canvas).InputPolicy,
					.Document = std::move(*Document),
					.TreeGeneration = SessionGeneration });
				Candidate->RuntimeUIView = { .LogicalSize = Candidate->RuntimeUI->Document.DesignSize,
					.FramebufferWidth = static_cast<uint32_t>(Candidate->RuntimeUI->Document.DesignSize.x),
					.FramebufferHeight = static_cast<uint32_t>(Candidate->RuntimeUI->Document.DesignSize.y) };
			}

			Candidate->AudioSessionState = m_Services.AudioSessionState;
			if (!Candidate->AudioSessionState)
			{
				AudioSessionDesc AudioDescription{ .Engine = m_Description.Audio, .Diagnostics = m_Services.Diagnostics };
				std::expected<std::unique_ptr<AudioSession>, AudioSessionError> CreatedSession =
					std::unexpected(AudioSessionError{ AudioSessionErrorCode::AssetProviderUnavailable,
						"Scene runtime requires a source project or cooked content provider" });
				if (m_CookedAssets)
					CreatedSession = AudioSession::CreateForCooked(
						m_CookedAssets, m_CookedAssets->GetAudioMixDefaults(), std::move(AudioDescription));
#if !defined(PF_COOKED_RUNTIME_ONLY)
				else if (m_Project)
					CreatedSession = AudioSession::CreateForProject(*m_Project, std::move(AudioDescription));
#endif
				if (!CreatedSession)
					return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed,
						SceneRuntimeSubsystem::AudioDevice, "Could not create audio session: " + CreatedSession.error().Message));
				Candidate->OwnedAudioSession = std::move(*CreatedSession);
				Candidate->AudioSessionState = Candidate->OwnedAudioSession.get();
			}
			const UUID* ExpectedProjectID = nullptr;
			if (m_CookedAssets)
				ExpectedProjectID = &m_CookedAssets->GetProjectID();
#if !defined(PF_COOKED_RUNTIME_ONLY)
			else if (m_Project)
				ExpectedProjectID = &m_Project->GetDescription().ID;
#endif
			if (!Candidate->AudioSessionState || !ExpectedProjectID ||
				Candidate->AudioSessionState->GetProjectID() != *ExpectedProjectID)
				return std::unexpected(MakeError(SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::AudioDevice,
					"Audio session identity does not match the active source project or cooked package"));

			Candidate->Physics = std::make_unique<PhysicsSceneRuntime>(m_Description.Physics);
			if (auto Result = Candidate->Physics->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Physics,
					"Could not start scene physics: " + Result.error().Message));

			Candidate->Audio = std::make_unique<AudioSceneRuntime>(
				Candidate->AudioSessionState->GetEngine(),
				Candidate->AudioSessionState->GetAssets(),
				Candidate->AudioSessionState->GetGeneration(),
				SessionGeneration);
			if (auto Result = Candidate->Audio->Start(Source, DeferAudioActivation); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Audio,
					"Could not start scene audio: " + Result.error().Message));

			ScriptRuntimeServices Services{
				.Physics = Candidate->Physics.get(),
				.Audio = Candidate->Audio.get(),
				.GameplayInput = m_Services.GameplayInput,
				.InputState = m_Services.InputState,
				.Observer = m_Services.ScriptObserver };
			auto* CandidateOwner = Candidate.get();
			Services.QueueEntityDestroy = [CandidateOwner](const Entity& Target)
			{
				return CandidateOwner->QueueEntityDestruction(Target);
			};
			const auto AudioSettingsAvailable = [CandidateOwner]() noexcept
			{
				return CandidateOwner->State == Impl::Lifecycle::Active && !CandidateOwner->AudioActivationDeferred &&
					CandidateOwner->AudioSessionState;
			};
			Services.GetAudioGain = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel) -> std::optional<float>
			{
				if (!AudioSettingsAvailable()) return std::nullopt;
				const AudioMixState Mix = CandidateOwner->AudioSessionState->GetMixState();
				if (Channel == "master") return Mix.Gains.Master;
				if (Channel == "music") return Mix.Gains.Music;
				if (Channel == "effects") return Mix.Gains.Effects;
				if (Channel == "ui") return Mix.Gains.UI;
				return std::nullopt;
			};
			Services.SetAudioGain = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel, float Gain)
			{
				if (!AudioSettingsAvailable()) return false;
				std::expected<void, AudioSessionError> Result = std::unexpected(AudioSessionError{
					AudioSessionErrorCode::InvalidMixSettings, "Unknown audio mix channel" });
				if (Channel == "master") Result = CandidateOwner->AudioSessionState->SetUserMasterGain(Gain);
				else if (Channel == "music") Result = CandidateOwner->AudioSessionState->SetUserBusGain(AudioBus::Music, Gain);
				else if (Channel == "effects") Result = CandidateOwner->AudioSessionState->SetUserBusGain(AudioBus::Effects, Gain);
				else if (Channel == "ui") Result = CandidateOwner->AudioSessionState->SetUserBusGain(AudioBus::UI, Gain);
				return Result.has_value();
			};
			Services.GetAudioMuted = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel) -> std::optional<bool>
			{
				if (!AudioSettingsAvailable()) return std::nullopt;
				const AudioMixState Mix = CandidateOwner->AudioSessionState->GetMixState();
				if (Channel == "master") return Mix.MasterMuted;
				if (Channel == "music") return Mix.MusicMuted;
				if (Channel == "effects") return Mix.EffectsMuted;
				if (Channel == "ui") return Mix.UIMuted;
				return std::nullopt;
			};
			Services.SetAudioMuted = [CandidateOwner, AudioSettingsAvailable](std::string_view Channel, bool Muted)
			{
				if (!AudioSettingsAvailable()) return false;
				std::expected<void, AudioSessionError> Result = std::unexpected(AudioSessionError{
					AudioSessionErrorCode::InvalidMixSettings, "Unknown audio mix channel" });
				if (Channel == "master") Result = CandidateOwner->AudioSessionState->SetMasterMuted(Muted);
				else if (Channel == "music") Result = CandidateOwner->AudioSessionState->SetBusMuted(AudioBus::Music, Muted);
				else if (Channel == "effects") Result = CandidateOwner->AudioSessionState->SetBusMuted(AudioBus::Effects, Muted);
				else if (Channel == "ui") Result = CandidateOwner->AudioSessionState->SetBusMuted(AudioBus::UI, Muted);
				return Result.has_value();
			};
			Services.SaveAudioSettings = [CandidateOwner, AudioSettingsAvailable]()
			{
				return AudioSettingsAvailable() && CandidateOwner->AudioSessionState->SaveUserSettings().has_value();
			};
			Services.GetAudioOutputState = [CandidateOwner, AudioSettingsAvailable]() -> std::optional<std::string_view>
			{
				if (!AudioSettingsAvailable()) return std::nullopt;
				return AudioDeviceStateName(CandidateOwner->AudioSessionState->GetDeviceState());
			};
			Services.RetryAudioOutput = [CandidateOwner, AudioSettingsAvailable]()
			{
				return AudioSettingsAvailable() && CandidateOwner->AudioSessionState->RetryDefaultOutput().has_value();
			};
			if (m_Services.QueueSceneTransition)
			{
				Services.QueueSceneTransition = [CandidateOwner, Request = m_Services.QueueSceneTransition](const AssetID& Asset)
				{
					if (CandidateOwner->State != Impl::Lifecycle::Active ||
						CandidateOwner->BoundaryRequestStatus == Impl::RequestStatus::Committed) return false;
					if (CandidateOwner->BoundaryRequest &&
						CandidateOwner->BoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop) return false;
					if (CandidateOwner->BoundaryRequest) return false;
					if (!Request(Asset)) return false;
					if (++CandidateOwner->NextBoundaryRequestGeneration == 0)
						++CandidateOwner->NextBoundaryRequestGeneration;
					CandidateOwner->BoundaryRequest = SceneRuntimeBoundaryRequest{
						.SessionGeneration = CandidateOwner->SessionGeneration,
						.RequestGeneration = CandidateOwner->NextBoundaryRequestGeneration,
						.Kind = SceneRuntimeBoundaryRequestKind::SceneTransition,
						.Scene = Asset };
					CandidateOwner->BoundaryRequestStatus = Impl::RequestStatus::Pending;
					return true;
				};
			}
			if (m_Services.QueueStop)
			{
				Services.QueueStop = [CandidateOwner, Request = m_Services.QueueStop]()
				{
					if (CandidateOwner->State != Impl::Lifecycle::Active ||
						CandidateOwner->BoundaryRequestStatus == Impl::RequestStatus::Committed) return false;
					if (CandidateOwner->BoundaryRequest &&
						CandidateOwner->BoundaryRequest->Kind == SceneRuntimeBoundaryRequestKind::Stop) return false;
					if (!Request()) return false;
					if (CandidateOwner->BoundaryRequest)
						CandidateOwner->BoundaryRequestStatus = Impl::RequestStatus::Superseded;
					if (++CandidateOwner->NextBoundaryRequestGeneration == 0)
						++CandidateOwner->NextBoundaryRequestGeneration;
					CandidateOwner->BoundaryRequest = SceneRuntimeBoundaryRequest{
						.SessionGeneration = CandidateOwner->SessionGeneration,
						.RequestGeneration = CandidateOwner->NextBoundaryRequestGeneration,
						.Kind = SceneRuntimeBoundaryRequestKind::Stop };
					CandidateOwner->BoundaryRequestStatus = Impl::RequestStatus::Pending;
					return true;
				};
			}
			Services.QueuePause = [CandidateOwner](bool Paused)
			{
				if (CandidateOwner->State != Impl::Lifecycle::Active || CandidateOwner->BoundaryRequest ||
					CandidateOwner->BoundaryRequestStatus == Impl::RequestStatus::Committed)
					return false;
				CandidateOwner->RequestedPause = Paused;
				return true;
			};
			Services.SetRuntimeUIText = [CandidateOwner](UUID ElementID, std::string Text)
			{
				if (CandidateOwner->State != Impl::Lifecycle::Active || !CandidateOwner->RuntimeUI ||
					Text.size() > RuntimeUIDocument::MaximumTextBytes)
					return false;
				RuntimeUIDocument CandidateDocument = CandidateOwner->RuntimeUI->Document;
				const auto Element = std::find_if(CandidateDocument.Elements.begin(), CandidateDocument.Elements.end(),
					[ElementID](const RuntimeUIElement& Value) { return Value.ID == ElementID; });
				if (Element == CandidateDocument.Elements.end() ||
					(Element->Kind != RuntimeUIElementKind::Text && Element->Kind != RuntimeUIElementKind::Button))
					return false;
				Element->Text = std::move(Text);
				if (!ValidateRuntimeUIDocument(CandidateDocument)) return false;
				CandidateOwner->RuntimeUI->Document = std::move(CandidateDocument);
				return true;
			};
			Candidate->Services = m_Services;
			Candidate->Services.QueueSceneTransition = Services.QueueSceneTransition;
			Candidate->Services.QueueStop = Services.QueueStop;
			Candidate->Services.QueuePause = Services.QueuePause;
			if (m_CookedAssets)
				Candidate->Scripting = std::make_unique<ScriptRuntime>(m_CookedAssets, m_Description.Scripting, Services);
#if !defined(PF_COOKED_RUNTIME_ONLY)
			else
				Candidate->Scripting = std::make_unique<ScriptRuntime>(*m_Project, m_Description.Scripting, Services);
#endif
			if (auto Result = Candidate->Scripting->Start(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::InitializationFailed,
					SceneRuntimeSubsystem::Scripting,
					"Could not start scene scripting: " + Result.error().Message));

			Candidate->State = Impl::Lifecycle::Active;
			Candidate->ApplyQueuedEntityDestructions(Source);
			Candidate->BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
			Candidate->BlueprintInstructionsExecuted = 0;
			Candidate->BlueprintDispatchesRemaining = BlueprintMaximumEventDispatchesPerFrame;
			Candidate->BlueprintDispatchLimitReported = false;
			if (m_Services.GetBlueprintProgramGeneration)
				Candidate->BlueprintProgramGeneration = m_Services.GetBlueprintProgramGeneration();
			for (const Entity& EntityValue : Source.GetEntities())
			{
				Candidate->BlueprintKnownEntities.emplace(EntityValue.GetUUID(), EntityValue);
				Candidate->DispatchBlueprintEvent(Source, EntityValue, BlueprintEventKind::OnCreate, Candidate->Services);
				Candidate->ApplyQueuedEntityDestructions(Source);
			}
			m_Impl = std::move(Candidate);
			return {};
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::InitializationFailed,
				SceneRuntimeSubsystem::None,
				std::string("Could not start scene runtime: ") + Exception.what()));
		}
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::ActivateAudio()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (!m_Impl->AudioActivationDeferred)
			return {};
		if (auto Activated = m_Impl->Audio->Activate(); !Activated)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not activate candidate scene audio: " + Activated.error().Message));
		m_Impl->AudioActivationDeferred = false;
		return {};
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::SuspendAudioForTransition()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (auto Suspended = m_Impl->Audio->SuspendForTransition(); !Suspended)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not suspend outgoing scene audio: " + Suspended.error().Message));
		return {};
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::ResumeAudioAfterTransition()
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (auto Resumed = m_Impl->Audio->ResumeAfterTransition(); !Resumed)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not resume retained scene audio: " + Resumed.error().Message));
		return {};
	}

	void SceneRuntime::Stop() noexcept
	{
		if (m_Impl)
		{
			m_Impl->BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
			m_Impl->BlueprintInstructionsExecuted = 0;
			m_Impl->BlueprintDispatchesRemaining = BlueprintMaximumEventDispatchesPerFrame;
			m_Impl->BlueprintDispatchLimitReported = false;
			m_Impl->State = Impl::Lifecycle::Stopping;
			if (m_Impl->Source)
				for (const Entity& EntityValue : m_Impl->Source->GetEntities())
					m_Impl->DispatchBlueprintEvent(*m_Impl->Source, EntityValue, BlueprintEventKind::OnDestroy, m_Impl->Services);
			if (m_Impl->Source)
				m_Impl->Source->ClearRuntimeSkeletalPoses();
			m_Impl->SkeletalInstances.clear();
			m_Impl->PendingContactEvents.clear();
			m_Impl->PendingBlueprintPhysicsForces.clear();
			m_Impl->PendingBlueprintTimers.clear();
			m_Impl->PendingBlueprintEntitySpawns.clear();
			m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Cancelled;
			m_Impl->BoundaryRequest.reset();
		}
		m_Impl.reset();
	}

	bool SceneRuntime::IsRunning() const noexcept
	{
		return m_Impl != nullptr;
	}

	std::expected<void, SceneRuntimeError> SceneRuntime::SetPaused(bool Paused)
	{
		if (!m_Impl)
			return std::unexpected(
				MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::None, "Scene runtime is not started"));
		if (m_Impl->Paused == Paused) return {};
		if (m_Services.AudioPauseGuard)
		{
			try
			{
				if (const auto Rejection = m_Services.AudioPauseGuard(Paused))
					return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
						"Audio pause change was rejected by the host: " + *Rejection));
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
					std::string("Audio pause guard failed: ") + Exception.what()));
			}
			catch (...)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
					"Audio pause guard failed with an unknown exception"));
			}
		}
		if (auto Result = m_Impl->Audio->SetPaused(Paused); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not change scene audio pause state: " + Result.error().Message));
		m_Impl->Paused = Paused;
		return {};
	}

	bool SceneRuntime::IsPaused() const noexcept
	{
		return m_Impl && m_Impl->Paused;
	}

	bool SceneRuntime::HasRuntimeUI() const noexcept
	{
		return m_Impl && m_Impl->RuntimeUI.has_value();
	}

	RuntimeUIInputPolicy SceneRuntime::GetRuntimeUIInputPolicy() const noexcept
	{
		if (!m_Impl || !m_Impl->RuntimeUI) return RuntimeUIInputPolicy::Passive;
		return m_Impl->Paused ? RuntimeUIInputPolicy::Modal : m_Impl->RuntimeUI->InputPolicy;
	}

	uint64_t SceneRuntime::GetRuntimeUIInputGeneration() const noexcept
	{
		return m_Impl && m_Impl->RuntimeUI ? m_Impl->RuntimeUI->TreeGeneration : 0;
	}

	bool SceneRuntime::SubmitRuntimeUIFrame(RuntimeUIViewport Viewport, std::span<const RuntimeUIInputEvent> Events)
	{
		if (!m_Impl || !m_Impl->RuntimeUI || Events.size() > 512) return false;
		m_Impl->RuntimeUIView = Viewport;
		m_Impl->RuntimeUIEvents.assign(Events.begin(), Events.end());
		return true;
	}

	std::span<const RuntimeUIDrawCommand> SceneRuntime::GetRuntimeUIDrawCommands() const noexcept
	{
		return m_Impl ? std::span<const RuntimeUIDrawCommand>(m_Impl->RuntimeUIDrawCommands) :
			std::span<const RuntimeUIDrawCommand>{};
	}

	RuntimeUIViewport SceneRuntime::GetRuntimeUIViewport() const noexcept
	{
		return m_Impl ? m_Impl->RuntimeUIView : RuntimeUIViewport{};
	}

	std::expected<uint64_t, SceneRuntimeError> SceneRuntime::RequestScriptReload(const Entity& Target)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::NotRunning, SceneRuntimeSubsystem::Scripting,
				"Script reload requires an active Play or Player session"));
		auto Queued = m_Impl->Scripting->RequestReload(Target);
		if (!Queued)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
				Queued.error().Message));
		return *Queued;
	}

	std::span<const ScriptDiagnostic> SceneRuntime::GetScriptDiagnostics() const noexcept
	{
		return m_Impl ? m_Impl->Scripting->GetDiagnostics() : std::span<const ScriptDiagnostic>{};
	}

	std::span<const BlueprintRuntimeDiagnostic> SceneRuntime::GetBlueprintDiagnostics() const noexcept
	{
		return m_Impl ? std::span<const BlueprintRuntimeDiagnostic>(m_Impl->BlueprintDiagnostics) :
			std::span<const BlueprintRuntimeDiagnostic>{};
	}

	std::span<const BlueprintRuntimeTrace> SceneRuntime::GetBlueprintTraces() const noexcept
	{
		return m_Impl ? std::span<const BlueprintRuntimeTrace>(m_Impl->BlueprintTraces) :
			std::span<const BlueprintRuntimeTrace>{};
	}

	std::optional<BlueprintRuntimeTrace> SceneRuntime::ReadBlueprintTrace(
		const BlueprintRuntimeTraceHandle& Handle) const
	{
		if (!m_Impl || m_Impl->State != Impl::Lifecycle::Active || !Handle.Target.IsValid() ||
			Handle.SessionGeneration != m_Impl->SessionGeneration ||
			Handle.ProgramGeneration != m_Impl->BlueprintProgramGeneration)
			return std::nullopt;
		const auto Trace = std::find_if(m_Impl->BlueprintTraces.begin(), m_Impl->BlueprintTraces.end(),
			[&](const BlueprintRuntimeTrace& Candidate)
			{
				return Candidate.Handle.Target == Handle.Target && Candidate.Handle.Asset == Handle.Asset &&
					Candidate.Handle.SessionGeneration == Handle.SessionGeneration &&
					Candidate.Handle.ProgramGeneration == Handle.ProgramGeneration &&
					Candidate.Handle.Revision == Handle.Revision;
			});
		return Trace == m_Impl->BlueprintTraces.end() ? std::nullopt : std::optional<BlueprintRuntimeTrace>(*Trace);
	}

	std::optional<SceneRuntimeBoundaryRequest> SceneRuntime::GetPendingBoundaryRequest() const noexcept
	{
		if (!m_Impl || m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending) return std::nullopt;
		return m_Impl->BoundaryRequest;
	}

	bool SceneRuntime::ResolveBoundaryRequest(
		const SceneRuntimeBoundaryRequest& Request,
		SceneRuntimeBoundaryResolution Resolution) noexcept
	{
		if (!m_Impl || m_Impl->State != Impl::Lifecycle::Active ||
			m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending || !m_Impl->BoundaryRequest ||
			m_Impl->BoundaryRequest->SessionGeneration != Request.SessionGeneration ||
			m_Impl->BoundaryRequest->RequestGeneration != Request.RequestGeneration ||
			m_Impl->BoundaryRequest->Kind != Request.Kind || m_Impl->BoundaryRequest->Scene != Request.Scene)
			return false;

		switch (Resolution)
		{
			case SceneRuntimeBoundaryResolution::Committed:
				m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Committed;
				return true;
			case SceneRuntimeBoundaryResolution::Rejected:
				m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Rejected;
				m_Impl->BoundaryRequest.reset();
				return true;
			case SceneRuntimeBoundaryResolution::Cancelled:
				m_Impl->BoundaryRequestStatus = Impl::RequestStatus::Cancelled;
				m_Impl->BoundaryRequest.reset();
				return true;
		}
		return false;
	}

	std::expected<SceneRuntimeFrameResult, SceneRuntimeError> SceneRuntime::Advance(
		Scene& Source,
		Timestep FrameDelta)
	{
		if (!m_Impl)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::NotRunning,
				SceneRuntimeSubsystem::None,
				"Scene runtime is not started"));
		if (m_Impl->Source != &Source)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::DifferentScene,
				SceneRuntimeSubsystem::None,
				"Scene runtime was started for a different Scene"));
		const double DeltaSeconds = FrameDelta.GetSeconds();
		if (!std::isfinite(DeltaSeconds) || DeltaSeconds < 0.0)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::InvalidDeltaTime,
				SceneRuntimeSubsystem::None,
				"Scene runtime frame delta must be finite and non-negative"));
		if (m_Services.GetBlueprintProgramGeneration)
		{
			uint64_t CurrentProgramGeneration = 0;
			try
			{
				CurrentProgramGeneration = m_Services.GetBlueprintProgramGeneration();
			}
			catch (const std::exception& Exception)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
					std::string("Could not read Blueprint catalog generation: ") + Exception.what()));
			}
			catch (...)
			{
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
					"Could not read Blueprint catalog generation"));
			}
			if (CurrentProgramGeneration != m_Impl->BlueprintProgramGeneration)
			{
				m_Impl->BlueprintEvents.clear();
				m_Impl->BlueprintInstances.clear();
				m_Impl->BlueprintTraces.clear();
				m_Impl->PendingBlueprintPhysicsForces.clear();
				m_Impl->PendingBlueprintTimers.clear();
				m_Impl->PendingBlueprintEntitySpawns.clear();
				m_Impl->BlueprintProgramGeneration = CurrentProgramGeneration;
			}
		}
		m_Impl->BlueprintInstructionsRemaining = MaximumBlueprintInstructionsPerFrame;
		m_Impl->BlueprintInstructionsExecuted = 0;
		m_Impl->BlueprintDispatchesRemaining = BlueprintMaximumEventDispatchesPerFrame;
		m_Impl->BlueprintDispatchLimitReported = false;
		const auto MakeFrameResult = [&](uint32_t PhysicsSteps = 0)
		{
			return SceneRuntimeFrameResult{ PhysicsSteps, m_Impl->Physics->GetStats(), m_Impl->BlueprintInstructionsExecuted };
		};
		if (m_Impl->AudioSessionState)
			m_Impl->AudioSessionState->PollDeviceState();
		if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
			m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
			return MakeFrameResult();
		m_Impl->ReconcileBlueprintEntities(Source, m_Impl->Services);
		if (auto Result = m_Impl->Scripting->ServicePendingReloads(Source); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Scripting,
				"Could not service queued script reloads: " + Result.error().Message));
		m_Impl->ApplyQueuedEntityDestructions(Source);
		if (m_Impl->RuntimeUI)
		{
			auto& Canvas = *m_Impl->RuntimeUI;
			auto Layout = Canvas.Controller.Layout(Canvas.Document, m_Impl->RuntimeUIView, Canvas.TreeGeneration);
			if (!Layout)
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::None,
					"Could not lay out Runtime UI: " + Layout.error().Message));
			Canvas.Layout = std::move(*Layout);
			const std::vector<RuntimeUIAction> Actions = Canvas.Controller.ProcessInput(
				Canvas.Document, Canvas.Layout, m_Impl->RuntimeUIEvents, Canvas.TreeGeneration);
			Canvas.DrawCommands = BuildRuntimeUIDrawList(Canvas.Document, Canvas.Layout,
				Canvas.Controller.GetFocusedElement(), Canvas.Controller.GetHoveredElement());
			m_Impl->RuntimeUIEvents.clear();
			m_Impl->RuntimeUIDrawCommands = Canvas.DrawCommands;
			const auto CurrentOwner = Source.FindEntity(Canvas.Owner.GetUUID());
			if (CurrentOwner && *CurrentOwner == Canvas.Owner)
				for (const RuntimeUIAction& Action : Actions)
				{
					const bool ContinueFrame = m_Impl->Scripting->DispatchRuntimeUIAction(Source, Canvas.Owner, Action.Name);
					m_Impl->ApplyQueuedEntityDestructions(Source);
					if (!ContinueFrame)
						return MakeFrameResult();
					m_Impl->DispatchBlueprintEvent(Source, Canvas.Owner, BlueprintEventKind::OnUIAction, m_Impl->Services, Action.Name);
					m_Impl->ApplyQueuedEntityDestructions(Source);
					if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
						m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
						return MakeFrameResult();
				}
		}
		else
			m_Impl->RuntimeUIDrawCommands.clear();
		if (m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending &&
			m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Committed)
		{
			if (auto Result = m_Impl->Audio->ProcessPendingCommands(); !Result)
				return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
					"Could not apply queued audio controls: " + Result.error().Message));
		}
		if (m_Impl->RequestedPause && m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Pending &&
			m_Impl->BoundaryRequestStatus != Impl::RequestStatus::Committed)
		{
			const bool RequestedPause = *m_Impl->RequestedPause;
			m_Impl->RequestedPause.reset();
			if (auto Result = SetPaused(RequestedPause); !Result) return std::unexpected(Result.error());
		}

		if (m_Impl->Paused) return MakeFrameResult();
		m_Impl->BlueprintClockSeconds += DeltaSeconds;

		{
			ScopedDiagnosticTimer Timer(m_Services.Diagnostics, "ScriptRuntime::Advance", "ScriptRuntime");
			if (auto Result = m_Impl->Scripting->Advance(Source, FrameDelta); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::UpdateFailed,
					SceneRuntimeSubsystem::Scripting,
					"Could not update scene scripts: " + Result.error().Message));
		}
		m_Impl->ApplyQueuedEntityDestructions(Source);
		if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
			m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
			return MakeFrameResult();
		// Timer callbacks run after Lua's frame update and before Blueprint input edges, update, and physics contacts.
		m_Impl->DispatchDueBlueprintTimers(Source);
		if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
			m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
			return MakeFrameResult();
		// Lua has read the current action snapshot; deliver its edges before Blueprint OnUpdate and physics contacts.
		if (m_Services.GameplayInput)
		{
			const GameplayActionMap& ActionMap = m_Services.GameplayInput->GetActionMap();
			const ActionSnapshot& Actions = m_Services.GameplayInput->GetSnapshot();
			for (const GameplayActionDefinition& Definition : ActionMap.Actions)
			{
				if (m_Impl->BlueprintDispatchesRemaining == 0) break;
				if (Definition.Context != InputContext::Gameplay) continue;
				const ActionValue* Value = Actions.Find(Definition.Id);
				if (!Value) continue;
				std::optional<BlueprintEventKind> Event;
				if (Value->Canceled) Event = BlueprintEventKind::OnActionCancelled;
				else if (Value->Released) Event = BlueprintEventKind::OnActionReleased;
				else if (Value->Pressed) Event = BlueprintEventKind::OnActionPressed;
				if (!Event) continue;
				for (const Entity& EntityValue : Source.GetEntities())
				{
					if (m_Impl->BlueprintDispatchesRemaining == 0) break;
					m_Impl->DispatchBlueprintEvent(Source, EntityValue, *Event, m_Impl->Services, Definition.Id);
					m_Impl->ApplyQueuedEntityDestructions(Source);
					if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
						m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
						return MakeFrameResult();
				}
				if (m_Impl->BlueprintDispatchesRemaining == 0 && !m_Impl->BlueprintDispatchLimitReported)
				{
					m_Impl->BlueprintDispatchLimitReported = true;
					m_Impl->RecordBlueprintDiagnostic({}, nullptr, *Event, BlueprintProgramErrorCode::EventBudgetExceeded, {},
						"Scene Blueprint event dispatch budget was exhausted for this frame");
				}
			}
		}
		for (const Entity& EntityValue : Source.GetEntities())
		{
			if (m_Impl->BlueprintDispatchesRemaining == 0) break;
			m_Impl->DispatchBlueprintEvent(Source, EntityValue, BlueprintEventKind::OnUpdate, m_Impl->Services);
			m_Impl->ApplyQueuedEntityDestructions(Source);
			if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
				m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
				return MakeFrameResult();
		}
		if (auto Result = m_Impl->Audio->ProcessPendingCommands(); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not apply queued audio controls: " + Result.error().Message));
		if (m_Impl->RequestedPause)
		{
			const bool RequestedPause = *m_Impl->RequestedPause;
			m_Impl->RequestedPause.reset();
			if (auto Result = SetPaused(RequestedPause); !Result) return std::unexpected(Result.error());
			if (m_Impl->Paused) return MakeFrameResult();
		}

		auto PhysicsResult = [&]
		{
			m_Impl->ApplyQueuedBlueprintPhysicsForces(Source);
			m_Impl->ApplyQueuedBlueprintEntitySpawns(Source);
			ScopedDiagnosticTimer Timer(m_Services.Diagnostics, "PhysicsSceneRuntime::Advance", "PhysicsSceneRuntime");
			return m_Impl->Physics->Advance(Source, FrameDelta);
		}();
		if (!PhysicsResult)
			return std::unexpected(MakeError(
				SceneRuntimeErrorCode::UpdateFailed,
				SceneRuntimeSubsystem::Physics,
				"Could not advance scene physics: " + PhysicsResult.error().Message));

		// Sample after physics and publish before contacts and rendering: physics uses the previous pose,
		// while color, AO, and shadow passes consume the same current pose snapshot.
		const Project* SourceProject = nullptr;
#if !defined(PF_COOKED_RUNTIME_ONLY)
		SourceProject = m_Project;
#endif
		if (auto Animation = m_Impl->AdvanceSkeletalAnimations(
				Source, static_cast<float>(DeltaSeconds), SourceProject, m_CookedAssets); !Animation)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Animation,
				"Could not advance scene skeletal animation: " + Animation.error()));

		const std::vector<PhysicsContactEvent> ContactEvents = m_Impl->Physics->TakeContactEvents();
		constexpr size_t MaximumQueuedContactEvents = 65536;
		bool ContactQueueOverflowed = false;
		for (const PhysicsContactEvent& Event : ContactEvents)
		{
			if (m_Impl->PendingContactEvents.size() == MaximumQueuedContactEvents)
			{
				ContactQueueOverflowed = true;
				break;
			}
			m_Impl->PendingContactEvents.push_back(Event);
		}
		if (ContactQueueOverflowed)
			m_Impl->RecordBlueprintDiagnostic({}, nullptr, BlueprintEventKind::OnCollisionBegin,
				BlueprintProgramErrorCode::EventBudgetExceeded, {},
				"Scene contact event queue reached its limit; excess events were discarded");
		const size_t ContactEventsToDispatch = std::min(m_Impl->PendingContactEvents.size(),
			static_cast<size_t>(BlueprintMaximumEventDispatchesPerFrame));
		const auto IsCurrentContactEndpoint = [&](const Entity& Target, UUID Identifier)
		{
			if (!Target.IsValid() || Target.GetUUID() != Identifier) return false;
			const auto Current = Source.FindEntity(Identifier);
			return Current && *Current == Target;
		};
		for (size_t ContactIndex = 0; ContactIndex < ContactEventsToDispatch; ++ContactIndex)
		{
			if (m_Impl->BlueprintDispatchesRemaining < 2) break;
			const PhysicsContactEvent Event = m_Impl->PendingContactEvents.front();
			m_Impl->PendingContactEvents.pop_front();
			if (Event.SessionGeneration != m_Impl->SessionGeneration ||
				!IsCurrentContactEndpoint(Event.First, Event.FirstIdentifier) ||
				!IsCurrentContactEndpoint(Event.Second, Event.SecondIdentifier))
				continue;
			const bool ContinueFrame = m_Impl->Scripting->DispatchPhysicsContact(Source, Event);
			if (!ContinueFrame)
			{
				m_Impl->ApplyQueuedEntityDestructions(Source);
				return MakeFrameResult(*PhysicsResult);
			}
			const BlueprintEventKind BlueprintEvent = Event.IsSensorPair
				? Event.Phase == PhysicsContactPhase::Begin ? BlueprintEventKind::OnTriggerEnter
					: Event.Phase == PhysicsContactPhase::Stay ? BlueprintEventKind::OnTriggerStay : BlueprintEventKind::OnTriggerExit
				: Event.Phase == PhysicsContactPhase::Begin ? BlueprintEventKind::OnCollisionBegin
					: Event.Phase == PhysicsContactPhase::Stay ? BlueprintEventKind::OnCollisionStay : BlueprintEventKind::OnCollisionEnd;
			m_Impl->DispatchBlueprintEvent(Source, Event.First, BlueprintEvent, m_Impl->Services);
			if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
				m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
			{
				m_Impl->ApplyQueuedEntityDestructions(Source);
				return MakeFrameResult(*PhysicsResult);
			}
			m_Impl->DispatchBlueprintEvent(Source, Event.Second, BlueprintEvent, m_Impl->Services);
			m_Impl->ApplyQueuedEntityDestructions(Source);
			if (m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Pending ||
				m_Impl->BoundaryRequestStatus == Impl::RequestStatus::Committed)
				return MakeFrameResult(*PhysicsResult);
		}
		if (auto Result = m_Impl->Audio->ProcessPendingCommands(); !Result)
			return std::unexpected(MakeError(SceneRuntimeErrorCode::UpdateFailed, SceneRuntimeSubsystem::Audio,
				"Could not apply queued contact audio controls: " + Result.error().Message));

		{
			ScopedDiagnosticTimer Timer(m_Services.Diagnostics, "AudioSceneRuntime::Advance", "AudioSceneRuntime");
			if (auto Result = m_Impl->Audio->Advance(Source); !Result)
				return std::unexpected(MakeError(
					SceneRuntimeErrorCode::UpdateFailed,
					SceneRuntimeSubsystem::Audio,
					"Could not update scene audio: " + Result.error().Message));
		}

		return MakeFrameResult(*PhysicsResult);
	}
}
