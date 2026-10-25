#include "TestSupport.h"

#include "Assets/DerivedDataArtifactStore.h"
#include "Scene/UUID.h"

#include <array>
#include <cstddef>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace PulseForge
{
	struct DerivedDataArtifactStoreTestAccess
	{
		static std::filesystem::path GetPath(
			const std::filesystem::path& Root,
			const DerivedDataArtifactKey& Key)
		{
			AssetFingerprintBuilder Builder("PulseForge.DerivedDataArtifactPath.v1");
			Builder.UpdateText("product-kind", Key.ProductKind);
			Builder.UpdateText("recipe-algorithm", Key.RecipeDigest.Algorithm);
			Builder.UpdateText("recipe-digest", Key.RecipeDigest.DigestHex);
			std::array<std::byte, 4> SchemaBytes{};
			for (size_t Index = 0; Index < SchemaBytes.size(); ++Index)
				SchemaBytes[Index] = static_cast<std::byte>((Key.ProductSchemaVersion >> (Index * 8)) & 0xff);
			Builder.UpdateBytes("product-schema", SchemaBytes);
			return Root / (Builder.Finish().DigestHex + ".pfartifact");
		}

		static DerivedDataArtifactWriteResult WriteFailingReplacement(
			DerivedDataArtifactStore& Store,
			const DerivedDataArtifactKey& Key,
			std::span<const std::byte> Payload)
		{
			return Store.WriteWithReplacement(Key, Payload,
				[](const std::filesystem::path&, const std::filesystem::path&)
				{
					return std::expected<void, std::string>(std::unexpected("injected replacement failure"));
				});
		}
	};
}

namespace
{
	class TemporaryArtifactRoot final
	{
	public:
		TemporaryArtifactRoot()
		{
			auto Id = PulseForge::UUID::Generate();
			if (Id)
				Path = std::filesystem::temp_directory_path() / ("PulseForgeArtifactTest-" + Id->ToString());
		}
		~TemporaryArtifactRoot()
		{
			std::error_code Error;
			std::filesystem::remove_all(Path, Error);
		}
		std::filesystem::path Path;
	};

	PulseForge::DerivedDataArtifactKey MakeKey(std::string Kind, uint32_t Schema = 1, std::string_view Recipe = "recipe-a")
	{
		const auto Bytes = std::as_bytes(std::span(Recipe.data(), Recipe.size()));
		return { std::move(Kind), PulseForge::ComputeAssetContentFingerprint(Bytes), Schema };
	}

	std::vector<std::byte> Bytes(std::string_view Text)
	{
		const auto View = std::as_bytes(std::span(Text.data(), Text.size()));
		return { View.begin(), View.end() };
	}

	bool WriteBytes(const std::filesystem::path& Path, std::span<const std::byte> Data)
	{
		std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
		if (!Output)
			return false;
		Output.write(reinterpret_cast<const char*>(Data.data()), static_cast<std::streamsize>(Data.size()));
		Output.close();
		return static_cast<bool>(Output);
	}

	std::vector<std::byte> ReadBytes(const std::filesystem::path& Path)
	{
		std::ifstream Input(Path, std::ios::binary);
		const std::vector<char> Characters{ std::istreambuf_iterator<char>(Input), std::istreambuf_iterator<char>() };
		std::vector<std::byte> Result;
		Result.reserve(Characters.size());
		for (char Character : Characters)
			Result.push_back(static_cast<std::byte>(static_cast<unsigned char>(Character)));
		return Result;
	}
}

