# Editor Workflow and Native Multi-Window Architecture

## Scope and progress

This record tracks the implementation authorized by `C:\Users\kaan_\Desktop\Pulseforge\PulseForge_Editor_UI_Architecture_Dossier.md` (read in full on 2026-10-13). It is a feature-branch implementation record; the dossier remains the product contract. The completed UI-polish and Phase 5 asset-preview work was merged to local `main` and validated before this branch was created.

| Milestone point | Status | Evidence / next action |
|---|---|---|
| Prior-work integration | Complete and published | `main` integration `e702e44c1a7e653c0af45dd3a398ee8d3931070e` was pushed normally to `origin/main`; the active feature branch is based on that commit. Debug/Release/Dist each passed 4,058 checks and CTest 2/2 at integration. |
| Fresh feature branch | Complete | `feat/editor-workflow-multi-window` starts at the integrated main SHA above. |
| Source and dependency audit | Complete | The current renderer ownership, pinned dependencies, ImGui backend, asset identity, and persistence boundaries are recorded below. |
| Asset/folder move transactions | Implemented and validated | `AssetOperations::MoveFolder` performs same-volume staged rename, rejects traversal/reparse points/self-descendants/collisions, rebuilds registry with rollback; Content Browser has distinct asset/folder drag payloads and defers the move until after item traversal. `TestAssetFolderOperations` passes in Debug/Release/Dist (4,081 checks); editor target builds in all three. |
| Document sessions and top strip | Implemented; interaction acceptance remains | The Editor uses project/asset/editor-kind identities, a persistent Level Editor document strip, per-host tabs, duplicate activation, dirty revisions, and the existing material draft/history state. |
| Native hosts and backend presentation | Implemented; runtime smoke passed | Vulkan uses per-window swapchain targets on the existing NVRHI device; OpenGL uses GLFW shared contexts and ImGui's OpenGL3 renderer. Isolated one-host and two-host startup/shutdown checks passed on both backends. |
| Transfer/preferences/persistence/dialogs | Implemented; acceptance matrix remains | Explicit top-strip transfers, opening preferences, versioned project workspace records, host bounds, dirty close routing, and in-editor asset destination workflows are present. Tab transfers are deferred until panel traversal ends; a tab context menu can detach to a new native host or reattach/regroup. Continue focused regression checks against dossier scenarios. |
| Final regression and handoff | In progress | Vulkan frame ordering and dirty-close prompt routing are corrected. Native minimize/restore, resize while the main window is minimized, fresh EditorWorkflow project-load runs, missing-asset workspace recovery, and native-host restart are recorded below. Application shutdown now suppresses collateral native-host close notifications until workspace state is saved; the fix passes all six Vulkan/OpenGL Debug/Release/Dist configurations. Pointer-driven transfer and dirty-close UI acceptance remain. |

## Repository and protected-state baseline

- Repository: `C:\Dev\PulseForge`.
- Starting source branch: `feature/editor-asset-previews`, HEAD `41d7560b94550907e134a4ba14e0a3e3ab052c13`.
- Integrated branch: `feat/editor-workflow-multi-window`, based on local `main` at `e702e44c1a7e653c0af45dd3a398ee8d3931070e`.
- Remote `origin/main` was verified at `e702e44c1a7e653c0af45dd3a398ee8d3931070e` after the authorized normal push of the completed predecessor integration. The new multi-window milestone remains isolated on its feature branch.
- Existing uncommitted user state remains unstaged: deleted `Docs/SpotlightShadowsResearch.md`; modified `PulseForgeGame/Assets/Scenes/Validation.scene`, `Samples/VisualValidation/PBRMaterials/Assets/Materials/EmissiveStudy.material`, `PackedAndNormal.material`, `Samples/VisualValidation/Transparency/Assets/Materials/BlueGlass.material`; untracked `CMakeSettings.json`.
- Repository-root `AGENTS.md` is intentionally absent. Applicable global instructions and the current visual-validation/build docs were read. No user changes were staged or rewritten.

## Requirements-to-code map (initial audit)

