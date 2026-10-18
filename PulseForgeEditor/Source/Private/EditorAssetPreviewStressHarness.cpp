#include "Editor/EditorAssetPreviewStressHarness.h"

#include "Core/Log.h"

#include <fstream>
#include <stdexcept>

namespace PulseForgeEditor
{
	EditorAssetPreviewStressHarness::EditorAssetPreviewStressHarness(std::optional<std::filesystem::path> Manifest, std::string Budget)
		: m_Manifest(std::move(Manifest)), m_Budget(std::move(Budget))
	{
	}

	AssetPreviewServiceLimits EditorAssetPreviewStressHarness::GetServiceLimits() const
	{
		AssetPreviewServiceLimits Limits;
		if (!IsEnabled()) return Limits;

		if (m_Budget != "entries" && m_Budget != "gpu") throw std::runtime_error("Asset preview stress budget must be 'entries' or 'gpu'");

		Limits.MaximumQueuedRequests = 32;
		Limits.MaximumCpuBytes = 2ull * 1024ull * 1024ull;
		Limits.MaximumUploadsPerUpdate = 4;
		Limits.MaximumUploadBytesPerUpdate = 1024ull * 1024ull;
		if (m_Budget == "entries")
		{
			Limits.MaximumEntries = 8;
			Limits.MaximumGpuBytes = 64ull * 1024ull * 1024ull;
		}
		else
		{
			Limits.MaximumEntries = 512;
			Limits.MaximumGpuBytes = 4ull * 192ull * 192ull * 4ull;
		}
		return Limits;
	}

	std::expected<std::vector<EditorAssetPreviewStressHarness::Asset>, std::string> EditorAssetPreviewStressHarness::ParseManifest(
		std::istream& Input)
	{
		std::vector<Asset> Assets;
		std::string Line;
		while (std::getline(Input, Line))
		{
			if (!Line.empty() && Line.back() == '\r') Line.pop_back();
			const size_t Separator = Line.find('|');
			if (Separator == std::string::npos || Separator == 0 || Separator + 1 >= Line.size())
				return std::unexpected("Invalid asset preview stress manifest row");
			Assets.push_back(Asset{ Line.substr(0, Separator), std::filesystem::u8path(Line.substr(Separator + 1)) });
		}
		if (Input.bad()) return std::unexpected("Could not read the asset preview stress manifest completely");
		if (Assets.size() < 32 || Assets.size() > 256)
			return std::unexpected("Stress manifest must contain between 32 and 256 deterministic assets");
		return Assets;
	}

	void EditorAssetPreviewStressHarness::Initialize(EditorAssetPreviewService& Previews)
	{
		if (!m_Manifest) return;
		(void)GetServiceLimits();
		std::ifstream Manifest(*m_Manifest, std::ios::binary);
		if (!Manifest) throw std::runtime_error("Could not open asset preview stress manifest");
		auto Parsed = ParseManifest(Manifest);
		if (!Parsed) throw std::runtime_error(Parsed.error());
		m_Assets = std::move(*Parsed);
		m_ReportPath = m_Manifest->parent_path() / ("vulkan_stress_" + m_Budget + ".json");
		Previews.SetProject(m_ProjectID);
		PF_INFO("Asset preview Vulkan stress enabled: {} assets, {} budget", m_Assets.size(), m_Budget);
	}

	void EditorAssetPreviewStressHarness::BeginFrame() noexcept
	{
		m_ReadyThisFrame = false;
		m_RenderFailedThisFrame = false;
	}

