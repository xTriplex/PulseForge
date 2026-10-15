#pragma once

#include "Editor/EditorAssetPreviewService.h"

#include <expected>
#include <filesystem>
#include <istream>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace PulseForgeEditor
{
	class EditorAssetPreviewStressHarness final
	{
	public:
		struct Asset final
		{
			std::string ID;
			std::filesystem::path SourcePath;
		};

		EditorAssetPreviewStressHarness(std::optional<std::filesystem::path> Manifest, std::string Budget);

		[[nodiscard]] bool IsEnabled() const noexcept { return m_Manifest.has_value(); }
		[[nodiscard]] AssetPreviewServiceLimits GetServiceLimits() const;
		[[nodiscard]] static std::expected<std::vector<Asset>, std::string> ParseManifest(std::istream& Input);

		void Initialize(EditorAssetPreviewService& Previews);
		void BeginFrame() noexcept;
		void MarkRenderFailure() noexcept { m_RenderFailedThisFrame = true; }
		void Draw(EditorAssetPreviewService& Previews);
		[[nodiscard]] bool AdvanceFrame(EditorAssetPreviewService& Previews);
		void WriteReport(const EditorAssetPreviewService& Previews, size_t RegisteredTextures) noexcept;

	private:
		std::optional<std::filesystem::path> m_Manifest;
		std::string m_Budget;
		std::vector<Asset> m_Assets;
		std::string m_ProjectID = "Phase5StressProjectA";
		std::filesystem::path m_ReportPath;
		size_t m_Batch = 0;
		size_t m_ReadyFrames = 0;
		size_t m_RenderedImages = 0;
		size_t m_RenderedFrames = 0;
		size_t m_ShutdownPendingRequests = 0;
		size_t m_Pass = 0;
		bool m_SwitchedProject = false;
		bool m_Finished = false;
		bool m_ReadyThisFrame = false;
		bool m_RenderFailedThisFrame = false;
		bool m_Failed = false;
		bool m_InvalidatedQueued = false;
		bool m_Refreshed = false;
		bool m_ReportWritten = false;
	};
} // namespace PulseForgeEditor
