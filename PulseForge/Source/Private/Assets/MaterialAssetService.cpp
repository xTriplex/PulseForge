#include "Assets/MaterialAssetService.h"

#include "Assets/AssetOperations.h"
#include "Assets/AssetPathResolver.h"
#include "Assets/AssetTypes.h"
#include "Core/BoundedInput.h"
#include "Core/PlatformServices.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#define NOMINMAX
#include <Windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace PulseForge
{
	namespace
	{
		std::expected<std::filesystem::path, MaterialAssetError> ResolveMaterialPath(
			const AssetID& MaterialAsset,
			const std::filesystem::path& ProjectRoot,
			const AssetRegistry& Registry)
		{
			const auto Record = Registry.Find(MaterialAsset);
			if (!Record)
			{
				return std::unexpected(MaterialAssetError{
					MaterialAssetErrorCode::AssetNotFound,
					MaterialAsset,
					"Material asset UUID " + MaterialAsset.ToString() + " is not present in the project asset registry" });
			}
			if (!HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Material))
			{
				return std::unexpected(MaterialAssetError{
					MaterialAssetErrorCode::UnsupportedAssetType,
					MaterialAsset,
					"Asset UUID " + MaterialAsset.ToString() + " does not refer to a .material file" });
			}

			auto Path = AssetPathResolver::ResolveManagedSourcePath(ProjectRoot, Record->ProjectRelativePath);
			if (!Path)
			{
				return std::unexpected(MaterialAssetError{
					MaterialAssetErrorCode::PathResolutionFailed,
					MaterialAsset,
					Path.error().Message });
			}
			return *Path;
		}

		std::expected<std::string, MaterialAssetError> ReadMaterialSource(const std::filesystem::path& Path, const AssetID& Asset)
		{
			std::error_code Error;
			if (!std::filesystem::is_regular_file(Path, Error) || Error)
				return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::FileReadFailed, Asset,
					"Material source is missing or is not a regular file" });
			const uintmax_t Size = std::filesystem::file_size(Path, Error);
			if (Error)
				return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::FileReadFailed, Asset,
					"Could not inspect material source: " + Error.message() });
			if (Size > 1024 * 1024)
				return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::InvalidDocument, Asset,
					"Material source exceeds the 1 MiB size limit" });
			std::ifstream Input(Path, std::ios::binary);
			if (!Input.is_open())
				return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::FileReadFailed, Asset,
					"Could not open material source" });
			auto Bytes = Detail::ReadBoundedInput(Input, 1024 * 1024);
			if (!Bytes)
				return std::unexpected(MaterialAssetError{ Bytes.error().Code == Detail::BoundedInputErrorCode::TooLarge
						? MaterialAssetErrorCode::InvalidDocument : MaterialAssetErrorCode::FileReadFailed,
					Asset, "Could not read the complete material source" });
			return *Bytes;
		}

		class TemporaryMaterialSource final
		{
		public:
			TemporaryMaterialSource(const TemporaryMaterialSource&) = delete;
			TemporaryMaterialSource& operator=(const TemporaryMaterialSource&) = delete;
			TemporaryMaterialSource(TemporaryMaterialSource&& Other) noexcept
				: m_Path(std::move(Other.m_Path)), m_Published(Other.m_Published)
#if defined(_WIN32)
				, m_Handle(std::exchange(Other.m_Handle, INVALID_HANDLE_VALUE))
#else
				, m_Descriptor(std::exchange(Other.m_Descriptor, -1)), m_Device(Other.m_Device), m_Inode(Other.m_Inode)
#endif
			{
				Other.m_Published = true;
			}
			TemporaryMaterialSource& operator=(TemporaryMaterialSource&&) = delete;
			~TemporaryMaterialSource() noexcept
			{
#if defined(_WIN32)
				if (m_Handle != INVALID_HANDLE_VALUE)
				{
					if (!m_Published)
					{
					FILE_DISPOSITION_INFO DeleteFile{};
					DeleteFile.DeleteFile = TRUE;
					(void)SetFileInformationByHandle(m_Handle, FileDispositionInfo, &DeleteFile, sizeof(DeleteFile));
					(void)CloseHandle(m_Handle);
				}
				else (void)CloseHandle(m_Handle);
				}
#else
				if (m_Descriptor >= 0)
				{
					if (!m_Published)
					{
					struct stat Current{};
					if (fstatat(AT_FDCWD, m_Path.c_str(), &Current, AT_SYMLINK_NOFOLLOW) == 0 &&
						Current.st_dev == m_Device && Current.st_ino == m_Inode)
						(void)unlink(m_Path.c_str());
					(void)close(m_Descriptor);
				}
				else (void)close(m_Descriptor);
				}
#endif
			}

			[[nodiscard]] static std::expected<TemporaryMaterialSource, std::string> Create(const std::filesystem::path& Parent)
			{
				for (size_t Attempt = 0; Attempt < 8; ++Attempt)
				{
					auto ID = UUID::Generate();
					if (!ID) return std::unexpected("Could not generate a temporary material filename: " + ID.error().Message);
					const std::filesystem::path Path = Parent / std::filesystem::u8path(".pfmaterial-save-" + ID->ToString() + ".tmp");
#if defined(_WIN32)
					HANDLE Handle = CreateFileW(Path.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ | FILE_SHARE_DELETE,
						nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr);
					if (Handle != INVALID_HANDLE_VALUE) return TemporaryMaterialSource(Path, Handle);
					const DWORD Code = GetLastError();
					if (Code == ERROR_FILE_EXISTS || Code == ERROR_ALREADY_EXISTS) continue;
					return std::unexpected("Could not exclusively create a material save file: " +
						std::error_code(static_cast<int>(Code), std::system_category()).message());
#else
					const int Descriptor = open(Path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
					if (Descriptor >= 0)
					{
						struct stat Created{};
						if (fstat(Descriptor, &Created) != 0)
						{
							const std::string Message = "Could not identify temporary material output: " +
								std::error_code(errno, std::generic_category()).message();
						(void)close(Descriptor);
						(void)unlink(Path.c_str());
						return std::unexpected(Message);
						}
						return TemporaryMaterialSource(Path, Descriptor, Created.st_dev, Created.st_ino);
					}
					const int Code = errno;
					if (Code == EEXIST) continue;
					return std::unexpected("Could not exclusively create a material save file: " +
						std::error_code(Code, std::generic_category()).message());
#endif
				}
				return std::unexpected("Could not find an unused temporary material filename after eight attempts");
			}

			[[nodiscard]] std::expected<void, std::string> Write(std::string_view Bytes)
			{
#if defined(_WIN32)
				size_t Offset = 0;
				while (Offset < Bytes.size())
				{
					const DWORD Chunk = static_cast<DWORD>((std::min)(Bytes.size() - Offset,
						static_cast<size_t>((std::numeric_limits<DWORD>::max)())));
					DWORD Written = 0;
					if (!WriteFile(m_Handle, Bytes.data() + Offset, Chunk, &Written, nullptr) || Written == 0)
						return std::unexpected("Could not write the complete temporary material file");
					Offset += Written;
				}
				if (!FlushFileBuffers(m_Handle))
					return std::unexpected("Could not flush the temporary material file");
#else
				size_t Offset = 0;
				while (Offset < Bytes.size())
				{
					const ssize_t Written = write(m_Descriptor, Bytes.data() + Offset, Bytes.size() - Offset);
					if (Written < 0)
					{
						if (errno == EINTR) continue;
						return std::unexpected("Could not write the complete temporary material file: " +
							std::error_code(errno, std::generic_category()).message());
					}
					if (Written == 0) return std::unexpected("Could not write the complete temporary material file");
					Offset += static_cast<size_t>(Written);
				}
				if (fsync(m_Descriptor) != 0)
					return std::unexpected("Could not flush the temporary material file: " +
						std::error_code(errno, std::generic_category()).message());
#endif
				return {};
			}

			[[nodiscard]] std::expected<void, std::string> Publish(const std::filesystem::path& Destination)
			{
				auto Result = Platform::ReplaceFileAtomically(m_Path, Destination);
				if (!Result) return std::unexpected(Result.error().Message);
				m_Published = true;
				return {};
			}

		private:
#if defined(_WIN32)
			TemporaryMaterialSource(std::filesystem::path Path, HANDLE Handle) : m_Path(std::move(Path)), m_Handle(Handle) {}
#else
			TemporaryMaterialSource(std::filesystem::path Path, int Descriptor, dev_t Device, ino_t Inode)
				: m_Path(std::move(Path)), m_Descriptor(Descriptor), m_Device(Device), m_Inode(Inode) {}
#endif
			std::filesystem::path m_Path;
			bool m_Published = false;
#if defined(_WIN32)
			HANDLE m_Handle = INVALID_HANDLE_VALUE;
#else
			int m_Descriptor = -1;
			dev_t m_Device{};
			ino_t m_Inode{};
#endif
		};
	}

	std::expected<AssetRecord, MaterialAssetError> MaterialAssetService::Create(
		AssetRegistry& Registry,
		const std::filesystem::path& ProjectRoot,
		const std::filesystem::path& ProjectRelativePath,
		const MaterialAssetDesc& Material)
	{
		if (!HasAssetTypeExtension(ProjectRelativePath, AssetFileType::Material))
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::UnsupportedAssetType, {}, "New material assets must use the .material extension" });

		const auto Serialized = MaterialAssetSerializer::Serialize(Material);
		if (!Serialized)
			return std::unexpected(Serialized.error());

		const std::span<const char> Characters(Serialized->data(), Serialized->size());
		auto Created = AssetOperations::CreateAssetFromBytes(
			Registry,
			ProjectRoot,
			std::as_bytes(Characters),
			ProjectRelativePath);
		if (!Created)
		{
			MaterialAssetError Error{
				Created.error().Code == AssetOperationErrorCode::CommittedWithCleanupFailure
					? MaterialAssetErrorCode::CommittedWithCleanupFailure
					: MaterialAssetErrorCode::AssetOperationFailed,
				{},
				Created.error().Message };
			Error.CommittedAsset = Created.error().CommittedAsset;
			Error.RecoveryPath = Created.error().RecoveryPath;
			if (Error.CommittedAsset)
				Error.Asset = Error.CommittedAsset->ID;
			return std::unexpected(std::move(Error));
		}
		return *Created;
	}

	std::expected<MaterialAssetDesc, MaterialAssetError> MaterialAssetService::Load(
		const AssetID& MaterialAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry)
	{
		auto Snapshot = LoadSourceSnapshot(MaterialAsset, ProjectRoot, Registry);
		if (!Snapshot) return std::unexpected(std::move(Snapshot.error()));
		return std::move(Snapshot->Description);
	}

	std::expected<MaterialAssetSourceSnapshot, MaterialAssetError> MaterialAssetService::LoadSourceSnapshot(
		const AssetID& MaterialAsset, const std::filesystem::path& ProjectRoot, const AssetRegistry& Registry)
	{
		auto Path = ResolveMaterialPath(MaterialAsset, ProjectRoot, Registry);
		if (!Path) return std::unexpected(std::move(Path.error()));
		auto Bytes = ReadMaterialSource(*Path, MaterialAsset);
		if (!Bytes) return std::unexpected(std::move(Bytes.error()));
		auto Description = MaterialAssetSerializer::Deserialize(*Bytes);
		if (!Description)
		{
			Description.error().Asset = MaterialAsset;
			return std::unexpected(std::move(Description.error()));
		}
		return MaterialAssetSourceSnapshot{ std::move(*Description), std::move(*Bytes), std::move(*Path) };
	}

	std::expected<void, MaterialAssetError> MaterialAssetService::Save(
		const AssetID& MaterialAsset,
		const std::filesystem::path& ProjectRoot,
		const AssetRegistry& Registry,
		const MaterialAssetDesc& Material)
	{
		auto Path = ResolveMaterialPath(MaterialAsset, ProjectRoot, Registry);
		if (!Path)
			return std::unexpected(std::move(Path.error()));
		if (auto SaveResult = MaterialAssetSerializer::SaveToFile(Material, *Path); !SaveResult)
		{
			SaveResult.error().Asset = MaterialAsset;
			return std::unexpected(std::move(SaveResult.error()));
		}
		return {};
	}

	std::expected<std::string, MaterialAssetError> MaterialAssetService::SaveIfSourceUnchanged(
		const AssetID& MaterialAsset, const std::filesystem::path& ProjectRoot, const AssetRegistry& Registry,
		const MaterialAssetDesc& Material, std::string_view ExpectedSourceBytes)
	{
		auto Path = ResolveMaterialPath(MaterialAsset, ProjectRoot, Registry);
		if (!Path) return std::unexpected(std::move(Path.error()));
		auto CurrentBytes = ReadMaterialSource(*Path, MaterialAsset);
		if (!CurrentBytes) return std::unexpected(std::move(CurrentBytes.error()));
		if (*CurrentBytes != ExpectedSourceBytes)
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::SourceChanged, MaterialAsset,
				"Material source changed since this draft was loaded; reload or discard before saving" });

		if (auto Valid = ValidateMaterialAssetDescription(Material); !Valid)
		{
			Valid.error().Asset = MaterialAsset;
			return std::unexpected(std::move(Valid.error()));
		}
		const std::array<AssetID, 5> References{ Material.BaseColorTexture, Material.MetallicRoughnessTexture,
			Material.NormalTexture, Material.OcclusionTexture, Material.EmissiveTexture };
		for (const AssetID& Reference : References)
		{
			if (Reference.IsNil()) continue;
			const auto Record = Registry.Find(Reference);
			if (!Record || !HasAssetTypeExtension(Record->ProjectRelativePath, AssetFileType::Image))
				return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::UnsupportedAssetType, MaterialAsset,
					"A material texture reference is missing or does not identify a managed image asset" });
		}

		auto Serialized = MaterialAssetSerializer::Serialize(Material);
		if (!Serialized)
		{
			Serialized.error().Asset = MaterialAsset;
			return std::unexpected(std::move(Serialized.error()));
		}
		auto Temporary = TemporaryMaterialSource::Create(Path->parent_path());
		if (!Temporary)
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::FileWriteFailed, MaterialAsset,
				Temporary.error() });
		if (auto Written = Temporary->Write(*Serialized); !Written)
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::FileWriteFailed, MaterialAsset, Written.error() });
		if (auto Published = Temporary->Publish(*Path); !Published)
			return std::unexpected(MaterialAssetError{ MaterialAssetErrorCode::FileReplaceFailed, MaterialAsset, Published.error() });
		return std::move(*Serialized);
	}
}
