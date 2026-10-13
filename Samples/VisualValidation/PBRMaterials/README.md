# PulseForge PBR Materials

## Purpose

This compact workbench demonstrates textured metallic-roughness shading, tangent-space normal mapping, material occlusion, emissive HDR output, and the material asset editor. The startup scene is already arranged as a comparison board; no manual scene construction is needed.

## Open and inspect

Open `PBRMaterials.pfproj` in the Vulkan PulseForgeEditor, then load `Assets/Scenes/PBRMaterialsOverview.scene` if it is not selected automatically. The project startup UUID points to this scene. The authored camera also frames the scene for standalone-game rendering.

The top row compares rough clay, satin ceramic, and polished metal using the same base-color map and lighting. The lower row shows packed metallic/roughness channels plus a normal map, a material-occlusion pattern, and a bright emissive texture. Select an entity and open its Material Editor from the Content Browser to compare factors, maps, and strengths. Save explicitly in the Material Editor; scene mesh references retain stable material UUIDs. Content Browser material tiles show a saved base-color swatch, not a physically rendered PBR preview.

## Expected behavior

- Rough surfaces show broad, subdued highlights; smoother materials show tighter, stronger reflections.
- Metallic response changes with `metallicFactor` and the packed map's blue channel; roughness uses the green channel.
- Normal-map grooves alter directional highlights without replacing the geometric silhouette.
- The occlusion pattern affects indirect ambient response, not direct light or cast shadows.
- Emissive stripes contribute scene-linear HDR radiance and are not attenuated by direct-light shadows. The current renderer has no bloom or emissive global illumination yet.
- Missing optional maps use neutral fallbacks. The included v2 compatibility material remains loadable with default values for newer fields.

The procedural cube includes authored normals and tangents. Its face-aligned UVs make tangent orientation and map-channel behavior easy to inspect. For importer-generated tangents, the CPU tests include mirrored UV islands and seam splitting.

## Materials and limitations

Material version 3 retains v1/v2 loading. Color textures use sRGB decoding; normal, occlusion, and packed metallic-roughness data use linear UNORM. Imported image textures receive a full CPU-generated mip chain with sRGB-aware color filtering. The current material path is opaque-only; alpha masking and blending are a separate upcoming stage. Material AO and SSAO combine conservatively using the lower visibility value to avoid over-darkening indirect light.

Only static glTF triangle primitives and supported image formats are imported. Transparent transmission, alpha-masked shadows, material graphs, and emissive global illumination are not implemented. Editor Scene View camera framing may differ from the authored game camera; use normal viewport focus/navigation if needed.

## Asset generation and attribution

The local procedural `.ppm` maps and flat-shaded glTF cube can be recreated with `GenerateAssets.ps1`; it refuses to overwrite existing files. HDRI source and license are recorded in `ASSET_CREDITS.md`. All assets are included locally, so opening the project does not require network access.
