#pragma once

#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/RenderTarget.h"
#include "Renderer/Texture.h"
#include "Editor/EditorTextureRegistry.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace PulseForge
{
	class Application;
}

namespace PulseForgeEditor
{
	class EditorImGuiRenderer final : public EditorTextureRegistry
	{
	public:
		EditorImGuiRenderer() = default;
		EditorImGuiRenderer(const EditorImGuiRenderer&) = delete;
		EditorImGuiRenderer& operator=(const EditorImGuiRenderer&) = delete;
		~EditorImGuiRenderer();

		[[nodiscard]] std::expected<void, std::string> Initialize(
			PulseForge::Application& Runtime,
			const std::filesystem::path& ShaderDirectory);
		[[nodiscard]] std::expected<uint64_t, std::string> RegisterTexture(const PulseForge::Texture& Texture) override;
		void UnregisterTexture(uint64_t TextureID) noexcept override;
		[[nodiscard]] std::expected<void, std::string> RenderDrawData();
		[[nodiscard]] std::expected<void, std::string> RenderDrawData(const ImDrawData* DrawData, uint64_t PresentationTargetID);
		void RetirePresentationTarget(uint64_t PresentationTargetID) noexcept;
		[[nodiscard]] uint64_t GetFontTextureID() const noexcept { return m_FontTextureID; }
		[[nodiscard]] size_t GetRegisteredTextureCount() const noexcept { return m_TextureBindings.size(); }
		void Shutdown() noexcept;

	private:
		struct Vertex
		{
			float Position[2];
			float TexCoord[2];
			float Color[4];
		};

		struct DrawBatch
		{
			uint32_t FirstIndex = 0;
			uint32_t IndexCount = 0;
			uint64_t TextureID = 0;
			PulseForge::ScissorRect Scissor;
		};

		struct RegisteredTexture
		{
			PulseForge::BindingSetHandle BindingSet;
		};

		struct FullscreenVertex
		{
			float Position[2];
		};

		struct PresentationResources
		{
			PulseForge::BufferHandle VertexBuffer;
			PulseForge::BufferHandle IndexBuffer;
			PulseForge::RenderTargetHandle UnormCompositionTarget;
			PulseForge::BindingSetHandle UnormPresentationBindingSet;
			uint64_t VertexBufferCapacity = 0;
			uint64_t IndexBufferCapacity = 0;
			uint32_t UnormCompositionWidth = 0;
			uint32_t UnormCompositionHeight = 0;
		};

		[[nodiscard]] std::expected<void, std::string> EnsureBuffers(
			PresentationResources& Resources,
			size_t VertexBytes,
			size_t IndexBytes);
		[[nodiscard]] std::expected<void, std::string> EnsureUnormCompositionTarget(
			PresentationResources& Resources,
			uint32_t Width,
			uint32_t Height);
		[[nodiscard]] std::expected<void, std::string> RenderUnormCompositionTarget(
			PresentationResources& Resources);
		[[nodiscard]] PresentationResources& GetPresentationResources(uint64_t PresentationTargetID);

		PulseForge::Application* m_Runtime = nullptr;
		PulseForge::ShaderHandle m_VertexShader;
		PulseForge::ShaderHandle m_FragmentShader;
		PulseForge::ShaderHandle m_PresentationVertexShader;
		PulseForge::ShaderHandle m_PresentationFragmentShader;
		PulseForge::SamplerHandle m_Sampler;
		PulseForge::TextureHandle m_FontTexture;
		PulseForge::BindingLayoutHandle m_BindingLayout;
		PulseForge::GraphicsPipelineHandle m_Pipeline;
		PulseForge::GraphicsPipelineHandle m_SrgbIntermediatePipeline;
		PulseForge::GraphicsPipelineHandle m_UnormPresentationPipeline;
		PulseForge::BufferHandle m_FullscreenVertexBuffer;
		uint64_t m_NextTextureID = 1;
		uint64_t m_FontTextureID = 0;
		std::unordered_map<uint64_t, RegisteredTexture> m_TextureBindings;
		std::vector<Vertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		std::vector<DrawBatch> m_Batches;
		std::unordered_map<uint64_t, PresentationResources> m_PresentationResources;
		bool m_Initialized = false;
	};
}
