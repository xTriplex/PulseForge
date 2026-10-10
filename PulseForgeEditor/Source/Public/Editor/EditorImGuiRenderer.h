#pragma once

#include "Renderer/Binding.h"
#include "Renderer/Buffer.h"
#include "Renderer/Graphics.h"
#include "Renderer/Texture.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

namespace PulseForge
{
	class Application;
}

namespace PulseForgeEditor
{
	class EditorImGuiRenderer final
	{
	public:
		EditorImGuiRenderer() = default;
		EditorImGuiRenderer(const EditorImGuiRenderer&) = delete;
		EditorImGuiRenderer& operator=(const EditorImGuiRenderer&) = delete;
		~EditorImGuiRenderer();

		[[nodiscard]] std::expected<void, std::string> Initialize(
			PulseForge::Application& Runtime,
			const std::filesystem::path& ShaderDirectory);
		[[nodiscard]] std::expected<uint64_t, std::string> RegisterTexture(const PulseForge::Texture& Texture);
		void UnregisterTexture(uint64_t TextureID) noexcept;
		[[nodiscard]] std::expected<void, std::string> RenderDrawData();
		[[nodiscard]] uint64_t GetFontTextureID() const noexcept { return m_FontTextureID; }
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
			bool IsSrgb = false;
		};

		struct alignas(16) ColorSpaceConstants
		{
			uint32_t OutputIsUnorm = 0;
			uint32_t TextureIsSrgb = 0;
			uint32_t Padding[2]{};
		};
		static_assert(sizeof(ColorSpaceConstants) == 16);

		[[nodiscard]] std::expected<void, std::string> EnsureBuffers(size_t VertexBytes, size_t IndexBytes);

		PulseForge::Application* m_Runtime = nullptr;
		PulseForge::ShaderHandle m_VertexShader;
		PulseForge::ShaderHandle m_FragmentShader;
		PulseForge::SamplerHandle m_Sampler;
		PulseForge::TextureHandle m_FontTexture;
		PulseForge::BindingLayoutHandle m_BindingLayout;
		PulseForge::BufferHandle m_ColorSpaceConstantsBuffer;
		PulseForge::GraphicsPipelineHandle m_Pipeline;
		PulseForge::BufferHandle m_VertexBuffer;
		PulseForge::BufferHandle m_IndexBuffer;
		uint64_t m_VertexBufferCapacity = 0;
		uint64_t m_IndexBufferCapacity = 0;
		uint64_t m_NextTextureID = 1;
		uint64_t m_FontTextureID = 0;
		std::unordered_map<uint64_t, RegisteredTexture> m_TextureBindings;
		std::vector<Vertex> m_Vertices;
		std::vector<uint32_t> m_Indices;
		std::vector<DrawBatch> m_Batches;
		bool m_Initialized = false;
		ColorSpaceConstants m_UploadedColorSpaceConstants{};
		bool m_HasUploadedColorSpaceConstants = false;
	};
}
