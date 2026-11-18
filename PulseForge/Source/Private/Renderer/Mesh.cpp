#include "Renderer/Mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace PulseForge
{
	namespace
	{
		GraphicsResult MakeGraphicsError(const char* Message)
		{
			return std::unexpected(GraphicsError{ GraphicsErrorCode::InvalidDrawArguments, Message });
		}

		bool IsPipelineVertexLayoutCompatible(const VertexLayoutDesc& PipelineLayout, const VertexLayoutDesc& MeshLayout)
		{
			if (PipelineLayout.Stride != MeshLayout.Stride || PipelineLayout.Attributes.empty() ||
				PipelineLayout.Attributes.size() > MeshLayout.Attributes.size())
				return false;

			for (const VertexAttributeDesc& Attribute : PipelineLayout.Attributes)
			{
				const auto Match = std::find_if(
					MeshLayout.Attributes.begin(),
					MeshLayout.Attributes.end(),
					[&Attribute](const VertexAttributeDesc& Candidate)
					{
						return Candidate.Semantic == Attribute.Semantic &&
							Candidate.Format == Attribute.Format &&
							Candidate.Offset == Attribute.Offset;
					});
				if (Match == MeshLayout.Attributes.end())
					return false;
			}

			return true;
		}
	}

	Mesh::Mesh(
		VertexLayoutDesc VertexLayout,
		uint32_t VertexCount,
		uint32_t IndexCount,
		BufferHandle VertexBuffer,
		BufferHandle IndexBuffer,
		std::span<const std::byte> VertexData)
		: m_VertexLayout(std::move(VertexLayout)),
		  m_VertexCount(VertexCount),
		  m_IndexCount(IndexCount),
		  m_VertexBuffer(std::move(VertexBuffer)),
		  m_IndexBuffer(std::move(IndexBuffer))
	{
		UpdateLocalBounds(VertexData);
	}

	void Mesh::UpdateLocalBounds(std::span<const std::byte> VertexData) noexcept
	{
		m_LocalBounds.reset();
		const auto Position = std::ranges::find(m_VertexLayout.Attributes, VertexSemantic::Position,
			&VertexAttributeDesc::Semantic);
		if (Position == m_VertexLayout.Attributes.end() || m_VertexLayout.Stride == 0 ||
			VertexData.size() != static_cast<size_t>(m_VertexCount) * m_VertexLayout.Stride)
			return;

		size_t PositionComponents = 0;
		switch (Position->Format)
		{
		case VertexFormat::Float2: PositionComponents = 2; break;
		case VertexFormat::Float3: PositionComponents = 3; break;
		case VertexFormat::Float4: PositionComponents = 4; break;
		default: return;
		}
		glm::vec3 Minimum(std::numeric_limits<float>::max());
		glm::vec3 Maximum(std::numeric_limits<float>::lowest());
		for (size_t Index = 0; Index < m_VertexCount; ++Index)
		{
			std::array<float, 4> Value{ 0.0f, 0.0f, 0.0f, 1.0f };
			const size_t Offset = Index * m_VertexLayout.Stride + Position->Offset;
			std::memcpy(Value.data(), VertexData.data() + Offset, PositionComponents * sizeof(float));
			if (!std::isfinite(Value[0]) || !std::isfinite(Value[1]) || !std::isfinite(Value[2]) ||
				(PositionComponents == 4 && (!std::isfinite(Value[3]) || std::abs(Value[3] - 1.0f) > 1.0e-6f))) return;
			Minimum = glm::min(Minimum, glm::vec3(Value[0], Value[1], Value[2]));
			Maximum = glm::max(Maximum, glm::vec3(Value[0], Value[1], Value[2]));
		}
		m_LocalBounds = MeshLocalBounds{ Minimum, Maximum };
	}

	GraphicsResult ValidateMeshDrawArguments(
		const DrawArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const VertexLayoutDesc& MeshLayout,
		const BufferDesc& VertexBuffer)
	{
		if (!IsPipelineVertexLayoutCompatible(Pipeline.VertexLayout, MeshLayout))
			return MakeGraphicsError("Graphics pipeline vertex layout does not match the mesh vertex layout");

		return ValidateDrawArguments(Arguments, Pipeline, VertexBuffer);
	}

	GraphicsResult ValidateIndexedDrawArguments(
		const DrawIndexedArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const VertexLayoutDesc& MeshLayout,
		uint32_t MeshIndexCount)
	{
		if (MeshIndexCount == 0)
			return MakeGraphicsError("Indexed draw requires a mesh with an index buffer");

		if (Arguments.IndexCount == 0 || Arguments.InstanceCount == 0)
			return MakeGraphicsError("Indexed draw index and instance counts must be non-zero");

		if (!IsPipelineVertexLayoutCompatible(Pipeline.VertexLayout, MeshLayout))
			return MakeGraphicsError("Graphics pipeline vertex layout does not match the mesh vertex layout");

		const uint64_t RequiredIndexCount = static_cast<uint64_t>(Arguments.FirstIndex) + Arguments.IndexCount;
		if (RequiredIndexCount > MeshIndexCount)
			return MakeGraphicsError("Indexed draw range exceeds the mesh index buffer capacity");

		return {};
	}
}
