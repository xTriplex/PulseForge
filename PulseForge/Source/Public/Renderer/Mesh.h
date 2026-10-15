#pragma once

#include "Renderer/Graphics.h"
#include "Renderer/MeshData.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace PulseForge
{
	class PULSEFORGE_API Mesh final
	{
	public:
		Mesh(const Mesh&) = delete;
		Mesh& operator=(const Mesh&) = delete;
		Mesh(Mesh&&) = delete;
		Mesh& operator=(Mesh&&) = delete;

		[[nodiscard]] const VertexLayoutDesc& GetVertexLayout() const noexcept { return m_VertexLayout; }
		[[nodiscard]] uint32_t GetVertexCount() const noexcept { return m_VertexCount; }
		[[nodiscard]] uint32_t GetIndexCount() const noexcept { return m_IndexCount; }
		[[nodiscard]] bool HasIndices() const noexcept { return m_IndexBuffer != nullptr; }

	private:
		friend class Application;
		friend class RenderDevice;
		[[nodiscard]] const Buffer& GetVertexBuffer() const noexcept { return *m_VertexBuffer; }
		[[nodiscard]] const Buffer* GetIndexBuffer() const noexcept { return m_IndexBuffer.get(); }

		Mesh(
			VertexLayoutDesc VertexLayout,
			uint32_t VertexCount,
			uint32_t IndexCount,
			BufferHandle VertexBuffer,
			BufferHandle IndexBuffer);

		VertexLayoutDesc m_VertexLayout;
		uint32_t m_VertexCount = 0;
		uint32_t m_IndexCount = 0;
		BufferHandle m_VertexBuffer;
		BufferHandle m_IndexBuffer;
	};

	using MeshHandle = std::unique_ptr<Mesh>;
	using MeshCreateResult = std::expected<MeshHandle, MeshError>;
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateMeshDrawArguments(
		const DrawArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const VertexLayoutDesc& MeshLayout,
		const BufferDesc& VertexBuffer);
	[[nodiscard]] PULSEFORGE_API GraphicsResult ValidateIndexedDrawArguments(
		const DrawIndexedArguments& Arguments,
		const GraphicsPipelineDesc& Pipeline,
		const VertexLayoutDesc& MeshLayout,
		uint32_t MeshIndexCount);
}
