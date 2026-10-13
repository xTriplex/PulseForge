# Editor Documents and Native Windows

This guide describes the editor's document-hosting, native-window, persistence, and asset-workflow infrastructure. The Level Editor remains the main workspace. Full asset editors use a separate document system; utility panels remain part of their workspace.

## Current scope

The editor has a persistent top document strip, a main Level Editor host, native-window presentation support, per-project document-host records, transfer and close policies, and shared project-asset destination dialogs. The hosting layer is ready for full specialized editors, but no full asset editor is currently registered.

The existing material property form is **Material Details**, a utility panel docked beside Inspector. It edits saved-material drafts and exposes Save and Revert. Opening a material in the Content Browser selects and reveals this utility. It does not create a document session or native window, does not add a top-strip tab, and ignores the full-editor opening preference. The stable ImGui window/settings identity remains `Material Editor` for compatibility with existing layouts.

Material Details drafts are keyed by project and asset UUID and remain in memory for the editor session. Switching the selected material or hiding the panel preserves a draft. The selected Material Details asset and utility layout are persisted, but draft values are not restored after an application restart. Project/application close uses the existing Save, Discard, or Cancel flow for dirty scene and material edits.

## Documents, hosts, and utility panels

`EditorDocumentManager` stores document identities, host membership, ordering, active documents, remembered placements, bounds, and edit/saved revisions. A document identity is the tuple `(project UUID, asset UUID, editor kind)`, so equal filenames or asset UUIDs in different projects remain distinct. Opening an existing identity activates its existing session and updates its displayed asset location without changing the session revision. The Level Editor remains in the main host.

Asset-document eligibility is explicit in [`EditorDocumentHostPolicy.h`](../PulseForgeEditor/Source/Public/Editor/EditorDocumentHostPolicy.h). A file extension, asset UUID, or UI panel name does not make an interface a document editor. Register a kind only when it has a complete editor workspace and document lifecycle. `EditorDocumentManager` is shared infrastructure; it does not provide an asset editor's editable model, preview state, or undo history by itself.

The dedicated top strip is the only document-transfer target. Utility panels and workspace bodies do not accept document tabs. The application captures a press on a document tab outside its close region and tracks registered strip hit regions across viewports. Releasing over a registered strip transfers the same session. Dragging a main-host asset tab outside the registered strips creates a native host; if the pointer leaves all ImGui viewports, the editor uses its last valid drag position. Dragging a native-host tab outside the registered strips leaves it in its current host. Transfers are queued until panel traversal finishes, then validate the destination before changing host membership. A failed preparation leaves the source session in place. A successful transfer preserves identity, revision, and dirty state. Empty native hosts are retired after successful transfer.

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

`PulseForgeTests` covers document identity, transfer and close policy, edit revisions, command focus, layout migration, bounds recovery, asset operations, destination dialogs, and command-line parsing. The Debug-only `--document-host-test-fixture` and related `--document-host-test-*` switches create synthetic, unregistered documents or inject bounded renderer failures. They exercise production host and tab-strip code without providing editable asset content.

Bounded Vulkan runtime checks cover native-window startup, resize, minimize/restore, main-window minimization, workspace restoration, presentation-creation failure, out-of-date acquisition, and partial swapchain-allocation rollback. Khronos and NVRHI validation were enabled for these runs; no Vulkan validation errors were reported. OpenGL runtime checks cover secondary host rendering. The OpenGL editor shell and document hosts render, while the Scene Viewport shows its fallback message and the Content Browser uses asset icons. `PULSEFORGE_EDITOR_OPENGL_FRAME_CAPTURE` captures the main and secondary framebuffers because Windows `PrintWindow` returned a white client area for this backend.

Material Details is an Inspector-side utility. Document hosts are reserved for full specialized editors, and no production full asset editor is registered. The status below separates policy tests, synthetic application-path checks, runtime presentation, and manual or hardware checks.

### Documents and docking (D01-D14)