| Requirement group | Current implementation | Required direction |
|---|---|---|
| Main window/frame | `PulseForge/Source/Private/Core/Application.cpp`: one `Window`, one `RendererBackend`, one `BeginFrame`/`EndFrame`; minimized main window skips all layer rendering. | Keep Level Editor main host, but schedule registered native document hosts independently of main-window acquisition/minimize. Add native source identity and queued close requests. |
| Native event source | `PulseForge/Source/Private/Window/Platform/Windows/WindowsWindow.cpp`; `Events/*Event.h`: callbacks are installed for the one window and events have no native-window identity. | Carry stable runtime window identity through input/focus/close routing; callbacks remain chained to ImGui. |
| Editor shell/layout | `PulseForgeEditor/Source/Private/EditorApplication.cpp`, `EditorLayout.cpp`; one ImGui context and one dockspace with utility panels. | Add a persistent top document strip separate from menu, global toolbar, and utility docking. Keep utility dockspaces workspace-scoped. |
| Document identity/session | `EditorApplication.cpp`: one `m_OpenedMaterialAsset`, material drafts keyed by project root + asset UUID, one scene history. | Introduce project UUID + asset UUID + editor kind document identity; maintain independent model, dirty revision, history, selection, preview state, and workspace for each supported asset document. |
| ImGui backend | Vulkan: `EditorImGuiRenderer.cpp` custom NVRHI renderer, only main `ImDrawData`; OpenGL uses ImGui GLFW/OpenGL3 platform viewports. | Extend the existing Vulkan/NVRHI presentation path safely per native viewport; preserve OpenGL callback/context ownership. |
| Vulkan ownership | `PulseForge/Source/Private/Renderer/Vulkan/VulkanRenderer.cpp`: one instance/device, main surface/swapchain, per-image wrappers and semaphores, NVRHI graphics queue and main frame ring. Swapchain recreation waits idle; no present-completion fences. | Share the device/queues; give each host its own surface, swapchain generation, image/frame sync and safe presentation retirement. Serialize external queue operations. Do not use graphics completion as proof of present completion. |
| OpenGL ownership | `PulseForge/Source/Private/Renderer/OpenGLRenderer.cpp`; GLFW contexts are currently one main context. | Preserve shared-context semantics, restore current context around each host, use backend-neutral document rendering and verify texture lifetime. |
| Asset identity/moves | `AssetRegistry` UUID sidecars; `AssetOperations::Move` preserves asset UUID; `Duplicate` allocates a new UUID. Content Browser exposes modal move/rename but no drag/drop. | Add folder move with canonical containment, collision/self-descendant and reparse-point checks; use distinct drag payloads and coherent registry/UI refresh. |
| Dirty close/save | Scene close prompt exists; material editor draft map is session-only and excluded from application close; no document close policy. | Centralize dirty-revision-aware close handling for tabs, host close, project switch and application shutdown. |
| Workspace persistence | `EditorLayout` stores versioned utility visibility and ImGui layout under `%LOCALAPPDATA%` or isolated settings directory. | Persist document/host membership and order separately from ImGui internals; validate schema, project and native geometry before restore. |
| Asset dialogs | Native file dialogs and bespoke asset dialogs exist in `EditorApplication.cpp`. | Share in-editor project asset navigation dialogs and route results to the focused document; retain native external file/project pickers. |

## Document host decision

The dossier's exact policy requires document transfer only through an explicit, dedicated top strip. The pinned ImGui `ImGuiWindowClass` docking controls scope window classes and prevent class mixing, but ordinary ImGui docking targets are dock nodes across the host window; they do not express a geometric rule that accepts a document drop only on a strip while rejecting workspace bodies and all side/bottom edges. Hiding docking previews or repairing a drop after layout mutation would violate the contract. Therefore the selected direction is **application-managed document hosts and explicit transfers**, with an explicit host/document policy independent of the UI mechanism. Utility windows may retain ImGui docking inside each registered workspace.

This decision is implemented through the application-managed `EditorDocumentManager` host records and explicit transfer actions. Native material hosts use the existing ImGui GLFW platform-window callbacks. The Vulkan renderer creates an independent surface/swapchain per native host while sharing the existing device and queue. A frame-ordering correction now submits the main frame before processing native windows and pairs each native render with its present, because the NVRHI immediate command context permits only one open command list at a time. The OpenGL path uses the pinned GLFW/OpenGL3 platform callbacks in the same per-window sequence. Runtime smoke evidence is recorded in the final section below; pointer/keyboard drag and mixed-monitor behavior still require interactive acceptance.

## Dependency snapshot

| Dependency | Observed revision/version | Evidence / qualification |
|---|---|---|
| Dear ImGui | gitlink `60d7fb207eeb46d6363dd4bde10b35991bae0ce7`; `IMGUI_VERSION_NUM 19265` (1.92.7 WIP) | `PulseForge/Vendor/imgui`; no vendored source changes planned. |
| GLFW | 3.4.0 | Parent-repository source snapshot under `PulseForge/Vendor/glfw`; source is not a Git submodule, so no upstream SHA is asserted. |
| NVRHI | `6b96fb03e07539f08327aea76c56d55f1de9d906` | CMake FetchContent pin and inspected fetched source. |
| Vulkan-Headers | `015e25c3c91b70eb1a754d36fb14c4ba6ad9b0b9` (configured tag `v1.4.352`) | Inspected fetched source. |
| DXC | 1.9.2602.24, release archive `2026_05_27` | CMake pinned version and SHA-512; executable present in the existing build tools directory. |

## Renderer ownership and retirement table (audited baseline)

