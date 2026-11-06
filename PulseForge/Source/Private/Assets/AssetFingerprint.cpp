#include "Assets/AssetImportProvenance.h"

#define XXH_INLINE_ALL
#include <tracy_xxhash.h>

static_assert(XXH_VERSION_NUMBER == 802, "Asset fingerprints require the pinned xxHash 0.8.2 implementation");

#include <array>
#include <stdexcept>

namespace PulseForge
{
	namespace
	{
		constexpr std::string_view FingerprintAlgorithm = "xxh3-128-v0.8.2";

		std::string HexDigest(XXH128_hash_t Digest)
		{
			constexpr char Hex[] = "0123456789abcdef";
			std::string Result(32, '0');
			for (size_t Index = 0; Index < 16; ++Index)
			{
				const uint64_t Value = Index < 8 ? Digest.high64 : Digest.low64;
				const size_t Byte = Index < 8 ? Index : Index - 8;
				const uint8_t NibblePair = static_cast<uint8_t>(Value >> ((7 - Byte) * 8));
				Result[Index * 2] = Hex[NibblePair >> 4];
				Result[Index * 2 + 1] = Hex[NibblePair & 0x0f];
			}
			return Result;
		}
	}

	struct AssetFingerprintBuilder::State
	{
		XXH3_state_t* Value = XXH3_createState();
		bool Finished = false;
		~State() { if (Value) (void)XXH3_freeState(Value); }
	};

	AssetFingerprintBuilder::AssetFingerprintBuilder(std::string_view Domain)
		: m_State(std::make_unique<State>())
	{
		if (!m_State->Value || XXH3_128bits_reset(m_State->Value) != XXH_OK)
			throw std::runtime_error("Could not initialize asset fingerprint state");
		UpdateText("domain", Domain);
	}

	AssetFingerprintBuilder::~AssetFingerprintBuilder() = default;
	AssetFingerprintBuilder::AssetFingerprintBuilder(AssetFingerprintBuilder&&) noexcept = default;
	AssetFingerprintBuilder& AssetFingerprintBuilder::operator=(AssetFingerprintBuilder&&) noexcept = default;

	void AssetFingerprintBuilder::UpdateBytes(std::string_view Label, std::span<const std::byte> Bytes)
	{
		if (!m_State || !m_State->Value || m_State->Finished) throw std::logic_error("Fingerprint builder is not writable");
		const uint64_t LabelLength = Label.size();
		const uint64_t DataLength = Bytes.size();
		std::array<std::byte, sizeof(uint64_t)> EncodedLength{};
		for (size_t Index = 0; Index < EncodedLength.size(); ++Index)
			EncodedLength[Index] = static_cast<std::byte>((LabelLength >> (Index * 8)) & 0xff);
		(void)XXH3_128bits_update(m_State->Value, EncodedLength.data(), EncodedLength.size());
		if (!Label.empty()) (void)XXH3_128bits_update(m_State->Value, Label.data(), Label.size());
		for (size_t Index = 0; Index < EncodedLength.size(); ++Index)
			EncodedLength[Index] = static_cast<std::byte>((DataLength >> (Index * 8)) & 0xff);
		(void)XXH3_128bits_update(m_State->Value, EncodedLength.data(), EncodedLength.size());
		if (!Bytes.empty()) (void)XXH3_128bits_update(m_State->Value, Bytes.data(), Bytes.size());
	}

	void AssetFingerprintBuilder::UpdateText(std::string_view Label, std::string_view Text)
	{
		UpdateBytes(Label, std::as_bytes(std::span(Text.data(), Text.size())));
	}

	AssetContentFingerprint AssetFingerprintBuilder::Finish()
	{
		if (!m_State || !m_State->Value || m_State->Finished) throw std::logic_error("Fingerprint builder was already finished");
		m_State->Finished = true;
		return { std::string(FingerprintAlgorithm), HexDigest(XXH3_128bits_digest(m_State->Value)) };
	}
} // namespace PulseForge