| ID | Status | Evidence and limits |
|---|---|---|
| D01 | Partial | Material Details restores beside Inspector while Level Editor remains active. Full-editor placement is deferred because no production asset editor is registered. |
| D02 | Unverified (deferred) | Identity tests distinguish assets, but there are no production asset editors with independent editing histories. |
| D03 | Passed (policy) | Opening an existing identity activates its existing session. |
| D04 | Passed (synthetic application path) | A synthetic session transfers between a native host and the main strip. Real mouse-driven transfers remain unverified. |
| D05 | Passed (synthetic application path) | Drops over Hierarchy, Inspector, Scene Viewport, and Content Browser do not change host membership. |
| D06 | Passed (synthetic application path) | Drops at panel centers and content edges are rejected. ImGui indicators and real pointer delivery need manual testing. |
| D07 | Unverified (deferred) | Policy tests preserve dirty revisions; no full editor exists to verify dirty state, undo, selection, or preview state through a transfer. |
| D08 | Passed (synthetic application path) | Repeated native/main transfers and grouping preserve the synthetic session and retire empty hosts. Physical interaction remains unverified. |
| D09 | Passed (synthetic application path) | Synthetic tab-strip input reorders sessions while preserving the active session and Level Editor tab. Real mouse input remains unverified. |
| D10 | Unverified (deferred) | Material Details leaves the Level Editor workspace available. Switching among production asset editors is deferred. |
| D11 | Passed (synthetic application path) | Grouping places both sessions in the destination host and retires the empty source host. Physical interaction remains unverified. |
| D12 | Unverified (manual interaction) | Default placement, visibility, persistence, and Reset Layout have policy/runtime coverage. Pointer-driven utility rearrangement remains unverified. |
| D13 | Passed (policy) | Same-basename assets in different folders produce distinct document identities. |
| D14 | Unverified (integrated UI) | Asset operations preserve IDs and the document manager refreshes paths, but an open production asset editor has not been moved in the UI. |

### Close and focus (C01-C10)

| ID | Status | Evidence and limits |
|---|---|---|
| C01 | Unverified (UI) | Cancel behavior is tested in policy code; no production asset document exercises a real tab-close prompt. |
| C02 | Unverified | Dirty Alt-F4 cancellation has not been tested in a real editor window. |
| C03 | Passed (policy) | A failed save leaves the document open and dirty. |
| C04 | Unverified | Batch dirty-document selection is tested; a multi-document host-close dialog is not available without production asset editors. |
| C05 | Unverified | Shutdown tests preserve host/layout state, not multiple dirty production asset documents. |
| C06 | Unverified (deferred) | No full asset editor exists to test focused Save or Undo in a floating document. |
| C07 | Passed (policy) | Text-input and modal focus suppress unrelated commands. Real typing workflows still need manual testing. |
| C08 | Passed (policy) | Viewport capture and focus-loss policies are tested; physical focus changes across native windows remain unverified. |
| C09 | Passed (policy) | Saving an earlier revision does not clear a later edit. |
| C10 | Passed (synthetic application path) | Reset Layout preserves dirty fixture sessions and their host membership. |

### Renderer and platform (R01-R12)

| ID | Status | Evidence and limits |
|---|---|---|
| R01 | Passed (runtime) | Secondary native hosts resize under Vulkan validation. |
| R02 | Passed (runtime) | Native host minimize/restore and secondary-window recreation were exercised. |
| R03 | Passed (synthetic-host runtime) | A secondary fixture host remained visible and rendered while the main window was minimized. |
| R04 | Unverified (hardware) | Policy tests cover DPI and bounds; physical mixed-DPI monitor movement has not been exercised. |
| R05 | Unverified (hardware) | Negative desktop coordinates are accepted by policy; physical placement on a negative-coordinate monitor is untested. |
| R06 | Passed (bounded runtime) | Native-window lifecycle and transfer runs exercised presentation retirement with submitted frames; validation reported no errors. |
| R07 | Unverified (deferred) | Multiple specialized asset-document previews do not exist. |
| R08 | Passed (bounded Vulkan Debug) | Resize, presentation failure, out-of-date acquire, partial swapchain allocation, and document-strip runs used Khronos and NVRHI validation without reported errors. |
| R09 | Passed (bounded OpenGL runtime) | OpenGL host fixtures render and the Content Browser uses type icons. OpenGL has no Vulkan thumbnails or rendered Scene Viewport. |
| R10 | Passed (synthetic failure injection) | Presentation creation failure leaves dirty fixture sessions usable in the main host. |
| R11 | Passed (Debug failure injection) | Injected `VK_ERROR_OUT_OF_DATE_KHR` before acquisition; the renderer recovered and presented a later frame. This does not replace an OS-generated resize test. |
| R12 | Passed (Debug allocation-failure injection) | Injected swapchain-image setup failure rolled back and returned the dirty synthetic session to the main strip. Allocator-level reclamation counters were not measured. |

