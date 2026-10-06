#pragma once

#include "Core/Core.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <span>
#include <string>

namespace PulseForge
{
	enum class BufferUsage : uint8_t
	{
		Vertex,
		Index,
		Constant
	};

	struct BufferDesc
	{
		uint64_t ByteSize = 0;
		BufferUsage Usage = BufferUsage::Vertex;
		std::string DebugName;
	};

	enum class BufferCreateErrorCode : uint8_t
	{
		InvalidByteSize,
		InitialDataTooLarge,
		UnsupportedUsage,
		UnsupportedFeature,
		BackendFailure
	};

	enum class BufferUpdateErrorCode : uint8_t
	{
		UnsupportedUsage,
		InvalidUpdateSize,
		OutOfBounds,
		InvalidFrameState,
		UnsupportedFeature,
		BackendFailure
	};

	struct BufferCreateError
	{
		BufferCreateErrorCode Code;
		std::string Message;
	};

	struct BufferUpdateError
	{
		BufferUpdateErrorCode Code;
		std::string Message;
	};

	class PULSEFORGE_API Buffer
	{
	public:
		virtual ~Buffer() = default;
		virtual const BufferDesc& GetDescription() const noexcept = 0;
	};

	using BufferHandle = std::unique_ptr<Buffer>;
	using BufferCreateResult = std::expected<BufferHandle, BufferCreateError>;
	using BufferUpdateResult = std::expected<void, BufferUpdateError>;

	[[nodiscard]] PULSEFORGE_API std::expected<void, BufferCreateError> ValidateBufferDescription(
		const BufferDesc& Description,
		size_t InitialDataSize);
	[[nodiscard]] PULSEFORGE_API BufferUpdateResult ValidateBufferUpdate(
		const BufferDesc& Description,
		uint64_t DestinationOffset,
		size_t UpdateSize);
}