	void EditorAssetPreviewStressHarness::Draw(EditorAssetPreviewService& Previews)
	{
		if (m_Finished) return;
		constexpr size_t BatchSize = 4;
		const size_t BatchCount = m_Assets.size() / BatchSize;
		if (m_Batch >= BatchCount) return;
		const size_t First = m_Batch * BatchSize;
		ImGui::SetNextWindowSize(ImVec2(500.0f, 210.0f), ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Asset Preview GPU Stress (opt-in)"))
		{
			ImGui::End();
			return;
		}
		ImGui::Text("Budget: %s | Batch: %zu / %zu", m_Budget.c_str(), m_Batch + 1, BatchCount);
		bool AllReady = true;
		for (size_t Offset = 0; Offset < BatchSize; ++Offset)
		{
			const Asset& Source = m_Assets[First + Offset];
			Previews.RequestImage(m_ProjectID, Source.ID, Source.SourcePath);
			const auto Preview = Previews.FindPreview(m_ProjectID, Source.ID);
			if (Preview)
			{
				if (Offset != 0) ImGui::SameLine();
				ImGui::Image(ImTextureRef(static_cast<ImTextureID>(Preview->TextureID)), ImVec2(88.0f, 88.0f));
				ImGui::Text("%s", Source.ID.c_str());
				++m_RenderedImages;
			}
			else
			{
				AllReady = false;
				ImGui::Text("Waiting: %s", Source.ID.c_str());
				ImGui::SameLine();
				ImGui::Dummy(ImVec2(4.0f, 1.0f));
			}
		}
		ImGui::End();
		m_ReadyThisFrame = AllReady;
	}

	bool EditorAssetPreviewStressHarness::AdvanceFrame(EditorAssetPreviewService& Previews)
	{
		if (m_Finished) return false;
		if (m_RenderFailedThisFrame)
		{
			m_Failed = true;
			m_Finished = true;
			return true;
		}
		if (m_Batch == 0 && !m_InvalidatedQueued)
		{
			Previews.InvalidateAll();
			m_InvalidatedQueued = true;
		}
		if (!m_ReadyThisFrame) return false;
		++m_RenderedFrames;
		++m_ReadyFrames;
		if (m_ReadyFrames < 4) return false;
		m_ReadyFrames = 0;
		const size_t BatchCount = m_Assets.size() / 4;
		++m_Batch;
		if (!m_Refreshed && m_Batch >= BatchCount / 5)
		{
			Previews.InvalidateAll();
			m_Refreshed = true;
		}
		if (!m_SwitchedProject && m_Batch >= BatchCount / 3)
		{
			Previews.InvalidateAll();
			m_ProjectID = "Phase5StressProjectB";
			Previews.SetProject(m_ProjectID);
			m_SwitchedProject = true;
		}
		if (m_Batch < BatchCount) return false;
		if (m_Pass == 0)
		{
			m_Pass = 1;
			m_Batch = 0;
			return false;
		}

		Previews.InvalidateAll();
		for (size_t Index = 0; Index < 8; ++Index)
		{
			const Asset& Source = m_Assets[Index];
			Previews.RequestImage(m_ProjectID, "shutdown-" + Source.ID, Source.SourcePath);
		}
		m_ShutdownPendingRequests = Previews.GetStats().PendingRequests;
		PF_INFO("Asset preview stress queued {} requests immediately before shutdown", m_ShutdownPendingRequests);
		m_Finished = true;
		PF_INFO("Asset preview stress workload completed; requesting clean editor close");
		return true;
	}

	void EditorAssetPreviewStressHarness::WriteReport(const EditorAssetPreviewService& Previews, size_t RegisteredTextures) noexcept
	{
		if (m_ReportPath.empty() || m_ReportWritten) return;
		m_ReportWritten = true;
		try
		{
			const auto Stats = Previews.GetStats();
			std::ofstream Report(m_ReportPath, std::ios::binary | std::ios::trunc);
			if (!Report) return;
			Report << "{\n"
				   << "  \"budget_mode\": \"" << m_Budget << "\",\n"
				   << "  \"fixtures\": " << m_Assets.size() << ",\n"
				   << "  \"project_switch\": " << (m_SwitchedProject ? "true" : "false") << ",\n"
				   << "  \"queued_result_invalidation\": " << (m_InvalidatedQueued ? "true" : "false") << ",\n"
				   << "  \"refresh_invalidation\": " << (m_Refreshed ? "true" : "false") << ",\n"
				   << "  \"rendered_preview_images\": " << m_RenderedImages << ",\n"
				   << "  \"render_frames\": " << m_RenderedFrames << ",\n"
				   << "  \"shutdown_pending_requests_before_close\": " << m_ShutdownPendingRequests << ",\n"
				   << "  \"cache_hits\": " << Stats.CacheHits << ",\n"
				   << "  \"cache_misses\": " << Stats.CacheMisses << ",\n"
				   << "  \"decodes_succeeded\": " << Stats.DecodesSucceeded << ",\n"
				   << "  \"decodes_failed\": " << Stats.DecodesFailed << ",\n"
				   << "  \"uploads\": " << Stats.Uploads << ",\n"
				   << "  \"evictions\": " << Stats.Evictions << ",\n"
				   << "  \"binding_registrations\": " << Stats.BindingRegistrations << ",\n"
				   << "  \"binding_unregistrations\": " << Stats.BindingUnregistrations << ",\n"
				   << "  \"stale_results_discarded\": " << Stats.StaleResultsDiscarded << ",\n"
				   << "  \"peak_entries\": " << Stats.PeakEntries << ",\n"
				   << "  \"peak_queued_result_cpu_bytes\": " << Stats.PeakQueuedResultCpuBytes << ",\n"
				   << "  \"peak_gpu_bytes\": " << Stats.PeakGpuBytes << ",\n"
				   << "  \"final_entries\": " << Stats.ActiveEntries << ",\n"
				   << "  \"final_gpu_bytes\": " << Stats.GpuBytes << ",\n"
				   << "  \"final_pending_requests\": " << Stats.PendingRequests << ",\n"
				   << "  \"renderer_registered_textures_after_service_shutdown\": " << RegisteredTextures << ",\n"
				   << "  \"worker_failed\": " << (Stats.WorkerFailed ? "true" : "false") << ",\n"
				   << "  \"render_failed\": " << (m_Failed ? "true" : "false") << ",\n"
				   << "  \"completed\": " << (m_Finished && !m_Failed ? "true" : "false") << "\n}\n";
		}
		catch (...)
		{
		}
	}
} // namespace PulseForgeEditor
