#pragma once

#include "Assets/MaterialAsset.h"
#include "Scene/UUID.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>

namespace PulseForge
{
	class Project;
	class RenderDevice;
}

namespace PulseForgeEditor
{
	class EditorTextureRegistry;

	class EditorMaterialPreviewService final
	{
	public:
		EditorMaterialPreviewService();
		EditorMaterialPreviewService(const EditorMaterialPreviewService&) = delete;
		EditorMaterialPreviewService& operator=(const EditorMaterialPreviewService&) = delete;
		~EditorMaterialPreviewService();

		void BeginFrame() noexcept;
		void Clear() noexcept;
		void RefreshProjectAssets() noexcept;
		void RetainSessions(std::span<const uint64_t> SessionGenerations) noexcept;
		void Prepare(PulseForge::RenderDevice& Runtime, EditorTextureRegistry& Textures, PulseForge::Project& Project,
			const PulseForge::UUID& Material, uint64_t SessionGeneration, uint64_t DraftRevision,
			const PulseForge::MaterialAssetDesc& Draft, const std::filesystem::path& CompiledShaderDirectory);
		[[nodiscard]] bool Draw(uint64_t SessionGeneration, uint64_t CurrentDraftRevision, float Width, float Height);
		[[nodiscard]] bool RequestRetry(uint64_t SessionGeneration) noexcept;
		[[nodiscard]] const std::string& GetError(uint64_t SessionGeneration) const noexcept;

	private:
		struct Entry;
		[[nodiscard]] Entry* FindOrCreate(PulseForge::RenderDevice& Runtime, EditorTextureRegistry& Textures,
			PulseForge::Project& Project, const PulseForge::UUID& Material, uint64_t SessionGeneration,
			const std::filesystem::path& CompiledShaderDirectory);

		std::unordered_map<uint64_t, std::unique_ptr<Entry>> m_Entries;
		uint64_t m_Frame = 0;
		uint64_t m_AssetRevision = 1;
		PulseForge::UUID m_ProjectID;
	};
}
