#include "Renderer/RenderDevice.h"

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
		const MeshValidationResult Validation = ValidateMeshDescription(Description);
		if (!Validation) return std::unexpected(Validation.error());

		const std::string& DebugName = Description.DebugName;
		BufferDesc VertexBufferDescription;
		VertexBufferDescription.ByteSize = Description.VertexData.size();
		VertexBufferDescription.Usage = BufferUsage::Vertex;
		VertexBufferDescription.DebugName = DebugName.empty() ? "PulseForge mesh vertices" : DebugName + " vertices";
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
} // namespace PulseForge
