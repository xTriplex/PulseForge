# PulseForge Transparency

This self-contained project demonstrates material alpha masking, straight-alpha transparent blending, depth-tested ordering, double-sided materials, and a spotlight-lit cutout shadow silhouette.

## Open and inspect

1. Launch PulseForgeEditor using the Vulkan renderer.
2. Choose **File → Open Project** and open `Transparency.pfproj`.
3. Open `Assets/Scenes/TransparencyOverview.scene` if it is not selected automatically.
4. Compare `Fence - alpha-masked and shadow casting` with `Opaque alpha-ignored fence reference`. Open the masked fence material's **Material Details** utility and toggle `Alpha Mode` between **MASK** and **OPAQUE**.
5. Open the blue and amber panel materials in **Material Details** and compare **BLEND** with **OPAQUE**. Adjust the base-color alpha factor to see the background mix change.
6. Toggle `Double Sided` in the fence's Material Details and inspect the panel from its back in the Scene View.

## Expected behavior

The fence's transparent grid openings reveal the colored object behind it, and its spotlight shadow retains the cutout silhouette rather than becoming a solid box. The blue and amber panels blend over the objects behind them while still depth-testing against opaque geometry. A separate directional fill light provides shadow-independent illumination. The same authored scene is used by the standalone game camera. The opt-in `--scene-transparency` Vulkan test captures actual sRGB images, compares the cutout silhouette with spotlight, point-light, and directional shadows both off and on, and compares the blue BLEND panel against a matching OPAQUE material control.

## Implementation boundaries

- BLEND uses linear-HDR straight-alpha source-over blending, is drawn after opaque/masked geometry, depth-tests, and does not write depth. Transparent meshes are sorted back-to-front by transformed entity origin with UUID tie-breaking; intersecting or highly concave transparent meshes can still require asset splitting.
- BLEND materials do not cast shadows and do not write the SSAO normal-depth prepass. MASK materials participate in directional, spotlight, and point-light shadow passes using the base-color alpha and material cutoff.
- MASK and BLEND alpha multiplies the material base-color alpha factor, base-color texture alpha, and imported vertex-color alpha. OPAQUE ignores alpha for visibility. The importer defaults missing vertex color, including alpha, to white/one.
- Double-sided rendering disables face culling and reverses the shading/prepass normal for back-facing fragments. It does not change the authored mesh or transform.
- This is conventional alpha blending, not physical transmission, refraction, order-independent transparency, or colored translucent shadows.

## Troubleshooting

- If the project appears dark, verify the Vulkan renderer and the scene's `Key directional light`; the project does not require an external HDRI.
- If the scene is empty, confirm `TransparencyOverview.scene` is open and its model/material assets resolve in the Content Browser.
- `FenceGrid.png` is generated from source with `GenerateAssets.ps1` and is included in the project. The project works offline after checkout.
- If the Scene View camera does not match the authored game camera, select the scene camera and use the editor's normal framing/navigation controls; standalone rendering uses the authored primary camera.

## Manual acceptance checklist

- [ ] MASK shows the grid openings; OPAQUE fills them.
- [ ] The fence's shadow has openings when MASK is selected.
- [ ] BLEND panes reveal and tint the rear objects; OPAQUE hides them.
- [ ] Lowering/increasing base-color alpha changes blend strength immediately.
- [ ] Opaque geometry remains depth-correct in front of transparent panes.
- [ ] Double-sided panels remain visible from either side.
- [ ] Save/reload preserves alpha mode, cutoff, double-sided state, and factors.

## Requirements

Use a PulseForge Vulkan build with alpha masking and transparency support. Project assets are bundled for offline use; see `ASSET_CREDITS.md` for the included CC0 HDRI provenance.
