#include "TestAssetSupport.h"

#include "Assets/DerivedDataArtifactStore.h"
#include "Assets/AssetDependencyExtractor.h"
#include "Assets/GltfMeshImporter.h"
#include "Assets/ImageAssetImporter.h"
#include "Assets/Detail/AssetCpuProducts.h"

#include <bit>
#include <cstring>
#include <fstream>

namespace
{
	using namespace PulseForge;

	std::vector<std::byte> AsBytes(std::string_view Text)
	{
		const auto Bytes = std::as_bytes(std::span(Text.data(), Text.size()));
		return { Bytes.begin(), Bytes.end() };
	}

	AssetImportRecipe MakeRecipe(const AssetImportObservation& Observation)
	{
		return { Observation.Asset, Observation.SubresourceKey, Observation.ImporterID, Observation.ImporterVersion,
			Observation.OutputSchemaVersion, Observation.EffectiveSettings, Observation.TargetID,
			Observation.Inputs, Observation.InputFingerprint };
	}

	struct TemporaryProject final
	{
		std::filesystem::path Root;
		TemporaryProject()
		{
			std::error_code Error;
			const auto Temp = std::filesystem::temp_directory_path(Error);
			const auto Identifier = UUID::Generate();
			if (!Error && Identifier)
				Root = Temp / ("PulseForgeDerivedProducts-" + Identifier->ToString());
			if (Root.empty()) return;
			std::filesystem::create_directories(Root / "Assets" / "Meshes", Error);
			std::filesystem::create_directories(Root / "Assets" / "Textures", Error);
		}
		~TemporaryProject()
		{
			std::error_code Error;
			std::filesystem::remove_all(Root, Error);
		}
	};

