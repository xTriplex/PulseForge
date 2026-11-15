#include "Renderer/RenderDevice.h"

#include <algorithm>

namespace PulseForge
{
	TextureCreateResult RenderDevice::CreateTexture(const TextureDesc& Description, std::span<const std::byte> InitialData)
	{
		const auto Validation = ValidateTextureUpload(Description, InitialData.size());
		if (!Validation) return std::unexpected(Validation.error());
		if (InitialData.empty()) return CreateTexture(Description, std::span<const TextureSubresourceData>{});

		const TextureSubresourceData Subresource{ 0, 0, InitialData, 0 };
		return CreateTexture(Description, std::span(&Subresource, 1));
	}

	MeshCreateResult RenderDevice::CreateMesh(const MeshDesc& Description)
	{
		return CreateMeshInternal(Description, false);
	}

	MeshCreateResult RenderDevice::CreateDynamicMesh(const MeshDesc& Description)
	{
		return CreateMeshInternal(Description, true);
	}

	MeshCreateResult RenderDevice::CreateMeshInternal(const MeshDesc& Description, bool DynamicVertices)
	{
		const MeshValidationResult Validation = ValidateMeshDescription(Description);
		if (!Validation) return std::unexpected(Validation.error());

		const std::string& DebugName = Description.DebugName;
		BufferDesc VertexBufferDescription;
		VertexBufferDescription.ByteSize = Description.VertexData.size();
		VertexBufferDescription.Usage = BufferUsage::Vertex;
		VertexBufferDescription.DebugName = DebugName.empty() ? "PulseForge mesh vertices" : DebugName + " vertices";
		VertexBufferDescription.IsDynamic = DynamicVertices;
		auto CreatedVertexBuffer = CreateBuffer(VertexBufferDescription, Description.VertexData);
		if (!CreatedVertexBuffer)
		{
			return std::unexpected(MeshError{
				MeshErrorCode::BufferCreationFailed, "Mesh vertex buffer creation failed: " + CreatedVertexBuffer.error().Message });
		}

		BufferHandle IndexBuffer;
		if (!Description.Indices.empty())
		{
			const std::span<const std::byte> IndexData = std::as_bytes(Description.Indices);
			BufferDesc IndexBufferDescription;
			IndexBufferDescription.ByteSize = IndexData.size();
			IndexBufferDescription.Usage = BufferUsage::Index;
			IndexBufferDescription.DebugName = DebugName.empty() ? "PulseForge mesh indices" : DebugName + " indices";
			auto CreatedIndexBuffer = CreateBuffer(IndexBufferDescription, IndexData);
			if (!CreatedIndexBuffer)
			{
				return std::unexpected(MeshError{
					MeshErrorCode::BufferCreationFailed, "Mesh index buffer creation failed: " + CreatedIndexBuffer.error().Message });
			}
			IndexBuffer = std::move(CreatedIndexBuffer.value());
		}

		return MeshHandle(new Mesh(Description.VertexLayout, Validation->VertexCount, Validation->IndexCount,
			std::move(CreatedVertexBuffer.value()), std::move(IndexBuffer)));
	}

	BufferUpdateResult RenderDevice::UpdateDynamicMeshVertices(
		Mesh& Target,
		const VertexLayoutDesc& VertexLayout,
		std::span<const std::byte> VertexData)
	{
		const BufferDesc& VertexBuffer = Target.GetVertexBuffer().GetDescription();
		if (!VertexBuffer.IsDynamic || VertexBuffer.Usage != BufferUsage::Vertex)
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::UnsupportedUsage,
				"Only meshes created with CreateDynamicMesh may update vertex data"
			});

		const VertexLayoutDesc& ExistingLayout = Target.GetVertexLayout();
		const bool SameAttributes = VertexLayout.Attributes.size() == ExistingLayout.Attributes.size() &&
			std::equal(VertexLayout.Attributes.begin(), VertexLayout.Attributes.end(), ExistingLayout.Attributes.begin(),
				[](const VertexAttributeDesc& First, const VertexAttributeDesc& Second)
				{
					return First.Semantic == Second.Semantic && First.Format == Second.Format && First.Offset == Second.Offset;
				});
		const bool SameLayout = VertexLayout.Stride == ExistingLayout.Stride && SameAttributes;
		if (!SameLayout)
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::InvalidUpdateSize,
				"Dynamic mesh updates must preserve the mesh vertex layout"
			});

		if (const auto LayoutValidation = ValidateVertexLayoutData(VertexLayout); !LayoutValidation)
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::InvalidUpdateSize,
				"Dynamic mesh update layout is invalid: " + LayoutValidation.error().Message
			});

		const uint64_t ExpectedByteSize = static_cast<uint64_t>(Target.GetVertexCount()) * VertexLayout.Stride;
		if (VertexData.size() != ExpectedByteSize || VertexData.size() != VertexBuffer.ByteSize)
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::InvalidUpdateSize,
				"Dynamic mesh update must replace all vertices without changing their count"
			});

		if (const auto Validation = ValidateBufferUpdate(VertexBuffer, 0, VertexData.size()); !Validation)
			return std::unexpected(Validation.error());
		return WriteBuffer(Target.GetVertexBuffer(), 0, VertexData);
	}
} // namespace PulseForge
