# PulseForge Agent Guidance

PulseForge uses C++23 and CMake. Engine public headers belong under `PulseForge/Source/Public`, implementations under `PulseForge/Source/Private`, client code under `PulseForgeGame`, and first-party tests under `Tests`. Preserve the engine/editor/client/vendor boundaries, use the existing Visual Studio 2022 CMake workflow on Windows, and do not edit pinned vendor code to work around engine integration issues.

Before changing the repository, inspect the actual Git state and applicable local instructions. Preserve existing user changes, projects, scenes, caches, layouts, preferences, and intentionally untracked files. Do not use destructive cleanup or rewrite published history. Review the full diff, run `git diff --check`, build/test affected targets, and report any unavailable validation rather than claiming it passed.

## Visual demonstration project requirement

Whenever a milestone introduces or substantially changes visually observable engine or editor functionality, determine whether interactive visual inspection is a meaningful acceptance requirement. If it is, create or update a dedicated, independently openable PulseForge demonstration project for that milestone before handoff. A milestone spanning several commits should maintain one project; a later feature with a materially different purpose gets a separate named project. Preserve previously accepted demonstration projects.

A suitable demonstration project must:

- Include its own `.pfproj`, startup scene, and all project-specific assets with valid sidecar metadata and unique asset UUIDs.
- Use representative working configurations plus useful edge-case scenarios; keep the startup view immediately understandable.
- Resolve all referenced managed assets without machine-specific paths, symlinks, or manual cross-project copying.
- Be directly openable in PulseForgeEditor and, where applicable, usable by the standalone game.
- Include a concise project README describing how to open it, expected behavior, controls to inspect, known limitations, and manual acceptance criteria.
- Include attribution and license records for externally sourced assets. Verify redistribution rights before inclusion. Prefer suitable in-repository assets or original procedural content when practical.
- Be validated beyond JSON parsing: rebuild its registry, check startup scene and asset references, and run the normal loading/render path where available. Clearly distinguish automated resource/draw checks from actual pixel or interactive visual verification.

Do not consider an implementation handoff complete without a suitable visual demonstration project when visual inspection is a meaningful acceptance requirement. Pure documentation, build-system, unit-test-only, backend-only infrastructure, and similarly nonvisual changes are exempt. Do not create empty placeholder projects or one oversized project for unrelated systems.

See [Docs/VisualValidationProjects.md](Docs/VisualValidationProjects.md) for the human workflow and current demonstrations.
