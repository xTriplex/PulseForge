# Image assets

`ImageAssetImporter::ImportRGBA8` resolves an LDR image by its sidecar UUID through the project `AssetRegistry` and returns owned, top-to-bottom RGBA8 pixels. The result is renderer-independent; the caller chooses whether the uploaded texture is interpreted as sRGB color or linear data.

Managed Radiance `.hdr` files use `ImportRGBA32F`, which returns owned, top-to-bottom RGBA32F pixels in linear light. HDR values are not clamped or gamma-decoded, and non-finite channels are rejected. The HDR path caps source files at 128 MiB, dimensions at 32768 per side, and decoded images at 8 million pixels. LDR import explicitly rejects `.hdr` to prevent accidental quantization. EXR is not supported.

The importer uses the vendored stb image decoder and does not modify global decoder orientation state. It accepts the formats supported by that decoder, limits source files to 128 MiB and decoded images to 32 million pixels, and reports malformed or missing assets without changing their identity. Moving an image with `AssetOperations` preserves its UUID and the registry can be rebuilt to resolve its new path.