| Resource | Current creator/owner | Current users/submission authority | Required multi-window lifetime |
|---|---|---|---|
| GLFW main window | `WindowsWindow` owned by `Application` | GLFW callbacks + main ImGui platform backend | Main host outlives all secondary presentations; destroy secondary windows on their owning UI/platform thread. |
| Vulkan instance/device/graphics queue | `VulkanRenderer` | NVRHI plus Vulkan WSI calls | Shared by all document hosts; serialize host access to any shared Vulkan queue. |
| Vulkan surface/swapchain | `VulkanRenderer` for main | Vulkan acquire/present; NVRHI wraps swapchain images | One independent surface/swapchain generation per host. Retire old generations only after graphics and presentation obligations complete. |
| Swapchain image wrapper/framebuffer/depth target | `VulkanRenderer::SwapchainImage` | NVRHI command list during acquired frame | Generation-owned, retained through any queued command/present use. |
| Image-available/present semaphores | Main renderer frame/image records | NVRHI queue waits/signals and `vkQueuePresentKHR` | Per-host synchronization; successful acquisition must be consumed or safely released; failed acquisition must not submit a reset/un-signaled fence. |
| NVRHI command lists/queue timeline | `VulkanRenderer` frame contexts | `RendererBackend` draw APIs and queue retirement | Secondary work must use same NVRHI device/queue integration or a rigorously synchronized compatible path. |
| ImGui registered image bindings | `EditorImGuiRenderer::m_TextureBindings` | UI batches and NVRHI submissions | Retain IDs/resources until all draw-data and submitted frame users retire; host close must drain or retire safely. |
| ImGui context/font atlas | `EditorLayer` | Main frame and OpenGL platform windows | Prefer one context; any additional context requires explicit input/font/texture/persistence ownership. |
| Scene/material preview targets | Existing editor/scene renderer services | Viewport and material UI | Independent view identity and resize/lifetime; no singleton output aliasing across visible documents. |

### Phase 1 implementation note

`PulseForge::AssetOperations::MoveFolder` moves an entire managed folder while preserving its `.meta` files and all contained UUIDs. It validates normalized project-relative paths, all directory components, destination collision, source-tree links/reparse points, and self/descendant moves. A unique sibling staging path allows a same-volume rename; registry rebuild failure attempts to roll the directory back and reports recovery-required if rollback fails. The Content Browser now uses distinct `PF_MANAGED_ASSET_MOVE` and `PF_MANAGED_FOLDER_MOVE` payloads; payload delivery is converted to a deferred action so the registry/tree is not mutated during item traversal. Existing single-asset move semantics remain unchanged. `Docs/Editor.md` records the behavior. `git diff --check` passed. Latest fresh target tests: Debug 4,081 checks / CTest 2/2; Release 4,081 / 2/2; Dist 4,081 / 2/2. Temporary per-config test logs are build artifacts and are not staged.

The initial audit phase is complete. The initial UI implementation was subsequently expanded into native document hosts, the persistent top strip, per-document material state, project-scoped workspace persistence, close routing, and asset destination dialogs. Implementation checkpoint `22d256866c7622a4391309fba4473513ea1f5182` is committed on `feat/editor-workflow-multi-window`; final acceptance and scenario coverage remain in progress.

### Phase 2 model prototype

The `EditorDocumentManager` model lives in
`PulseForgeEditor/Source/Public/Editor/EditorDocumentManager.h` and
`PulseForgeEditor/Source/Private/EditorDocumentManager.cpp`. It uses `(project UUID, asset UUID, editor kind)` as stable identity, so same-basename assets and equal UUIDs from different projects remain distinct. The Level Editor is constrained to the main host. Opening an existing identity updates its path/display label and activates the existing host rather than duplicating the session. Transfers call a destination preparation callback before changing membership, and return an emptied native-host identity for the caller to retire only after presentation succeeds. Dirty tracking uses edit/saved revisions; a save callback that advances the session during save leaves the later revision dirty.

Deterministic tests in `TestEditorDocumentManager` cover duplicate activation, project isolation, remembered host placement, Level host protection, transfer rejection/commit, dirty save/discard/cancel, failed save, an edit arriving during save, and project cleanup returning empty native hosts for renderer retirement. The Editor now consumes those host/session records for actual material document windows and keeps the Level Editor in the main host. The native-window runtime checks and the later Vulkan frame-ordering correction are recorded below.

## Vulkan frame-ordering defect and correction (2026-10-13)

When opening `EditorWorkflow` with a restored floating material document, Khronos validation reported NVRHI's “Two or more immediate command lists cannot be open at the same time” followed by commands issued on an `INITIAL` list, an unsignaled present semaphore, an unconsumed acquire semaphore, and a presented image still in `UNDEFINED` layout.

The source trace showed that `Application::Run()` opened the main Vulkan frame command list, then `EditorLayer::OnRender()` called Dear ImGui's `RenderPlatformWindowsDefault()` before `RendererBackend::EndFrame()`. The pinned ImGui helper runs every platform `Renderer_RenderWindow` callback before any `Renderer_SwapBuffers` callback. The custom Vulkan callback therefore attempted to open a second NVRHI immediate command list while the main one was still open; with multiple native windows, the default two-pass callback order would also overlap target lifetimes.

`Layer::OnRenderPlatformWindows()` now runs after the main `EndFrame()`. The Editor performs platform update and processes each viewport's platform render, renderer render, platform swap, and renderer swap callbacks as one sequence before moving to the next viewport. `VulkanRenderer::BeginPlatformPresentationTarget()` also refuses to start while the main frame or another target is active. This keeps the one immediate command list open at a time. A deterministic callback-order regression test was added in `TestEditorPlatformViewportSubmissionOrder`.

Fresh Visual Studio 2022 x64 results after this correction:

| Build | `PulseForgeTests` | CTest |
|---|---:|---:|
| Vulkan Debug | 4,162 checks passed | 2/2 |
| Vulkan Release | 4,162 checks passed | 2/2 |
| Vulkan Dist | 4,162 checks passed | 2/2 |
| OpenGL Debug | 4,162 checks passed | 2/2 |
| OpenGL Release | 4,162 checks passed | 2/2 |
| OpenGL Dist | 4,162 checks passed | 2/2 |

