# Editor Documents and Native Windows

This guide describes the editor's document-hosting, native-window, persistence, and asset-workflow infrastructure. The Level Editor remains the main workspace. Full asset editors use a separate document system; utility panels remain part of their workspace.

## Current scope

The editor has a persistent top document strip, a main Level Editor host, native-window presentation support, per-project document-host records, transfer and close policies, and shared project-asset destination dialogs. The hosting layer is ready for full specialized editors, but no full asset editor is currently registered.

The existing material property form is **Material Details**, a utility panel docked beside Inspector. It edits saved-material drafts and exposes Save and Revert. Opening a material in the Content Browser selects and reveals this utility. It does not create a document session or native window, does not add a top-strip tab, and ignores the full-editor opening preference. The stable ImGui window/settings identity remains `Material Editor` for compatibility with existing layouts.

Material Details drafts are keyed by project and asset UUID and remain in memory for the editor session. Switching the selected material or hiding the panel preserves a draft. The selected Material Details asset and utility layout are persisted, but draft values are not restored after an application restart. Project/application close uses the existing Save, Discard, or Cancel flow for dirty scene and material edits.

## Documents, hosts, and utility panels

`EditorDocumentManager` stores document identities, host membership, ordering, active documents, remembered placements, bounds, and edit/saved revisions. A document identity is the tuple `(project UUID, asset UUID, editor kind)`, so equal filenames or asset UUIDs in different projects remain distinct. Opening an existing identity activates its existing session and updates its displayed asset location without changing the session revision. The Level Editor remains in the main host.

Asset-document eligibility is explicit in [`EditorDocumentHostPolicy.h`](../PulseForgeEditor/Source/Public/Editor/EditorDocumentHostPolicy.h). A file extension, asset UUID, or UI panel name does not make an interface a document editor. Register a kind only when it has a complete editor workspace and document lifecycle. `EditorDocumentManager` is shared infrastructure; it does not provide an asset editor's editable model, preview state, or undo history by itself.

The dedicated top strip is the only document-transfer target. Utility panels and workspace bodies do not accept document tabs. Transfers are queued until panel traversal finishes, then validate the destination before changing host membership. A failed preparation leaves the source session in place. A successful transfer moves the same identity, revision, and dirty state; it does not close and reopen the document. Empty native hosts are retired after successful transfer.

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
| OpenGL | GLFW secondary windows use shared OpenGL contexts and Dear ImGui's OpenGL3 renderer. The editor restores the main context after rendering secondary windows. | The editor shell is available, but the Scene Viewport does not render a scene. Image thumbnails use asset icons. |

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

`PulseForgeTests` covers document identity, duplicate activation, host placement, transfer preparation and rollback, dirty revisions, save/discard/cancel policy, command focus, workspace migration, native-bound recovery, asset moves, and project destination policies. A Debug-only `--document-host-test-fixture` option creates two synthetic unregistered document sessions for host and transfer testing; it requires `--project` and is rejected outside Debug. The fixture does not provide an asset editor or test real asset editing state. `--document-host-reset-layout-test`, `--document-host-test-tab-tear-off`, and `--document-host-test-presentation-failure` are opt-in Debug fixture modes.

At source revision `352fd26c43cc5951b7ade2969e81360ac6408baa` on 2026-10-13, the Vulkan/OpenGL Debug, Release, and Dist matrix passed 4,338 checks per configuration with CTest 2/2. The Debug host fixtures and bounded native-window runs validate the editor's hosting, transfer, and presentation-failure recovery paths, not a production asset editor.

Native-window startup, resize, minimize/restore, main-window minimization, workspace restart, and OpenGL shell rendering have bounded runtime coverage. The ImGui-input fixture feeds synthetic pointer events through the production main-strip-to-native tear-off path. It does not test native-to-main tab dragging or real pointer interaction. Native tabs also expose a context-menu command to move a document to the main strip; the transfer policy is covered by unit tests. Mouse-driven detach, reattachment and regrouping, dirty native-window dialogs, live preference-menu interaction, and physical mixed-DPI monitor movement still need manual acceptance. No production full asset editor exists to validate its editing, preview, and dirty-close behavior.
