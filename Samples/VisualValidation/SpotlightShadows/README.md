# PulseForge Spotlight Shadows

## Purpose

A small lighting room for inspecting Phase D's real-time spotlight shadow maps in the Vulkan renderer. The startup scene has a broad floor and back wall, matte and metallic block casters, two differently colored shadow-casting spotlights, three additional spotlights for slot/overflow tests, a modest HDR environment, and an authored game camera.

## Requirements and opening

Use a PulseForge build containing commit `6d167831b7c2aef3167fbfe852e04ffa1d5a760a` or later, with the Vulkan renderer and its normal deployed shader assets.

1. Launch `PulseForgeEditor` from a logged-in Windows desktop session.
2. Use **File → Open Project...** and choose `SpotlightShadows.pfproj` in this directory.
3. The project startup scene is `Assets/Scenes/SpotlightShadowsOverview.scene`. The editor loads that scene on project open; select it in the Content Browser and open it if switching projects left a different scene active.
4. Select one of the `Spotlight A/B/...` entities in the Hierarchy to inspect its Spot Light component.

The Scene Viewport uses the editor's transient viewport camera, not the authored primary camera. Frame the room with the existing viewport navigation if necessary. The primary camera is positioned for the standalone game's authored-camera path.

The current standalone sample client looks for a file named `PulseForgeGame.pfproj` and a deployed `Shaders` directory in its working directory; it does not yet accept an arbitrary project path. The editor is the supported way to open this named demo directly. The project was separately smoke-tested through the game path using a disposable runtime copy with the expected filename and the build's deployed `Shaders` directory; the repository demo itself was not renamed or altered.

## Scene and checks

### SpotlightShadowsOverview

The floor and rear wall receive shadows. A tall pillar, warm block, and suspended brass block provide varied silhouettes and separation from the floor. Warm matte, cool rough, brushed gold, and neutral material factors contrast without requiring transparency or alpha-masked shadows. Spotlight A and B cast shadows immediately and overlap at the central display area. C, D, and E illuminate but start with **Cast Shadows** disabled.

Try these checks from the Inspector:

- Toggle A or B **Cast Shadows** off and on. Direct illumination should remain when shadowing is off.
- Move or rotate a spotlight; its shadow should follow the light. Move a caster; its shadow should update.
- Compare **Shadow Softness** at `0`, `1.5`, and `4`. This is a 3x3 PCF texel radius, not physical penumbra simulation.
- Change **Shadow Bias** and **Shadow Normal Bias** to inspect acne versus detachment trade-offs.
- Enable C and D shadow casting to use all four map slots. Then enable E; it remains directly lit even when the shadow budget is full.
- Disable one of the current shadow owners and enable E to exercise deterministic slot reassignment.
- Save and reopen the scene to verify the settings persist. Enter Play and Stop to verify runtime edits remain isolated from authored scene state.

### SpotlightShadowLimits

Open `Assets/Scenes/SpotlightShadowLimits.scene` from the Content Browser for a preconfigured budget case. Five spots request shadows and are aimed at the same opaque display area from different positions. Four maps are available; the low-intensity, short-range E is deliberately the least relevant candidate at the authored view and should remain illuminated without a map. Each prepared frame selects the four highest-ranked eligible shadow requesters using camera relevance and stable UUID ties. Existing selected lights retain their physical map slots, but an overflow light cannot hold a slot over a newly higher-ranked eligible light. Toggle an owner off to let E enter the selected four, then back on to verify that E yields its slot immediately. The four slots are independent of the 32-light direct-illumination budget; an overflow spotlight continues to illuminate without shadows.

## Expected behavior and limits

Cast Shadows is opt-in and the project explicitly enables it on A/B. PulseForge supports four concurrent spotlight shadow maps at 1024x1024 D32. The existing 32-light combined point/spot selection is separate. A fifth selected shadow-requesting spot still contributes direct light but has no shadow map. Spotlights wider than 80 degrees outer half-angle also remain illuminated but unshadowed. Point lights do not cast shadows. Opaque mesh instances cast; alpha-tested and blended shadow casters, caster-frustum culling, and point-light shadows are not implemented. Directional shadows are intentionally omitted from this overview to make the spotlight contribution easier to isolate; add a Directional Light component to compare with CSM.

## Troubleshooting

- If the project is rejected, confirm the full folder (including `Assets` and sidecar `.meta` files) is present and open this `.pfproj`, not the parent Samples directory.
- If assets appear missing, use the Content Browser to confirm the `Models`, `Materials`, `Textures`, and `Environments` entries are registered. Every scene reference uses this project's UUIDs; no other project's registry is required.
- If geometry is dark, select Spotlight A/B and confirm they are enabled, Cast Shadows is checked, and their transforms point toward the room. Their local forward is `-Z`; the bundled transforms are already aimed into the scene. Exposure can also affect appearance.
- If the viewport initially shows another direction, use the editor's normal viewport navigation; the authored primary camera controls the standalone game, not the editor's transient viewport camera.
- Shaders are the engine's deployed `Triangle.hlsl` and shadow shaders. Do not copy an older shader into the project; rebuild/deploy the engine from a compatible revision if shader behavior does not match.
- This project contains no generated environment cache. The bundled HDR source can be processed offline on first use; subsequent use follows PulseForge's normal derived-cache behavior.

## Validation status

The project manifest, scene, UUID registry, managed references, material documents, mesh import, HDR asset, snapshot preparation, and actual GPU rendering should be checked with the documented workflow. The existing `--scene-spot-shadows` GPU test verifies shadow passes/resource behavior but does not read pixels. Automated frame submission is not visual acceptance; inspect the loaded viewport and game view interactively.
