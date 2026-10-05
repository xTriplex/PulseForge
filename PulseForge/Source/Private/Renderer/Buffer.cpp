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
}