void RunDerivedDataArtifactStoreTests(TestRunner& Tests)
{
	using namespace PulseForge;
	TemporaryArtifactRoot Root;
	PF_CHECK(Tests, !Root.Path.empty());
	DerivedDataArtifactStore Store({ Root.Path, true, 1024 * 1024, 1024, 8 });
	const DerivedDataArtifactKey MeshKey = MakeKey("static-mesh");
	const std::vector<std::byte> Payload = Bytes("mesh payload v1");

	const auto Cold = Store.Read(MeshKey);
	PF_CHECK(Tests, Cold.Status == DerivedDataArtifactReadStatus::Miss);
	const auto Written = Store.Write(MeshKey, Payload);
	PF_CHECK(Tests, Written.Status == DerivedDataArtifactWriteStatus::Written);
	const auto Warm = Store.Read(MeshKey);
	PF_CHECK(Tests, Warm.Status == DerivedDataArtifactReadStatus::Hit);
	PF_CHECK(Tests, Warm.Payload == Payload);
	PF_CHECK(Tests, !Warm.EntryChecksum.Algorithm.empty());
	const std::vector<std::byte> RepairedPayload = Bytes("repaired decoded mesh payload");
	const auto Repaired = Store.ReplaceRejectedProduct(MeshKey, Warm.EntryChecksum, RepairedPayload);
	PF_CHECK(Tests, Repaired.Status == DerivedDataArtifactWriteStatus::Written);
	PF_CHECK(Tests, Store.Read(MeshKey).Payload == RepairedPayload);
	PF_CHECK(Tests, Store.ReplaceRejectedProduct(MeshKey, Warm.EntryChecksum, Payload).Status ==
		DerivedDataArtifactWriteStatus::Rejected);
	PF_CHECK(Tests, Store.Read(MeshKey).Payload == RepairedPayload);
	const auto RepairedRead = Store.Read(MeshKey);
	const std::vector<std::byte> NewerPayload = Bytes("newer valid concurrent product");
		PF_CHECK(Tests, std::filesystem::remove(DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, MeshKey)));
		PF_CHECK(Tests, Store.Write(MeshKey, NewerPayload).Status == DerivedDataArtifactWriteStatus::Written);
	PF_CHECK(Tests, Store.ReplaceRejectedProduct(MeshKey, RepairedRead.EntryChecksum, RepairedPayload).Status ==
		DerivedDataArtifactWriteStatus::Rejected);
	PF_CHECK(Tests, Store.Read(MeshKey).Payload == NewerPayload);
	std::error_code RestoreError;
	std::filesystem::remove(DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, MeshKey), RestoreError);
	PF_CHECK(Tests, !RestoreError);
	PF_CHECK(Tests, Store.Write(MeshKey, Payload).Status == DerivedDataArtifactWriteStatus::Written);
	DerivedDataArtifactStore NewSession({ Root.Path, true, 1024 * 1024, 1024, 8 });
	const auto Reopened = NewSession.Read(MeshKey);
	PF_CHECK(Tests, Reopened.Status == DerivedDataArtifactReadStatus::Hit);
	PF_CHECK(Tests, Reopened.Payload == Payload);

	const DerivedDataArtifactKey OtherKey = MakeKey("static-mesh", 1, "recipe-b");
	const DerivedDataArtifactKey OtherKind = MakeKey("processed-image");
	const DerivedDataArtifactKey OtherSchema = MakeKey("static-mesh", 2);
	PF_CHECK(Tests, Store.Read(OtherKey).Status == DerivedDataArtifactReadStatus::Miss);
	PF_CHECK(Tests, Store.Read(OtherKind).Status == DerivedDataArtifactReadStatus::Miss);
	PF_CHECK(Tests, Store.Read(OtherSchema).Status == DerivedDataArtifactReadStatus::Miss);

	// A misplaced artifact with a valid envelope but a different recipe identity is incompatible.
	const std::filesystem::path MisplacedPath = DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, OtherKey);
	PF_CHECK(Tests, std::filesystem::copy_file(
		DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, MeshKey), MisplacedPath,
		std::filesystem::copy_options::overwrite_existing));
	PF_CHECK(Tests, Store.Read(OtherKey).Status == DerivedDataArtifactReadStatus::Incompatible);
	std::error_code Error;
	std::filesystem::remove(MisplacedPath, Error);
	const std::filesystem::path MisplacedKindPath = DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, OtherKind);
	PF_CHECK(Tests, std::filesystem::copy_file(
		DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, MeshKey), MisplacedKindPath,
		std::filesystem::copy_options::overwrite_existing));
	PF_CHECK(Tests, Store.Read(OtherKind).Status == DerivedDataArtifactReadStatus::Incompatible);
	std::filesystem::remove(MisplacedKindPath, Error);
	const std::filesystem::path MisplacedSchemaPath = DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, OtherSchema);
	PF_CHECK(Tests, std::filesystem::copy_file(
		DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, MeshKey), MisplacedSchemaPath,
		std::filesystem::copy_options::overwrite_existing));
	PF_CHECK(Tests, Store.Read(OtherSchema).Status == DerivedDataArtifactReadStatus::Incompatible);
	std::filesystem::remove(MisplacedSchemaPath, Error);

	const std::filesystem::path MeshPath = DerivedDataArtifactStoreTestAccess::GetPath(Root.Path, MeshKey);
	std::vector<std::byte> Corrupt = ReadBytes(MeshPath);
	PF_CHECK(Tests, Corrupt.size() > 8);
	Corrupt.pop_back();
	PF_CHECK(Tests, WriteBytes(MeshPath, Corrupt));
	PF_CHECK(Tests, Store.Read(MeshKey).Status == DerivedDataArtifactReadStatus::Corrupt);
	PF_CHECK(Tests, Store.Write(MeshKey, Payload).Status == DerivedDataArtifactWriteStatus::Written);
	Corrupt = ReadBytes(MeshPath);
	PF_CHECK(Tests, Corrupt.size() > 12);
	Corrupt[8] = std::byte{ 2 };
	PF_CHECK(Tests, WriteBytes(MeshPath, Corrupt));
	PF_CHECK(Tests, Store.Read(MeshKey).Status == DerivedDataArtifactReadStatus::Incompatible);
	PF_CHECK(Tests, Store.Write(MeshKey, Payload).Status == DerivedDataArtifactWriteStatus::Written);
	Corrupt = ReadBytes(MeshPath);
	PF_CHECK(Tests, !Corrupt.empty());
	Corrupt.back() ^= std::byte{ 1 };
	PF_CHECK(Tests, WriteBytes(MeshPath, Corrupt));
	PF_CHECK(Tests, Store.Read(MeshKey).Status == DerivedDataArtifactReadStatus::Corrupt);
	PF_CHECK(Tests, Store.Write(MeshKey, Payload).Status == DerivedDataArtifactWriteStatus::Written);

	const std::vector<std::byte> TooLarge = Bytes(std::string(1025, 'x'));
	PF_CHECK(Tests, Store.Write(MakeKey("too-large"), TooLarge).Status == DerivedDataArtifactWriteStatus::Rejected);
	PF_CHECK(Tests, Store.Read(MeshKey).Payload == Payload);

	const DerivedDataArtifactKey FailingKey = MakeKey("failing-write");
	const auto Failure = DerivedDataArtifactStoreTestAccess::WriteFailingReplacement(NewSession, FailingKey, Payload);
	PF_CHECK(Tests, Failure.Status == DerivedDataArtifactWriteStatus::Unavailable);
	PF_CHECK(Tests, NewSession.Read(MeshKey).Status == DerivedDataArtifactReadStatus::Hit);
	PF_CHECK(Tests, NewSession.Read(FailingKey).Status == DerivedDataArtifactReadStatus::Miss);
	for (const auto& Entry : std::filesystem::directory_iterator(Root.Path))
		PF_CHECK(Tests, Entry.path().extension() == ".pfartifact");

	// Returned payload ownership is independent of later entry deletion/eviction.
	const auto Retained = NewSession.Read(MeshKey);
	PF_CHECK(Tests, Retained.Payload == Payload);
	PF_CHECK(Tests, std::filesystem::remove(MeshPath));
	PF_CHECK(Tests, Retained.Payload == Payload);

	TemporaryArtifactRoot EvictionRoot;
	const std::filesystem::path Sentinel = EvictionRoot.Path / "keep-this.pfartifact";
	const std::vector<std::byte> SentinelContents = Bytes("not owned by the artifact store");
	std::filesystem::create_directories(EvictionRoot.Path);
	PF_CHECK(Tests, WriteBytes(Sentinel, SentinelContents));
	DerivedDataArtifactStore Bounded({ EvictionRoot.Path, true, 1024 * 1024, 1024, 1 });
	const DerivedDataArtifactKey First = MakeKey("static-mesh", 1, "first");
	const DerivedDataArtifactKey Second = MakeKey("static-mesh", 1, "second");
	PF_CHECK(Tests, Bounded.Write(First, Payload).Status == DerivedDataArtifactWriteStatus::Written);
	PF_CHECK(Tests, Bounded.Write(Second, Payload).Status == DerivedDataArtifactWriteStatus::Written);
	PF_CHECK(Tests, Bounded.Read(First).Status == DerivedDataArtifactReadStatus::Miss);
	PF_CHECK(Tests, Bounded.Read(Second).Status == DerivedDataArtifactReadStatus::Hit);
	PF_CHECK(Tests, ReadBytes(Sentinel) == SentinelContents);

	TemporaryArtifactRoot LinkRoot;
	const std::filesystem::path ActualParent = LinkRoot.Path / "actual";
	const std::filesystem::path LinkedParent = LinkRoot.Path / "linked";
	std::filesystem::create_directories(ActualParent);
	std::error_code LinkError;
	std::filesystem::create_directory_symlink(ActualParent, LinkedParent, LinkError);
	if (!LinkError)
	{
		DerivedDataArtifactStore Linked({ LinkedParent / "DerivedData", true, 1024 * 1024, 1024, 8 });
		PF_CHECK(Tests, Linked.Write(MakeKey("linked-root"), Payload).Status == DerivedDataArtifactWriteStatus::Unavailable);
		PF_CHECK(Tests, !std::filesystem::exists(ActualParent / "DerivedData"));
	}

	DerivedDataArtifactStore Disabled({ Root.Path / "disabled", false });
	PF_CHECK(Tests, Disabled.Read(MeshKey).Status == DerivedDataArtifactReadStatus::Unavailable);
	PF_CHECK(Tests, Disabled.Write(MeshKey, Payload).Status == DerivedDataArtifactWriteStatus::Unavailable);
}
