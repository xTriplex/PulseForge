#include "Core/PulseForgePCH.h"
#include "Renderer/Binding.h"
#include "Renderer/Graphics.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>

namespace PulseForge
{
	namespace
	{
		GraphicsResult MakeError(GraphicsErrorCode Code, const char* Message)
		{
			return std::unexpected(GraphicsError{ Code, Message });
		}

		bool IsValidShaderStage(ShaderStage Stage)
		{
			return Stage == ShaderStage::Vertex || Stage == ShaderStage::Fragment;
		}

		std::optional<uint32_t> GetVertexFormatSize(VertexFormat Format)
		{
			switch (Format)
			{
				case VertexFormat::Float2: return static_cast<uint32_t>(sizeof(float) * 2);
				case VertexFormat::Float3: return static_cast<uint32_t>(sizeof(float) * 3);
				case VertexFormat::Float4: return static_cast<uint32_t>(sizeof(float) * 4);
				default: return std::nullopt;
			}
		}

		bool IsValidSemantic(VertexSemantic Semantic)
		{
			return Semantic == VertexSemantic::Position ||
				Semantic == VertexSemantic::Color ||
				Semantic == VertexSemantic::TexCoord ||
				Semantic == VertexSemantic::Normal;
		}

		bool IsValidDepthComparison(DepthCompareOperation Operation)
		{
			return Operation == DepthCompareOperation::Never ||
				Operation == DepthCompareOperation::Less ||
				Operation == DepthCompareOperation::Equal ||
				Operation == DepthCompareOperation::LessEqual ||
				Operation == DepthCompareOperation::Greater ||
				Operation == DepthCompareOperation::NotEqual ||
				Operation == DepthCompareOperation::GreaterEqual ||
				Operation == DepthCompareOperation::Always;
		}
	}

	GraphicsResult ValidateShaderBytecode(const ShaderDesc& Description, std::span<const std::byte> Bytecode)
	{
		if (!IsValidShaderStage(Description.Stage))
			return MakeError(GraphicsErrorCode::InvalidDescription, "Shader stage must be Vertex or Fragment");

		if (Description.BytecodeFormat != ShaderBytecodeFormat::SpirV)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Only SPIR-V shader bytecode is currently supported");

		if (Description.EntryPoint.empty())
			return MakeError(GraphicsErrorCode::InvalidDescription, "Shader entry point must not be empty");

		constexpr size_t SpirVHeaderSize = 5 * sizeof(uint32_t);
		if (Bytecode.size() < SpirVHeaderSize || Bytecode.size() % sizeof(uint32_t) != 0)
			return MakeError(GraphicsErrorCode::InvalidBytecode, "SPIR-V bytecode must contain a complete, word-aligned module header");

		const uint32_t Magic =
			std::to_integer<uint32_t>(Bytecode[0]) |
			(std::to_integer<uint32_t>(Bytecode[1]) << 8) |
			(std::to_integer<uint32_t>(Bytecode[2]) << 16) |
			(std::to_integer<uint32_t>(Bytecode[3]) << 24);
		if (Magic != 0x07230203)
			return MakeError(GraphicsErrorCode::InvalidBytecode, "Shader bytecode does not begin with the SPIR-V magic word");

		return {};
	}

	GraphicsResult ValidateVertexLayout(const VertexLayoutDesc& Description)
	{
		if (Description.Stride == 0)
			return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex layout stride must be non-zero");

		if (Description.Attributes.empty())
			return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex layout must contain at least one attribute");

		if (Description.Attributes.size() > 16)
			return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex layout exceeds PulseForge's 16-attribute limit");

		std::array<bool, 4> SeenSemantics = {};
		for (const VertexAttributeDesc& Attribute : Description.Attributes)
		{
			if (!IsValidSemantic(Attribute.Semantic))
				return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex layout contains an unsupported semantic");

			const size_t SemanticIndex = static_cast<size_t>(Attribute.Semantic);
			if (SeenSemantics[SemanticIndex])
				return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex layout contains a duplicate semantic");
			SeenSemantics[SemanticIndex] = true;

			const auto AttributeSize = GetVertexFormatSize(Attribute.Format);
			if (!AttributeSize)
				return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex layout contains an unsupported attribute format");

			if (Attribute.Offset > Description.Stride || *AttributeSize > Description.Stride - Attribute.Offset)
				return MakeError(GraphicsErrorCode::InvalidVertexLayout, "Vertex attribute extends beyond the declared vertex stride");
		}

		return {};
	}

