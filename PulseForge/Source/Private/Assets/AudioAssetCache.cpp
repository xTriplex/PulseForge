#include "Core/PulseForgePCH.h"
#include "Assets/AudioAssetCache.h"

#include "Assets/AssetPathResolver.h"

#include <fstream>
#include <limits>
#include <span>
#include <system_error>
#include <utility>
#include <vector>

namespace PulseForge
{
	namespace
	{
		constexpr uintmax_t MaxAudioSourceBytes = 256ull * 1024ull * 1024ull;

		AudioAssetError MakeError(AudioAssetErrorCode Code, const AssetID& Asset, std::string Message)
		{
			return { Code, Asset, std::move(Message) };
		}
	}

	AudioAssetCache::AudioAssetCache(
		AudioEngine& Engine,
		std::filesystem::path ProjectRoot,
		const AssetRegistry& Registry)
		: m_Engine(Engine), m_ProjectRoot(std::move(ProjectRoot)), m_Registry(Registry)
	{
	}

	std::expected<AudioClip, AudioAssetError> AudioAssetCache::GetOrLoad(const AssetID& Asset)
	{
		if (Asset.IsNil())
			return std::unexpected(MakeError(AudioAssetErrorCode::AssetNotFound, Asset, "Cannot load an audio asset with a nil UUID"));
		if (const auto Existing = m_Clips.find(Asset); Existing != m_Clips.end())
			return Existing->second;

		const auto Record = m_Registry.Find(Asset);
		if (!Record)
		{
			return std::unexpected(MakeError(
				AudioAssetErrorCode::AssetNotFound,
				Asset,
				"Audio asset UUID " + Asset.ToString() + " is not present in the project registry"));
		}

		const auto ResolvedPath = AssetPathResolver::ResolveManagedSourcePath(m_ProjectRoot, Record->ProjectRelativePath);
		if (!ResolvedPath)
		{
			return std::unexpected(MakeError(
				AudioAssetErrorCode::PathResolutionFailed,
				Asset,
				ResolvedPath.error().Message));
		}

		try
		{
			std::error_code FileError;
			const uintmax_t FileSize = std::filesystem::file_size(*ResolvedPath, FileError);
			if (FileError || FileSize == 0 || FileSize > MaxAudioSourceBytes ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<size_t>::max()) ||
				FileSize > static_cast<uintmax_t>(std::numeric_limits<std::streamsize>::max()))
			{
				return std::unexpected(MakeError(
					AudioAssetErrorCode::FileReadFailed,
					Asset,
					FileError
						? "Could not determine audio source size: " + FileError.message()
						: "Audio source is empty or exceeds the 256 MiB import limit"));
			}

			std::vector<std::byte> EncodedData(static_cast<size_t>(FileSize));
			std::ifstream Input(*ResolvedPath, std::ios::binary);
			if (!Input.is_open())
				return std::unexpected(MakeError(AudioAssetErrorCode::FileReadFailed, Asset, "Could not open managed audio source"));
			Input.read(reinterpret_cast<char*>(EncodedData.data()), static_cast<std::streamsize>(EncodedData.size()));
			if (!Input || Input.bad())
				return std::unexpected(MakeError(AudioAssetErrorCode::FileReadFailed, Asset, "Could not read the complete managed audio source"));

			auto Clip = m_Engine.CreateClip("Audio asset " + Asset.ToString(), EncodedData);
			if (!Clip)
			{
				return std::unexpected(MakeError(
					AudioAssetErrorCode::ClipCreationFailed,
					Asset,
					Clip.error().Message));
			}

			auto Stored = m_Clips.try_emplace(Asset, std::move(*Clip)).first;
			return Stored->second;
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(MakeError(
				AudioAssetErrorCode::CacheFailure,
				Asset,
				std::string("Could not retain managed audio asset: ") + Exception.what()));
		}
	}

	void AudioAssetCache::Clear() noexcept
	{
		m_Clips.clear();
	}
}
