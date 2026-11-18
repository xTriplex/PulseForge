#include "Assets/GltfMeshImportCoordinator.h"

#include "Assets/AssetMetadata.h"
#include "Assets/AssetPathResolver.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>

namespace PulseForge
{
	namespace
	{
		std::shared_ptr<std::mutex> GetReceiptPathMutex(const std::filesystem::path& Path)
		{
			static std::mutex RegistryMutex;
			static std::map<std::filesystem::path, std::weak_ptr<std::mutex>> PathMutexes;
			std::lock_guard RegistryLock(RegistryMutex);
			for (auto It = PathMutexes.begin(); It != PathMutexes.end();)
				if (It->second.expired()) It = PathMutexes.erase(It);
				else ++It;

			auto& WeakMutex = PathMutexes[Path];
			if (auto Existing = WeakMutex.lock()) return Existing;
			auto Created = std::make_shared<std::mutex>();
			WeakMutex = Created;
			return Created;
		}

		std::expected<std::array<uint32_t, 3>, std::string> ParseSkeletalSourceLocator(std::string_view Locator)
		{
			constexpr std::string_view Prefix = "skeletal/node/";
			if (!Locator.starts_with(Prefix)) return std::unexpected("Skeletal source locator is invalid");
			Locator.remove_prefix(Prefix.size());
			const size_t MeshMarker = Locator.find("/mesh/");
			const size_t PrimitiveMarker = MeshMarker == std::string_view::npos
				? std::string_view::npos : Locator.find("/primitive/", MeshMarker + 6);
			if (MeshMarker == std::string_view::npos || PrimitiveMarker == std::string_view::npos)
				return std::unexpected("Skeletal source locator is incomplete");
			const std::array<std::string_view, 3> Parts = {
				Locator.substr(0, MeshMarker),
				Locator.substr(MeshMarker + 6, PrimitiveMarker - (MeshMarker + 6)),
				Locator.substr(PrimitiveMarker + std::string_view("/primitive/").size())
			};
			std::array<uint32_t, 3> Values{};
			for (size_t Index = 0; Index < Parts.size(); ++Index)
			{
				uint64_t Value = 0;
				const auto [End, Error] = std::from_chars(Parts[Index].data(), Parts[Index].data() + Parts[Index].size(), Value);
				if (Error != std::errc{} || End != Parts[Index].data() + Parts[Index].size() ||
					Value > std::numeric_limits<uint32_t>::max())
					return std::unexpected("Skeletal source locator index is invalid or out of range");
				Values[Index] = static_cast<uint32_t>(Value);
			}
			return Values;
		}
	}

