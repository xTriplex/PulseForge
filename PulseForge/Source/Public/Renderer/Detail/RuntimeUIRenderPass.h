#pragma once

#include "Renderer/RenderDevice.h"
#include "UI/RuntimeUI.h"

#include <expected>
#include <array>
#include <functional>
#include <map>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace PulseForge
{
	struct RuntimeUIRenderStatistics final
	{
		size_t QuadCount = 0;
		size_t BatchCount = 0;
		size_t TextureCount = 0;
		size_t FontAtlasCount = 0;
	};

	class RuntimeUIRenderPass final
	{
	public:
		explicit RuntimeUIRenderPass(RenderDevice& Runtime) noexcept;
		~RuntimeUIRenderPass();
		[[nodiscard]] std::expected<void, GraphicsError> Initialize(
			std::span<const std::byte> VertexBytecode, std::span<const std::byte> FragmentBytecode);
		using ImageResolver = std::function<const Texture*(AssetID)>;
		using FontResolver = std::function<std::expected<std::vector<std::byte>, std::string>(AssetID)>;
		[[nodiscard]] std::expected<void, GraphicsError> Prepare(
			std::span<const RuntimeUIDrawCommand> Commands,
			const RuntimeUIViewport& Viewport,
			const ImageResolver& ResolveImage,
			const FontResolver& ResolveFont);
		void ResetAssetResources() noexcept;
		[[nodiscard]] RuntimeUIRenderStatistics GetStatistics() const noexcept;
		[[nodiscard]] std::expected<size_t, GraphicsError> Draw(
			std::span<const RuntimeUIDrawCommand> Commands,
			const RuntimeUIViewport& Viewport,
			uint32_t TargetWidth,
			uint32_t TargetHeight,
			ColorTargetFormat TargetFormat,
			bool DepthAttachmentEnabled);

	private:
		struct FontState;
		struct Vertex final
		{
			glm::vec2 Position{};
			glm::vec4 Color{ 1.0f };
			glm::vec2 UV{};
		};
		struct PreparedQuad final
		{
			RuntimeUIRect Bounds{};
			RuntimeUIRect Clip{};
			glm::vec4 Color{ 1.0f };
			glm::vec2 UvMinimum{};
			glm::vec2 UvMaximum{ 1.0f };
			const Texture* Resource = nullptr;
		};
		struct Constants final
		{
			uint32_t EncodeSrgb = 0;
			uint32_t Padding[3]{};
		};

		[[nodiscard]] std::expected<GraphicsPipeline*, GraphicsError> EnsurePipeline(
			ColorTargetFormat TargetFormat, bool DepthAttachmentEnabled);
		[[nodiscard]] FontState* EnsureFont(AssetID Font, const FontResolver& ResolveFont);
		[[nodiscard]] std::expected<void, GraphicsError> EnsureTextureBinding(const Texture& Texture);

		RenderDevice& m_Runtime;
		ShaderHandle m_VertexShader;
		ShaderHandle m_FragmentShader;
		BindingLayoutHandle m_Layout;
		BufferHandle m_VertexBuffer;
		BufferHandle m_IndexBuffer;
		BufferHandle m_ConstantsBuffer;
		SamplerHandle m_Sampler;
		TextureHandle m_WhiteTexture;
		std::map<std::pair<ColorTargetFormat, bool>, GraphicsPipelineHandle> m_Pipelines;
		std::unordered_map<AssetID, std::unique_ptr<FontState>, UUIDHash> m_Fonts;
		std::unordered_set<AssetID, UUIDHash> m_FailedFonts;
		std::unordered_map<const Texture*, BindingSetHandle> m_TextureBindings;
		std::vector<PreparedQuad> m_PreparedQuads;
		size_t m_LastDrawCount = 0;
		Constants m_Constants{};
	};
}
