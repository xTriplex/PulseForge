#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <vector>

namespace PulseForge
{
	enum class VertexSemantic : uint8_t
	{
		Position,
		Color,
		TexCoord,
		Normal,
		Tangent
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

	using MeshValidationResult = std::expected<MeshCounts, MeshError>;

	[[nodiscard]] PULSEFORGE_API std::expected<void, MeshError> ValidateVertexLayoutData(const VertexLayoutDesc& Description);
	[[nodiscard]] PULSEFORGE_API MeshValidationResult ValidateMeshDescription(const MeshDesc& Description);
} // namespace PulseForge
