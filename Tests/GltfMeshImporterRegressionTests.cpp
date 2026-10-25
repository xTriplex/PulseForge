#include "TestEngineSupport.h"

#include "Assets/AssetDependencyCatalog.h"
#include "Assets/AssetImportProvenance.h"
#include "Assets/GltfMeshImporter.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace
{
	using namespace PulseForge;

	class RepeatedBufferFixture final
	{
	public:
		RepeatedBufferFixture()
		{
			std::error_code Error;
			const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(Error);
			if (Error) return;

			const auto ProjectIdentifier = UUID::Generate();
			if (!ProjectIdentifier) return;
			Root = TemporaryDirectory / ("PulseForgeRepeatedGltfBuffer-" + ProjectIdentifier->ToString());
			std::filesystem::create_directories(Root / "Assets" / "Models", Error);
			if (Error) return;

			const auto GeneratedProjectID = UUID::Generate();
			if (!GeneratedProjectID) return;
			ProjectID = *GeneratedProjectID;
			Valid = true;
		}

		~RepeatedBufferFixture()
		{
			if (Root.empty()) return;
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}

		[[nodiscard]] AssetID WriteSource(std::string_view FileName, std::string_view FirstUri, std::string_view SecondUri)
		{
			const std::filesystem::path Path = Root / "Assets" / "Models" / std::string(FileName);
			const std::string Document = MakeSharedBufferDocument(FirstUri, SecondUri);
			if (!WriteText(Path, Document)) return {};
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? Metadata->ID : AssetID{};
		}

		[[nodiscard]] AssetID WriteSourceWithDistinctBuffers(std::string_view FileName)
		{
			const std::filesystem::path Directory = Root / "Assets" / "Models";
			if (!WriteBinary(Directory / "Position.bin", PositionBytes()) || !WriteBinary(Directory / "Index.bin", IndexBytes()))
				return {};
			if (!AssetMetadataSerializer::CreateForNewAsset(Directory / "Position.bin") ||
				!AssetMetadataSerializer::CreateForNewAsset(Directory / "Index.bin"))
				return {};

			const std::string Document = MakeDistinctBufferDocument();
			const std::filesystem::path Path = Directory / std::string(FileName);
			if (!WriteText(Path, Document)) return {};
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? Metadata->ID : AssetID{};
		}

		[[nodiscard]] AssetID WriteSharedBuffer()
		{
			const std::filesystem::path Path = Root / "Assets" / "Models" / "Shared.bin";
			std::vector<std::byte> Bytes = PositionBytes();
			const std::vector<std::byte> Indices = IndexBytes();
			Bytes.insert(Bytes.end(), Indices.begin(), Indices.end());
			if (!WriteBinary(Path, Bytes)) return {};
			const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Path);
			return Metadata ? Metadata->ID : AssetID{};
		}

		[[nodiscard]] std::optional<AssetRegistry> BuildRegistry() const
		{
			AssetRegistry Registry;
			if (!Registry.Rebuild(Root)) return std::nullopt;
			return Registry;
		}

		std::filesystem::path Root;
		UUID ProjectID;
		bool Valid = false;

	private:
		[[nodiscard]] static bool WriteText(const std::filesystem::path& Path, std::string_view Content)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(Content.data(), static_cast<std::streamsize>(Content.size()));
			return static_cast<bool>(Output);
		}

		[[nodiscard]] static bool WriteBinary(const std::filesystem::path& Path, std::span<const std::byte> Content)
		{
			std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
			if (!Output) return false;
			Output.write(reinterpret_cast<const char*>(Content.data()), static_cast<std::streamsize>(Content.size()));
			return static_cast<bool>(Output);
		}

		[[nodiscard]] static std::vector<std::byte> PositionBytes()
		{
			std::vector<std::byte> Bytes;
			for (const float Value : std::array<float, 9>{ 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f })
			{
				const uint32_t Bits = std::bit_cast<uint32_t>(Value);
				for (size_t Byte = 0; Byte < sizeof(Bits); ++Byte)
					Bytes.push_back(static_cast<std::byte>((Bits >> (Byte * 8)) & 0xff));
			}
			return Bytes;
		}

		[[nodiscard]] static std::vector<std::byte> IndexBytes()
		{
			std::vector<std::byte> Bytes;
			for (const uint16_t Index : std::array<uint16_t, 3>{ 0, 1, 2 })
			{
				Bytes.push_back(static_cast<std::byte>(Index & 0xff));
				Bytes.push_back(static_cast<std::byte>(Index >> 8));
			}
			return Bytes;
		}

		[[nodiscard]] static std::string MakeSharedBufferDocument(std::string_view FirstUri, std::string_view SecondUri)
		{
			return "{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"" + std::string(FirstUri) +
				"\",\"byteLength\":42},{\"uri\":\"" + std::string(SecondUri) +
				"\",\"byteLength\":42}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}," +
				"{\"buffer\":1,\"byteOffset\":36,\"byteLength\":6}],\"accessors\":[" +
				"{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
				"{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]," +
				"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}";
		}

		[[nodiscard]] static std::string MakeDistinctBufferDocument()
		{
			return std::string("{\"asset\":{\"version\":\"2.0\"},\"buffers\":[{\"uri\":\"Position.bin\",\"byteLength\":36},") +
				"{\"uri\":\"Index.bin\",\"byteLength\":6}],\"bufferViews\":[{\"buffer\":0,\"byteOffset\":0,\"byteLength\":36}," +
				"{\"buffer\":1,\"byteOffset\":0,\"byteLength\":6}],\"accessors\":[" +
				"{\"bufferView\":0,\"componentType\":5126,\"count\":3,\"type\":\"VEC3\"}," +
				"{\"bufferView\":1,\"componentType\":5123,\"count\":3,\"type\":\"SCALAR\"}]," +
				"\"meshes\":[{\"primitives\":[{\"attributes\":{\"POSITION\":0},\"indices\":1}]}]}";
		}
	};

	const AssetImportObservation* FindObservation(
		const AssetDependencyCatalogSnapshot& Snapshot,
		const AssetID& Asset,
		std::string_view Subresource)
	{
		const auto& Observations = Snapshot.GetImportObservations();
		const auto It = std::find_if(Observations.begin(), Observations.end(), [&](const AssetImportObservation& Observation)
			{ return Observation.Asset == Asset && Observation.SubresourceKey == Subresource; });
		return It == Observations.end() ? nullptr : &*It;
	}

	void CheckRecipeMatchesCatalog(
		TestRunner& Tests,
		const RepeatedBufferFixture& Fixture,
		const AssetID& Asset,
		const AssetImportRecipe& Recipe)
	{
		const auto Catalog = AssetDependencyCatalogBuilder::Build(Fixture.Root, Fixture.ProjectID, 1);
		PF_CHECK(Tests, Catalog.has_value());
		if (!Catalog) return;
		const AssetImportObservation* Observation = FindObservation(**Catalog, Asset, Recipe.SubresourceKey);
		PF_CHECK(Tests, Observation != nullptr);
		if (!Observation) return;
		PF_CHECK(Tests, Observation->Inputs == Recipe.Inputs);
		PF_CHECK(Tests, Observation->InputFingerprint == Recipe.InputFingerprint);
	}

	void TestRepeatedAndEquivalentBufferInputs(TestRunner& Tests)
	{
		RepeatedBufferFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		const AssetID SharedBuffer = Fixture.WriteSharedBuffer();
		const AssetID RepeatedUriSource = Fixture.WriteSource("Repeated.gltf", "Shared.bin", "Shared.bin");
		const AssetID EquivalentUriSource = Fixture.WriteSource("Equivalent.gltf", "Shared.bin", "./Shared%2Ebin");
		PF_CHECK(Tests, !SharedBuffer.IsNil() && !RepeatedUriSource.IsNil() && !EquivalentUriSource.IsNil());
		if (SharedBuffer.IsNil() || RepeatedUriSource.IsNil() || EquivalentUriSource.IsNil()) return;

		const auto Registry = Fixture.BuildRegistry();
		PF_CHECK(Tests, Registry.has_value());
		if (!Registry) return;

		for (const AssetID Source : std::array<AssetID, 2>{ RepeatedUriSource, EquivalentUriSource })
		{
			const auto Product = GltfMeshImporter::ProcessStaticPrimitive(Source, Fixture.Root, *Registry);
			PF_CHECK(Tests, Product.has_value());
			if (!Product) continue;
			PF_CHECK(Tests, Product->Mesh.Vertices.size() == 3);
			PF_CHECK(Tests, Product->Mesh.Indices == std::vector<uint32_t>({ 0, 1, 2 }));
			PF_CHECK(Tests, Product->Mesh.Vertices[1].Position[0] == 1.0f);
			PF_CHECK(Tests, Product->Recipe.Inputs.size() == 2);
			PF_CHECK(Tests, std::is_sorted(Product->Recipe.Inputs.begin(), Product->Recipe.Inputs.end(),
				[](const AssetImportInputFingerprint& First, const AssetImportInputFingerprint& Second)
				{ return First.ProjectRelativePath < Second.ProjectRelativePath; }));
			if (Product->Recipe.Inputs.size() == 2)
			{
				const std::string ExpectedSourcePath = "Assets/Models/" +
					std::string(Source == RepeatedUriSource ? "Repeated.gltf" : "Equivalent.gltf");
				PF_CHECK(Tests, Product->Recipe.Inputs[0].ProjectRelativePath == ExpectedSourcePath);
				PF_CHECK(Tests, Product->Recipe.Inputs[1].ProjectRelativePath == "Assets/Models/Shared.bin");
			}
			CheckRecipeMatchesCatalog(Tests, Fixture, Source, Product->Recipe);
		}
	}

	void TestDistinctBufferInputsRemainDistinct(TestRunner& Tests)
	{
		RepeatedBufferFixture Fixture;
		PF_CHECK(Tests, Fixture.Valid);
		if (!Fixture.Valid) return;

		const AssetID Source = Fixture.WriteSourceWithDistinctBuffers("Distinct.gltf");
		PF_CHECK(Tests, !Source.IsNil());
		if (Source.IsNil()) return;

		const auto Registry = Fixture.BuildRegistry();
		PF_CHECK(Tests, Registry.has_value());
		if (!Registry) return;

		const auto Product = GltfMeshImporter::ProcessStaticPrimitive(Source, Fixture.Root, *Registry);
		PF_CHECK(Tests, Product.has_value());
		if (!Product) return;
		PF_CHECK(Tests, Product->Mesh.Indices == std::vector<uint32_t>({ 0, 1, 2 }));
		PF_CHECK(Tests, Product->Recipe.Inputs.size() == 3);
		if (Product->Recipe.Inputs.size() == 3)
		{
			PF_CHECK(Tests, Product->Recipe.Inputs[0].ProjectRelativePath == "Assets/Models/Distinct.gltf");
			PF_CHECK(Tests, Product->Recipe.Inputs[1].ProjectRelativePath == "Assets/Models/Index.bin");
			PF_CHECK(Tests, Product->Recipe.Inputs[2].ProjectRelativePath == "Assets/Models/Position.bin");
		}
		CheckRecipeMatchesCatalog(Tests, Fixture, Source, Product->Recipe);
	}

	void TestRecipeInputNormalizationRejectsConflictingObservations(TestRunner& Tests)
	{
		const std::array<std::byte, 1> FirstBytes{ std::byte{ 0x17 } };
		const std::array<std::byte, 1> SecondBytes{ std::byte{ 0x29 } };
		std::vector<AssetImportInputFingerprint> Inputs{
			{ "Assets/Models/Shared.bin", ComputeAssetContentFingerprint(FirstBytes) },
			{ "Assets/Models/Shared.bin", ComputeAssetContentFingerprint(SecondBytes) }
		};

		const auto Normalized = NormalizeAssetImportRecipeInputs(Inputs);
		PF_CHECK(Tests, !Normalized.has_value());
		if (!Normalized)
			PF_CHECK(Tests, Normalized.error().find("conflicting content") != std::string::npos);
	}
} // namespace

void RunGltfMeshImporterRegressionTests(TestRunner& Tests)
{
	TestRepeatedAndEquivalentBufferInputs(Tests);
	TestDistinctBufferInputsRemainDistinct(Tests);
	TestRecipeInputNormalizationRejectsConflictingObservations(Tests);
}
