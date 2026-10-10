# Visual Validation Projects

PulseForge's visual features should be inspectable in a project the user can open immediately, not only in an automated triangle test or a developer's private scene. This workflow makes a compact interactive demonstration part of the implementation deliverable whenever visual inspection is a meaningful acceptance requirement. The corresponding repository agent rule is in [`AGENTS.md`](../AGENTS.md).

## When a demonstration is required

Create or update a dedicated demonstration project when a milestone introduces or substantially changes visible engine/editor behavior such as lighting, shadows, materials, camera rendering, post-processing, effects, or viewport composition. Backend-only infrastructure, tests, build changes, and documentation-only work do not require a new project. For corrections within one milestone, maintain that milestone's project. A later capability with a distinct purpose should have its own named project. Never overwrite an earlier project's accepted evidence.

## Project expectations

Place focused examples under `Samples/VisualValidation/<Feature>/` unless the feature or existing project tooling makes another location more suitable. Each project should be independently openable and include a `.pfproj`, a registered startup scene, project-owned supported assets and `.meta` sidecars, and a concise README. Use engine project/scene/asset services and formats; asset UUIDs must be unique within the project and all references must resolve. Avoid absolute machine paths, symlinks, hidden dependencies on another project, build outputs, caches, and user workspace settings.

Use a clear first-open camera/view and representative feature configurations. Provide a small number of scenes when separate test conditions are useful; do not multiply nearly identical scenes when a property toggle is clearer. Explain how to inspect normal behavior, relevant edge cases, and known limitations. Verify project parsing, startup-scene registration, asset registry construction, scene loading, references, and the supported runtime path. A clean GPU submission is not proof of pixel correctness; state when visual acceptance remains manual.

For downloaded or third-party content, verify the actual source, author, redistribution license, and attribution requirements before adding it. Keep provenance and required license notices in the project. Prefer lightweight compatible assets or locally authored geometry. The project must work offline once checked out.

## Current demonstration projects

| Project | Demonstrates | Status |
|---|---|---|
| [`Spotlight Shadows`](../Samples/VisualValidation/SpotlightShadows/README.md) | Persistent spotlight shadow maps, bias, PCF softness, multiple lights, and the four-map limit. | Includes Overview and preconfigured Shadow Limits scenes; interactive visual acceptance is requested in its README. |
| [`Point Light Shadows`](../Samples/VisualValidation/PointLightShadows/README.md) | Omnidirectional point-light shadows, six cube faces, face boundaries, bias/softness, multiple lights, and the two-map limit. | Startup Overview plus Faces, Bias, and Limits scenes; interactive visual acceptance is requested in its README. |

## Handoff checklist

1. State why the milestone does or does not need an interactive demonstration.
2. Include the project and assets in the same implementation change when required.
3. Give the exact project path, startup scene, and opening steps in the final report.
4. Report asset/reference and application validation separately from manual visual acceptance.
5. Preserve existing demos and all unrelated user project/settings data.