	GraphicsResult ValidateGraphicsPipelineDescription(const GraphicsPipelineDesc& Description)
	{
		if (!Description.VertexShader)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline requires a vertex shader");

		if (Description.VertexShader->GetDescription().Stage != ShaderStage::Vertex)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline vertex shader handle has the wrong stage");

		if (Description.FragmentShader && Description.FragmentShader->GetDescription().Stage != ShaderStage::Fragment)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline fragment shader handle has the wrong stage");

		if (Description.Topology != PrimitiveTopology::TriangleList)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Only triangle-list topology is currently supported");

		if (Description.ColorFormat != ColorTargetFormat::None && Description.ColorFormat != ColorTargetFormat::Swapchain &&
			Description.ColorFormat != ColorTargetFormat::RGBA8_UNorm &&
			Description.ColorFormat != ColorTargetFormat::RGBA8_Srgb)
		{
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline specifies an unsupported color target format");
		}
		if ((Description.ColorFormat == ColorTargetFormat::None) != !Description.FragmentShader)
			return MakeError(GraphicsErrorCode::InvalidDescription,
				"Depth-only pipelines require no fragment shader and no color target; color pipelines require both");
		if (Description.ColorFormat == ColorTargetFormat::None && Description.Blend.Enabled)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Blending is not meaningful for a depth-only pipeline");
		if (Description.ColorFormat == ColorTargetFormat::None &&
			(!Description.Depth.TestEnabled || !Description.Depth.WriteEnabled))
			return MakeError(GraphicsErrorCode::InvalidDescription,
				"Depth-only pipelines require depth testing and depth writes to be enabled");
		if (!std::isfinite(Description.Rasterizer.DepthBias) || !std::isfinite(Description.Rasterizer.SlopeScaledDepthBias))
			return MakeError(GraphicsErrorCode::InvalidDescription, "Raster depth bias values must be finite");

		if (Description.BindingLayouts.size() > 8)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline exceeds PulseForge's 8-layout limit");

		std::vector<uint32_t> ShaderRegisterSpaces;
		ShaderRegisterSpaces.reserve(Description.BindingLayouts.size());
		for (const BindingLayoutHandle& Layout : Description.BindingLayouts)
		{
			if (!Layout)
				return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline contains an invalid binding-layout handle");
			const uint32_t RegisterSpace = Layout->GetDescription().ShaderRegisterSpace;
			if (std::find(ShaderRegisterSpaces.begin(), ShaderRegisterSpaces.end(), RegisterSpace) != ShaderRegisterSpaces.end())
				return MakeError(GraphicsErrorCode::InvalidDescription,
					"Graphics pipeline binding layouts must use distinct shader register spaces");
			ShaderRegisterSpaces.push_back(RegisterSpace);

			const auto Validation = ValidateBindingLayout(Layout->GetDescription());
			if (!Validation)
				return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDescription, Validation.error().Message });
		}

		if (Description.Rasterizer.Cull != CullMode::None &&
			Description.Rasterizer.Cull != CullMode::Back &&
			Description.Rasterizer.Cull != CullMode::Front)
		{
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline specifies an unsupported culling mode");
		}

		if (!IsValidDepthComparison(Description.Depth.Compare))
			return MakeError(GraphicsErrorCode::InvalidDescription, "Graphics pipeline specifies an unsupported depth comparison operation");

		if (Description.Depth.WriteEnabled && !Description.Depth.TestEnabled)
			return MakeError(GraphicsErrorCode::InvalidDescription, "Depth writes require depth testing to be enabled");

		return ValidateVertexLayout(Description.VertexLayout);
	}

	GraphicsResult ValidateDrawArguments(
		const DrawArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const BufferDesc& VertexBuffer)
	{
		if (Arguments.VertexCount == 0 || Arguments.InstanceCount == 0)
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Draw vertex and instance counts must be non-zero");

		if (VertexBuffer.Usage != BufferUsage::Vertex)
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Non-indexed draw requires a vertex buffer");

		const GraphicsResult LayoutValidation = ValidateVertexLayout(Pipeline.VertexLayout);
		if (!LayoutValidation)
			return LayoutValidation;

		const uint64_t RequiredVertexCount =
			static_cast<uint64_t>(Arguments.FirstVertex) + Arguments.VertexCount;
		const uint64_t AvailableVertexCount = VertexBuffer.ByteSize / Pipeline.VertexLayout.Stride;
		if (RequiredVertexCount > AvailableVertexCount)
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Draw vertex range exceeds the vertex buffer capacity");

		return {};
	}

	GraphicsResult ValidateIndexedBufferDrawArguments(
		const DrawIndexedArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const BufferDesc& VertexBuffer,
		const BufferDesc& IndexBuffer)
	{
		if (Arguments.IndexCount == 0 || Arguments.InstanceCount == 0)
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Indexed draw index and instance counts must be non-zero");

		if (VertexBuffer.Usage != BufferUsage::Vertex || VertexBuffer.ByteSize == 0)
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Indexed draw requires a non-empty vertex buffer");

		if (IndexBuffer.Usage != BufferUsage::Index || IndexBuffer.ByteSize == 0 ||
			IndexBuffer.ByteSize % sizeof(uint32_t) != 0)
		{
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Indexed draw requires a 32-bit index buffer");
		}

		const GraphicsResult LayoutValidation = ValidateVertexLayout(Pipeline.VertexLayout);
		if (!LayoutValidation)
			return LayoutValidation;
		if (VertexBuffer.ByteSize / Pipeline.VertexLayout.Stride == 0)
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Indexed draw vertex buffer is smaller than one vertex");

		const uint64_t RequiredIndexCount = static_cast<uint64_t>(Arguments.FirstIndex) + Arguments.IndexCount;
		if (RequiredIndexCount > IndexBuffer.ByteSize / sizeof(uint32_t))
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Indexed draw range exceeds the index buffer capacity");

		if (Arguments.Scissor)
		{
			const ScissorRect& Scissor = *Arguments.Scissor;
			if (!Pipeline.Rasterizer.ScissorEnabled)
				return MakeError(GraphicsErrorCode::InvalidDrawArguments, "A draw scissor requires scissor testing in the graphics pipeline");
			if (Scissor.Width == 0 || Scissor.Height == 0 ||
				static_cast<uint64_t>(Scissor.X) + Scissor.Width > std::numeric_limits<uint32_t>::max() ||
				static_cast<uint64_t>(Scissor.Y) + Scissor.Height > std::numeric_limits<uint32_t>::max())
			{
				return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Indexed draw scissor must have a valid, non-empty extent");
			}
		}

		return {};
	}

	GraphicsResult ValidateDrawBindingSets(
		const GraphicsPipelineDesc& Pipeline,
		std::span<const BindingSet* const> BindingSets)
	{
		if (BindingSets.size() != Pipeline.BindingLayouts.size())
			return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Draw must provide one binding set for each graphics-pipeline layout");

		for (size_t Index = 0; Index < BindingSets.size(); ++Index)
		{
			if (!BindingSets[Index])
				return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Draw contains an invalid binding-set reference");

			if (&BindingSets[Index]->GetLayout() != Pipeline.BindingLayouts[Index].get())
				return MakeError(GraphicsErrorCode::InvalidDrawArguments, "Draw binding set was created from a different pipeline layout");
		}

		return {};
	}

	std::optional<ScissorRect> IntersectScissorRect(
		const ScissorRect& Scissor,
		uint32_t TargetWidth,
		uint32_t TargetHeight) noexcept
	{
		if (TargetWidth == 0 || TargetHeight == 0 || Scissor.Width == 0 || Scissor.Height == 0)
			return std::nullopt;

		const uint64_t Right = static_cast<uint64_t>(Scissor.X) + Scissor.Width;
		const uint64_t Bottom = static_cast<uint64_t>(Scissor.Y) + Scissor.Height;
		const uint64_t ClippedRight = std::min<uint64_t>(Right, TargetWidth);
		const uint64_t ClippedBottom = std::min<uint64_t>(Bottom, TargetHeight);
		if (Scissor.X >= ClippedRight || Scissor.Y >= ClippedBottom)
			return std::nullopt;

		return ScissorRect{
			Scissor.X,
			Scissor.Y,
			static_cast<uint32_t>(ClippedRight - Scissor.X),
			static_cast<uint32_t>(ClippedBottom - Scissor.Y)
		};
	}
}
