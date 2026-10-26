#include "Assets/AssetCookService.h"

#include "Assets/AssetDependencyExtractor.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Assets/CookedProductCodec.h"
#include "Assets/Detail/AssetCpuProducts.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/MaterialAsset.h"
#include "Assets/Project.h"
#include "Assets/PrefabSerializer.h"
#include "Core/PlatformServices.h"
#include "Input/GameplayInput.h"
#include "Input/GameplayInputStore.h"
#include "Renderer/EnvironmentLightingCache.h"
#include "Scene/SceneSerializer.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <span>
#include <system_error>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace PulseForge
{
	namespace
	{
		constexpr uint64_t MaximumAuthoredPayloadBytes = 512ull * 1024ull * 1024ull;
		constexpr uint64_t MaximumProjectBytes = ProjectSerializer::MaximumDocumentBytes;
		constexpr uint64_t MaximumInputMapBytes = 1024ull * 1024ull;
		constexpr std::string_view MeshDefaultSubresource = "mesh/0/primitive/0";
		constexpr std::string_view TextureSrgbSubresource = "srgb";
		constexpr std::string_view TextureLinearSubresource = "linear";

		AssetCookError Error(AssetCookErrorCode Code, std::string Message, std::filesystem::path Path = {}, AssetID Asset = {})
		{
			return { Code, Asset, std::move(Path), std::move(Message) };
		}

		std::expected<std::filesystem::path, AssetCookError> AbsoluteNormalized(const std::filesystem::path& Path)
		{
			std::error_code ErrorCode;
			std::filesystem::path Absolute = std::filesystem::absolute(Path, ErrorCode);
			if (ErrorCode) return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Could not make a path absolute: " + ErrorCode.message(), Path));
			return Absolute.lexically_normal();
		}

		bool IsSameOrWithin(const std::filesystem::path& Child, const std::filesystem::path& Parent)
		{
			auto ChildIt = Child.begin();
			for (auto ParentIt = Parent.begin(); ParentIt != Parent.end(); ++ParentIt, ++ChildIt)
				if (ChildIt == Child.end() || *ChildIt != *ParentIt) return false;
			return true;
		}

		std::expected<void, AssetCookError> RejectLinkedAncestors(const std::filesystem::path& Absolute)
		{
			std::filesystem::path Current;
			for (const auto& Component : Absolute)
			{
				Current /= Component;
				std::error_code StatusError;
				const auto Status = std::filesystem::symlink_status(Current, StatusError);
				if (StatusError == std::errc::no_such_file_or_directory) continue;
				if (StatusError)
					return std::unexpected(Error(AssetCookErrorCode::InvalidRoot,
						"Could not inspect cook path component: " + StatusError.message(), Current));
				if (std::filesystem::is_symlink(Status))
					return std::unexpected(Error(AssetCookErrorCode::InvalidRoot,
						"Cook source and output paths may not traverse symbolic links", Current));
			}
			return {};
		}

		std::expected<std::vector<std::byte>, AssetCookError> ReadBounded(
			const std::filesystem::path& Path, uint64_t Limit, AssetID Asset = {})
		{
			std::error_code SizeError;
			const uintmax_t Size = std::filesystem::file_size(Path, SizeError);
			if (SizeError || Size > Limit || Size > static_cast<uintmax_t>((std::numeric_limits<size_t>::max)()) ||
				Size > static_cast<uintmax_t>((std::numeric_limits<std::streamsize>::max)()))
				return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed,
					SizeError ? "Could not inspect source payload: " + SizeError.message() : "Source payload exceeds its configured limit", Path, Asset));
			std::ifstream Input(Path, std::ios::binary);
			if (!Input) return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed, "Could not open source payload", Path, Asset));
			std::vector<std::byte> Bytes(static_cast<size_t>(Size));
			if (!Bytes.empty()) Input.read(reinterpret_cast<char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			if (!Input || Input.bad())
				return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed, "Could not read complete source payload", Path, Asset));
			return Bytes;
		}

		std::expected<void, AssetCookError> WriteFile(const std::filesystem::path& Path, std::span<const std::byte> Bytes)
		{
			std::error_code CreateError;
			std::filesystem::create_directories(Path.parent_path(), CreateError);
			if (CreateError) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not create staging directory: " + CreateError.message(), Path.parent_path()));
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not create staged payload", Path));
			if (!Bytes.empty()) Output.write(reinterpret_cast<const char*>(Bytes.data()), static_cast<std::streamsize>(Bytes.size()));
			Output.flush();
			if (!Output) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not write complete staged payload", Path));
			return {};
		}

		struct ProjectInput
		{
			ProjectDesc Description;
			AssetContentFingerprint Fingerprint;
			uint64_t Bytes = 0;
		};

		std::expected<ProjectInput, AssetCookError> ReadProjectDescription(
			const std::filesystem::path& ProjectFile)
		{
			auto Bytes = ReadBounded(ProjectFile, MaximumProjectBytes);
			if (!Bytes) return std::unexpected(Bytes.error());
			const std::string Text(Bytes->empty() ? "" : reinterpret_cast<const char*>(Bytes->data()), Bytes->size());
			auto Description = ProjectSerializer::Deserialize(Text);
			if (!Description)
				return std::unexpected(Error(AssetCookErrorCode::InvalidRequest,
					"Cook source project document is invalid: " + Description.error().Message, ProjectFile));
			return ProjectInput{ std::move(*Description), ComputeAssetContentFingerprint(*Bytes), Bytes->size() };
		}

		std::expected<void, AssetCookError> ValidateRequest(const AssetCookRequest& Request, ProjectDesc* Project = nullptr,
			AssetContentFingerprint* ProjectFingerprint = nullptr, uint64_t* ProjectBytes = nullptr)
		{
			if (!Request.SourceSnapshot || Request.SourceSnapshot->GetProjectID().IsNil() || Request.StartupScene.IsNil() ||
				Request.SourceProjectFile.empty() || Request.OutputRoot.empty() || Request.Compatibility.TargetPlatform.empty() ||
				Request.Compatibility.RendererBackend.empty() || Request.Compatibility.RuntimeABI.empty())
				return std::unexpected(Error(AssetCookErrorCode::InvalidRequest, "Cook request is missing its source snapshot, project, roots, compatibility, or destination"));

			auto ProjectFile = AbsoluteNormalized(Request.SourceProjectFile);
			auto SourceRoot = AbsoluteNormalized(Request.SourceSnapshot->GetProjectRootPath());
			auto Destination = AbsoluteNormalized(Request.OutputRoot);
			if (!ProjectFile) return std::unexpected(ProjectFile.error());
			if (!SourceRoot) return std::unexpected(SourceRoot.error());
			if (!Destination) return std::unexpected(Destination.error());
			if (ProjectFile->parent_path() != *SourceRoot)
				return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Project file is not directly associated with the captured project root", *ProjectFile));
			if (IsSameOrWithin(*Destination, *SourceRoot) || IsSameOrWithin(*SourceRoot, *Destination))
				return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Cook output must not overlap the source project tree", *Destination));
			if (Request.SourceSnapshot->GetProjectRootPath().empty() || Request.SourceSnapshot->GetProjectID().IsNil())
				return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Dependency snapshot has no project identity or root"));
			if (auto Links = RejectLinkedAncestors(*ProjectFile); !Links) return Links;
			if (auto Links = RejectLinkedAncestors(*SourceRoot); !Links) return Links;
			if (auto Links = RejectLinkedAncestors(*Destination); !Links) return Links;

			const auto HostTarget =
#if defined(_WIN32)
				"Windows";
#elif defined(__APPLE__)
				"macOS";
#elif defined(__linux__)
				"Linux";
#else
				"Unsupported";
#endif
			if (Request.Compatibility.TargetPlatform != HostTarget)
				return std::unexpected(Error(AssetCookErrorCode::UnsupportedTarget,
					"This cook adapter supports only the host target platform '" + std::string(HostTarget) + "'"));
			if (Request.Compatibility.RendererBackend != "Vulkan" && Request.Compatibility.RendererBackend != "OpenGL")
				return std::unexpected(Error(AssetCookErrorCode::UnsupportedTarget, "Requested renderer backend is not supported by the cook service"));

			auto Description = ReadProjectDescription(*ProjectFile);
			if (!Description) return std::unexpected(Description.error());
			if (Description->Description.ID != Request.SourceSnapshot->GetProjectID())
				return std::unexpected(Error(AssetCookErrorCode::InvalidRequest, "Project document identity does not match the retained dependency snapshot", *ProjectFile));
			if (Project) *Project = Description->Description;
			if (ProjectFingerprint) *ProjectFingerprint = Description->Fingerprint;
			if (ProjectBytes) *ProjectBytes = Description->Bytes;
			return {};
		}

		std::expected<void, AssetCookError> ValidateRegistryMembership(const AssetDependencyCatalogSnapshot& Snapshot)
		{
			AssetRegistry Current;
			auto Rebuilt = Current.Rebuild(Snapshot.GetProjectRootPath());
			if (!Rebuilt)
			{
				const std::string Detail = Rebuilt.error().Issues.empty() ? "registry rebuild failed" : Rebuilt.error().Issues.front().Message;
				return std::unexpected(Error(AssetCookErrorCode::SourceChanged,
					"Source registry membership is invalid or changed: " + Detail,
					Rebuilt.error().Issues.empty() ? Snapshot.GetProjectRootPath() : Rebuilt.error().Issues.front().ProjectRelativePath));
			}
			const auto Expected = Snapshot.GetRegistry().GetAssets();
			const auto Actual = Current.GetAssets();
			if (Expected.size() != Actual.size())
				return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Managed asset membership changed after dependency analysis", Snapshot.GetProjectRootPath()));
			for (size_t Index = 0; Index < Expected.size(); ++Index)
				if (Expected[Index].ID != Actual[Index].ID || Expected[Index].ProjectRelativePath != Actual[Index].ProjectRelativePath)
					return std::unexpected(Error(AssetCookErrorCode::SourceChanged,
						"Managed asset identity or location changed after dependency analysis",
						Snapshot.GetProjectRootPath(), Actual[Index].ID));
			return {};
		}

		std::expected<void, AssetCookError> ValidateCookSourceFresh(const AssetCookPlan& Plan)
		{
			const auto& Request = Plan.Request;
			if (auto ReadSet = AssetDependencyExtractor::ValidateInputReceipts(
				Request.SourceSnapshot->GetProjectRootPath(), Request.SourceSnapshot->GetInputReadSet()); !ReadSet)
				return std::unexpected(Error(AssetCookErrorCode::SourceChanged,
					"Source project changed during cook: " + ReadSet.error().Message, ReadSet.error().Path));
			if (auto Membership = ValidateRegistryMembership(*Request.SourceSnapshot); !Membership)
				return std::unexpected(Membership.error());
			auto Project = ReadProjectDescription(Request.SourceProjectFile);
			if (!Project || Project->Fingerprint != Plan.ProjectFileFingerprint || Project->Bytes != Plan.ProjectFileBytes)
				return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Project startup settings changed during cook", Request.SourceProjectFile));
			const std::filesystem::path InputActionsPath = Request.SourceSnapshot->GetProjectRootPath() / "InputActions.json";
			std::error_code InputError;
			const bool InputPresent = std::filesystem::exists(InputActionsPath, InputError);
			if (InputError || InputPresent != Plan.GameplayInputPresent)
				return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Gameplay input defaults changed during cook", InputActionsPath));
			if (Plan.GameplayInputFingerprint)
			{
				auto InputBytes = ReadBounded(InputActionsPath, MaximumInputMapBytes);
				if (!InputBytes || InputBytes->size() != Plan.GameplayInputBytes ||
					ComputeAssetContentFingerprint(*InputBytes) != *Plan.GameplayInputFingerprint)
					return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Gameplay input defaults changed during cook", InputActionsPath));
			}
			return {};
		}

		std::optional<AssetFileType> GetCookAssetType(const std::filesystem::path& Path)
		{
			for (const AssetFileType Type : { AssetFileType::Scene, AssetFileType::Prefab, AssetFileType::Mesh, AssetFileType::Material,
				AssetFileType::Script, AssetFileType::Audio, AssetFileType::Environment, AssetFileType::Image })
				if (HasAssetTypeExtension(Path, Type)) return Type;
			return std::nullopt;
		}

		std::optional<CookedAssetProductKind> ProductForType(AssetFileType Type)
		{
			switch (Type)
			{
			case AssetFileType::Scene: return CookedAssetProductKind::Scene;
			case AssetFileType::Prefab: return CookedAssetProductKind::Prefab;
			case AssetFileType::Mesh: return CookedAssetProductKind::MeshPrimitive;
			case AssetFileType::Material: return CookedAssetProductKind::Material;
			case AssetFileType::Script: return CookedAssetProductKind::Script;
			case AssetFileType::Audio: return CookedAssetProductKind::Audio;
			case AssetFileType::Environment: return CookedAssetProductKind::Environment;
			case AssetFileType::Image: return CookedAssetProductKind::Texture;
			default: return std::nullopt;
			}
		}

		std::string TextureSubresource(const AssetDependencyEdge& Edge)
		{
			if (Edge.Location == "material.baseColorTexture" || Edge.Location == "material.emissiveTexture")
				return std::string(TextureSrgbSubresource);
			if (Edge.Location == "material.metallicRoughnessTexture" || Edge.Location == "material.normalTexture" ||
				Edge.Location == "material.occlusionTexture")
				return std::string(TextureLinearSubresource);
			return std::string(TextureSrgbSubresource);
		}

		CookedAssetDependency ProductDependencyForEdge(const AssetDependencyEdge& Edge, AssetFileType Type)
		{
			const auto Product = Edge.Location.starts_with("material.")
				? CookedAssetProductKind::Texture : ProductForType(Type).value_or(CookedAssetProductKind::Scene);
			std::string Subresource;
			if (Product == CookedAssetProductKind::MeshPrimitive) Subresource = MeshDefaultSubresource;
			else if (Product == CookedAssetProductKind::Texture) Subresource = TextureSubresource(Edge);
			return { Edge.Target.Asset, Product, std::move(Subresource) };
		}

		std::string ProductIdentityKey(const CookedAssetDependency& Product)
		{
			return Product.Asset.ToString() + ":" + std::to_string(static_cast<uint32_t>(Product.Product)) + ":" + Product.Subresource;
		}

		std::string Hex(std::string_view Text)
		{
			constexpr char Digits[] = "0123456789abcdef";
			std::string Output;
			Output.reserve(Text.size() * 2);
			for (unsigned char Character : Text)
			{
				Output.push_back(Digits[Character >> 4]);
				Output.push_back(Digits[Character & 0x0f]);
			}
			return Output;
		}

		std::string ProductPath(const CookedAssetEntry& Entry, std::string_view Generation)
		{
			std::string Path = "g/" + std::string(Generation) + "/" + Entry.Asset.ToString() + "_" +
				std::to_string(static_cast<uint32_t>(Entry.Product));
			if (!Entry.Subresource.empty()) Path += "_" + Hex(Entry.Subresource);
			return Path + ".bin";
		}

		bool SameProducts(const AssetCookPlan& First, const AssetCookPlan& Second)
		{
			if (First.ProjectID != Second.ProjectID || First.ProjectFileFingerprint != Second.ProjectFileFingerprint ||
				First.ProjectFileBytes != Second.ProjectFileBytes || First.GameplayInputFingerprint != Second.GameplayInputFingerprint ||
				First.GameplayInputBytes != Second.GameplayInputBytes || First.GameplayInputPresent != Second.GameplayInputPresent ||
				First.SortedRoots != Second.SortedRoots ||
				First.Request.SourceProjectFile != Second.Request.SourceProjectFile || First.Request.StartupScene != Second.Request.StartupScene ||
				First.Request.DynamicRoots != Second.Request.DynamicRoots || First.Request.OutputRoot != Second.Request.OutputRoot ||
				First.Request.Compatibility.TargetPlatform != Second.Request.Compatibility.TargetPlatform ||
				First.Request.Compatibility.RendererBackend != Second.Request.Compatibility.RendererBackend ||
				First.Request.Compatibility.RuntimeABI != Second.Request.Compatibility.RuntimeABI ||
				First.Request.SourceSnapshot.get() != Second.Request.SourceSnapshot.get() ||
				First.Products.size() != Second.Products.size()) return false;
			for (size_t Index = 0; Index < First.Products.size(); ++Index)
			{
				const auto& A = First.Products[Index];
				const auto& B = Second.Products[Index];
				if (A.Asset != B.Asset || A.Product != B.Product || A.Subresource != B.Subresource ||
					A.SourcePath != B.SourcePath || A.RuntimeDependencies != B.RuntimeDependencies) return false;
			}
			return true;
		}

		bool StageContentsAreOwned(const std::filesystem::path& Stage, const std::vector<std::filesystem::path>& ExpectedFiles)
		{
			std::set<std::filesystem::path> Expected;
			for (const std::filesystem::path& File : ExpectedFiles) Expected.insert(File.lexically_normal());
			std::error_code IterateError;
			for (std::filesystem::recursive_directory_iterator It(Stage, IterateError), End; It != End && !IterateError; It.increment(IterateError))
			{
				const auto Status = It->symlink_status(IterateError);
				if (IterateError || std::filesystem::is_symlink(Status)) return false;
				if (std::filesystem::is_regular_file(Status))
				{
					if (!Expected.contains(It->path().lexically_normal())) return false;
				}
				else if (!std::filesystem::is_directory(Status)) return false;
			}
			return !IterateError;
		}

		bool RemoveOwnedStage(const std::filesystem::path& Stage, const std::vector<std::filesystem::path>& Files,
			const std::vector<std::span<const std::byte>>& ExpectedBytes)
		{
			if (Files.size() != ExpectedBytes.size() || !StageContentsAreOwned(Stage, Files)) return false;
			for (size_t Index = 0; Index < Files.size(); ++Index)
			{
				std::error_code StatusError;
				const auto Status = std::filesystem::symlink_status(Files[Index], StatusError);
				if (StatusError == std::errc::no_such_file_or_directory) continue;
				if (StatusError || std::filesystem::is_symlink(Status) || !std::filesystem::is_regular_file(Status)) return false;
				auto Bytes = ReadBounded(Files[Index], CookedAssetCatalog::MaximumPayloadBytes);
				if (!Bytes || Bytes->size() != ExpectedBytes[Index].size() ||
					!std::equal(Bytes->begin(), Bytes->end(), ExpectedBytes[Index].begin())) return false;
			}
			std::error_code RemoveError;
			for (const std::filesystem::path& File : Files)
				if (!std::filesystem::remove(File, RemoveError) && RemoveError) return false;
			std::vector<std::filesystem::path> Directories;
			std::error_code IterateError;
			for (std::filesystem::recursive_directory_iterator It(Stage, IterateError), End; It != End && !IterateError; It.increment(IterateError))
				if (It->is_directory(IterateError) && !IterateError) Directories.push_back(It->path());
			if (IterateError) return false;
			std::sort(Directories.begin(), Directories.end(), [](const auto& A, const auto& B) { return A.native().size() > B.native().size(); });
			for (const auto& Directory : Directories)
				if (!std::filesystem::remove(Directory, RemoveError) && RemoveError) return false;
			return std::filesystem::remove(Stage, RemoveError) && !RemoveError;
		}

		struct ScopedCookStageCleanup final
		{
			const std::filesystem::path& Stage;
			const std::vector<std::filesystem::path>& Files;
			const std::vector<std::span<const std::byte>>& ExpectedBytes;
			bool Active = true;

			~ScopedCookStageCleanup()
			{
				if (Active) (void)RemoveOwnedStage(Stage, Files, ExpectedBytes);
			}

			bool Cleanup()
			{
				if (!RemoveOwnedStage(Stage, Files, ExpectedBytes)) return false;
				Active = false;
				return true;
			}
		};
	}

	std::expected<AssetCookPlan, AssetCookError> AssetCookService::Plan(const AssetCookRequest& Request)
	{
		ProjectDesc Description;
		AssetContentFingerprint ProjectFingerprint;
		uint64_t ProjectBytes = 0;
		if (auto Valid = ValidateRequest(Request, &Description, &ProjectFingerprint, &ProjectBytes); !Valid)
			return std::unexpected(Valid.error());
		if (!Description.StartScene && Request.StartupScene.IsNil())
			return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Cook requires an explicit startup scene", Request.SourceProjectFile));
		if (Request.SourceSnapshot->GetRegistry().GetAssetCount() == 0)
			return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Source dependency snapshot contains no managed assets"));
		if (auto ReadSet = AssetDependencyExtractor::ValidateInputReceipts(
			Request.SourceSnapshot->GetProjectRootPath(), Request.SourceSnapshot->GetInputReadSet()); !ReadSet)
			return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Source project changed after dependency analysis: " + ReadSet.error().Message,
				ReadSet.error().Path));
		if (auto Membership = ValidateRegistryMembership(*Request.SourceSnapshot); !Membership)
			return std::unexpected(Membership.error());

		AssetCookPlan Plan;
		Plan.Request = Request;
		std::sort(Plan.Request.DynamicRoots.begin(), Plan.Request.DynamicRoots.end());
		Plan.Request.DynamicRoots.erase(std::unique(Plan.Request.DynamicRoots.begin(), Plan.Request.DynamicRoots.end()),
			Plan.Request.DynamicRoots.end());
		Plan.ProjectID = Description.ID;
		Plan.ProjectFileFingerprint = ProjectFingerprint;
		Plan.ProjectFileBytes = ProjectBytes;
		Plan.SortedRoots = Plan.Request.DynamicRoots;
		Plan.SortedRoots.push_back(Request.StartupScene);
		std::sort(Plan.SortedRoots.begin(), Plan.SortedRoots.end());
		Plan.SortedRoots.erase(std::unique(Plan.SortedRoots.begin(), Plan.SortedRoots.end()), Plan.SortedRoots.end());

		struct Pending
		{
			CookedAssetDependency Product;
		};
		std::vector<Pending> Queue;
		for (const AssetID& Root : Plan.SortedRoots)
		{
			const auto Record = Request.SourceSnapshot->GetRegistry().Find(Root);
			if (!Record)
				return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Cook root is absent from the captured asset registry", {}, Root));
			const auto Type = GetCookAssetType(Record->ProjectRelativePath);
			if (!Type || !ProductForType(*Type))
				return std::unexpected(Error(AssetCookErrorCode::UnsupportedProduct, "Cook root has an unsupported asset type", Record->ProjectRelativePath, Root));
			if (Root == Request.StartupScene && *Type != AssetFileType::Scene)
				return std::unexpected(Error(AssetCookErrorCode::InvalidRoot, "Startup root must identify a scene asset", Record->ProjectRelativePath, Root));
			if (auto Path = AssetPathResolver::ResolveManagedSourcePath(Request.SourceSnapshot->GetProjectRootPath(), Record->ProjectRelativePath); !Path)
				return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed, Path.error().Message, Path.error().Path, Root));
			CookedAssetDependency Product{ Root, *ProductForType(*Type), {} };
			if (Product.Product == CookedAssetProductKind::MeshPrimitive) Product.Subresource = MeshDefaultSubresource;
			else if (Product.Product == CookedAssetProductKind::Texture) Product.Subresource = std::string(TextureSrgbSubresource);
			Queue.push_back({ std::move(Product) });
		}

		std::map<std::string, AssetCookPlannedProduct> Products;
		for (size_t Cursor = 0; Cursor < Queue.size(); ++Cursor)
		{
			const CookedAssetDependency Current = Queue[Cursor].Product;
			const std::string Key = ProductIdentityKey(Current);
			if (Products.contains(Key)) continue;
			const auto Record = Request.SourceSnapshot->GetRegistry().Find(Current.Asset);
			if (!Record) return std::unexpected(Error(AssetCookErrorCode::InvalidDependency, "Resolved cook dependency disappeared from its captured registry", {}, Current.Asset));
			const auto Type = GetCookAssetType(Record->ProjectRelativePath);
			const bool MaterialTextureInterpretation = Type && Current.Product == CookedAssetProductKind::Texture &&
				*Type == AssetFileType::Environment && (Current.Subresource == TextureSrgbSubresource || Current.Subresource == TextureLinearSubresource);
			if (!Type || (ProductForType(*Type) != Current.Product && !MaterialTextureInterpretation))
				return std::unexpected(Error(AssetCookErrorCode::InvalidDependency, "Dependency product type does not match its registered source type", Record->ProjectRelativePath, Current.Asset));
			auto SourcePath = AssetPathResolver::ResolveManagedSourcePath(Request.SourceSnapshot->GetProjectRootPath(), Record->ProjectRelativePath);
			if (!SourcePath) return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed, SourcePath.error().Message, SourcePath.error().Path, Current.Asset));
			AssetCookPlannedProduct Product{ Current.Asset, Current.Product, Current.Subresource, *SourcePath, {} };

			if (Current.Product == CookedAssetProductKind::MeshPrimitive)
			{
				const auto Subresource = std::find_if(Request.SourceSnapshot->GetSubresources().begin(), Request.SourceSnapshot->GetSubresources().end(),
					[&](const AssetSubresourceRecord& Candidate)
					{ return Candidate.Identity.Asset == Current.Asset && Candidate.Identity.SubresourceKey == Current.Subresource; });
				if (Subresource == Request.SourceSnapshot->GetSubresources().end() ||
					(Subresource->Classification != AssetSubresourceClassification::SupportedCandidate &&
					 Subresource->Classification != AssetSubresourceClassification::Imported))
					return std::unexpected(Error(AssetCookErrorCode::UnsupportedProduct,
						"Requested mesh subresource is not classified as a supported static output", *SourcePath, Current.Asset));
			}

			for (const AssetDependencyEdge& Edge : Request.SourceSnapshot->GetDirectDependencies({ Current.Asset, {}, {} }))
			{
				if (Edge.Kind != AssetDependencyEdgeKind::RuntimeReference) continue;
				if (Edge.Status != AssetDependencyStatus::Resolved || Edge.Target.Asset.IsNil())
					return std::unexpected(Error(AssetCookErrorCode::InvalidDependency,
						"Reachable runtime dependency is unresolved at " + Edge.Location + ": " + Edge.OriginalReference,
						Record->ProjectRelativePath, Edge.Target.Asset));
				const auto TargetRecord = Request.SourceSnapshot->GetRegistry().Find(Edge.Target.Asset);
				if (!TargetRecord)
					return std::unexpected(Error(AssetCookErrorCode::InvalidDependency,
						"Resolved runtime dependency is not registered at " + Edge.Location, Record->ProjectRelativePath, Edge.Target.Asset));
				const auto TargetType = GetCookAssetType(TargetRecord->ProjectRelativePath);
				if (!TargetType || !ProductForType(*TargetType))
					return std::unexpected(Error(AssetCookErrorCode::UnsupportedProduct,
						"Reachable runtime dependency has no cook adapter at " + Edge.Location,
						TargetRecord->ProjectRelativePath, Edge.Target.Asset));
				CookedAssetDependency Dependency = ProductDependencyForEdge(Edge, *TargetType);
				Product.RuntimeDependencies.push_back(Dependency);
				Queue.push_back({ std::move(Dependency) });
			}
			std::sort(Product.RuntimeDependencies.begin(), Product.RuntimeDependencies.end(), [](const auto& A, const auto& B)
				{ return std::tie(A.Asset, A.Product, A.Subresource) < std::tie(B.Asset, B.Product, B.Subresource); });
			Product.RuntimeDependencies.erase(std::unique(Product.RuntimeDependencies.begin(), Product.RuntimeDependencies.end()),
				Product.RuntimeDependencies.end());
			Products.emplace(Key, std::move(Product));
			if (Current.Product == CookedAssetProductKind::MeshPrimitive)
			{
				for (const AssetSubresourceRecord& Candidate : Request.SourceSnapshot->GetSubresources())
				{
					if (Candidate.Identity.Asset != Current.Asset || Candidate.Identity.SubresourceKey == Current.Subresource ||
						(Candidate.Classification != AssetSubresourceClassification::SupportedCandidate &&
							Candidate.Classification != AssetSubresourceClassification::Imported))
						continue;
					Queue.push_back({ CookedAssetDependency{
						Candidate.Identity.Asset, CookedAssetProductKind::MeshPrimitive, Candidate.Identity.SubresourceKey } });
				}
			}
		}

		for (auto& [Key, Product] : Products)
		{
			(void)Key;
			Plan.Products.push_back(std::move(Product));
		}
		const std::filesystem::path InputActionsPath = Request.SourceSnapshot->GetProjectRootPath() / "InputActions.json";
		std::error_code InputExistsError;
		const bool HasInputActions = std::filesystem::exists(InputActionsPath, InputExistsError);
		if (InputExistsError)
			return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed, "Could not inspect project gameplay input defaults: " + InputExistsError.message(), InputActionsPath));
		if (HasInputActions)
		{
			Plan.GameplayInputPresent = true;
			auto InputBytes = ReadBounded(InputActionsPath, MaximumInputMapBytes);
			if (!InputBytes) return std::unexpected(InputBytes.error());
			const std::string InputText(InputBytes->empty() ? "" : reinterpret_cast<const char*>(InputBytes->data()), InputBytes->size());
			auto Input = ParseGameplayActionMap(InputText);
			if (!Input) return std::unexpected(Error(AssetCookErrorCode::SourceReadFailed, Input.error().Message, InputActionsPath));
			Plan.GameplayInputFingerprint = ComputeAssetContentFingerprint(*InputBytes);
			Plan.GameplayInputBytes = InputBytes->size();
			AssetCookPlannedProduct InputProduct;
			InputProduct.Asset = Plan.ProjectID;
			InputProduct.Product = CookedAssetProductKind::GameplayInput;
			InputProduct.SourcePath = InputActionsPath;
			Plan.Products.push_back(std::move(InputProduct));
		}
		std::sort(Plan.Products.begin(), Plan.Products.end(), [](const auto& A, const auto& B)
			{ return std::tie(A.Asset, A.Product, A.Subresource) < std::tie(B.Asset, B.Product, B.Subresource); });
		return Plan;
	}

	std::expected<AssetCookReport, AssetCookError> AssetCookService::Cook(const AssetCookRequest& Request)
	{
		auto Planned = Plan(Request);
		if (!Planned) return std::unexpected(Planned.error());
		return Cook(*Planned);
	}

	std::expected<AssetCookReport, AssetCookError> AssetCookService::Cook(const AssetCookPlan& Plan)
	{
		auto Fresh = AssetCookService::Plan(Plan.Request);
		if (!Fresh) return std::unexpected(Fresh.error());
		if (!SameProducts(Plan, *Fresh))
			return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Cook plan no longer matches its retained source project and dependency snapshot"));
		const auto& Request = Plan.Request;
		std::error_code DirectoryError;
		std::filesystem::create_directories(Request.OutputRoot, DirectoryError);
		if (DirectoryError) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not create cook output root: " + DirectoryError.message(), Request.OutputRoot));
		auto OutputAbsolute = AbsoluteNormalized(Request.OutputRoot);
		if (!OutputAbsolute) return std::unexpected(OutputAbsolute.error());
		if (auto Links = RejectLinkedAncestors(*OutputAbsolute); !Links) return std::unexpected(Links.error());
		const std::filesystem::path LockPath = *OutputAbsolute / ".pf-cook-lock";
		if (!std::filesystem::create_directory(LockPath, DirectoryError))
			return std::unexpected(Error(AssetCookErrorCode::OutputConflict,
				DirectoryError ? "Could not acquire cook output ownership: " + DirectoryError.message()
					: "Another cook owns this destination or an interrupted cook lock requires inspection", LockPath));
		struct LockGuard
		{
			std::filesystem::path Path;
			~LockGuard() { std::error_code Ignored; std::filesystem::remove(Path, Ignored); }
		} Lock{ LockPath };
		const std::filesystem::path ManifestPath = *OutputAbsolute / CookedAssetCatalog::ManifestFileName;
		std::optional<AssetContentFingerprint> ExistingManifestFingerprint;
		std::shared_ptr<const CookedAssetCatalog> PreviousCatalog;
		std::error_code ManifestStatusError;
		const auto ManifestStatus = std::filesystem::symlink_status(ManifestPath, ManifestStatusError);
		if (ManifestStatusError != std::errc::no_such_file_or_directory && ManifestStatusError)
			return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Could not inspect existing cook manifest: " + ManifestStatusError.message(), ManifestPath));
		if (!ManifestStatusError && std::filesystem::exists(ManifestStatus))
		{
			if (std::filesystem::is_symlink(ManifestStatus) || !std::filesystem::is_regular_file(ManifestStatus))
				return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Existing cook manifest is not a regular file", ManifestPath));
			auto Existing = CookedAssetCatalog::Open(*OutputAbsolute, Request.Compatibility);
			if (!Existing || (*Existing)->GetProjectID() != Plan.ProjectID)
				return std::unexpected(Error(AssetCookErrorCode::OutputConflict,
					"Existing output is not a valid cooked catalog for this project and compatibility; it was left untouched", ManifestPath));
			auto ExistingBytes = ReadBounded(ManifestPath, CookedAssetCatalog::MaximumManifestBytes);
			if (!ExistingBytes) return std::unexpected(ExistingBytes.error());
			ExistingManifestFingerprint = ComputeAssetContentFingerprint(*ExistingBytes);
			PreviousCatalog = std::move(*Existing);
		}

		std::vector<std::vector<std::byte>> Payloads;
		std::vector<CookedAssetEntry> Entries;
		Payloads.reserve(Plan.Products.size());
		Entries.reserve(Plan.Products.size());
		for (const AssetCookPlannedProduct& Product : Plan.Products)
		{
			std::vector<std::byte> Bytes;
			uint32_t Version = 1;
			if (Product.Product == CookedAssetProductKind::MeshPrimitive)
			{
				uint32_t MeshIndex = 0, PrimitiveIndex = 0;
				if (std::sscanf(Product.Subresource.c_str(), "mesh/%u/primitive/%u", &MeshIndex, &PrimitiveIndex) != 2)
					return std::unexpected(Error(AssetCookErrorCode::UnsupportedProduct, "Unsupported source-local mesh locator", Product.SourcePath, Product.Asset));
				auto Imported = GltfMeshImporter::ProcessStaticPrimitive(Product.Asset, Request.SourceSnapshot->GetProjectRootPath(),
					Request.SourceSnapshot->GetRegistry(), MeshIndex, PrimitiveIndex);
				if (!Imported) return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, Imported.error().Message, Imported.error().Path, Product.Asset));
				auto Encoded = Detail::EncodeMeshProduct(Imported->Mesh);
				if (!Encoded) return std::unexpected(Error(AssetCookErrorCode::SerializationFailed, Encoded.error(), Product.SourcePath, Product.Asset));
				Bytes = std::move(*Encoded);
				Version = 1;
			}
			else if (Product.Product == CookedAssetProductKind::Texture)
			{
				const bool Srgb = Product.Subresource != TextureLinearSubresource;
				const auto ColorSpace = Srgb ? Detail::ImageColorSpace::Srgb : Detail::ImageColorSpace::Linear;
				auto Imported = ImageAssetImporter::ImportRGBA8(Product.Asset, Request.SourceSnapshot->GetProjectRootPath(),
					Request.SourceSnapshot->GetRegistry());
				if (!Imported) return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, Imported.error().Message, Imported.error().Path, Product.Asset));
				auto Mips = Detail::BuildTextureMipChain(Imported->Width, Imported->Height, Imported->RGBA8Pixels, ColorSpace);
				Detail::PreparedTextureCpuProduct Texture{ Imported->Width, Imported->Height, ColorSpace, std::move(Mips) };
				auto Encoded = Detail::EncodeTextureProduct(Texture);
				if (!Encoded) return std::unexpected(Error(AssetCookErrorCode::SerializationFailed, Encoded.error(), Product.SourcePath, Product.Asset));
				Bytes = std::move(*Encoded);
			}
			else if (Product.Product == CookedAssetProductKind::Environment)
			{
				const EnvironmentProcessingDesc Processing{};
				auto Prepared = Detail::PrepareEnvironmentCpuProduct(Product.Asset,
					Request.SourceSnapshot->GetProjectRootPath(), Request.SourceSnapshot->GetRegistry(), nullptr, Processing);
				if (!Prepared)
					return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed,
						Prepared.error().Message, Product.SourcePath, Product.Asset));
				auto Environment = Detail::EnvironmentLightingArtifactCodec::EncodeEnvironment(Prepared->Data, Processing);
				auto Brdf = Detail::EnvironmentLightingArtifactCodec::EncodeBrdfLut(
					Prepared->Data.BrdfIntegrationLut, Processing.BrdfSamples);
				if (!Environment || !Brdf)
					return std::unexpected(Error(AssetCookErrorCode::SerializationFailed,
						!Environment ? Environment.error() : Brdf.error(), Product.SourcePath, Product.Asset));
				if (Environment->size() > (std::numeric_limits<uint32_t>::max)() ||
					Brdf->size() > (std::numeric_limits<uint32_t>::max)())
					return std::unexpected(Error(AssetCookErrorCode::SerializationFailed,
						"Processed environment payload exceeds its bounded envelope", Product.SourcePath, Product.Asset));
				// Portable v1 envelope: ASCII magic, little-endian environment and BRDF byte lengths, then both
				// independently versioned existing CPU products. No renderer objects or source paths are serialized.
				static constexpr std::array<uint8_t, 8> Magic{ 'P', 'F', 'E', 'N', 'V', 0, 0, 1 };
				for (const uint8_t Byte : Magic) Bytes.push_back(static_cast<std::byte>(Byte));
				auto AppendU32 = [&Bytes](uint32_t Value)
				{
					for (size_t I = 0; I < 4; ++I)
						Bytes.push_back(static_cast<std::byte>((Value >> (I * 8)) & 0xffu));
				};
			AppendU32(static_cast<uint32_t>(Environment->size()));
			AppendU32(static_cast<uint32_t>(Brdf->size()));
			Bytes.insert(Bytes.end(), Environment->begin(), Environment->end());
			Bytes.insert(Bytes.end(), Brdf->begin(), Brdf->end());
			}
			else
			{
				auto Read = ReadBounded(Product.SourcePath, MaximumAuthoredPayloadBytes, Product.Asset);
				if (!Read) return std::unexpected(Read.error());
				Bytes = std::move(*Read);
				if (Product.Product == CookedAssetProductKind::Scene)
				{
					Scene SceneValue;
					const std::string Text(Bytes.empty() ? "" : reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
					auto Valid = SceneSerializer::Deserialize(Text, SceneValue);
					if (!Valid) return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, Valid.error().Message, Product.SourcePath, Product.Asset));
				}
				else if (Product.Product == CookedAssetProductKind::Prefab)
				{
					Scene ValidationScene;
					const std::string Text(Bytes.empty() ? "" : reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
					auto Valid = PrefabSerializer::Instantiate(Text, ValidationScene);
					if (!Valid) return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, Valid.error().Message, Product.SourcePath, Product.Asset));
				}
				else if (Product.Product == CookedAssetProductKind::Material)
				{
					const std::string Text(Bytes.empty() ? "" : reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
					auto Material = MaterialAssetSerializer::Deserialize(Text);
					if (!Material) return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, Material.error().Message, Product.SourcePath, Product.Asset));
					CookedMaterialProduct Cooked;
					Cooked.BaseColorTexture = Material->BaseColorTexture;
					Cooked.MetallicRoughnessTexture = Material->MetallicRoughnessTexture;
					Cooked.NormalTexture = Material->NormalTexture;
					Cooked.OcclusionTexture = Material->OcclusionTexture;
					Cooked.EmissiveTexture = Material->EmissiveTexture;
					for (size_t Component = 0; Component < Cooked.BaseColorFactor.size(); ++Component)
						Cooked.BaseColorFactor[Component] = Material->BaseColorFactor[static_cast<int>(Component)];
					Cooked.MetallicFactor = Material->MetallicFactor;
					Cooked.RoughnessFactor = Material->RoughnessFactor;
					Cooked.NormalScale = Material->NormalScale;
					Cooked.OcclusionStrength = Material->OcclusionStrength;
					for (size_t Component = 0; Component < Cooked.EmissiveFactor.size(); ++Component)
						Cooked.EmissiveFactor[Component] = Material->EmissiveFactor[static_cast<int>(Component)];
					Cooked.AlphaMode = static_cast<CookedMaterialAlphaMode>(Material->AlphaMode);
					Cooked.AlphaCutoff = Material->AlphaCutoff;
					Cooked.DoubleSided = Material->DoubleSided;
					auto Encoded = Detail::EncodeMaterialProduct(Cooked);
					if (!Encoded) return std::unexpected(Error(AssetCookErrorCode::SerializationFailed, Encoded.error(), Product.SourcePath, Product.Asset));
					Bytes = std::move(*Encoded);
					Version = Detail::MaterialProductSchemaVersion;
				}
				else if (Product.Product == CookedAssetProductKind::Audio)
				{
					if (Bytes.empty()) return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, "Audio source is empty", Product.SourcePath, Product.Asset));
				}
				else if (Product.Product == CookedAssetProductKind::GameplayInput)
				{
					if (!Plan.GameplayInputFingerprint || Bytes.size() != Plan.GameplayInputBytes ||
						ComputeAssetContentFingerprint(Bytes) != *Plan.GameplayInputFingerprint)
						return std::unexpected(Error(AssetCookErrorCode::SourceChanged, "Gameplay input defaults changed after planning", Product.SourcePath));
					const std::string InputText(Bytes.empty() ? "" : reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
					auto Parsed = ParseGameplayActionMap(InputText);
					if (!Parsed)
						return std::unexpected(Error(AssetCookErrorCode::ProductProcessingFailed, Parsed.error().Message, Product.SourcePath, Product.Asset));
				}
			}
			if (Bytes.size() > CookedAssetCatalog::MaximumPayloadBytes)
				return std::unexpected(Error(AssetCookErrorCode::SerializationFailed, "Cooked product exceeds the runtime payload limit", Product.SourcePath, Product.Asset));
			const CookedAssetDigest Digest = CookedAssetCatalog::ComputePayloadDigest(Bytes);
			CookedAssetEntry Entry;
			Entry.Asset = Product.Asset;
			Entry.Product = Product.Product;
			Entry.Subresource = Product.Subresource;
			Entry.PayloadVersion = Version;
			Entry.PayloadBytes = Bytes.size();
			Entry.DigestAlgorithm = Digest.Algorithm;
			Entry.Digest = Digest.Value;
			Entry.RuntimeDependencies = Product.RuntimeDependencies;
			Payloads.push_back(std::move(Bytes));
			Entries.push_back(std::move(Entry));
		}

		if (auto Freshness = ValidateCookSourceFresh(Plan); !Freshness)
			return std::unexpected(Freshness.error());

		AssetFingerprintBuilder GenerationBuilder("pulseforge-cooked-generation-v1");
		GenerationBuilder.UpdateText("project", Plan.ProjectID.ToString());
		GenerationBuilder.UpdateText("target", Request.Compatibility.TargetPlatform);
		GenerationBuilder.UpdateText("backend", Request.Compatibility.RendererBackend);
		GenerationBuilder.UpdateText("abi", Request.Compatibility.RuntimeABI);
		for (const AssetID& Root : Plan.SortedRoots) GenerationBuilder.UpdateText("root", Root.ToString());
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			GenerationBuilder.UpdateText("asset", Entries[Index].Asset.ToString());
			GenerationBuilder.UpdateText("product", std::to_string(static_cast<uint32_t>(Entries[Index].Product)));
			GenerationBuilder.UpdateText("subresource", Entries[Index].Subresource);
			GenerationBuilder.UpdateText("digest", Entries[Index].DigestAlgorithm + ":" + Entries[Index].Digest);
			for (const CookedAssetDependency& Dependency : Entries[Index].RuntimeDependencies)
				GenerationBuilder.UpdateText("dependency", ProductIdentityKey(Dependency));
		}
		const std::string Generation = GenerationBuilder.Finish().DigestHex;
		for (CookedAssetEntry& Entry : Entries) Entry.RelativePayloadPath = ProductPath(Entry, Generation);
		auto Manifest = CookedAssetCatalog::SerializeManifest(Plan.ProjectID, Request.StartupScene,
				Request.Compatibility, Plan.Request.DynamicRoots, Entries);
		if (!Manifest) return std::unexpected(Error(AssetCookErrorCode::SerializationFailed, Manifest.error().Message, Manifest.error().Path));

		auto StageId = UUID::Generate();
		if (!StageId) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not allocate a unique cook staging identity"));
		const std::filesystem::path Stage = *OutputAbsolute / (".pf-cs-" + StageId->ToString());
		if (!std::filesystem::create_directory(Stage, DirectoryError) || DirectoryError)
			return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not create unique cook staging directory: " + DirectoryError.message(), Stage));
		std::vector<std::filesystem::path> StagedFiles;
		StagedFiles.reserve(Payloads.size() + 1);
		std::vector<std::span<const std::byte>> StagedExpectedBytes;
		StagedExpectedBytes.reserve(Payloads.size() + 1);
		ScopedCookStageCleanup StageCleanup{ Stage, StagedFiles, StagedExpectedBytes };
		for (size_t Index = 0; Index < Entries.size(); ++Index)
		{
			const std::filesystem::path Relative = std::filesystem::u8path(Entries[Index].RelativePayloadPath);
			const std::filesystem::path Destination = Stage / "p" / Relative;
			if (!IsSameOrWithin(Destination.lexically_normal(), Stage.lexically_normal()))
				return std::unexpected(Error(AssetCookErrorCode::SerializationFailed, "Cooked payload path escapes its staging generation", Destination));
			StagedFiles.push_back(Destination);
			StagedExpectedBytes.emplace_back(Payloads[Index]);
			if (auto Written = WriteFile(Destination, Payloads[Index]); !Written) return std::unexpected(Written.error());
		}
		if (auto Freshness = ValidateCookSourceFresh(Plan); !Freshness)
			return std::unexpected(Freshness.error());
		const std::filesystem::path GenerationDirectory = *OutputAbsolute / "g" / Generation;
		if (auto Links = RejectLinkedAncestors(GenerationDirectory); !Links) return std::unexpected(Links.error());
		std::filesystem::create_directories(GenerationDirectory.parent_path(), DirectoryError);
		if (DirectoryError) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not create generation directory: " + DirectoryError.message(), GenerationDirectory.parent_path()));
		std::error_code ExistsError;
		const bool GenerationExists = std::filesystem::exists(GenerationDirectory, ExistsError);
		if (ExistsError) return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Could not inspect existing cooked generation: " + ExistsError.message(), GenerationDirectory));
		if (GenerationExists)
		{
			// Deterministic repeat: reuse byte-identical payloads. A missing payload may be restored from
			// this cook's owned staging tree, but a present or unobservable entry is never overwritten.
			for (size_t Index = 0; Index < Entries.size(); ++Index)
			{
				const std::filesystem::path Existing = *OutputAbsolute / std::filesystem::u8path(Entries[Index].RelativePayloadPath);
				std::error_code ExistingStatusError;
				const auto ExistingStatus = std::filesystem::symlink_status(Existing, ExistingStatusError);
				if (ExistingStatusError == std::errc::no_such_file_or_directory)
				{
					const std::filesystem::path Staged = Stage / "p" / std::filesystem::u8path(Entries[Index].RelativePayloadPath);
					if (auto Links = RejectLinkedAncestors(Existing); !Links)
						return std::unexpected(Error(AssetCookErrorCode::OutputConflict, Links.error().Message, Existing, Entries[Index].Asset));
					std::error_code ParentError;
					std::filesystem::create_directories(Existing.parent_path(), ParentError);
					if (ParentError)
						return std::unexpected(Error(AssetCookErrorCode::StagingFailed,
							"Could not restore a missing deterministic payload parent: " + ParentError.message(), Existing.parent_path(), Entries[Index].Asset));
					if (auto Links = RejectLinkedAncestors(Existing); !Links)
						return std::unexpected(Error(AssetCookErrorCode::OutputConflict, Links.error().Message, Existing, Entries[Index].Asset));
					std::error_code LinkError;
					std::filesystem::create_hard_link(Staged, Existing, LinkError);
					if (LinkError)
						return std::unexpected(Error(AssetCookErrorCode::OutputConflict,
							"Could not restore a missing deterministic payload without replacing an existing file: " + LinkError.message(), Existing, Entries[Index].Asset));
					continue;
				}
				if (ExistingStatusError || std::filesystem::is_symlink(ExistingStatus) || !std::filesystem::is_regular_file(ExistingStatus))
					return std::unexpected(Error(AssetCookErrorCode::OutputConflict,
						"An existing generation payload is not a safely readable regular file", Existing, Entries[Index].Asset));
				auto ExistingBytes = ReadBounded(Existing, CookedAssetCatalog::MaximumPayloadBytes, Entries[Index].Asset);
				if (!ExistingBytes || *ExistingBytes != Payloads[Index])
					return std::unexpected(Error(AssetCookErrorCode::OutputConflict,
						"An existing generation identity has different or unreadable payload bytes", Existing, Entries[Index].Asset));
			}
		}
		else
		{
			std::filesystem::path StagedGeneration = Stage / "p" / "g" / Generation;
			std::filesystem::create_directories(GenerationDirectory.parent_path(), DirectoryError);
			if (DirectoryError) return std::unexpected(Error(AssetCookErrorCode::StagingFailed, "Could not prepare generation parent", GenerationDirectory.parent_path()));
			std::filesystem::rename(StagedGeneration, GenerationDirectory, DirectoryError);
			if (DirectoryError) return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Could not publish complete cooked generation: " + DirectoryError.message(), GenerationDirectory));
		}

		const std::filesystem::path ManifestTemporary = Stage / "manifest.new";
		const auto ManifestBytes = std::as_bytes(std::span(Manifest->data(), Manifest->size()));
		StagedFiles.push_back(ManifestTemporary);
		StagedExpectedBytes.emplace_back(ManifestBytes);
		if (auto Written = WriteFile(ManifestTemporary, ManifestBytes); !Written) return std::unexpected(Written.error());
		if (auto Freshness = ValidateCookSourceFresh(Plan); !Freshness)
			return std::unexpected(Freshness.error());
		std::error_code ManifestCheckError;
		const auto CurrentManifestStatus = std::filesystem::symlink_status(ManifestPath, ManifestCheckError);
		if (ManifestCheckError != std::errc::no_such_file_or_directory && ManifestCheckError)
			return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Could not revalidate existing manifest before publication", ManifestPath));
		const bool ManifestExists = !ManifestCheckError && std::filesystem::exists(CurrentManifestStatus);
		if (ManifestExists != ExistingManifestFingerprint.has_value())
			return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Cook manifest appeared or disappeared during generation", ManifestPath));
		if (ManifestExists)
		{
			auto CurrentBytes = ReadBounded(ManifestPath, CookedAssetCatalog::MaximumManifestBytes);
			if (!CurrentBytes || ComputeAssetContentFingerprint(*CurrentBytes) != *ExistingManifestFingerprint)
				return std::unexpected(Error(AssetCookErrorCode::OutputConflict, "Cook manifest was independently modified during generation", ManifestPath));
		}
		if (auto Replaced = Platform::ReplaceFileAtomically(ManifestTemporary, ManifestPath); !Replaced)
			return std::unexpected(Error(AssetCookErrorCode::PublicationFailed, "Cooked generation exists, but manifest publication failed: " + Replaced.error().Message, ManifestPath));
		uint64_t PayloadBytes = 0;
		for (const CookedAssetEntry& Entry : Entries) PayloadBytes += Entry.PayloadBytes;
		std::error_code CleanupError;
		const bool Cleaned = StageCleanup.Cleanup();
		AssetCookReport Report{ ManifestPath, Generation, PayloadBytes, Entries.size() };
		Report.StagingCleanupPending = !Cleaned;
		if (!Cleaned) Report.RetainedStagingPath = Stage;
		if (PreviousCatalog)
		{
			std::set<std::string> CurrentPayloads;
			for (const CookedAssetEntry& Entry : Entries) CurrentPayloads.insert(Entry.RelativePayloadPath);
			for (const CookedAssetEntry& OldEntry : PreviousCatalog->GetEntries())
			{
				if (CurrentPayloads.contains(OldEntry.RelativePayloadPath)) continue;
				const std::filesystem::path ObsoletePath = *OutputAbsolute / std::filesystem::u8path(OldEntry.RelativePayloadPath);
				std::error_code StatusError;
				const auto Status = std::filesystem::symlink_status(ObsoletePath, StatusError);
				const auto SafePath = RejectLinkedAncestors(ObsoletePath);
				if (StatusError == std::errc::no_such_file_or_directory) continue;
				bool Removed = false;
				if (!StatusError && SafePath && std::filesystem::is_regular_file(Status) && !std::filesystem::is_symlink(Status))
				{
					auto ObsoleteBytes = ReadBounded(ObsoletePath, CookedAssetCatalog::MaximumPayloadBytes, OldEntry.Asset);
					if (ObsoleteBytes)
					{
						const CookedAssetDigest ActualDigest = CookedAssetCatalog::ComputePayloadDigest(*ObsoleteBytes);
						if (ActualDigest.Algorithm == OldEntry.DigestAlgorithm && ActualDigest.Value == OldEntry.Digest)
							Removed = std::filesystem::remove(ObsoletePath, StatusError) && !StatusError;
					}
				}
				if (!Removed) Report.RetainedStalePayloads.push_back(ObsoletePath);
			}
		}
		return Report;
	}
} // namespace PulseForge
