#include "TestEngineSupport.h"

namespace
{
	void TestImageAssetImport(TestRunner& Tests)
	{
		using namespace PulseForge;
		std::error_code FileError;
		const std::filesystem::path TemporaryDirectory = std::filesystem::temp_directory_path(FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

		const auto ProjectIdentifier = UUID::Generate();
		PF_CHECK(Tests, ProjectIdentifier.has_value());
		if (!ProjectIdentifier) return;

		const std::filesystem::path ProjectRoot = TemporaryDirectory / ("PulseForgeImageImport-" + ProjectIdentifier->ToString());
		struct ProjectDirectoryCleanup
		{
			std::filesystem::path Path;
			~ProjectDirectoryCleanup()
			{
				std::error_code Error;
				std::filesystem::remove_all(Path, Error);
			}
		} Cleanup{ ProjectRoot };

		const std::filesystem::path TextureDirectory = ProjectRoot / "Assets" / "Textures";
		std::filesystem::create_directories(TextureDirectory, FileError);
		PF_CHECK(Tests, !FileError);
		if (FileError) return;

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
		Bitmap.push_back('B');
		Bitmap.push_back('M');
		AppendUInt32(70);
		AppendUInt32(0);
		AppendUInt32(54);
		AppendUInt32(40);
		AppendUInt32(2);
		AppendUInt32(2);
		AppendUInt16(1);
		AppendUInt16(24);
		AppendUInt32(0);
		AppendUInt32(16);
		AppendUInt32(0);
		AppendUInt32(0);
		AppendUInt32(0);
		AppendUInt32(0);
		// BMP storage is bottom-up and BGR: bottom row red/green, top row blue/white.
		Bitmap.insert(Bitmap.end(), { 0, 0, 255, 0, 255, 0, 0, 0, 255, 0, 0, 255, 255, 255, 0, 0 });
		PF_CHECK(Tests, Bitmap.size() == 70);

		const std::filesystem::path SourcePath = TextureDirectory / "checker.bmp";
		{
			std::ofstream Output(SourcePath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto Metadata = AssetMetadataSerializer::CreateForNewAsset(SourcePath);
		PF_CHECK(Tests, Metadata.has_value());
		if (!Metadata) return;

		AssetRegistry Registry;
		PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
		const auto Imported = ImageAssetImporter::ImportRGBA8(Metadata->ID, ProjectRoot, Registry);
		PF_CHECK(Tests, Imported.has_value());
		if (!Imported) return;
		PF_CHECK(Tests, Imported->Width == 2 && Imported->Height == 2);
		PF_CHECK(Tests, Imported->RGBA8Pixels.size() == 16);
		PF_CHECK(
			Tests, Imported->RGBA8Pixels == std::vector<uint8_t>({ 0, 0, 255, 255, 255, 255, 255, 255, 255, 0, 0, 255, 0, 255, 0, 255 }));
		const auto Preview = ImageAssetImporter::DecodePreviewRGBA8(SourcePath, 192);
		PF_CHECK(Tests, Preview && Preview->Width == 2 && Preview->Height == 2);
		PF_CHECK(Tests, Preview && Preview->RGBA8Pixels == Imported->RGBA8Pixels);
		PF_CHECK(Tests, Preview && Preview->SourceFingerprint == Imported->SourceFingerprint);
		const auto ZeroEdgePreview = ImageAssetImporter::DecodePreviewRGBA8(SourcePath, 0);
		PF_CHECK(Tests, !ZeroEdgePreview && ZeroEdgePreview.error().Code == ImageAssetImportErrorCode::ResourceLimitExceeded);

		const auto MissingAssetID = UUID::Generate();
		PF_CHECK(Tests, MissingAssetID.has_value());
		if (MissingAssetID)
		{
			const auto Missing = ImageAssetImporter::ImportRGBA8(*MissingAssetID, ProjectRoot, Registry);
			PF_CHECK(Tests, !Missing && Missing.error().Code == ImageAssetImportErrorCode::AssetNotFound);
		}

		const std::filesystem::path RenamedPath = TextureDirectory / "checker-renamed.bmp";
		const auto Move = AssetOperations::Move(Registry, ProjectRoot, Metadata->ID, "Assets/Textures/checker-renamed.bmp");
		PF_CHECK(Tests, Move.has_value() && Move->ID == Metadata->ID);
		if (Move)
		{
			const auto ImportedAfterMove = ImageAssetImporter::ImportRGBA8(Metadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, ImportedAfterMove.has_value());
			PF_CHECK(Tests, ImportedAfterMove && ImportedAfterMove->RGBA8Pixels == Imported->RGBA8Pixels);
		}
		PF_CHECK(Tests, std::filesystem::exists(RenamedPath));
		const auto PreviewAfterMove = ImageAssetImporter::DecodePreviewRGBA8(RenamedPath);
		PF_CHECK(Tests, PreviewAfterMove && PreviewAfterMove->SourceFingerprint == Imported->SourceFingerprint);
		Bitmap.back() ^= 0x7fu;
		{
			std::ofstream Output(RenamedPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Bitmap.data()), static_cast<std::streamsize>(Bitmap.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto ReplacedPreview = ImageAssetImporter::DecodePreviewRGBA8(RenamedPath);
		PF_CHECK(Tests, ReplacedPreview && ReplacedPreview->SourceFingerprint != Imported->SourceFingerprint);

		const std::filesystem::path LargePreviewPath = TextureDirectory / "wide.ppm";
		{
			const std::string Header = "P6\n400 200\n255\n";
			std::vector<uint8_t> Ppm(Header.begin(), Header.end());
			Ppm.reserve(Ppm.size() + 400u * 200u * 3u);
			for (uint32_t Y = 0; Y < 200; ++Y)
				for (uint32_t X = 0; X < 400; ++X)
				{
					Ppm.push_back(static_cast<uint8_t>(X & 0xffu));
					Ppm.push_back(static_cast<uint8_t>(Y & 0xffu));
					Ppm.push_back(127);
				}
			std::ofstream Output(LargePreviewPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Ppm.data()), static_cast<std::streamsize>(Ppm.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(LargePreviewPath).has_value());
		const auto DownsampledPreview = ImageAssetImporter::DecodePreviewRGBA8(LargePreviewPath, 192);
		PF_CHECK(Tests, DownsampledPreview && DownsampledPreview->Width == 192 && DownsampledPreview->Height == 96);
		PF_CHECK(Tests, DownsampledPreview && DownsampledPreview->RGBA8Pixels.size() == 192u * 96u * 4u);

		const std::filesystem::path AlphaPreviewPath = TextureDirectory / "alpha.tga";
		{
			std::array<uint8_t, 26> Tga{};
			Tga[2] = 2;
			Tga[12] = 2;
			Tga[14] = 1;
			Tga[16] = 32;
			Tga[17] = 0x28;
			Tga[18] = 255; // Blue, fully transparent.
			Tga[23] = 0;
			Tga[24] = 255;
			Tga[25] = 128;
			std::ofstream Output(AlphaPreviewPath, std::ios::binary | std::ios::trunc);
			Output.write(reinterpret_cast<const char*>(Tga.data()), static_cast<std::streamsize>(Tga.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		PF_CHECK(Tests, AssetMetadataSerializer::CreateForNewAsset(AlphaPreviewPath).has_value());
		const auto AlphaPreview = ImageAssetImporter::DecodePreviewRGBA8(AlphaPreviewPath);
		PF_CHECK(Tests, AlphaPreview && AlphaPreview->Width == 2 && AlphaPreview->Height == 1);
		PF_CHECK(Tests, AlphaPreview && AlphaPreview->RGBA8Pixels[0] == 0 && AlphaPreview->RGBA8Pixels[2] == 255 &&
							AlphaPreview->RGBA8Pixels[3] == 0 && AlphaPreview->RGBA8Pixels[4] == 255 &&
							AlphaPreview->RGBA8Pixels[7] == 128);

		const std::filesystem::path InvalidImagePath = TextureDirectory / "invalid.bmp";
		{
			std::ofstream Output(InvalidImagePath, std::ios::binary | std::ios::trunc);
			Output.write("not an image", 12);
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto InvalidImageMetadata = AssetMetadataSerializer::CreateForNewAsset(InvalidImagePath);
		PF_CHECK(Tests, InvalidImageMetadata.has_value());
		const auto InvalidPreview = ImageAssetImporter::DecodePreviewRGBA8(InvalidImagePath);
		PF_CHECK(Tests, !InvalidPreview && InvalidPreview.error().Code == ImageAssetImportErrorCode::InvalidImage);
		if (InvalidImageMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto InvalidImage = ImageAssetImporter::ImportRGBA8(InvalidImageMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !InvalidImage && InvalidImage.error().Code == ImageAssetImportErrorCode::InvalidImage);
		}

		const std::filesystem::path HdrPath = TextureDirectory / "tiny.hdr";
		{
			constexpr std::string_view HdrHeader = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n";
			std::ofstream Output(HdrPath, std::ios::binary | std::ios::trunc);
			Output.write(HdrHeader.data(), static_cast<std::streamsize>(HdrHeader.size()));
			constexpr std::array<uint8_t, 8> RgbePixels = { 128, 64, 32, 130, 128, 128, 128, 129 };
			Output.write(reinterpret_cast<const char*>(RgbePixels.data()), static_cast<std::streamsize>(RgbePixels.size()));
			PF_CHECK(Tests, static_cast<bool>(Output));
		}
		const auto HdrMetadata = AssetMetadataSerializer::CreateForNewAsset(HdrPath);
		PF_CHECK(Tests, HdrMetadata.has_value());
		if (HdrMetadata)
		{
			PF_CHECK(Tests, Registry.Rebuild(ProjectRoot).has_value());
			const auto ImportedHdr = ImageAssetImporter::ImportRGBA32F(HdrMetadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, ImportedHdr.has_value());
			if (ImportedHdr)
			{
				PF_CHECK(Tests, ImportedHdr->Width == 2 && ImportedHdr->Height == 1);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels.size() == 8);
				PF_CHECK(Tests, std::abs(ImportedHdr->RGBA32FPixels[0] - 2.0f) < 0.001f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[1] > 0.99f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[2] > 0.49f);
				PF_CHECK(Tests, ImportedHdr->RGBA32FPixels[3] == 1.0f);
			}
			const auto RejectedLdrAsHdr = ImageAssetImporter::ImportRGBA32F(Metadata->ID, ProjectRoot, Registry);
			PF_CHECK(Tests, !RejectedLdrAsHdr && RejectedLdrAsHdr.error().Code == ImageAssetImportErrorCode::InvalidImage);
		}
	}
} // namespace

void RunImageAssetImportTests(TestRunner& Tests)
{
	TestImageAssetImport(Tests);
}
