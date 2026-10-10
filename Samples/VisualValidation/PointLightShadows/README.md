# PulseForge Point Light Shadows

## Purpose

This self-contained project demonstrates real-time omnidirectional point-light shadows in PulseForge. The startup scene has shadow casting enabled on its primary lights; no manual scene setup is required.

## Requirements and opening

Use a PulseForge build containing point-shadow support (introduced in the point-shadow milestone). In PulseForgeEditor, choose **File → Open Project**, open `PointLightShadows.pfproj`, then open `PointLightShadowsOverview.scene` if it is not selected automatically. The `.pfproj` startup-scene UUID refers to the Overview scene.

## Scenes

- `PointLightShadowsOverview.scene`: room overview, multiple receivers/casters, and independently configurable point lights.
- `PointLightShadowFaces.scene`: central emitter and named casters/receivers arranged around the six cardinal shadow directions.
- `PointLightShadowBias.scene`: close caster/receiver view for shadow acne and detachment controls.
- `PointLightShadowLimits.scene`: more eligible point lights than the renderer's two point-shadow slots.

## What to inspect

Select a named point-light entity and use the Inspector's **Cast Shadows** checkbox. Turning it off removes that light's occlusion while leaving its direct illumination on; turning it back on is applied on the next frame without changing Hierarchy selection. Move a caster or point light, and rotate the point-light entity: translation updates the maps, while point-light rotation does not rotate omnidirectional coverage. Navigate around the Faces scene to inspect +X, -X, +Y, -Y, +Z, and -Z coverage and the cube-face boundaries.

In the Bias scene, compare `Shadow Bias` around `0`, `0.001`, and `0.01`. Bias is normalized radial depth. `Shadow Normal Bias` is in world units; increase it cautiously because large values detach shadows. In the Faces or Overview scene, compare `Shadow Softness` at `0`, `1.5`, and `4`; it controls a bounded cube-direction PCF filter radius in texels, not physical area-light penumbra.

The renderer supports two simultaneously shadowed point lights. Selection is deterministic by the existing selected-light relevance ranking, with UUID tie-breaking; eligible overflow lights continue illuminating without shadows. Disable one owner or change relevance to exercise reassignment. This budget is independent of the four spotlight shadow maps and the 32 combined local-light illumination limit.

Save/reload to check component persistence. Enter Play and move lights/casters using available runtime tools; Stop restores the authored edit scene. Existing directional and spotlight shadows should continue working alongside point shadows.

## Assets and limitations

All models, materials, textures, metadata, and scenes are project-local. `ASSET_CREDITS.md` records the HDRI provenance. The demonstration uses supported static glTF cube geometry and the bundled PPM texture. The first-generation renderer uses two 512×512 D32 depth cubemaps (about 12 MiB raw); each assigned light renders six depth passes. The filter is a modest PCF approximation. Transparent/alpha-masked shadow casters, point-light shadow caching, adaptive resolution, and OpenGL scene rendering are not supported. Face-edge appearance requires visual inspection; automated command validation is not pixel-accuracy proof.

## Troubleshooting

- If assets appear missing, open this project manifest (not the game validation project) and allow the editor's project asset registry to build from the local `Assets` directory.
- If geometry is missing, confirm the `.gltf` and adjacent `.meta` files are both present and that the project was not opened from only a copied scene file.
- If the room looks unexpectedly dark, check the selected point-light's intensity/range, whether it is within the local-light selection budget, and whether exposure/IBL settings are appropriate. Overflow lights still illuminate but may not cast shadows.
- If the initial transient editor camera does not frame the authored game camera, use the editor viewport's normal focus/navigation controls; the standalone game uses the authored primary camera.
- If a shader deployment mismatch is suspected, rebuild `PulseForgeEditor` and `PulseForgeGame` from the same source checkout so the current engine shader assets are deployed.
