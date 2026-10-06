#pragma once

#include "Renderer/Graphics.h"

#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace PulseForge
{
	struct MeshDesc
	{
		VertexLayoutDesc VertexLayout;
		// Input spans are borrowed during CreateMesh and copied into renderer-owned buffers.
		std::span<const std::byte> VertexData;
		// This initial mesh API uses 32-bit indices.
		std::span<const uint32_t> Indices;
		std::string DebugName;
	};

	struct MeshCounts
	{
		uint32_t VertexCount = 0;
		uint32_t IndexCount = 0;
	};

	enum class MeshErrorCode : uint8_t
	{
		InvalidVertexLayout,
		InvalidVertexData,
		InvalidIndexData,
		BufferCreationFailed
	};

	struct MeshError
	{
		MeshErrorCode Code;
		std::string Message;
	};

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
	using MeshValidationResult = std::expected<MeshCounts, MeshError>;

	[[nodiscard]] PULSEFORGE_API MeshValidationResult ValidateMeshDescription(const MeshDesc& Description);
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