	void TestMeshColdWarmArtifactBypassesImporter(TestRunner& Tests)
	{
		TemporaryProject Project;
		PF_CHECK(Tests, !Project.Root.empty() && std::filesystem::is_directory(Project.Root / "Assets" / "Meshes"));
		if (Project.Root.empty()) return;
		const auto GltfPath = Project.Root / "Assets" / "Meshes" / "Triangle.gltf";
		const auto BufferPath = Project.Root / "Assets" / "Meshes" / "Triangle.bin";
		const std::string Json = R"({
			"asset":{"version":"2.0"},"buffers":[{"uri":"Triangle.bin","byteLength":42}],
			"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
			"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3"},
				{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
			"meshes":[{"name":"Triangle","primitives":[{"attributes":{"POSITION":0},"indices":1}]}]})";
		{
			std::ofstream Out(GltfPath, std::ios::binary);
			Out.write(Json.data(), static_cast<std::streamsize>(Json.size()));
			PF_CHECK(Tests, static_cast<bool>(Out));
		}
		std::vector<std::byte> Buffer;
		const auto U32 = [&Buffer](uint32_t Value)
		{
			for (unsigned I = 0; I < 4; ++I) Buffer.push_back(static_cast<std::byte>((Value >> (I * 8)) & 0xff));
		};
		for (float Value : { 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f }) U32(std::bit_cast<uint32_t>(Value));
		for (uint16_t Index : { 0, 1, 2 })
		{
			Buffer.push_back(static_cast<std::byte>(Index & 0xff));
			Buffer.push_back(static_cast<std::byte>(Index >> 8));
		}
		{
			std::ofstream Out(BufferPath, std::ios::binary);
			Out.write(reinterpret_cast<const char*>(Buffer.data()), static_cast<std::streamsize>(Buffer.size()));
			PF_CHECK(Tests, static_cast<bool>(Out) && Buffer.size() == 42);
		}
		auto SourceMetadata = AssetMetadataSerializer::CreateForNewAsset(GltfPath);
		auto BufferMetadata = AssetMetadataSerializer::CreateForNewAsset(BufferPath);
		PF_CHECK(Tests, SourceMetadata.has_value() && BufferMetadata.has_value());
		if (!SourceMetadata || !BufferMetadata) return;
		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(Project.Root).has_value());
		auto Settings = GltfMeshImportSettings{}.Canonicalize();
		PF_CHECK(Tests, Settings.has_value());
		if (!Settings) return;
		const auto CacheRoot = Project.Root / "Out" / "TestMeshCache";
		DerivedDataArtifactStore ColdStore({ CacheRoot, true, 1024 * 1024, 512 * 1024, 16 });
		uint32_t ImportCount = 0;
		Detail::MeshProcessor CountedImporter = [&ImportCount](const AssetID& ID, const auto& Root, const AssetRegistry& Assets)
			-> std::expected<GltfMeshImportProduct, std::string>
		{
			++ImportCount;
			auto Imported = GltfMeshImporter::ProcessStaticPrimitive(ID, Root, Assets);
			if (!Imported) return std::unexpected(Imported.error().Message);
			return std::move(*Imported);
		};
		auto Cold = Detail::PrepareMeshCpuProduct(SourceMetadata->ID, Project.Root, Registry, *Settings, &ColdStore, {}, CountedImporter);
		PF_CHECK(Tests, Cold.has_value() && Cold->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ProcessedAndStored);
		PF_CHECK(Tests, ImportCount == 1);
		PF_CHECK(Tests, Cold && Cold->Product.Vertices.size() == 3 && Cold->Product.Indices == std::vector<uint32_t>({ 0, 1, 2 }));
		DerivedDataArtifactStore WarmStore({ CacheRoot, true, 1024 * 1024, 512 * 1024, 16 });
		auto Warm = Detail::PrepareMeshCpuProduct(SourceMetadata->ID, Project.Root, Registry, *Settings, &WarmStore, {}, CountedImporter);
		PF_CHECK(Tests, Warm.has_value() && Warm->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit);
		PF_CHECK(Tests, ImportCount == 1);
		PF_CHECK(Tests, Warm && Cold && Warm->Product.Indices == Cold->Product.Indices && Warm->Product.Vertices.size() == Cold->Product.Vertices.size());
		if (Cold && Warm)
			std::cout << "G11 mesh CPU cold: lookup=" << Cold->ObservationAndLookupMicroseconds << "us process="
				<< Cold->ProcessingMicroseconds << "us write=" << Cold->CacheWriteMicroseconds << "us; warm: lookup="
				<< Warm->ObservationAndLookupMicroseconds << "us process=" << Warm->ProcessingMicroseconds << "us\n";

		auto Extraction = AssetDependencyExtractor::ExtractAsset(Project.Root, SourceMetadata->ID, Registry);
		PF_CHECK(Tests, Extraction.has_value());
		if (Extraction)
		{
			const auto Observation = std::find_if(Extraction->ImportObservations.begin(), Extraction->ImportObservations.end(),
				[](const AssetImportObservation& Item) { return Item.SubresourceKey == "mesh/0/primitive/0"; });
			PF_CHECK(Tests, Observation != Extraction->ImportObservations.end());
			if (Observation != Extraction->ImportObservations.end())
			{
				auto Key = Detail::MakeMeshArtifactKey(MakeRecipe(*Observation), SourceMetadata->ID, *Settings);
				PF_CHECK(Tests, Key.has_value());
				if (Key)
				{
					const auto SemanticRoot = Project.Root / "Out" / "SemanticMeshCache";
					DerivedDataArtifactStore SemanticStore({ SemanticRoot, true, 1024 * 1024, 512 * 1024, 16 });
					PF_CHECK(Tests, SemanticStore.Write(*Key, AsBytes("checksummed but invalid mesh payload")).Status ==
						DerivedDataArtifactWriteStatus::Written);
					uint32_t SemanticImportCount = 0;
					auto Processor = [&SemanticImportCount](const AssetID& ID, const auto& Root, const AssetRegistry& Assets)
						-> std::expected<GltfMeshImportProduct, std::string>
					{
						++SemanticImportCount;
						auto Imported = GltfMeshImporter::ProcessStaticPrimitive(ID, Root, Assets);
						if (!Imported) return std::unexpected(Imported.error().Message);
						return std::move(*Imported);
					};
					auto Repaired = Detail::PrepareMeshCpuProduct(SourceMetadata->ID, Project.Root, Registry, *Settings,
						&SemanticStore, {}, Processor);
					PF_CHECK(Tests, Repaired.has_value() && SemanticImportCount == 1);
					DerivedDataArtifactStore ReopenedStore({ SemanticRoot, true, 1024 * 1024, 512 * 1024, 16 });
					auto Reopened = Detail::PrepareMeshCpuProduct(SourceMetadata->ID, Project.Root, Registry, *Settings,
						&ReopenedStore, {}, Processor);
					PF_CHECK(Tests, Reopened.has_value() && Reopened->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit);
					PF_CHECK(Tests, SemanticImportCount == 1);
				}
			}
		}

		const auto SourceTimestamp = std::filesystem::last_write_time(BufferPath);
		const auto WriteBuffer = [&BufferPath, &Buffer, &Tests](float FirstPositionX)
		{
			std::vector<std::byte> Changed = Buffer;
			const uint32_t Bits = std::bit_cast<uint32_t>(FirstPositionX);
			for (unsigned Byte = 0; Byte < 4; ++Byte)
				Changed[Byte] = static_cast<std::byte>((Bits >> (Byte * 8)) & 0xffu);
			std::ofstream Out(BufferPath, std::ios::binary | std::ios::trunc);
			Out.write(reinterpret_cast<const char*>(Changed.data()), static_cast<std::streamsize>(Changed.size()));
			PF_CHECK(Tests, static_cast<bool>(Out));
			return Changed;
		};
		const auto RestoreTimestamp = [&BufferPath, SourceTimestamp, &Tests]()
		{
			std::error_code Error;
			std::filesystem::last_write_time(BufferPath, SourceTimestamp, Error);
			PF_CHECK(Tests, !Error);
		};
		const auto RunStaleProcessing = [&](DerivedDataArtifactStore* Store, float InitialX, float ChangedX)
		{
			const auto BeforeProcessing = WriteBuffer(InitialX);
			RestoreTimestamp();
			bool ProcessorRan = false;
			Detail::MeshProcessor MutatingImporter = [&ProcessorRan, &WriteBuffer, &RestoreTimestamp, ChangedX] (
				const AssetID& ID, const auto& Root, const AssetRegistry& Assets)
				-> std::expected<GltfMeshImportProduct, std::string>
			{
				ProcessorRan = true;
				auto Imported = GltfMeshImporter::ProcessStaticPrimitive(ID, Root, Assets);
				if (!Imported) return std::unexpected(Imported.error().Message);
				(void)WriteBuffer(ChangedX);
				RestoreTimestamp();
				return std::move(*Imported);
			};
			auto Stale = Detail::PrepareMeshCpuProduct(SourceMetadata->ID, Project.Root, Registry, *Settings,
				Store, {}, std::move(MutatingImporter));
			PF_CHECK(Tests, ProcessorRan);
			PF_CHECK(Tests, !Stale && Stale.error().find("changed during preparation") != std::string::npos);
			std::ofstream Restore(BufferPath, std::ios::binary | std::ios::trunc);
			Restore.write(reinterpret_cast<const char*>(BeforeProcessing.data()), static_cast<std::streamsize>(BeforeProcessing.size()));
			PF_CHECK(Tests, static_cast<bool>(Restore));
			Restore.close();
			RestoreTimestamp();
		};

		// A same-size buffer edit with its timestamp restored must invalidate a result even
		// when the disposable artifact store is unavailable.
		RunStaleProcessing(nullptr, 0.25f, 0.5f);
		// The same stale-result rejection applies when persistence is enabled; cache write
		// failure is optional, but freshness validation is not.
		DerivedDataArtifactStore StaleStore({ Project.Root / "Out" / "StaleMeshCache", true, 1024 * 1024, 512 * 1024, 16 });
		RunStaleProcessing(&StaleStore, 0.75f, 1.0f);
	}

	void TestTextureColdWarmArtifactBypassesDecoder(TestRunner& Tests)
	{
		TemporaryProject Project;
		const auto Destination = Project.Root / "Assets" / "Textures" / "checker.BMP";
		std::vector<uint8_t> Bitmap;
		const auto AppendUInt16 = [&Bitmap](uint16_t Value)
		{
			Bitmap.push_back(static_cast<uint8_t>(Value & 0xff));
			Bitmap.push_back(static_cast<uint8_t>(Value >> 8));
		};
		const auto AppendUInt32 = [&Bitmap](uint32_t Value)
		{
			for (size_t Byte = 0; Byte < sizeof(Value); ++Byte)
				Bitmap.push_back(static_cast<uint8_t>((Value >> (Byte * 8)) & 0xff));
		};
		Bitmap.insert(Bitmap.end(), { 'B', 'M' });
		AppendUInt32(70); AppendUInt32(0); AppendUInt32(54); AppendUInt32(40);
		AppendUInt32(2); AppendUInt32(2); AppendUInt16(1); AppendUInt16(24);
		AppendUInt32(0); AppendUInt32(16); AppendUInt32(0); AppendUInt32(0); AppendUInt32(0); AppendUInt32(0);
		Bitmap.insert(Bitmap.end(), { 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0 });
		PF_CHECK(Tests, Bitmap.size() == 70);
		{
			std::ofstream Out(Destination, std::ios::binary);
			Out.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Out));
		}
		auto Metadata = AssetMetadataSerializer::CreateForNewAsset(Destination);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata) return;
		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(Project.Root).has_value());
		const auto CacheRoot = Project.Root / "Out" / "TestImageCache";
		DerivedDataArtifactStore ColdStore({ CacheRoot, true, 8 * 1024 * 1024, 4 * 1024 * 1024, 16 });
		uint32_t DecodeCount = 0;
		Detail::ImageProcessor CountedDecoder = [&DecodeCount](const AssetID& ID, const auto& Root, const AssetRegistry& Assets)
			-> std::expected<ImportedImage, std::string>
		{
			++DecodeCount;
			auto Image = ImageAssetImporter::ImportRGBA8(ID, Root, Assets);
			if (!Image) return std::unexpected(Image.error().Message);
			return std::move(*Image);
		};
		auto Cold = Detail::PrepareTextureCpuProduct(Metadata->ID, Project.Root, Registry, Detail::ImageColorSpace::Srgb,
			&ColdStore, {}, CountedDecoder);
		PF_CHECK(Tests, Cold.has_value() && Cold->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ProcessedAndStored);
		PF_CHECK(Tests, DecodeCount == 1);
		PF_CHECK(Tests, Cold && !Cold->Product.MipLevels.empty() && Cold->Product.MipLevels.front().Width == Cold->Product.Width);
		DerivedDataArtifactStore WarmStore({ CacheRoot, true, 8 * 1024 * 1024, 4 * 1024 * 1024, 16 });
		auto Warm = Detail::PrepareTextureCpuProduct(Metadata->ID, Project.Root, Registry, Detail::ImageColorSpace::Srgb,
			&WarmStore, {}, CountedDecoder);
		PF_CHECK(Tests, Warm.has_value() && Warm->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit);
		PF_CHECK(Tests, DecodeCount == 1);
		PF_CHECK(Tests, Warm && Cold && Warm->Product.MipLevels.size() == Cold->Product.MipLevels.size() &&
			Warm->Product.MipLevels.front().Pixels == Cold->Product.MipLevels.front().Pixels);
		if (Cold && Warm)
			std::cout << "G11 image CPU cold: lookup=" << Cold->ObservationAndLookupMicroseconds << "us decode+mips="
				<< Cold->ProcessingMicroseconds << "us write=" << Cold->CacheWriteMicroseconds << "us; warm: lookup="
				<< Warm->ObservationAndLookupMicroseconds << "us process=" << Warm->ProcessingMicroseconds << "us\n";

		const auto SourceFingerprint = ComputeAssetContentFingerprint(std::as_bytes(std::span(Bitmap)));
		const auto TextureKey = Detail::MakeTextureArtifactKey(SourceFingerprint, Detail::ImageColorSpace::Srgb);
		const auto SemanticRoot = Project.Root / "Out" / "SemanticImageCache";
		DerivedDataArtifactStore SemanticStore({ SemanticRoot, true, 8 * 1024 * 1024, 4 * 1024 * 1024, 16 });
		PF_CHECK(Tests, SemanticStore.Write(TextureKey, AsBytes("checksummed but invalid texture payload")).Status ==
			DerivedDataArtifactWriteStatus::Written);
		uint32_t SemanticDecodeCount = 0;
		auto Processor = [&SemanticDecodeCount](const AssetID& ID, const auto& Root, const AssetRegistry& Assets)
			-> std::expected<ImportedImage, std::string>
		{
			++SemanticDecodeCount;
			auto Image = ImageAssetImporter::ImportRGBA8(ID, Root, Assets);
			if (!Image) return std::unexpected(Image.error().Message);
			return std::move(*Image);
		};
		auto Repaired = Detail::PrepareTextureCpuProduct(Metadata->ID, Project.Root, Registry, Detail::ImageColorSpace::Srgb,
			&SemanticStore, {}, Processor);
		PF_CHECK(Tests, Repaired.has_value() && SemanticDecodeCount == 1);
		DerivedDataArtifactStore ReopenedStore({ SemanticRoot, true, 8 * 1024 * 1024, 4 * 1024 * 1024, 16 });
		auto Reopened = Detail::PrepareTextureCpuProduct(Metadata->ID, Project.Root, Registry, Detail::ImageColorSpace::Srgb,
			&ReopenedStore, {}, Processor);
		PF_CHECK(Tests, Reopened.has_value() && Reopened->CacheOutcome == Detail::AssetCpuProductCacheOutcome::ArtifactHit);
		PF_CHECK(Tests, SemanticDecodeCount == 1);
	}
}

void RunDerivedAssetPreparationTests(TestRunner& Tests)
{
	TestMeshColdWarmArtifactBypassesImporter(Tests);
	TestTextureColdWarmArtifactBypassesDecoder(Tests);
}
