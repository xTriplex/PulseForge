# Asset provenance

This project is self-contained and has no runtime network dependency.

- `Assets/Models/ShadowDemoCube.gltf` is a project-owned copy of PulseForge's bundled `PulseForgeGame/Assets/Models/ValidationCube.gltf`, used as a reusable block mesh. No external model code or artwork is included.
- `Assets/Textures/ValidationTexture.ppm` is a project-owned copy of PulseForge's bundled `PulseForgeGame/Assets/Textures/Validation.ppm`. Authored material factors provide the scene's broad color and PBR differences.
- `Assets/Environments/studio_small_01_1k.hdr` is copied from PulseForge's bundled validation HDRI. Its source is Poly Haven's Studio Small 01 HDRI, credited to Poly Haven and distributed under CC0 1.0 Universal. See the repository's [HDRI provenance record](../../../Docs/PolyHavenHDRI.md), including asset page, official download, license, and checksums. The HDR is included locally so the demo works offline.
- The `.material` files and scene arrangement are authored for this demonstration using PulseForge's existing material and scene formats.

The mesh and texture copies receive fresh project-local asset UUIDs. They do not refer back to the validation project's registry.