An isolated Vulkan Debug run restored one native `Mint.material` window, then a second run restored two native windows (`Amber.material` and `Blue.material`). Both reported that Khronos and NVRHI validation were enabled. The two-host run created two presentation targets, ran through multiple frames, emitted no NVRHI command-list errors, Vulkan VUIDs, or acquire/present semaphore validation errors, and exited with code 0 after a normal main-window close. Logs are in `Build/CMake/VS2022-x64/Diagnostics/VulkanFrameSchedule/`.

An isolated OpenGL Debug run restored two native material windows, captured the main editor and both actual OpenGL framebuffers, showed the documented Vulkan-only Scene Viewport fallback, emitted no OpenGL errors or Vulkan-preview operations, and exited with code 0. The captured main editor and both material windows were opened and visually inspected. The BMPs are under `Build/CMake/VS2022-x64-OpenGL/Diagnostics/OpenGLFrameSchedule/Settings/`. The dedicated CMake tree reports `PULSEFORGE_EDITOR_RENDERER=OpenGL`; its Debug, Release, and Dist builds/tests all passed. These test artifacts are ignored build outputs and are not source deliverables.

The Vulkan loader also printed three `Loader Message` warnings for optional third-party layer manifests that were missing on this machine (TikTok LIVE Studio and RivaTuner). They are distinct from Khronos validation messages; the test's captured VUID and NVRHI command-list error counts were both zero.

### EditorWorkflow project-load rerun (2026-10-13)

The Debug executable at `Build/CMake/VS2022-x64/Debug/PulseForgeEditor.exe` (built 2026-10-13 10:16) was launched with the `EditorWorkflow.pfproj` project and an isolated settings directory under `Build/CMake/VS2022-x64/Diagnostics/EditorWorkflow_ProjectLoad_20261013/`. It reached the first rendered viewport frame; project-open-to-first-frame profiling reported 156.63 ms. Khronos validation and NVRHI validation both logged that they were enabled. The process closed through its normal window-close path.

The captured output contains zero NVRHI immediate-command-list state errors, zero Vulkan VUID/UNASSIGNED validation errors, and zero stderr lines. Three Vulkan loader error-severity messages report missing optional TikTok LIVE Studio and RivaTuner layer manifest files; they are external loader warnings and did not prevent validation-layer or renderer startup. Full logs are `stdout.log` and `stderr.log` in that diagnostics directory. This run confirms the current Debug binary's EditorWorkflow project-load path; native-window frame ordering is additionally covered by the two-host Vulkan run above. The user's 09:31 messages match the frame-order defect corrected in commit `22d2568`; because those messages predate the 10:16 Debug executable, the evidence is consistent with a pre-correction binary, but the exact executable path from that earlier run was not captured.

A second run used a copy of the previously validated two-material workspace in `Build/CMake/VS2022-x64/Diagnostics/EditorWorkflow_TwoHostRerun_20261013/settings/`. The same current Debug executable restored both `Amber.material` and `Blue.material`, created two Vulkan presentation targets, and reached the first viewport frame in 205.34 ms. Khronos and NVRHI validation were enabled; the run logged zero immediate-command-list state errors, zero VUID/UNASSIGNED errors, and zero stderr lines. It exited after a clean native-window and main-window close sequence. The captured run logs are `stdout.log` and `stderr.log` in `EditorWorkflow_TwoHostRerun_20261013/`.

The reported `RoughClay.material` case was also launched from a disposable copy of `Samples/VisualValidation/PBRMaterials` with a one-document workspace restoring RoughClay (UUID `10000000-0000-4000-8000-000000000001`) as a native window. The current Debug executable created the native presentation target, reached the first rendered viewport frame in 585.11 ms, and exited with code 0 after clean close. Khronos and NVRHI validation were enabled; counts for NVRHI immediate-list errors, VUID/UNASSIGNED errors, and stderr lines were zero. Logs are under `Build/CMake/VS2022-x64/Diagnostics/RoughClay_Open_Repro_20261013/`. The project and its materials were copied before launch, so this reproduction did not write to the protected sample assets.

### Native-window lifecycle regression run (2026-10-13)

On the verified private desktop `WinSta0\CodexSandboxDesktop-26df385c4bfa1d48642b940a93416f14`, Debug PID 17580 hosted the main editor and two material windows. The test minimized/restored a native material window, minimized the main editor, resized another material host from 560×760 to 624×808, and captured the visible material UI while the main editor remained minimized. Renderer output created the resized native swapchain depth images. Both native hosts and then the main window received clean close requests; the process exited with code 0. Khronos and NVRHI validation were enabled. Counts for VUID/UNASSIGNED messages, NVRHI command-list state errors, and stderr lines were all zero. Artifacts are in `Build/CMake/VS2022-x64/Diagnostics/NativeWindowLifecycle_20261013/`, including `material-while-main-minimized.png`, `lifecycle-capture.json`, `lifecycle-result.json`, and the Vulkan stdout/stderr logs. The screenshot is direct evidence that the material host rendered while the main window was minimized; no pointer drag was completed in this run.

### Dirty-close prompt routing correction (2026-10-13)