### Persistence and assets (P01-P10)

| ID | Status | Evidence and limits |
|---|---|---|
| P01 | Passed (bounded synthetic-host restart) | Two synthetic document hosts restore across an editor restart. Production asset-editor restoration and unsaved crash recovery are not covered. |
| P02 | Unverified (hardware) | Missing-monitor recovery is tested against a simulated monitor list; physical monitor removal and restart are untested. |
| P03 | Passed (policy/runtime) | Obsolete or corrupt workspace settings recover while valid document state is retained. |
| P04 | Passed (runtime) | Missing asset records are skipped while valid workspace state remains available. |
| P05 | Passed (policy/runtime) | Opening preferences affect document placement; Material Details remains a utility under each preference. |
| P06 | Passed (policy/runtime) | Remembered-host reuse and deterministic fallback are covered by manager and preference tests. |
| P07 | Unverified (integrated UI) | Asset tests preserve UUIDs and dirty revisions, but moving a referenced asset with a production editor open is untested. |
| P08 | Passed (policy/runtime) | Asset-operation tests cover collisions, invalid paths, rollback, and registry-refresh failure. |
| P09 | Passed (policy) | Moving a folder into itself or a descendant is rejected before mutation. |
| P10 | Unverified (deferred) | Shared asset dialogs work in the main workspace; there is no floating full editor to verify dialog ownership or result routing. |

### Manual checks still required

Run the following from the repository root. The Debug-only synthetic host fixture checks pointer-driven tab movement without opening or modifying project assets:

```powershell
cmake --build Build/CMake/VS2022-x64 --config Debug --target PulseForgeEditor --parallel 1

$repo = (Get-Location).Path
$env:PULSEFORGE_EDITOR_SETTINGS_DIR = Join-Path $env:TEMP ("PulseForge-DocumentHost-Manual-" + [guid]::NewGuid())
Push-Location (Join-Path $repo 'Build/CMake/VS2022-x64/Debug')
try {
	$project = Join-Path $repo 'Samples/VisualValidation/EditorWorkflow/EditorWorkflow.pfproj'
	.\PulseForgeEditor.exe --project $project --document-host-test-fixture
}
finally {
	Pop-Location
	Remove-Item Env:\PULSEFORGE_EDITOR_SETTINGS_DIR -ErrorAction SilentlyContinue
}
```

The fixture creates three synthetic, unregistered document sessions with no editable asset content. Use a tab's context menu to move it to the main document strip, then drag that top-strip tab outside the editor window and release it. The same session should appear in a native window. Repeat the transfer in both directions and confirm that Level Editor stays in the main host. The fixture does not validate editing, saving, or dirty-document prompts for a production asset editor.

- Use real mouse input to detach, reattach, reorder, and group document tabs. Confirm utility panels and workspace edges reject drops.
- Use each opening preference and confirm Material Details opens beside Inspector while Level Editor remains active and the Scene Viewport stays available.
- Exercise dirty-close Cancel/Save/Discard with a real editable document when a full editor is registered.
- Move hosts between mixed-DPI monitors and a monitor with negative desktop coordinates, then remove a monitor and restart.
- Confirm shared asset dialogs behave correctly from a floating full editor when one is available.
