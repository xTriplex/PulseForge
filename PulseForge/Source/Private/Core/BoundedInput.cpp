#include "Core/BoundedInput.h"

#include <algorithm>
#include <array>
#include <exception>

namespace PulseForge::Detail
{
	std::expected<std::string, BoundedInputError> ReadBoundedInput(std::istream& Input, std::size_t MaximumBytes)
	{
		std::string Data;
		std::array<char, 8192> Buffer{};

		try
		{
			Data.reserve(std::min<std::size_t>(MaximumBytes, 64 * 1024));
			while (true)
			{
				const std::size_t RemainingBytes = MaximumBytes - Data.size();
				const std::size_t BytesToRead = RemainingBytes < Buffer.size() ? RemainingBytes + 1 : Buffer.size();
				Input.read(Buffer.data(), static_cast<std::streamsize>(BytesToRead));
				const std::size_t BytesRead = static_cast<std::size_t>(Input.gcount());

				if (BytesRead > RemainingBytes) return std::unexpected(BoundedInputError{ BoundedInputErrorCode::TooLarge });
				Data.append(Buffer.data(), BytesRead);

				if (Input.bad()) return std::unexpected(BoundedInputError{ BoundedInputErrorCode::ReadFailed });
				if (Input.eof()) return Data;
				if (Input.fail() || BytesRead == 0) return std::unexpected(BoundedInputError{ BoundedInputErrorCode::ReadFailed });
			}
		}
		catch (const std::exception&)
		{
			return std::unexpected(BoundedInputError{ BoundedInputErrorCode::ReadFailed });
		}
	}
} // namespace PulseForge::Detail
