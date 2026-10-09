#include "Core/PulseForgePCH.h"
#include "Renderer/Mesh.h"

#include <algorithm>
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
		BufferHandle IndexBuffer)
		: m_VertexLayout(std::move(VertexLayout)),
		  m_VertexCount(VertexCount),
		  m_IndexCount(IndexCount),
		  m_VertexBuffer(std::move(VertexBuffer)),
		  m_IndexBuffer(std::move(IndexBuffer))
	{
	}

	MeshValidationResult ValidateMeshDescription(const MeshDesc& Description)
	{
		const auto LayoutValidation = ValidateVertexLayout(Description.VertexLayout);
		if (!LayoutValidation)
		{
			return std::unexpected(MeshError{
				MeshErrorCode::InvalidVertexLayout,
				LayoutValidation.error().Message
			});
		}

		if (Description.VertexData.empty())
		{
			return std::unexpected(MeshError{
				MeshErrorCode::InvalidVertexData,
				"Mesh vertex data must not be empty"
			});
		}

		if (Description.VertexData.size() % Description.VertexLayout.Stride != 0)
		{
			return std::unexpected(MeshError{
				MeshErrorCode::InvalidVertexData,
				"Mesh vertex data size must be a multiple of the vertex stride"
			});
		}

		const size_t VertexCount = Description.VertexData.size() / Description.VertexLayout.Stride;
		if (VertexCount == 0 || VertexCount > std::numeric_limits<uint32_t>::max())
		{
			return std::unexpected(MeshError{
				MeshErrorCode::InvalidVertexData,
				"Mesh vertex count exceeds the supported 32-bit draw range"
			});
		}

		if (Description.Indices.size() > std::numeric_limits<uint32_t>::max())
		{
			return std::unexpected(MeshError{
				MeshErrorCode::InvalidIndexData,
				"Mesh index count exceeds the supported 32-bit draw range"
			});
		}

		for (const uint32_t Index : Description.Indices)
		{
			if (Index >= VertexCount)
			{
				return std::unexpected(MeshError{
					MeshErrorCode::InvalidIndexData,
					"Mesh index references a vertex outside the supplied vertex data"
				});
			}
		}

		return MeshCounts{ static_cast<uint32_t>(VertexCount), static_cast<uint32_t>(Description.Indices.size()) };
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
