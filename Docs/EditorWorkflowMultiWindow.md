# Editor Documents and Native Windows

This guide describes the editor's document-hosting, native-window, persistence, and asset-workflow infrastructure. The Level Editor remains the main workspace. Full asset editors use a separate document system; utility panels remain part of their workspace.

## Current scope

The editor has a persistent top document strip, a main Level Editor host, native-window presentation support, per-project document-host records, transfer and close policies, and shared project-asset destination dialogs. The hosting layer is ready for full specialized editors, but no full asset editor is currently registered.

The existing material property form is **Material Details**, a utility panel docked beside Inspector. It edits saved-material drafts and exposes Save and Revert. Opening a material in the Content Browser selects and reveals this utility. It does not create a document session or native window, does not add a top-strip tab, and ignores the full-editor opening preference. The stable ImGui window/settings identity remains `Material Editor` for compatibility with existing layouts.

Material Details drafts are keyed by project and asset UUID and remain in memory for the editor session. Switching the selected material or hiding the panel preserves a draft. The selected Material Details asset and utility layout are persisted, but draft values are not restored after an application restart. Project/application close uses the existing Save, Discard, or Cancel flow for dirty scene and material edits.

## Documents, hosts, and utility panels

`EditorDocumentManager` stores document identities, host membership, ordering, active documents, remembered placements, bounds, and edit/saved revisions. A document identity is the tuple `(project UUID, asset UUID, editor kind)`, so equal filenames or asset UUIDs in different projects remain distinct. Opening an existing identity activates its existing session and updates its displayed asset location without changing the session revision. The Level Editor remains in the main host.

Asset-document eligibility is explicit in [`EditorDocumentHostPolicy.h`](../PulseForgeEditor/Source/Public/Editor/EditorDocumentHostPolicy.h). A file extension, asset UUID, or UI panel name does not make an interface a document editor. Register a kind only when it has a complete editor workspace and document lifecycle. `EditorDocumentManager` is shared infrastructure; it does not provide an asset editor's editable model, preview state, or undo history by itself.

The dedicated top strip is the only document-transfer target. Utility panels and workspace bodies do not accept document tabs. The application captures a press on a document tab outside its close region and tracks registered strip hit regions across viewports. Releasing over a registered strip transfers the same session; releasing a main-host asset tab elsewhere creates a native host, while releasing a native-host tab elsewhere leaves it in its current host. Transfers are queued until panel traversal finishes, then validate the destination before changing host membership. A failed preparation leaves the source session in place. A successful transfer preserves identity, revision, and dirty state. Empty native hosts are retired after successful transfer.

Document opening preferences apply only to registered full editors:

| Preference | Placement |
|---|---|
| New Native Window | Opens each new document in a native host. This is the default. |
| Main Document Strip | Adds the document to the main host's top strip. |
| Remembered Placement | Reuses the remembered host for that editor kind when it is still valid; otherwise it creates a native host. |

Changing the preference affects later opens and does not move documents that are already open. Material Details always follows utility-panel docking, regardless of the preference.

The opening preference is a per-user editor setting. Remembered host membership and document order belong to the project workspace record, so a remembered placement is restored only when a compatible host exists in the active project.

Panels are described by `EditorPanelDescriptors` in [`EditorLayout.h`](../PulseForgeEditor/Source/Public/Editor/EditorLayout.h). The default workspace places Hierarchy on the left, Scene Viewport in the center, Inspector and Material Details in the right-side utility dock, and Content Browser, Console, and Scene utilities in the bottom dock. Material Details starts hidden. First opening docks it beside Inspector only if it has no saved location; a saved user location is respected. Recovery of a legacy Material document record moves the utility beside Inspector once. **Window > Reset Layout** restores the default utility layout without closing document sessions or clearing material drafts.

## Native presentation and frame scheduling

The editor uses one ImGui context. Dear ImGui's GLFW platform backend creates and updates native platform windows; PulseForge supplies renderer callbacks for their presentation. The Vulkan and OpenGL paths are implemented separately:

