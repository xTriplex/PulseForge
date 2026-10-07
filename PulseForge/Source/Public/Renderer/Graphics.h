#pragma once

#include "Core/Core.h"
#include "Renderer/Buffer.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	class BindingLayout;
	using BindingLayoutHandle = std::shared_ptr<BindingLayout>;

	enum class ShaderStage : uint8_t
	{
		Vertex,
		Fragment
	};

	enum class ShaderBytecodeFormat : uint8_t
	{
		SpirV
	};

	struct ShaderDesc
	{
		ShaderStage Stage = ShaderStage::Vertex;
		ShaderBytecodeFormat BytecodeFormat = ShaderBytecodeFormat::SpirV;
		std::string EntryPoint = "main";
		std::string DebugName;
	};

	enum class VertexSemantic : uint8_t
	{
		Position,
		Color,
		TexCoord
	};

	enum class VertexFormat : uint8_t
	{
		Float2,
		Float3,
		Float4
	};

	struct VertexAttributeDesc
	{
		VertexSemantic Semantic = VertexSemantic::Position;
		VertexFormat Format = VertexFormat::Float3;
		uint32_t Offset = 0;
	};

	struct VertexLayoutDesc
	{
		uint32_t Stride = 0;
		std::vector<VertexAttributeDesc> Attributes;
	};

	enum class PrimitiveTopology : uint8_t
	{
		TriangleList
	};

	enum class ColorTargetFormat : uint8_t
	{
		Swapchain,
		RGBA8_UNorm,
		RGBA8_Srgb
	};

	enum class CullMode : uint8_t
	{
		None,
		Back,
		Front
	};

	struct RasterState
	{
		CullMode Cull = CullMode::None;
		bool Wireframe = false;
		bool ScissorEnabled = false;
	};

	struct BlendState
	{
		bool Enabled = false;
	};

	enum class DepthCompareOperation : uint8_t
	{
		Never,
		Less,
		Equal,
		LessEqual,
		Greater,
		NotEqual,
		GreaterEqual,
		Always
	};

	struct DepthState
	{
		bool TestEnabled = false;
		bool WriteEnabled = false;
		DepthCompareOperation Compare = DepthCompareOperation::Less;
	};

	class PULSEFORGE_API Shader
	{
	public:
		virtual ~Shader() = default;
		[[nodiscard]] virtual const ShaderDesc& GetDescription() const noexcept = 0;
	};

	// Shared because graphics-pipeline descriptions retain their shader dependencies.
	using ShaderHandle = std::shared_ptr<Shader>;

	struct GraphicsPipelineDesc
	{
		ShaderHandle VertexShader;
		ShaderHandle FragmentShader;
		std::vector<BindingLayoutHandle> BindingLayouts;
		VertexLayoutDesc VertexLayout;
		PrimitiveTopology Topology = PrimitiveTopology::TriangleList;
		ColorTargetFormat ColorFormat = ColorTargetFormat::Swapchain;
		RasterState Rasterizer;
		BlendState Blend;
		DepthState Depth;
		std::string DebugName;
	};

	class PULSEFORGE_API GraphicsPipeline
	{
	public:
		virtual ~GraphicsPipeline() = default;
		[[nodiscard]] virtual const GraphicsPipelineDesc& GetDescription() const noexcept = 0;
	};

	using GraphicsPipelineHandle = std::unique_ptr<GraphicsPipeline>;

	struct DrawArguments
	{
		uint32_t VertexCount = 0;
		uint32_t InstanceCount = 1;
		uint32_t FirstVertex = 0;
		uint32_t FirstInstance = 0;
	};

	struct ScissorRect
	{
		uint32_t X = 0;
		uint32_t Y = 0;
		uint32_t Width = 0;
		uint32_t Height = 0;
	};

	struct DrawIndexedArguments
	{
		uint32_t IndexCount = 0;
		uint32_t InstanceCount = 1;
		uint32_t FirstIndex = 0;
		uint32_t FirstInstance = 0;
		std::optional<ScissorRect> Scissor;
	};

	enum class GraphicsErrorCode : uint8_t
	{
		InvalidDescription,
		InvalidBytecode,
		InvalidVertexLayout,
		InvalidDrawArguments,
		UnsupportedFeature,
		InvalidFrameState,
		BackendFailure
	};

	struct GraphicsError
	{
		GraphicsErrorCode Code;
		std::string Message;
	};

	using ShaderCreateResult = std::expected<ShaderHandle, GraphicsError>;
	using GraphicsPipelineCreateResult = std::expected<GraphicsPipelineHandle, GraphicsError>;
	using GraphicsResult = std::expected<void, GraphicsError>;

	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateShaderBytecode(
		const ShaderDesc& Description,
		std::span<const std::byte> Bytecode);
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateVertexLayout(const VertexLayoutDesc& Description);
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateGraphicsPipelineDescription(
		const GraphicsPipelineDesc& Description);
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateDrawArguments(
		const DrawArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const BufferDesc& VertexBuffer);
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateIndexedBufferDrawArguments(
		const DrawIndexedArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const BufferDesc& VertexBuffer,
		const BufferDesc& IndexBuffer);
	[[nodiscard]] PULSEFORGE_API std::optional<ScissorRect> IntersectScissorRect(
		const ScissorRect& Scissor,
		uint32_t TargetWidth,
		uint32_t TargetHeight) noexcept;
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateDrawBindingSets(
		const GraphicsPipelineDesc& Pipeline,
		std::span<const class BindingSet* const> BindingSets);
}
