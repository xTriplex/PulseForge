#pragma once

#include "Core/Core.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace PulseForge
{
	enum class UUIDErrorCode : uint8_t
	{
		InvalidFormat,
		RandomGenerationFailed
	};

	struct UUIDError
	{
		UUIDErrorCode Code;
		std::string Message;
	};

	class PULSEFORGE_API UUID final
	{
	public:
		constexpr UUID() = default;
		constexpr UUID(uint64_t High, uint64_t Low) : m_High(High), m_Low(Low) {}

		[[nodiscard]] constexpr uint64_t GetHigh() const noexcept { return m_High; }
		[[nodiscard]] constexpr uint64_t GetLow() const noexcept { return m_Low; }
		[[nodiscard]] constexpr bool IsNil() const noexcept { return m_High == 0 && m_Low == 0; }
		[[nodiscard]] PULSEFORGE_API std::string ToString() const;
		[[nodiscard]] PULSEFORGE_API static std::expected<UUID, UUIDError> Generate();
		[[nodiscard]] PULSEFORGE_API static std::expected<UUID, UUIDError> Parse(std::string_view Text);

		friend constexpr bool operator==(const UUID&, const UUID&) = default;
		friend constexpr auto operator<=>(const UUID&, const UUID&) = default;

	private:
		uint64_t m_High = 0;
		uint64_t m_Low = 0;
	};

	struct PULSEFORGE_API UUIDHash
	{
		[[nodiscard]] size_t operator()(const UUID& Value) const noexcept;
	};
}