| Backend | Presentation path | Current limitation |
|---|---|---|
| Vulkan | The editor's custom ImGui renderer uses PulseForge's NVRHI device. Each secondary native window has its own Vulkan surface, swapchain, swapchain images, frame contexts, and acquire/present synchronization while sharing the renderer's Vulkan instance, device, and queues. | Full specialized asset editors are not registered yet. |
| OpenGL | GLFW secondary windows use shared OpenGL contexts and Dear ImGui's OpenGL3 renderer. The editor restores the main context after rendering secondary windows. | The editor shell and document-host windows render, but the Scene Viewport does not render a scene. Image thumbnails use asset icons. `PULSEFORGE_EDITOR_OPENGL_FRAME_CAPTURE` enables an in-process framebuffer capture for the main window and secondary viewports; Windows `PrintWindow` returned a white client area during OpenGL validation and is not reliable evidence for this backend. |

The application submits the main Vulkan frame before rendering platform windows. It then processes each secondary viewport in sequence: platform update, renderer target begin, UI draw submission, target end, and buffer swap. NVRHI permits only one immediate command list to be open at a time. The earlier schedule rendered platform windows before closing the main frame and could produce `Two or more immediate command lists cannot be open at the same time`, commands against an `INITIAL` list, and invalid present semaphores/layouts. Keeping main and secondary submissions sequential fixes that ordering failure. `Application::Run()` continues layer rendering while secondary presentation targets exist even if the main window cannot render.

The Vulkan renderer checks present-queue support, surface formats, FIFO presentation support, and nonzero framebuffer extents when creating a secondary target. Out-of-date or suboptimal swapchains are recreated. A zero-sized or minimized native viewport is skipped. Swapchain recreation waits for that host's frame contexts and idles the present queue before releasing images; native-host retirement uses the same presentation-completion rule. The queue idle is limited to resize and host retirement, where graphics completion alone does not prove presentation has finished. If a native presentation target cannot be created, the editor transfers all sessions from that host to the main document strip before retiring the empty host; a batch validation failure leaves the source host and its sessions intact.

### Resource ownership

| Resource | Owner and use | Retirement rule |
|---|---|---|
| Main GLFW window | `Application`/`WindowsWindow`; main ImGui platform viewport | Outlives editor layers and secondary platform windows. |
| Secondary GLFW windows | Dear ImGui GLFW platform backend; editor viewport callbacks | Close is treated as a document-host request. Renderer resources are retired before the platform window is destroyed. |
| Vulkan instance, device, queues | `VulkanRenderer`; NVRHI and Vulkan presentation | Shared across native windows. Queue calls follow the renderer's serialized submission path. |
| Secondary Vulkan surface and swapchain | `VulkanRenderer::CreatePlatformPresentationTarget` | Retire per host after frame contexts complete and the present queue is idle. |
| Swapchain image wrappers, depth targets, semaphores, frame contexts | Per-host Vulkan presentation record | Retained for that swapchain generation and released after graphics and presentation work completes. |
| ImGui context and UI texture bindings | `EditorLayer` and `EditorImGuiRenderer` | Remain alive while draw data or submitted frames can reference them; renderer teardown precedes device teardown. |
| OpenGL shared contexts | GLFW platform backend and current-context callbacks | Restore the main context after each secondary viewport; destroy viewports before the main context. |

The Vulkan implementation lives in [`VulkanRenderer.cpp`](../PulseForge/Source/Private/Renderer/Vulkan/VulkanRenderer.cpp), the OpenGL fallback in [`OpenGLRenderer.cpp`](../PulseForge/Source/Private/Renderer/OpenGLRenderer.cpp), and the editor callbacks and frame schedule in [`EditorApplication.cpp`](../PulseForgeEditor/Source/Private/EditorApplication.cpp). The engine-facing presentation API is in [`RendererBackend.h`](../PulseForge/Source/Public/Renderer/RendererBackend.h).

### Pinned dependencies

| Dependency | Repository pin | Use in the editor |
|---|---|---|
| Dear ImGui | `60d7fb207eeb46d6363dd4bde10b35991bae0ce7` (1.92.7 WIP, `IMGUI_VERSION_NUM 19265`) | Docking, platform viewports, GLFW integration, and OpenGL3 backend. |
| GLFW | 3.4.0 | Main and secondary native windows and OpenGL contexts. |
| NVRHI | `6b96fb03e07539f08327aea76c56d55f1de9d906` | Vulkan renderer device and command submission. |
| Vulkan-Headers | `v1.4.352` | Vulkan API declarations fetched by NVRHI. |
| DXC | 1.9.2602.24 | Build-time HLSL to SPIR-V compilation. |

