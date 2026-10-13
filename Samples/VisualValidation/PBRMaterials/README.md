# PulseForge PBR Materials

This project is a compact workbench for textured metallic/roughness shading, tangent-space normal mapping, material occlusion, emissive HDR output, and alpha modes. Its startup scene is arranged as a comparison board.

## Open and inspect

Open `PBRMaterials.pfproj` in the Vulkan editor. The project startup UUID selects `Assets/Scenes/PBRMaterialsOverview.scene`; open that scene from the Content Browser if another scene is active. The authored camera also frames the scene for standalone-game rendering.

The top row compares rough clay, satin ceramic, and polished metal with a shared base-color map and lighting. The lower row shows packed metallic/roughness channels, a normal map, material occlusion, and an emissive texture. Select a material in the Content Browser and choose **Show Material Details** to edit its saved-asset draft. Use **Save Material** to commit changes or **Revert Material** to reload the asset. Mesh references retain the material UUID when it is saved.

Content Browser material tiles display a saved base-color-factor swatch. The swatch does not render the base-color map, metallic/roughness response, normal detail, or scene lighting.

## Expected behavior

- Rough surfaces show broad, subdued highlights; smoother materials show tighter highlights.
- Metallic response changes with the material's `metallicFactor` and the packed map's blue channel. Roughness uses the green channel.
- Normal-map grooves alter directional highlights without changing the geometric silhouette.
- Material occlusion affects indirect ambient response, not direct light or shadow casting.
- Emissive stripes add scene-linear HDR radiance; they do not produce bloom or emissive global illumination.
- Missing optional maps use neutral fallbacks. Older material versions remain loadable with defaults for newer fields.
- OPAQUE, MASK, and BLEND materials follow the alpha behavior described in [Material Assets](../../../Docs/MaterialAssets.md).

The procedural cube includes authored normals and tangents. Its face-aligned UVs make tangent orientation and map channels easy to inspect. Importer-generated tangents are covered by CPU tests for mirrored UV islands and seam splitting.

## Materials and limitations

Material versions 1 through 4 are readable; the current serializer writes version 4. Color textures use sRGB decoding, while normal, occlusion, and packed metallic/roughness maps use linear UNORM. Imported image textures receive a CPU-generated mip chain with sRGB-aware color filtering. Material AO and SSAO combine conservatively using the lower visibility value to avoid over-darkening indirect light.

Only static glTF triangle primitives and supported image formats are imported. The renderer does not implement physical transmission, refraction, order-independent transparency, material graphs, or emissive global illumination. Scene View camera framing may differ from the authored game camera; use the viewport's focus and navigation controls as needed.

## Asset generation and attribution

The procedural `.ppm` maps and flat-shaded glTF cube can be recreated with `Assets/GenerateAssets.ps1`; the script refuses to overwrite existing files. HDRI source and license are recorded in [ASSET_CREDITS.md](ASSET_CREDITS.md). All assets are included locally, so the project opens offline.
