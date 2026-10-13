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
| OpenGL | GLFW secondary windows use shared OpenGL contexts and Dear ImGui's OpenGL3 renderer. The editor restores the main context after rendering secondary windows. | The editor shell and document-host windows render, but the Scene Viewport does not render a scene. Image thumbnails use asset icons. |

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

The document model and policy tests cover stable identities, duplicate opens, host membership, transfer preparation, close decisions, save revisions, focused commands, layout migration and recovery, placement preferences, path bounds, asset operations, destination dialogs, and command-line parsing.

On 2026-10-13, at source revision `61dcc905`, Debug, Release, and Dist passed 4,376 `PulseForgeTests` checks each, with CTest passing 2/2 in each configuration. The same configurations were built for the Vulkan and OpenGL editor selections. Bounded Debug Vulkan runs used Khronos and NVRHI validation and exercised native-host presentation, resize, minimize/restore, main-window minimization, workspace restoration, transfer fixtures, and injected presentation failures. No Vulkan validation errors were reported in those runs. Bounded OpenGL runs exercised secondary document-host rendering and the fallback editor shell.

The Debug-only `--document-host-test-fixture` and related `--document-host-test-*` switches create two synthetic, unregistered document sessions or inject bounded renderer failures. They run through production host and tab-strip code but display placeholders instead of editable asset content. These checks do not establish real mouse-driven tab transfers or native pointer delivery.

Material Details is an Inspector-side utility. No production full asset editor is registered, so independent asset-editor workspaces, per-document editing histories, and dirty-document prompts in a real asset editor remain unverified. Manual testing is also needed for pointer-driven detach, reattach, reorder, and utility docking; mixed-DPI and negative-coordinate monitor placement; and recovery after physically removing a monitor. OpenGL does not render scene snapshots or GPU asset previews. Asset changes made outside the editor are detected by an explicit Refresh, not a file watcher.

### Manual document-host interaction

Run the following from the repository root to add synthetic document tabs for manual host testing. The Debug-only fixture does not open or modify project assets:

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

The fixture creates two synthetic, unregistered document sessions with no editable asset content. Use a tab's context menu to move one to the main document strip, then drag that top-strip tab outside the editor window and release it. The same session should appear in a native window. Repeat the transfer in both directions and confirm that Level Editor stays in the main host. The fixture does not synthesize OS pointer input, validate editing or saving, or exercise dirty-document prompts for a production asset editor.

- Use real mouse input to detach, reattach, reorder, and group document tabs. Confirm utility panels and workspace edges reject drops.
- Use each opening preference and confirm Material Details opens beside Inspector while Level Editor remains active and the Scene Viewport stays available.
- Exercise dirty-close Cancel/Save/Discard with a real editable document when a full editor is registered.
- Move hosts between mixed-DPI monitors and a monitor with negative desktop coordinates, then remove a monitor and restart.
- Confirm shared asset dialogs behave correctly from a floating full editor when one is available.
