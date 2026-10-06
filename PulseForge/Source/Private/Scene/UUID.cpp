#include "Core/PulseForgePCH.h"
#include "Scene/UUID.h"

#include <algorithm>
#include <array>
#include <exception>
#include <functional>
#include <mutex>
#include <random>

namespace PulseForge
{
	namespace
	{
		struct RandomSource
		{
			RandomSource()
			{
				std::random_device Device;
				std::seed_seq Seed{ Device(), Device(), Device(), Device(), Device(), Device(), Device(), Device() };
				Generator.seed(Seed);
			}

			std::mutex Mutex;
			std::mt19937_64 Generator;
		};

		int HexValue(char Character)
		{
			if (Character >= '0' && Character <= '9')
				return Character - '0';
			if (Character >= 'a' && Character <= 'f')
				return Character - 'a' + 10;
			if (Character >= 'A' && Character <= 'F')
				return Character - 'A' + 10;
			return -1;
		}

		UUIDError InvalidUUIDFormat()
		{
			return { UUIDErrorCode::InvalidFormat, "UUID must use the canonical 8-4-4-4-12 hexadecimal format" };
		}
	}

	std::string UUID::ToString() const
	{
		constexpr char HexDigits[] = "0123456789abcdef";
		constexpr std::array<size_t, 4> HyphenPositions = { 8, 13, 18, 23 };
		std::string Text;
		Text.reserve(36);

		for (size_t ByteIndex = 0, TextIndex = 0; ByteIndex < 16; ++ByteIndex)
		{
			if (std::find(HyphenPositions.begin(), HyphenPositions.end(), TextIndex) != HyphenPositions.end())
			{
				Text.push_back('-');
				++TextIndex;
			}

			const uint64_t Word = ByteIndex < 8 ? m_High : m_Low;
			const size_t WordByteIndex = ByteIndex % 8;
			const auto Byte = static_cast<uint8_t>((Word >> ((7 - WordByteIndex) * 8)) & 0xff);
			Text.push_back(HexDigits[Byte >> 4]);
			Text.push_back(HexDigits[Byte & 0x0f]);
			TextIndex += 2;
		}

		return Text;
	}

	std::expected<UUID, UUIDError> UUID::Generate()
	{
		try
		{
			static RandomSource Source;
			std::lock_guard Lock(Source.Mutex);
			uint64_t High = Source.Generator();
			uint64_t Low = Source.Generator();

			// Mark generated identities as RFC 4122 version 4 UUIDs.
			High = (High & ~(0xfull << 12)) | (0x4ull << 12);
			Low = (Low & ~(0x3ull << 62)) | (0x2ull << 62);
			return UUID{ High, Low };
		}
		catch (const std::exception& Exception)
		{
			return std::unexpected(UUIDError{
				UUIDErrorCode::RandomGenerationFailed,
				std::string("UUID generation failed: ") + Exception.what()
			});
		}
	}

	std::expected<UUID, UUIDError> UUID::Parse(std::string_view Text)
	{
		if (Text.size() != 36)
			return std::unexpected(InvalidUUIDFormat());

		constexpr std::array<size_t, 4> HyphenPositions = { 8, 13, 18, 23 };
		std::array<uint8_t, 16> Bytes{};
		size_t ByteIndex = 0;
		for (size_t TextIndex = 0; TextIndex < Text.size();)
		{
			if (std::find(HyphenPositions.begin(), HyphenPositions.end(), TextIndex) != HyphenPositions.end())
			{
				if (Text[TextIndex] != '-')
					return std::unexpected(InvalidUUIDFormat());
				++TextIndex;
				continue;
			}

			if (ByteIndex >= Bytes.size() || TextIndex + 1 >= Text.size())
				return std::unexpected(InvalidUUIDFormat());

			const int HighNibble = HexValue(Text[TextIndex]);
			const int LowNibble = HexValue(Text[TextIndex + 1]);
			if (HighNibble < 0 || LowNibble < 0)
				return std::unexpected(InvalidUUIDFormat());

			Bytes[ByteIndex++] = static_cast<uint8_t>((HighNibble << 4) | LowNibble);
			TextIndex += 2;
		}

		if (ByteIndex != Bytes.size())
			return std::unexpected(InvalidUUIDFormat());

		uint64_t High = 0;
		uint64_t Low = 0;
		for (size_t Index = 0; Index < Bytes.size(); ++Index)
		{
			if (Index < 8)
				High = (High << 8) | Bytes[Index];
			else
				Low = (Low << 8) | Bytes[Index];
		}

		return UUID{ High, Low };
	}

	size_t UUIDHash::operator()(const UUID& Value) const noexcept
	{
		const size_t High = std::hash<uint64_t>{}(Value.GetHigh());
		const size_t Low = std::hash<uint64_t>{}(Value.GetLow());
		return High ^ (Low + static_cast<size_t>(0x9e3779b9) + (High << 6) + (High >> 2));
	}
}