These versions are pinned by the repository; the ImGui gitlink and GLFW headers are under `PulseForge/Vendor`, and the NVRHI, Vulkan-Headers, and DXC pins are declared in the root `CMakeLists.txt`.

## Focus, commands, and close behavior

Document commands resolve against the focused document identity and current project. With no focused document, commands retain the Level Editor fallback. A stale identity from another project is rejected. Current tests cover Level, Material-kind fixture, unsupported Texture-kind fixture, and stale-project routing; a production full asset editor is needed to verify commands against multiple real editors.

The editor shortcut policy suppresses scene/history commands while text input, active ImGui controls, menus, popups, or modals own interaction. The Scene Viewport remains attached to the Level Editor host; native asset windows do not take over its camera navigation or gizmos.

Document tab close, native-window close, project switch, and application shutdown use Save, Discard, or Cancel policy. `EditorDocumentSession` stores edit and saved revisions. Saving revision N marks the document clean only through N, so a later edit remains dirty. A failed save or cancelled close preserves the session. Host close filters its members to dirty documents; shutdown prevents secondary close callbacks from deleting saved host state before the final workspace snapshot.

The current Material Details utility uses the same application-level dirty decision for its in-memory draft. Revert reloads the serialized material and resets its edit history; migration of an old material document record selects the material for the utility without saving or discarding the draft.

## Workspace persistence and recovery

`EditorLayout` stores the user docking layout and panel state under `%LOCALAPPDATA%/PulseForge/Editor`. Set `PULSEFORGE_EDITOR_SETTINGS_DIR` to use an isolated settings directory for tests; isolated settings do not import the legacy working-directory `imgui.ini`.

Document state is stored separately in `documents-<project UUID>.json` beside the ImGui workspace settings. Schema version 2 records project identity, document hosts and order, active documents, remembered placement by editor kind, host bounds, and the selected Material Details asset. Schema version 1 is migrated. Raw window, GPU, and viewport pointers are not persisted. Malformed, duplicate, foreign-project, missing-asset, or unregistered-editor records are rejected or skipped during restore. A saved Material-kind record is migrated to the Material Details utility; obsolete empty hosts are removed while the utility layout and unrelated valid state remain available.

Saved native bounds include monitor name, position, size, DPI scale, and maximized state. Invalid geometry is clamped to a usable work area; valid negative desktop coordinates are accepted. When the saved monitor is unavailable, restoration falls back to an available monitor. These recovery rules have deterministic tests and a single-monitor missing-monitor runtime check. Physical monitor removal and mixed-DPI movement have not been verified.

Reset Layout rebuilds the utility dock without clearing document membership or edit state. Saved layouts are not replaced just because the source default-layout version changes. A missing or incomplete ImGui dockspace falls back to the default layout while retaining independently parsed panel state where valid.

## Asset moves and destination dialogs

Content Browser asset and folder moves use separate drag payloads and the asset-operation service. `AssetOperations::MoveFolder` moves the directory on the same volume, carries `.meta` sidecars, preserves UUIDs, and rebuilds the registry. It rejects traversal, reparse points, collisions, and moves into the source folder or its descendants. If registry rebuilding fails, it attempts to roll back the directory and reports when recovery is still required. Open document labels and paths are refreshed by UUID without changing document identity or dirty revision.

The shared project-folder/name dialog is used for asset Move/Rename, Duplicate, New Scene, Save Scene As, and Create Prefab. It provides project-folder search, breadcrumbs, filename and extension validation, sidecar collision feedback, Enter to accept, and Escape to cancel. External image import and project-directory selection continue to use native file dialogs. The asset registry refresh is explicit; PulseForge does not continuously watch asset files for external changes.

## Adding a full specialized editor

Future Material, Blueprint, and other specialized editors must register a document kind explicitly in `RegisteredAssetDocumentEditors`. An editor integration should:

