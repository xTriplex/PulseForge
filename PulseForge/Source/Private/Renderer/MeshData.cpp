#include "Renderer/MeshData.h"

#include <array>
#include <limits>
#include <optional>

namespace PulseForge
{
	namespace
	{
		std::optional<uint32_t> GetVertexFormatSize(VertexFormat Format)
		{
			switch (Format)
			{
			case VertexFormat::Float2:
				return static_cast<uint32_t>(sizeof(float) * 2);
			case VertexFormat::Float3:
				return static_cast<uint32_t>(sizeof(float) * 3);
			case VertexFormat::Float4:
				return static_cast<uint32_t>(sizeof(float) * 4);
			default:
				return std::nullopt;
			}
		}

		bool IsValidSemantic(VertexSemantic Semantic)
		{
			return Semantic == VertexSemantic::Position || Semantic == VertexSemantic::Color || Semantic == VertexSemantic::TexCoord ||
				   Semantic == VertexSemantic::Normal || Semantic == VertexSemantic::Tangent;
		}

		std::unexpected<MeshError> MakeLayoutError(const char* Message)
		{
			return std::unexpected(MeshError{ MeshErrorCode::InvalidVertexLayout, Message });
		}
	} // namespace

	std::expected<void, MeshError> ValidateVertexLayoutData(const VertexLayoutDesc& Description)
	{
		if (Description.Stride == 0) return MakeLayoutError("Vertex layout stride must be non-zero");

		if (Description.Attributes.empty()) return MakeLayoutError("Vertex layout must contain at least one attribute");

		if (Description.Attributes.size() > 16) return MakeLayoutError("Vertex layout exceeds PulseForge's 16-attribute limit");

		std::array<bool, 5> SeenSemantics = {};
		for (const VertexAttributeDesc& Attribute : Description.Attributes)
		{
			if (!IsValidSemantic(Attribute.Semantic)) return MakeLayoutError("Vertex layout contains an unsupported semantic");

			const size_t SemanticIndex = static_cast<size_t>(Attribute.Semantic);
			if (SeenSemantics[SemanticIndex]) return MakeLayoutError("Vertex layout contains a duplicate semantic");
			SeenSemantics[SemanticIndex] = true;

			const auto AttributeSize = GetVertexFormatSize(Attribute.Format);
			if (!AttributeSize) return MakeLayoutError("Vertex layout contains an unsupported attribute format");

			if (Attribute.Offset > Description.Stride || *AttributeSize > Description.Stride - Attribute.Offset)
				return MakeLayoutError("Vertex attribute extends beyond the declared vertex stride");
		}

		return {};
	}

	MeshValidationResult ValidateMeshDescription(const MeshDesc& Description)
	{
		const auto LayoutValidation = ValidateVertexLayoutData(Description.VertexLayout);
		if (!LayoutValidation) return std::unexpected(LayoutValidation.error());

		if (Description.VertexData.empty())
		{
			return std::unexpected(MeshError{ MeshErrorCode::InvalidVertexData, "Mesh vertex data must not be empty" });
		}

		if (Description.VertexData.size() % Description.VertexLayout.Stride != 0)
		{
			return std::unexpected(
				MeshError{ MeshErrorCode::InvalidVertexData, "Mesh vertex data size must be a multiple of the vertex stride" });
		}

		const size_t VertexCount = Description.VertexData.size() / Description.VertexLayout.Stride;
		if (VertexCount == 0 || VertexCount > std::numeric_limits<uint32_t>::max())
		{
			return std::unexpected(
				MeshError{ MeshErrorCode::InvalidVertexData, "Mesh vertex count exceeds the supported 32-bit draw range" });
		}

		if (Description.Indices.size() > std::numeric_limits<uint32_t>::max())
		{
			return std::unexpected(
				MeshError{ MeshErrorCode::InvalidIndexData, "Mesh index count exceeds the supported 32-bit draw range" });
		}

		for (const uint32_t Index : Description.Indices)
		{
			if (Index >= VertexCount)
			{
				return std::unexpected(
					MeshError{ MeshErrorCode::InvalidIndexData, "Mesh index references a vertex outside the supplied vertex data" });
			}
		}

		return MeshCounts{ static_cast<uint32_t>(VertexCount), static_cast<uint32_t>(Description.Indices.size()) };
	}
} // namespace PulseForge
