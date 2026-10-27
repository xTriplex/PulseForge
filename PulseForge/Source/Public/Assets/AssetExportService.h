#pragma once

#include "Assets/AssetCookService.h"
#include "Assets/ExportBuildProfile.h"
#include "Core/Core.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace PulseForge
{
	inline constexpr std::string_view ExportTemplateFormat = "PulseForgePlayerTemplate";
	inline constexpr uint32_t ExportTemplateFormatVersion = 1;
	inline constexpr size_t MaximumExportLogBytes = 128u * 1024u;
	inline constexpr size_t MaximumExportFileCount = 4096;
	inline constexpr uint64_t MaximumExportFileBytes = 2ull * 1024ull * 1024ull * 1024ull;

	struct ExportTemplateDescriptor final
	{
		uint32_t FormatVersion = ExportTemplateFormatVersion;
		ExportTargetPlatform TargetPlatform = ExportTargetPlatform::Windows;
		ExportTargetArchitecture TargetArchitecture = ExportTargetArchitecture::X64;
		CookedBuildConfiguration Configuration = CookedBuildConfiguration::Release;
		std::string RendererBackend;
		std::string RuntimeABI;
		uint32_t CookManifestSchema = 0;
		std::string Entrypoint;
		std::vector<std::string> RuntimeFiles;
	};

	enum class AssetExportErrorCode : uint8_t
	{
		InvalidRequest,
		InvalidProfile,
		UnsupportedTarget,
		TemplateMissing,
		TemplateIncompatible,
		TemplateIncomplete,
		ProjectOpenFailed,
		DependencyResolutionFailed,
		CookFailed,
		StageFailed,
		VerificationFailed,
		PublicationFailed,
		RecoveryRequired,
		Cancelled,
		Busy,
		InternalFailure
	};

	struct AssetExportError final
	{
		AssetExportErrorCode Code = AssetExportErrorCode::InternalFailure;
		std::filesystem::path Path;
		std::string Message;
	};

	enum class AssetExportPhase : uint8_t
	{
		Idle,
		Validate,
		ResolveTemplate,
		Cook,
		Stage,
		Verify,
		Publish,
		Complete,
		Failed,
		Cancelled
	};

	struct AssetExportRequest final
	{
		std::filesystem::path SourceProjectFile;
		ExportBuildProfile Profile;
		std::filesystem::path TemplateRoot;
		std::filesystem::path TemplateDescriptor;
		std::optional<std::filesystem::path> OutputOverride;
	};

	struct AssetExportSnapshot final
	{
		uint64_t RequestID = 0;
		AssetExportPhase Phase = AssetExportPhase::Idle;
		float Progress = 0.0f;
		bool Running = false;
		bool CancellationRequested = false;
		bool LogTruncated = false;
		std::string Log;
		std::optional<AssetExportError> Error;
		std::filesystem::path PublishedOutput;
	};

	class PULSEFORGE_API AssetExportService final
	{
	public:
		AssetExportService() = default;
		AssetExportService(const AssetExportService&) = delete;
		AssetExportService& operator=(const AssetExportService&) = delete;
		~AssetExportService();

		[[nodiscard]] std::expected<uint64_t, AssetExportError> Start(AssetExportRequest Request);
		[[nodiscard]] AssetExportSnapshot GetSnapshot(uint64_t RequestID) const;
		[[nodiscard]] bool RequestCancel(uint64_t RequestID) noexcept;
		void Shutdown() noexcept;
		[[nodiscard]] static std::expected<void, AssetExportError> RecoverInterruptedPublication(
			const std::filesystem::path& Output);

	private:
		void Run(uint64_t RequestID, AssetExportRequest Request) noexcept;
		void Update(uint64_t RequestID, AssetExportPhase Phase, float Progress, std::string Message = {});
		void Finish(uint64_t RequestID, AssetExportPhase Phase, std::optional<AssetExportError> Error,
			std::filesystem::path Output = {}) noexcept;

		mutable std::mutex m_Mutex;
		std::jthread m_Worker;
		AssetExportSnapshot m_Snapshot;
		std::atomic<bool> m_CancelRequested = false;
		uint64_t m_NextRequestID = 1;
		bool m_ShuttingDown = false;
	};

	[[nodiscard]] PULSEFORGE_API std::expected<ExportTemplateDescriptor, AssetExportError> LoadExportTemplateDescriptor(
		const std::filesystem::path& Path);
	[[nodiscard]] PULSEFORGE_API std::expected<std::string, AssetExportError> SerializeExportTemplateDescriptor(
		const ExportTemplateDescriptor& Descriptor);
} // namespace PulseForge