1. Add a stable `EditorDocumentKind` and update workspace schema validation or migration if persisted enum values change.
2. Define a session owner keyed by project UUID, asset UUID, and editor kind. Keep editable data, independent undo/redo, selection, preview state, and asynchronous operations with that session rather than in transient panel locals.
3. Open and activate the identity through `EditorDocumentManager`; repeated opens should focus the existing session.
4. Render the full editor workspace inside its document host. Keep utility panels in the workspace's utility docking system and out of the top-strip transfer target.
5. Supply save, discard, dirty-revision, close, and focused-command behavior to the shared lifecycle policies.
6. Register stable utility and workspace layout identities. Transfer the same document session when it detaches, reattaches, or moves to another host.
7. Give every preview target and UI texture a generation and retirement policy that keeps resources alive through every consuming host's in-flight draw work.
8. Add deterministic registration, persistence, close, transfer, and failure tests, then validate the real editor in the supported Vulkan and OpenGL presentation paths.

Do not register Material Details as a document editor. A future full Material Editor is a separate workspace and opening operation.

## Validation and known limitations

On 2026-10-13, Vulkan and OpenGL Debug, Release, and Dist builds each passed 4,359 `PulseForgeTests` checks and CTest 2/2. The tests cover document identity and transfer policy, dirty revisions and close decisions, focused commands, layout migration and bounds recovery, asset operations, destination-dialog policy, and command-line validation for renderer failure fixtures. Build and test logs are under `Build/CMake/VS2022-x64/Diagnostics/DocumentHostFailureInjection_20261013/ValidationAfterFix/`. The Debug-only `--document-host-test-fixture` creates two synthetic, unregistered sessions; it tests the host shell and does not provide editable asset content.

The Debug tab fixture feeds synthetic ImGui pointer events through production tab-strip code. Vulkan and OpenGL runs rejected drops over Hierarchy, Inspector, Scene Viewport, and Content Browser, then reattached the same fixture session to the main strip and retired its empty native host. The Vulkan log and screenshot are under `Build/CMake/VS2022-x64/Diagnostics/DocumentStripTargetPolicy_20261013/`; the OpenGL log is under the matching `Build/CMake/VS2022-x64-OpenGL/Diagnostics/` directory. These runs validate the application input path, not delivery of real OS mouse input.

The Debug-only `--document-host-test-acquire-out-of-date` and `--document-host-test-image-allocation-failure` switches exercise Vulkan recovery paths through the same native-host callbacks used at runtime. The first skips acquisition with a synthetic out-of-date result and checks that a later frame is presented. The second injects an exception after the first swapchain image is wrapped and its depth target is created, then checks session recovery. Neither switch is active during normal startup.

Bounded runtime checks also cover Vulkan native-window startup and resize, minimize/restore, main-window minimization, workspace restoration, presentation-creation failure, acquire recovery, partial swapchain-allocation rollback, and OpenGL host rendering. Vulkan runs report both Khronos and NVRHI validation enabled and no VUID or validation errors. OpenGL keeps the editor shell and document-host windows; its Scene Viewport displays the fallback message and its Content Browser uses asset icons instead of Vulkan previews. The OpenGL framebuffer capture hook reads the main and secondary back buffers because Windows `PrintWindow` returned a white client area for this backend.

Material Details is an Inspector-side utility. Document hosts are reserved for full specialized editors, and no production full asset editor is currently registered. The matrix below distinguishes policy tests, synthetic application-path tests, runtime presentation checks, and unverified pointer or hardware interaction.

### Documents and docking