Commit `0c41a7e0a1e09b9c8a5227301185ff2f64a8ca19` routes a document-close prompt to the host that initiated the close. A native material-window close now presents Save/Discard/Cancel in that material host; application-wide close prompts remain in the main host. The popup identity is scoped per host and the route clears on resolution or dismissal. This avoids hiding the decision behind a minimized main window. The correction builds and passes the full six-configuration test matrix below; rendering and interaction of a dirty native-host prompt still require manual acceptance.

### Deferred document-strip transfers and tab-close presentation (2026-10-13)

Document transfer and tab close requests from the tab strip/context menu are queued during ImGui panel rendering and applied only after `DrawWorkspacePanels()` completes. This avoids erasing a source native host or document session while `DrawNativeDocumentHosts()` or `DrawDocumentTabStrip()` is traversing host/session pointers. The context menu provides `Move to New Native Window` for main-strip asset documents, `Move to Main Document Strip` for native documents, and compatible same-project native destinations for regrouping. A failed transfer preparation leaves the source membership and session revision unchanged; manager policy rejects cross-project hosts and native hosts registered for a different editor kind. `TestEditorDocumentManager` covers detach, reattach, failed preparation, dirty revision preservation, and project/type isolation.

The tab close affordance is drawn inside the same selectable rectangle and item identity as its document label. The close glyph sits beside the label in a reserved click region; it uses the tab's active/hover treatment rather than a separate button backing, with a close tooltip on hover. User review found the 1.75× glyph too small, so it is now 2.5× the active UI font with a 24 logical pixel click region. The glyph remains within the tab's existing selectable surface. The post-fix isolated OpenGL capture at 1280×720 shows four material documents in the main strip. It was captured on `WinSta0\\CodexSandboxDesktop-26df385c4bfa1d48642b940a93416f14` using the post-ImGui `GL_BACK` framebuffer readback diagnostic; manifest: `Build/CMake/VS2022-x64-OpenGL/Diagnostics/DocumentTabTearOff_20261013/tab-close-large.manifest.json`; image: `Build/CMake/VS2022-x64-OpenGL/Diagnostics/DocumentTabTearOff_20261013/tab-close-large.bmp`. I opened and inspected the image; the close marks now match the neighboring dock-tab close marks in apparent size. This is visual evidence for presentation, not proof of close-click interaction.

The tear-off path uses the captured source-tab identity through the mouse-release frame. It no longer requires ImGui's drag payload to remain alive after release outside a drop target; an already queued destination transfer still takes precedence, and release over any editor viewport does not tear off. This addresses the reverse of native-to-main reattachment. Direct drag interaction still needs manual acceptance because the private-desktop input probe could not move the pointer.

The document tab strip supports drag-out detachment in addition to strip-to-strip transfer. Releasing a captured tab drag outside every editor viewport queues a new native host for the same asset session and places the host near the drop point. It no longer requires ImGui's drag payload to remain alive on that release frame; that condition prevented a reattached tab from reliably tearing off when the payload was cleared outside a drop target. A queued strip transfer still takes precedence, and release over an editor viewport without an accepted strip target leaves the session in its current host. Utility panels and workspace bodies are not docking targets. The operation runs after ImGui panel traversal, uses the existing transfer rollback path, and removes an emptied native source host only after transfer succeeds. It applies to tabs in either the main strip or an existing native document host. A focused policy test covers release, captured drag state, accepted transfer, and viewport conditions; document-manager tests verify transfer and dirty revision retention. The private-desktop input probe could not move the pointer, so direct drag/tear-off remains for manual acceptance.

Open document labels and project-relative paths are synchronized by asset UUID whenever the project asset list is refreshed. The session location update does not change host membership, edit revision, or dirty state. Unit checks cover the metadata update; moving/renaming a material while its editor is open remains a direct UI acceptance check.

After the tab size, drag-out, and asset-location changes, Visual Studio 2022 x64 rebuilt `PulseForgeEditor` and `PulseForgeTests` for both configured renderers and all three configurations. Each freshly built test executable passed 4,198 checks and CTest passed 2/2:

| Configuration | PulseForgeTests | CTest |
|---|---:|---:|
| Vulkan Debug | 4,198 | 2/2 |
| Vulkan Release | 4,198 | 2/2 |
| Vulkan Dist | 4,198 | 2/2 |
| OpenGL Debug | 4,198 | 2/2 |
| OpenGL Release | 4,198 | 2/2 |
| OpenGL Dist | 4,198 | 2/2 |

A fresh isolated OpenGL EditorWorkflow launch rendered four material tabs with the updated glyph and exited normally with code 0. The startup log confirms the OpenGL ImGui backend; stderr was empty. The screenshot is a genuine OpenGL framebuffer readback. The manifest records the verified visible `GLFW30` process window, capture dimensions, and private-desktop launch. Direct drag interaction remains untested. The image, manifest, logs, and six-configuration tests are under `Build/CMake/VS2022-x64-OpenGL/Diagnostics/DocumentTabTearOff_20261013/` and `Build/CMake/VS2022-x64/Diagnostics/DocumentTabTearOff_20261013/`.

The follow-up preference recovery and tab tear-off changes were each rebuilt and tested across Vulkan and OpenGL Debug, Release, and Dist. The latest source passes 4,212 `PulseForgeTests` checks and CTest 2/2 in all six configurations. The close-glyph capture reflects the updated Debug OpenGL source; drag interaction is still manual.

