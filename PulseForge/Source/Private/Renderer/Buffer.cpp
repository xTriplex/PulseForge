#include "Core/PulseForgePCH.h"
#include "Renderer/Buffer.h"

namespace PulseForge
{
	std::expected<void, BufferCreateError> ValidateBufferDescription(
		const BufferDesc& Description,
		size_t InitialDataSize)
	{
		if (Description.ByteSize == 0)
			return std::unexpected(BufferCreateError{
				BufferCreateErrorCode::InvalidByteSize,
				"A renderer buffer must have a non-zero byte size"
			});

		if (static_cast<uint64_t>(InitialDataSize) > Description.ByteSize)
			return std::unexpected(BufferCreateError{
				BufferCreateErrorCode::InitialDataTooLarge,
				"Initial buffer data exceeds the declared buffer size"
			});

		switch (Description.Usage)
		{
			case BufferUsage::Vertex:
			case BufferUsage::Index:
			case BufferUsage::Constant:
				if (Description.Usage == BufferUsage::Constant && Description.ByteSize % 16 != 0)
					return std::unexpected(BufferCreateError{
						BufferCreateErrorCode::InvalidByteSize,
						"Constant buffer size must be a multiple of 16 bytes"
					});
				return {};
			default:
				return std::unexpected(BufferCreateError{
					BufferCreateErrorCode::UnsupportedUsage,
					"The requested renderer buffer usage is not supported"
				});
		}
	}

	BufferUpdateResult ValidateBufferUpdate(
		const BufferDesc& Description,
		uint64_t DestinationOffset,
		size_t UpdateSize)
	{
		if (Description.Usage != BufferUsage::Constant)
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::UnsupportedUsage,
				"The current renderer only supports updates to constant buffers"
			});

		if (UpdateSize == 0 || DestinationOffset % 4 != 0 || UpdateSize % 4 != 0)
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::InvalidUpdateSize,
				"Constant buffer updates must be non-empty and 4-byte aligned"
			});

		if (DestinationOffset > Description.ByteSize ||
			static_cast<uint64_t>(UpdateSize) > Description.ByteSize - DestinationOffset)
		{
			return std::unexpected(BufferUpdateError{
				BufferUpdateErrorCode::OutOfBounds,
				"Constant buffer update extends beyond the declared buffer size"
			});
		}

		return {};
	}
}
