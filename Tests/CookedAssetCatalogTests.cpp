#include "Assets/CookedAssetCatalog.h"
#include "Scene/UUID.h"
#include "TestSupport.h"

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace
{
	using namespace PulseForge;

	constexpr std::string_view PayloadRelativePath = "Payloads/Startup.scene.bin";
	constexpr std::string_view PayloadText = "cooked scene payload";
	const CookedAssetCompatibility Compatibility{ "test-target", "test-renderer", "test-runtime-abi" };

	struct TemporaryCookRoot final
	{
		std::filesystem::path Root;
		UUID Project;
		AssetID StartupScene;
		std::vector<std::byte> Payload;

		TemporaryCookRoot()
		{
			std::error_code Error;
			const auto Identifier = UUID::Generate();
			const auto Temporary = std::filesystem::canonical(std::filesystem::temp_directory_path(Error), Error);
			if (!Identifier || Error) return;
			Project = *Identifier;
			const auto Scene = UUID::Generate();
			if (!Scene) return;
			StartupScene = *Scene;
			Root = Temporary / std::filesystem::u8path("PulseForge Cook Catalog \xCE\xA9 " + Project.ToString());
			Payload = Bytes(PayloadText);
			std::filesystem::create_directories(Root / "Payloads", Error);
			if (Error) Root.clear();
			if (!Root.empty()) WritePayload(Payload);
		}

		~TemporaryCookRoot()
		{
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}

		static std::vector<std::byte> Bytes(std::string_view Text)
		{
			const auto Span = std::as_bytes(std::span(Text.data(), Text.size()));
			return { Span.begin(), Span.end() };
		}

		void WritePayload(const std::vector<std::byte>& Value, std::string_view RelativePath = PayloadRelativePath) const
		{
			const auto Path = Root / std::filesystem::u8path(std::string(RelativePath));
			std::filesystem::create_directories(Path.parent_path());
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Value.data()), static_cast<std::streamsize>(Value.size()));
		}

		void WriteManifest(std::string_view Text) const
		{
			std::ofstream Output(Root / CookedAssetCatalog::ManifestFileName, std::ios::binary | std::ios::trunc);
			Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
		}

		std::string ReadManifest() const
		{
			std::ifstream Input(Root / CookedAssetCatalog::ManifestFileName, std::ios::binary);
			return { std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
		}

		std::expected<std::string, CookedAssetCatalogError> Serialize() const
		{
			const CookedAssetDigest Digest = CookedAssetCatalog::ComputePayloadDigest(Payload);
			CookedAssetEntry Entry{ StartupScene, CookedAssetProductKind::Scene, {}, std::string(PayloadRelativePath),
				1, Payload.size(), Digest.Algorithm, Digest.Value, {} };
			return CookedAssetCatalog::SerializeManifest(Project, StartupScene, Compatibility, {}, { std::move(Entry) });
		}

		std::expected<std::shared_ptr<const CookedAssetCatalog>, CookedAssetCatalogError> Open(
			const CookedAssetCompatibility& Expected = Compatibility) const
		{
			return CookedAssetCatalog::Open(Root, Expected);
		}
	};

	bool ReplaceOnce(std::string& Text, std::string_view Search, std::string_view Replacement)
	{
		const size_t Position = Text.find(Search);
		if (Position == std::string::npos) return false;
		Text.replace(Position, Search.size(), Replacement);
		return true;
	}

	bool DuplicateOnlyEntry(std::string& Manifest)
	{
		const size_t EntriesField = Manifest.find("\"entries\": [");
		if (EntriesField == std::string::npos) return false;
		const size_t EntryBegin = Manifest.find('{', EntriesField);
		const size_t EntriesEnd = Manifest.find("\n  ]", EntryBegin);
		if (EntryBegin == std::string::npos || EntriesEnd == std::string::npos) return false;
		const std::string Entry = Manifest.substr(EntryBegin, EntriesEnd - EntryBegin);
		Manifest.replace(EntryBegin, EntriesEnd - EntryBegin, Entry + ",\n" + Entry);
		return true;
	}

	void TestRelocationLookupAndDigest(TestRunner& Tests)
	{
		TemporaryCookRoot Fixture;
		PF_CHECK(Tests, !Fixture.Root.empty());
		if (Fixture.Root.empty()) return;
		auto Manifest = Fixture.Serialize();
		PF_CHECK(Tests, Manifest.has_value());
		if (!Manifest) return;
		Fixture.WriteManifest(*Manifest);

		auto RelocatedRoot = Fixture.Root;
		RelocatedRoot += " relocated";
		std::error_code Error;
		std::filesystem::rename(Fixture.Root, RelocatedRoot, Error);
		PF_CHECK(Tests, !Error);
		if (Error) return;
		Fixture.Root = RelocatedRoot;

		auto Catalog = Fixture.Open();
		PF_CHECK(Tests, Catalog.has_value());
		if (!Catalog) return;
		PF_CHECK(Tests, (*Catalog)->GetContentRoot() == std::filesystem::absolute(Fixture.Root).lexically_normal());
		const CookedAssetEntry* Entry = (*Catalog)->Find(Fixture.StartupScene, CookedAssetProductKind::Scene);
		PF_CHECK(Tests, Entry != nullptr);
		if (!Entry) return;
		const auto Read = (*Catalog)->ReadPayload(*Entry);
		PF_CHECK(Tests, Read.has_value() && *Read == Fixture.Payload);
		PF_CHECK(Tests, (*Catalog)->Find(Fixture.StartupScene, CookedAssetProductKind::Material) == nullptr);
	}

	void TestCorruptAndTruncatedPayloads(TestRunner& Tests)
	{
		TemporaryCookRoot Fixture;
		PF_CHECK(Tests, !Fixture.Root.empty());
		if (Fixture.Root.empty()) return;
		auto Manifest = Fixture.Serialize();
		PF_CHECK(Tests, Manifest.has_value());
		if (!Manifest) return;
		Fixture.WriteManifest(*Manifest);
		auto Catalog = Fixture.Open();
		PF_CHECK(Tests, Catalog.has_value());
		if (!Catalog) return;
		const auto* Entry = (*Catalog)->Find(Fixture.StartupScene, CookedAssetProductKind::Scene);
		PF_CHECK(Tests, Entry != nullptr);
		if (!Entry) return;

		auto Corrupt = Fixture.Payload;
		Corrupt.front() ^= std::byte{ 0x20 };
		Fixture.WritePayload(Corrupt);
		const auto CorruptRead = (*Catalog)->ReadPayload(*Entry);
		PF_CHECK(Tests, !CorruptRead && CorruptRead.error().Code == CookedAssetCatalogErrorCode::PayloadDigestMismatch);

		Fixture.WritePayload(std::vector<std::byte>(Fixture.Payload.begin(), Fixture.Payload.end() - 1));
		const auto TruncatedRead = (*Catalog)->ReadPayload(*Entry);
		PF_CHECK(Tests, !TruncatedRead && TruncatedRead.error().Code == CookedAssetCatalogErrorCode::PayloadTooLarge);
		const auto TruncatedOpen = Fixture.Open();
		PF_CHECK(Tests, !TruncatedOpen && TruncatedOpen.error().Code == CookedAssetCatalogErrorCode::PayloadReadFailed);
	}

	void TestManifestRejections(TestRunner& Tests)
	{
		TemporaryCookRoot Fixture;
		PF_CHECK(Tests, !Fixture.Root.empty());
		if (Fixture.Root.empty()) return;
		auto Baseline = Fixture.Serialize();
		PF_CHECK(Tests, Baseline.has_value());
		if (!Baseline) return;

		Fixture.WriteManifest("{\"format\":\"PulseForgeCookManifest\"");
		const auto Truncated = Fixture.Open();
		PF_CHECK(Tests, !Truncated && Truncated.error().Code == CookedAssetCatalogErrorCode::InvalidManifest);

		std::string Unsupported = *Baseline;
		PF_CHECK(Tests, ReplaceOnce(Unsupported, "\"version\": 1", "\"version\": 77"));
		Fixture.WriteManifest(Unsupported);
		const auto Version = Fixture.Open();
		PF_CHECK(Tests, !Version && Version.error().Code == CookedAssetCatalogErrorCode::UnsupportedVersion);

		Fixture.WriteManifest(*Baseline);
		auto Incompatible = Fixture.Open({ "other-target", Compatibility.RendererBackend, Compatibility.RuntimeABI });
		PF_CHECK(Tests, !Incompatible && Incompatible.error().Code == CookedAssetCatalogErrorCode::IncompatibleTarget);

		std::string Duplicate = *Baseline;
		PF_CHECK(Tests, DuplicateOnlyEntry(Duplicate));
		Fixture.WriteManifest(Duplicate);
		const auto DuplicateIdentity = Fixture.Open();
		PF_CHECK(Tests, !DuplicateIdentity && DuplicateIdentity.error().Code == CookedAssetCatalogErrorCode::DuplicateIdentity);

		std::string EscapingPath = *Baseline;
		PF_CHECK(Tests, ReplaceOnce(EscapingPath, PayloadRelativePath, "../Startup.scene.bin"));
		Fixture.WriteManifest(EscapingPath);
		const auto Escaping = Fixture.Open();
		PF_CHECK(Tests, !Escaping && Escaping.error().Code == CookedAssetCatalogErrorCode::InvalidManifest);

		Fixture.WriteManifest(*Baseline);
		std::error_code Error;
		std::filesystem::remove(Fixture.Root / std::filesystem::u8path(std::string(PayloadRelativePath)), Error);
		PF_CHECK(Tests, !Error);
		const auto Missing = Fixture.Open();
		PF_CHECK(Tests, !Missing && Missing.error().Code == CookedAssetCatalogErrorCode::MissingPayload);
	}

	void TestLinkedPayloadRejectedWhenSupported(TestRunner& Tests)
	{
		TemporaryCookRoot Fixture;
		PF_CHECK(Tests, !Fixture.Root.empty());
		if (Fixture.Root.empty()) return;
		auto Manifest = Fixture.Serialize();
		PF_CHECK(Tests, Manifest.has_value());
		if (!Manifest) return;
		Fixture.WriteManifest(*Manifest);

		const auto Original = Fixture.Root / std::filesystem::u8path(std::string(PayloadRelativePath));
		const auto Linked = Fixture.Root / "Payloads" / "Linked.scene.bin";
		std::error_code Error;
		std::filesystem::create_symlink(Original.filename(), Linked, Error);
		if (Error)
		{
			std::cout << "SKIP: this host does not permit symlink creation for the cooked catalog test.\n";
			return;
		}
		std::string LinkedManifest = *Manifest;
		PF_CHECK(Tests, ReplaceOnce(LinkedManifest, PayloadRelativePath, "Payloads/Linked.scene.bin"));
		Fixture.WriteManifest(LinkedManifest);
		const auto Catalog = Fixture.Open();
		PF_CHECK(Tests, !Catalog && Catalog.error().Code == CookedAssetCatalogErrorCode::UnsafePayloadPath);
	}
}

int main()
{
	TestRunner Tests;
	TestRelocationLookupAndDigest(Tests);
	TestCorruptAndTruncatedPayloads(Tests);
	TestManifestRejections(Tests);
	TestLinkedPayloadRejectedWhenSupported(Tests);
	return Tests.Finish();
}