### Shutdown persistence correction

An isolated workspace containing one valid material document and one document whose asset UUID was absent from the reopened project's registry was used for a Vulkan Debug restart check. The editor restored the valid native host and ignored the missing asset. A normal close initially proved risky because the main-window close can deliver platform-viewport close requests during the same shutdown frame; processing those requests could retire the restored hosts before the final workspace snapshot. `m_ApplicationShutdownRequested` now gates native viewport close-request handling after an accepted application-wide close. Dirty-close cancellation leaves the gate off, and accepting a dirty close sets it through the same close helper. This preserves native host membership for the final workspace save without rebuilding the dock layout.

The corrected run restored only `native:valid-recovery-check`; after a graceful main-window close, `documents-f0111111-1111-4111-8111-111111111111.json` still contained that host and its material UUID. The missing host was absent from the post-restore workspace. Khronos validation was enabled, stderr was empty, and the runtime log contained no Vulkan VUID or NVRHI command-list errors. Evidence is under `Build/CMake/VS2022-x64/Diagnostics/WorkspaceMissingAssetRestore_20261013/absolute-path-rerun/`; the pre-close snapshot is `workspace-after-restore-before-close.json`. The test process was closed by posting `WM_CLOSE` to its identified main `GLFW30` project window on `WinSta0\\CodexSandboxDesktop-26df385c4bfa1d48642b940a93416f14`; it exited normally.

After this correction, Visual Studio 2022 x64 rebuilt `PulseForgeEditor` and `PulseForgeTests` for Vulkan and OpenGL in Debug, Release, and Dist. Every fresh test executable passed 4,200 checks and CTest passed 2/2. The Vulkan cache is configured with `PULSEFORGE_EDITOR_RENDERER=Vulkan`; the separate OpenGL build tree is configured with `PULSEFORGE_EDITOR_RENDERER=OpenGL`.

| Configuration | PulseForgeTests | CTest |
|---|---:|---:|
| Vulkan Debug | 4,200 | 2/2 |
| Vulkan Release | 4,200 | 2/2 |
| Vulkan Dist | 4,200 | 2/2 |
| OpenGL Debug | 4,200 | 2/2 |
| OpenGL Release | 4,200 | 2/2 |
| OpenGL Dist | 4,200 | 2/2 |

### Native material load and multi-host restart validation

The earlier user-reported `RoughClay.material` open failure and NVRHI/Vulkan presentation errors were retested on the current Vulkan Debug editor. A disposable copy of the PBRMaterials project restored `RoughClay.material` into a real native document host while the main `PBRMaterialsOverview.scene` window was active. The process reached the first presented Vulkan frame, had Khronos and NVRHI validation enabled, produced no VUID, NVRHI command-list, semaphore, or presentation-layout errors, wrote zero stderr bytes, and exited after a graceful main-window close. Evidence: `Build/CMake/VS2022-x64/Diagnostics/NativeMaterialDocumentRuntime_20261013/` (PID 14800 on the private sandbox desktop).

A copied EditorWorkflow project then restored `Amber.material` and `Blue.material` into separate native GLFW windows alongside the main `EditorWorkflowOverview.scene` host. The run was closed cleanly, and its saved workspace was copied to a fresh settings directory and opened again. The restart enumerated the same two material windows and main scene host, with the same validation layers enabled and no VUID, NVRHI command-list, semaphore, or stderr errors. Both runs used one editor process; PIDs were 23704 and 12156 on `WinSta0\\CodexSandboxDesktop-26df385c4bfa1d48642b940a93416f14`. Logs, project copy, and saved/restarted settings are under `Build/CMake/VS2022-x64/Diagnostics/EditorWorkflowMultiHostRuntime_20261013/`.

### Native swapchain resize and retirement

Two restored EditorWorkflow material hosts were resized together through four bounded size pairs while the main window and normal frame loop remained active. All eight `SetWindowPos` requests succeeded, both native windows survived all cycles, Khronos and NVRHI validation were enabled, and the run produced no VUID, command-list, semaphore, device-loss, or stderr errors. The main window then closed the editor cleanly. Machine-readable results and complete logs are under `Build/CMake/VS2022-x64/Diagnostics/NativeWindowResize_20261013_Bounded/`.

The resize path is synchronized in `VulkanRenderer.cpp`: it waits each native host's frame contexts and idles the present queue before releasing the old swapchain images; native-host retirement follows the same frame/present completion rule. The bounded test allowed the normal frame loop to process each resize pair; those host frame and queue waits provide the resource-lifetime guarantee, not the test's 900 ms spacing. This is scoped to swapchain recreation/host retirement, although the present-queue wait can pause presentation during a resize.

### Opening preference parsing and recovery

`EditorLayout::LoadPanelState()` now applies `DocumentOpeningPreference` only after the complete recognized workspace state has been checked. A missing or invalid state format, absent preference, malformed value, or value outside the supported range resolves to the default placement instead of leaking a partially parsed preference from rejected state. Deterministic tests cover all three supported values, leading-zero compatibility, malformed and out-of-range values, invalid/missing-state fallback, and an `EditorLayout` save/reload round trip across fresh ImGui contexts using a unique temporary settings directory. The preference menu choice and a full editor-process restart remain manual acceptance items.