| ID | Status | Evidence and limits |
|---|---|---|
| D01 | Unverified (full editor deferred; utility partly verified) | The preference matrix stored only Level Editor in the document host and placed Material Details in Inspector's dock node for all three preferences. The Preference 0 screenshot shows the adjacent tabs and central workspace; the other two screenshots contain black client pixels, so their visual placement is unverified. No full Material Editor is registered. |
| D02 | Unverified (deferred) | `TestEditorDocumentManager` distinguishes asset identities, but no two production asset editors exist to test independent editing histories. |
| D03 | Passed (policy) | `TestEditorDocumentManager` verifies that opening an existing identity activates its existing session. A production editor UI is not registered. |
| D04 | Passed (synthetic application path) | Debug tear-off/reattachment fixtures transfer the same synthetic session between main and native hosts; this is not a material-editor test. |
| D05 | Passed (synthetic application path) | `DocumentStripTargetPolicy_20261013` rejects releases over Hierarchy, Inspector, Scene Viewport, and Content Browser without changing host membership. Real pointer delivery remains unverified. |
| D06 | Unverified (edge interaction) | The fixture tests panel centers, not every workspace edge or physical docking indicator. Document hit regions are registered only for top strips; real edge drops need manual testing. |
| D07 | Unverified (deferred) | Transfer-policy tests preserve dirty revisions. There is no full asset editor with dirty edits, undo history, selection, or preview state to detach. |
| D08 | Unverified (repeated UI interaction) | Manager tests cover detach/reattach cycles and the Debug fixture completes one native-to-main return. Repeated real drags with editor state are not tested. |
| D09 | Unverified (UI reorder) | `TestEditorDocumentManager` and `TestEditorDocumentWorkspaceCodec` cover ordering and persistence data; real tab reordering has not been manually verified. |
| D10 | Unverified (full-editor switching) | Material Details preference captures show the Level Editor workspace remains available. Switching among multiple production asset editors is deferred. |
| D11 | Unverified (grouped UI transfer) | Manager tests exercise host membership and transfer policy; grouped native tabs have not been dragged and regrouped through real input. |
| D12 | Unverified (utility drag) | Default placement, visibility, persistence, and Reset Layout have policy/runtime coverage. Pointer-driven utility-panel rearrangement remains a manual check. |
| D13 | Passed (policy) | `TestEditorDocumentManager` uses same-project, same-basename identities from different folders and verifies distinct sessions. |
| D14 | Unverified (integrated UI) | `TestAssetOperations` covers moves/renames and `TestEditorDocumentManager` preserves a moved session's identity and dirty revision; an open production asset editor has not been moved in the UI. |

### Close and focus

| ID | Status | Evidence and limits |
|---|---|---|
| C01 | Unverified (UI) | `TestEditorDocumentManager` verifies Cancel preserves a dirty session. No production asset document exists for a real tab-close prompt. |
| C02 | Unverified (dirty native close) | Native-window lifecycle and close-policy tests cover host shutdown separately; dirty Alt-F4 cancellation has not been exercised in a real editor window. |
| C03 | Passed (policy) | `TestEditorDocumentManager` verifies a failed save leaves the document open and dirty. |
| C04 | Unverified (batch UI) | Dirty-batch selection is tested by `TestEditorDocumentManager`; a host close dialog with multiple editable documents is not available. |
| C05 | Unverified (dirty application shutdown) | Shutdown preservation has runtime coverage for host/layout state, but not for multiple dirty production asset documents. |
| C06 | Unverified (deferred) | No full Material Editor is registered to test focused Save or Undo against a floating material document. |
| C07 | Passed (policy) | `TestEditorInteractionPolicy` and `ShortcutIsolationMatrix_20261013` cover command suppression while text input or modal UI owns focus; real typing workflows still need manual confirmation. |
| C08 | Passed (policy) | Viewport capture and focus-loss policies are covered by deterministic tests. Physical focus changes across native windows remain unverified. |
| C09 | Passed (policy) | `TestEditorDocumentManager` verifies saving an earlier revision does not clear a later edit. |
| C10 | Passed (synthetic application path) | `ResetLayoutDirtyDocuments_20261013` resets the layout while retaining dirty fixture sessions and their host membership. |

### Renderer and platform

