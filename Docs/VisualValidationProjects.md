# Visual Validation Projects

Visible engine and editor features should have a small project that can be opened and inspected directly. A focused sample complements automated tests when visual behavior is part of acceptance.

## When a demonstration is required

Create or update a dedicated demonstration project when a milestone introduces or substantially changes visible engine/editor behavior such as lighting, shadows, materials, camera rendering, post-processing, effects, or viewport composition. Backend-only infrastructure, tests, build changes, and documentation-only work do not require a new project. For corrections within one milestone, maintain that milestone's project. A later capability with a distinct purpose should have its own named project. Never overwrite an earlier project's accepted evidence.

## Project expectations

Place focused examples under `Samples/VisualValidation/<Feature>/` unless the feature or existing project tooling makes another location more suitable. Each project should be independently openable and include a `.pfproj`, a registered startup scene, project-owned supported assets and `.meta` sidecars, and a concise README. Use engine project/scene/asset services and formats; asset UUIDs must be unique within the project and all references must resolve. Avoid absolute machine paths, symlinks, hidden dependencies on another project, build outputs, caches, and user workspace settings.

Use a clear first-open camera/view and representative feature configurations. Provide a small number of scenes when separate test conditions are useful; do not multiply nearly identical scenes when a property toggle is clearer. Explain how to inspect normal behavior, relevant edge cases, and known limitations. Verify project parsing, startup-scene registration, asset registry construction, scene loading, references, and the supported runtime path. A clean GPU submission is not proof of pixel correctness; state when visual acceptance remains manual.

For downloaded or third-party content, verify the actual source, author, redistribution license, and attribution requirements before adding it. Keep provenance and required license notices in the project. Prefer lightweight compatible assets or locally authored geometry. The project must work offline once checked out.

## Current demonstration projects

| Project | Demonstrates | Inspection notes |
|---|---|---|
| [`Spotlight Shadows`](../Samples/VisualValidation/SpotlightShadows/README.md) | Persistent spotlight shadow maps, bias, PCF softness, multiple lights, and the four-map limit. | Includes Overview and preconfigured Shadow Limits scenes; the README describes the visual checks. |
| [`Point Light Shadows`](../Samples/VisualValidation/PointLightShadows/README.md) | Omnidirectional point-light shadows, six cube faces, face boundaries, bias/softness, multiple lights, and the two-map limit. | Includes Overview, Faces, Bias, and Limits scenes; the README describes the visual checks. |
| [`PBR Materials`](../Samples/VisualValidation/PBRMaterials/README.md) | Textured base color, packed metallic-roughness, tangent-space normal mapping, material occlusion, HDR emission, and Material Details. | Startup workbench has seven indexed meshes; GPU validation renders the project through the production output path. Material tiles show base-color-factor swatches, not rendered PBR previews. |
| [`Transparency`](../Samples/VisualValidation/Transparency/README.md) | OPAQUE, alpha-masked, and straight-alpha BLEND materials; double-sided shading; and cutout shadows. | Startup workbench includes a cutout fence and transparent colored panels; Vulkan image validation compares masked spotlight, point-light, and directional shadows. |
| [`Editor Workflow`](../Samples/VisualValidation/EditorWorkflow/README.md) | Editor selection, multiselection, hierarchy clipboard and duplication, visibility, focus, viewport navigation, transform snapping, and End surface placement. | Startup workbench includes flat ground, raised and sloped supports, centered-pivot objects, and a parent/child hierarchy; the README lists interaction checks. |

## Validation

When changing visible behavior, update the relevant sample in the same change where practical. Document the project path, startup scene, and inspection steps in its README. Validate asset registration, references, and project loading separately from visual appearance. Automated rendering or command submission does not establish visual acceptance; state which interactions still need direct inspection.