## Acceptance tracking at implementation checkpoint

`Pass` means the listed evidence directly covers the behavior. `Partial` means a policy or bounded smoke check passed while the complete user-facing scenario remains unverified. `Unverified` means no suitable evidence has been collected. Screenshots and runtime startup do not prove pointer/keyboard behavior.

| Scenario | Status | Evidence / remaining check |
|---|---|---|
| D01 Level Editor remains in main host | Pass | Document-manager policy and captured main workspace. |
| D02 supported material documents open in native windows by default | Pass | Isolated Vulkan and OpenGL runs created actual material windows. |
| D03 document transfer occurs through the top strip | Partial | Strip transfer targets remain the only docking targets; releasing a tab outside all editor viewports now queues native detachment. Policy tests pass; direct drag/reattach pointer use remains manual because sandbox pointer movement was denied. |
| D04 utility panels/workspace body reject document docking | Partial | Native hosts use `ImGuiWindowFlags_NoDocking`, and document payloads are accepted only by explicit document-strip targets; a pointer-driven negative drop remains manual. |
| D05 side/bottom-edge drops cannot split a document host | Partial | The host-level NoDocking flag prevents document-host splits; a pointer-driven edge-drop attempt remains manual. |
| D06 detach, reattach and regroup retain the same session | Partial | Transfer rollback and dirty revision retention are unit-tested, including a native-to-new-native tear-off. Real drag/reattach pointer state retention remains manual. |
| D07 equal basenames remain distinct by asset identity | Pass | Document-manager identity tests include same-name/different-folder identities. |
| D08 move/rename keeps an open session attached to its asset | Partial | Registry refresh updates the open session label/path by UUID without changing dirty revision or host; managed move with the editor open remains manual. |
| D09 Level Editor cannot be moved out of the main host | Pass | Document-manager protection test. |
| D10 multiple real documents share one editor process | Pass | Two native material windows observed under one process on both backends. |
| D11 tab/native close uses dirty-document policy | Partial | Close policy is unit-tested; dirty native-X interaction remains. |
| D12 saved document arrangement restores | Pass | Two native material hosts restored from an isolated versioned workspace, were saved on clean close, and restored again on a fresh process; host membership, active host, order, and native bounds remained in the workspace record. |
| D13 opening preference options and default behavior | Unverified | Exercise preference menu and each placement choice. |
| D14 activating another document preserves Level Editor availability | Pass | Main Level Editor remained present alongside two material hosts. |
| C01 dirty tab close offers Save/Discard/Cancel | Partial | Policy tests cover choices and prompt routing is implemented per host; actual dirty native-host dialog remains unverified. |
| C02 native X and Alt+F4 request document-aware close | Partial | Clean `WM_CLOSE` requests for two native hosts and the main host exited normally; dirty native X and Alt+F4 behavior remain unverified. |
| C03 Cancel keeps the session open | Pass | Document-manager close policy test. |
| C04 failed Save preserves the dirty session | Pass | Save-failure policy test. |
| C05 multi-document host close resolves all dirty sessions | Partial | The host-close prompt now lists only dirty member documents; deterministic mixed clean/dirty and all-clean filtering tests pass. Actual multi-document Save/Discard/Cancel interaction remains manual. |
| C06 focused document receives Save/Undo commands | Unverified | Test commands while a floating material document is focused. |
| C07 text fields and modals isolate shortcuts | Unverified | Keyboard interaction check remains. |
| C08 camera capture releases on focus/document changes | Partial | Existing capture policy tests pass; multi-window focus transitions remain untested. |
| C09 edits arriving during Save remain dirty | Pass | Revision test simulates a later edit during save. |
| C10 layout reset preserves unsaved documents | Unverified | Direct reset-layout and dirty-document check remains. |
| R01 multiple native windows create and close independently | Pass | Two-window Vulkan and OpenGL startup and clean shutdown. |
| R02 native minimize/restore and resize handling | Pass | Private-desktop run directly minimized/restored a material window and resized a second host; new swapchain extent was logged. |
| R03 secondary rendering continues with main minimized | Pass | Material host rendered and was captured after the main editor was minimized; see `NativeWindowLifecycle_20261013/material-while-main-minimized.png` and capture manifest. |
| R04 negative desktop coordinates and mixed DPI | Partial | Negative coordinates are accepted by workspace validation; physical mixed-DPI movement is untested. |
| R05 resources retire while frames are in flight | Pass | Native swapchain recreation/host retirement waits the host frame contexts and presentation queue; two-window resize cycles and shutdown completed with validation enabled and no errors. |
| R06 independent asset preview targets/state | Unverified | Material contents render in separate windows; independent preview-target behavior needs a dedicated check. |
| R07 Vulkan Khronos/NVRHI validation | Pass | Both layers enabled; two-host run had zero VUID and NVRHI command-list errors. |
| R08 OpenGL native windows and ImGui contexts | Pass | Two actual OpenGL material framebuffers captured and inspected; exit code 0. |
| R09 injected presentation/resource failures recover safely | Unverified | Failure injection is not implemented or exercised. |
| R10 concurrent native resize generations remain valid | Pass | Two native windows were resized together in four bounded cycles; all resize calls succeeded, both hosts remained present, and validation stayed clean. |
| R11 surface/device loss handling | Unverified | Hardware/backend failure path not exercised. |
| R12 in-flight resource retirement on native close | Pass | The Vulkan host-destroy path waits per-host frame completion and presentation-queue idle before releasing swapchain resources; the multi-host runtime closed cleanly after normal frame submissions with validation enabled. |
| P01 valid document workspace restores | Pass | Isolated workspace file restored real native material sessions. |
| P02 removed/unavailable monitor recovery | Unverified | Requires monitor-topology testing. |
| P03 corrupt workspace/missing asset recovery | Pass | Codec rejects malformed/duplicate state in tests; an isolated restart restored the valid material host, omitted the missing-asset host, and preserved the valid host on normal shutdown. |
| P04 opening preference persistence | Partial | Parsing, range checks, bad-version fallback, and `EditorLayout` save/reload are covered in deterministic tests; selecting each menu option and verifying a full editor-process restart remain manual. |
| P05 project-internal asset/folder move preserves managed identity | Pass | Move operation and folder transaction tests pass. |
| P06 self/descendant folder moves reject | Pass | Asset-folder policy tests. |
| P07 name collision and case-only path checks | Pass | Move operation tests cover collisions and normalized paths. |
| P08 symlink/reparse and project-boundary move checks | Pass | Move validation rejects reparse paths and traversal. |
| P09 floating-host asset dialogs route results to the caller | Unverified | Direct dialog invocation from a native editor remains. |
| P10 asset destination dialog navigation/validation | Partial | Dialog implementation exists; keyboard, collision and error presentation need manual acceptance. |