| ID | Status | Evidence and limits |
|---|---|---|
| R01 | Passed (runtime) | `NativeWindowResize_20261013_Bounded` repeatedly resizes a secondary host under Vulkan validation. |
| R02 | Passed (runtime) | `NativeWindowLifecycle_20261013` records successful minimize/restore and secondary-window recreation. |
| R03 | Passed (bounded synthetic-host runtime) | `MainMinimizedSyntheticHost_20261013` minimizes the main window, confirms a secondary GLFW30 host remains visible, and captures its fixture UI with GDI+ `PrintWindow` flag 2. The fixture contains no production editor; see `Build/CMake/VS2022-x64/Diagnostics/MainMinimizedSyntheticHost_20261013/`. |
| R04 | Unverified (hardware) | DPI-scale and window-bound policies are tested, but movement between physical mixed-DPI monitors has not been exercised. |
| R05 | Unverified (hardware) | Bounds policy accepts negative coordinates; placement and pointer geometry on a physical negative-coordinate monitor have not been tested. |
| R06 | Passed (bounded runtime) | Native-window lifecycle/transfer runs exercise presentation retirement with submitted frames; Khronos and NVRHI logs contain no VUID or validation errors. |
| R07 | Unverified (deferred) | The Scene Viewport and Content Browser can render together, but two specialized asset-document previews do not exist. |
| R08 | Passed (bounded Vulkan Debug) | Resize, presentation-failure, out-of-date acquire, partial swapchain-allocation, and document-strip runs confirm both validation layers were enabled and contain no VUID or validation errors. |
| R09 | Passed (bounded OpenGL runtime) | OpenGL host fixtures render secondary windows and the PBR browser uses type icons. OpenGL does not provide Vulkan thumbnails or a rendered Scene Viewport. |
| R10 | Passed (synthetic failure injection) | `PresentationFailureRecovery_20261013` injects native presentation creation failure and verifies dirty fixture sessions remain usable in the main host. |
| R11 | Passed (Debug failure injection) | `Build/CMake/VS2022-x64/Diagnostics/AcquireOutOfDateRecovery_20261013/VerifiedRerun2/` injects `VK_ERROR_OUT_OF_DATE_KHR` before image acquisition or command-list opening, then verifies swapchain recovery, a later successful acquire, and a presented frame. Khronos and NVRHI validation were enabled; no VUID or validation errors were logged, and the editor exited with code 0. This is a deterministic renderer result, not an operating-system-generated resize failure. |
| R12 | Passed (Debug allocation-failure injection) | `Build/CMake/VS2022-x64/Diagnostics/SwapchainImageAllocationFailure_20261013/VerifiedRun2/` injects failure after wrapping swapchain image 0 and creating its depth target. Construction rollback returns control to the editor, and the dirty synthetic document session moves to the main strip while Level Editor remains available. Khronos and NVRHI validation were enabled; no VUID or validation errors were logged, and the editor exited with code 0. This verifies the rollback path and clean shutdown, not allocator-level memory reclamation counters. |

### Persistence and assets

| ID | Status | Evidence and limits |
|---|---|---|
| P01 | Passed (persistence policy) | `TestEditorDocumentWorkspaceCodec` round-trips synthetic host membership and ordering and rejects invalid project/layout data. This does not establish application restart restoration. |
| P02 | Unverified (hardware) | Missing-monitor bounds recovery is tested against a simulated monitor list; physical monitor removal and restart have not been performed. |
| P03 | Passed (policy/runtime) | `WorkspaceRecoveryMatrix_20261013` and `CorruptWorkspaceDockRecovery_20261013` cover obsolete/corrupt settings recovery without discarding valid document state. |
| P04 | Passed (runtime) | `WorkspaceMissingAssetRestore_20261013` verifies that missing asset records are skipped while valid workspace state remains available. |
| P05 | Passed (runtime) | `DocumentOpeningPreferenceRuntime_20261013` checks new-document placement for the main-strip preference; Material Details remains a utility under all preferences. |
| P06 | Passed (policy/runtime) | `TestEditorDocumentManager` and the opening-preference runtime matrix cover remembered host reuse and deterministic fallback. |
| P07 | Unverified (integrated UI) | Asset-operation tests preserve UUIDs and document-manager tests refresh paths without changing dirty revision; moving a referenced asset with a production editor open has not been tested. |
| P08 | Passed (policy) | `TestAssetOperations` covers collisions, invalid paths, rollback, and registry-refresh failure handling. |
| P09 | Passed (policy) | `TestAssetOperations` rejects moving a folder into itself or a descendant before mutation. |
| P10 | Unverified (deferred) | Shared project-asset dialogs run in the main workspace. No floating full Material Editor exists to verify host-modal ownership or result routing. |

### Manual checks still required

Use a Debug build and an isolated `PULSEFORGE_EDITOR_SETTINGS_DIR`. The host fixture creates synthetic tabs and is not a substitute for a production asset editor.

- Use real mouse input to tear a fixture document off the main strip and reattach it. Confirm drops over utility panels and workspace edges do not change host membership; the synthetic fixture covers four panel centers.
- Reorder and group native document tabs, close/reopen the host, and confirm order and active-tab restoration.
- Use each opening preference and confirm Material Details opens beside Inspector while Level Editor remains active and the Scene Viewport stays available.
- Exercise dirty-close Cancel/Save/Discard with a real editable document when a full editor is registered.
- Move hosts between mixed-DPI monitors and a monitor with negative desktop coordinates, then remove a monitor and restart.
- Confirm shared asset dialogs behave correctly from a floating full editor when one is available.