	std::expected<void, GltfMeshImportCoordinatorError> GltfMeshImportCoordinator::RecordSuccess(
		const AssetID& Asset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const AssetImportSuccessReceipt& Receipt)
	{
		const auto Record = Registry.Find(Asset);
		if (!Record)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ImportFailed, ProjectRoot,
				"Asset disappeared from the registry after CPU processing", std::nullopt, std::nullopt
			});
		std::error_code AbsoluteError;
		const std::filesystem::path AbsoluteRoot = std::filesystem::absolute(ProjectRoot, AbsoluteError).lexically_normal();
		if (AbsoluteError)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				ProjectRoot,
				"CPU import succeeded, but the project root could not be resolved for receipt persistence: " + AbsoluteError.message(),
				std::nullopt,
				std::nullopt
			});
		auto Source = AssetPathResolver::ResolveManagedSourcePath(AbsoluteRoot, Record->ProjectRelativePath);
		if (!Source)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				Source.error().Path,
				"CPU import succeeded, but its receipt source path could not be resolved: " + Source.error().Message,
				std::nullopt,
				std::nullopt
			});

		const std::filesystem::path ReceiptPath = AssetImportProvenanceSerializer::GetSidecarPath(*Source).lexically_normal();
		std::error_code ReceiptPathError;
		const std::filesystem::path AbsoluteReceiptPath = std::filesystem::absolute(ReceiptPath, ReceiptPathError).lexically_normal();
		if (ReceiptPathError)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				ReceiptPath,
				"CPU import succeeded, but its receipt path could not be normalized: " + ReceiptPathError.message(),
				std::nullopt,
				std::nullopt
			});

		// CPU work remains concurrent; only read-modify-write of this source's sidecar is serialized.
		const std::shared_ptr<std::mutex> PathMutex = GetReceiptPathMutex(AbsoluteReceiptPath);
		std::lock_guard ReceiptLock(*PathMutex);
		auto Receipts = AssetImportProvenanceSerializer::LoadAll(AbsoluteReceiptPath);
		if (!Receipts)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				AbsoluteReceiptPath,
				"CPU import succeeded, but existing provenance could not be read safely: " + Receipts.error().Message,
				std::nullopt,
				std::move(Receipts.error())
			});
		if (std::any_of(Receipts->begin(), Receipts->end(), [&Asset](const AssetImportSuccessReceipt& Existing)
			{ return Existing.Recipe.SourceAsset != Asset; }))
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				AbsoluteReceiptPath,
				"CPU import succeeded, but the existing receipt belongs to a different asset UUID",
				std::nullopt,
				std::nullopt
			});

		const auto ExistingOutput = std::lower_bound(Receipts->begin(), Receipts->end(), Receipt.Recipe.SubresourceKey,
			[](const AssetImportSuccessReceipt& Existing, std::string_view Key)
			{ return Existing.Recipe.SubresourceKey < Key; });
		if (ExistingOutput != Receipts->end() && ExistingOutput->Recipe.SubresourceKey == Receipt.Recipe.SubresourceKey)
			*ExistingOutput = Receipt;
		else Receipts->insert(ExistingOutput, Receipt);
		if (auto Saved = AssetImportProvenanceSerializer::SaveAll(AbsoluteReceiptPath, *Receipts); !Saved)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ReceiptPersistenceFailed,
				AbsoluteReceiptPath,
				"CPU import succeeded, but successful provenance was not persisted: " + Saved.error().Message,
				std::nullopt,
				std::move(Saved.error())
			});
		return {};
	}

	std::expected<GltfMeshImportWithReceipt, GltfMeshImportCoordinatorError>
		GltfMeshImportCoordinator::ProcessAndRecordStaticPrimitive(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			uint32_t MeshIndex,
			uint32_t PrimitiveIndex)
	{
		auto Product = GltfMeshImporter::ProcessStaticPrimitive(Asset, ProjectRoot, Registry, MeshIndex, PrimitiveIndex);
		if (!Product)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ImportFailed,
				Product.error().Path,
				"CPU mesh import failed: " + Product.error().Message,
				std::move(Product.error()),
				std::nullopt
			});

		const AssetImportSuccessReceipt Receipt{ Product->Recipe, Product->ResultFingerprint, Product->Recipe.SubresourceKey };
		if (auto Recorded = RecordSuccess(Asset, ProjectRoot, Registry, Receipt); !Recorded)
			return std::unexpected(std::move(Recorded.error()));
		return GltfMeshImportWithReceipt{ std::move(Product->Mesh), Receipt };
	}

	std::expected<GltfSkeletalMeshImportWithReceipt, GltfMeshImportCoordinatorError>
		GltfMeshImportCoordinator::ProcessAndRecordSkeletalPrimitive(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			uint32_t NodeIndex,
			uint32_t MeshIndex,
			uint32_t PrimitiveIndex)
	{
		auto Product = GltfMeshImporter::ProcessSkeletalPrimitive(
			Asset, ProjectRoot, Registry, NodeIndex, MeshIndex, PrimitiveIndex);
		if (!Product)
			return std::unexpected(GltfMeshImportCoordinatorError{
				GltfMeshImportCoordinatorErrorCode::ImportFailed,
				Product.error().Path,
				"CPU skeletal mesh import failed: " + Product.error().Message,
				std::move(Product.error()),
				std::nullopt
			});

		const AssetImportSuccessReceipt Receipt{ Product->Recipe, Product->ResultFingerprint, Product->Recipe.SubresourceKey };
		if (auto Recorded = RecordSuccess(Asset, ProjectRoot, Registry, Receipt); !Recorded)
			return std::unexpected(std::move(Recorded.error()));
		return GltfSkeletalMeshImportWithReceipt{ std::move(Product->Mesh), Receipt };
	}

	std::expected<GltfSkeletalMeshImportWithReceipt, GltfMeshImportCoordinatorError>
		GltfMeshImportCoordinator::LoadLastSuccessfulSkeletalProduct(
			const AssetID& Asset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry,
			std::string_view StableProductKey)
	{
		const auto Record = Registry.Find(Asset);
		if (!Record)
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				ProjectRoot, "Skeletal source asset is no longer registered", std::nullopt, std::nullopt });
		auto SourcePath = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
		if (!SourcePath)
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				SourcePath.error().Path, SourcePath.error().Message, std::nullopt, std::nullopt });
		const auto Receipts = AssetImportProvenanceSerializer::LoadAll(
			AssetImportProvenanceSerializer::GetSidecarPath(*SourcePath));
		if (!Receipts)
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				Receipts.error().Path, "Could not read successful skeletal import receipt: " + Receipts.error().Message,
				std::nullopt, std::move(Receipts.error()) });
		const auto Receipt = std::find_if(Receipts->begin(), Receipts->end(), [&](const AssetImportSuccessReceipt& Candidate)
		{
			return Candidate.Recipe.SourceAsset == Asset && Candidate.Recipe.SubresourceKey == StableProductKey &&
				Candidate.Recipe.ImporterID == "PulseForge.SkeletalGltfMeshImporter";
		});
		if (Receipt == Receipts->end())
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				*SourcePath, "No successful skeletal import receipt matches the stable product key", std::nullopt, std::nullopt });
		auto Locator = ParseSkeletalSourceLocator(Receipt->Recipe.SourceLocator);
		if (!Locator)
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				*SourcePath, Locator.error(), std::nullopt, std::nullopt });
		auto Product = GltfMeshImporter::ProcessSkeletalPrimitive(
			Asset, ProjectRoot, Registry, (*Locator)[0], (*Locator)[1], (*Locator)[2]);
		if (!Product)
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				Product.error().Path, "Could not rebuild the last successful skeletal product: " + Product.error().Message,
				std::move(Product.error()), std::nullopt });
		if (Product->Recipe.SubresourceKey != StableProductKey ||
			Product->Recipe.InputFingerprint != Receipt->Recipe.InputFingerprint ||
			Product->ResultFingerprint != Receipt->ResultFingerprint)
			return std::unexpected(GltfMeshImportCoordinatorError{ GltfMeshImportCoordinatorErrorCode::ImportFailed,
				*SourcePath, "Skeletal source no longer matches the last successful imported product; reimport is required",
				std::nullopt, std::nullopt });
		return GltfSkeletalMeshImportWithReceipt{ std::move(Product->Mesh), *Receipt };
	}
}