## Manual acceptance still required

- Drag a material tab from a native window to the main document strip, then drag it back out onto the desktop to create a native window. Reattach it and regroup it; verify the same dirty draft and undo history remain.
- Click the enlarged tab close mark; test dirty native-window X and Alt+F4 with Save, Discard, and Cancel, plus a failed Save and a multi-document host close.
- Focus two different material windows and verify Save/Undo target the focused document. Check text entry and modal dialogs do not trigger editor shortcuts.
- In Window preferences, select new native window, main document strip, and remembered placement; restart and verify the choice and host arrangement persist.
- Reset the utility layout with an unsaved material open; verify the document and draft remain. Close/reopen utility panels and confirm their saved layout.
- Move or rename an open material through the Content Browser; verify its UUID, open session, label, draft, and references remain correct. Exercise destination dialogs from a floating material editor.
- Test mixed-DPI and negative desktop coordinates on physical monitors, including monitor removal if available. Verify native windows remain reachable after minimize/restore and main-window minimization.
- Recheck viewport picking, gizmos, camera capture/focus loss, Play/Stop, Inspector editing, Content Browser navigation, and Console shortcuts after switching among hosts.

No physical multi-monitor transition or direct pointer-driven document drag has been claimed as tested in the isolated Codex desktop.

## Git checkpoint

- Integrated local/remote `main` baseline: `e702e44c1a7e653c0af45dd3a398ee8d3931070e`.
- Feature branch: `feat/editor-workflow-multi-window`.
- Implementation checkpoint: `22d256866c7622a4391309fba4473513ea1f5182` (`Implement native editor document workflows`).
- Frame-order correction: `22d256866c7622a4391309fba4473513ea1f5182` (`Implement native editor document workflows`); dirty-close prompt routing: `0c41a7e0a1e09b9c8a5227301185ff2f64a8ca19` (`Route dirty document prompts to their host`). Later commits update this validation record.
- Tab/transfer correction: `96782c3` (`Defer document transfers and integrate tab close affordance`) integrates the close glyph into the selectable tab and adds deferred transfer/context-menu actions; `42f23f9` (`Defer document tab close until after UI traversal`) also defers close mutation until tab/host traversal has finished.
- Vulkan/OpenGL Debug, Release and Dist were rebuilt after `42f23f9`; each test executable passed 4,182 checks and CTest passed 2/2.
- Opening preference recovery policy is covered by 12 additional checks. Fresh Vulkan and OpenGL Debug/Release/Dist editor and test targets built; each test executable passed 4,212 checks and CTest passed 2/2. Logs are under the corresponding build tree's `Diagnostics/P04-preference-validation/` directory.
- `PulseForgeTests` now compiles `EditorLayout.cpp` and links the existing ImGui core so its preference save/reload path can run with fresh contexts. The round-trip and invalid-format recovery add 17 deterministic checks. Fresh Vulkan and OpenGL Debug/Release/Dist editor and test targets each pass 4,229 checks; CTest remains 2/2 in all six configurations. Full logs are under each build tree's `Diagnostics/P04-preference-validation/` directory.
- Host-close prompts now filter the host's member list through live session dirty state, so clean documents are not counted as unsaved. Two deterministic mixed-state/all-clean checks cover the filtering policy. Fresh Vulkan and OpenGL Debug/Release/Dist editor and test targets pass 4,231 checks; CTest passes 2/2 in all six configurations. Logs are under each build tree's `Diagnostics/DocumentTabTearOff_20261013/` directory. The actual multi-document prompt choices remain a manual acceptance item.
- The feature branch remains local, unpushed, and unmerged.
- The deleted Spotlight research document, modified validation scene/materials, and untracked `CMakeSettings.json` remain outside the commit. Root `AGENTS.md` remains absent.
