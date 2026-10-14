# Editor Documents and Native Windows

The Level Editor is the primary workspace. Its document strip hosts the Level Editor and future full asset editors; utility panels remain in workspace docks. Native document hosts use the same editor process, project, and ImGui context.

## Current scope

The editor provides a top document strip, native-window presentation, per-project document-host persistence, document transfer and close policies, and shared project-asset destination dialogs. No full asset editor is currently registered.

The existing material property form is **Material Details**, a utility panel docked beside Inspector. Opening a material in the Content Browser selects it and reveals Material Details. It does not create a document session or native window, add a top-strip tab, or follow the full-editor opening preference. The stable ImGui identity remains `Material Editor` for compatibility with saved layouts.

Material Details keeps drafts by project UUID and asset UUID. Selecting another material or hiding the panel preserves the draft for the current editor session. The selected material and utility layout persist across application restarts; draft values do not. Closing the project or application uses the existing Save, Discard, or Cancel flow for dirty scene and material edits.

## Documents, hosts, and utility panels

`EditorDocumentManager` tracks document identities, host membership, order, active documents, remembered placement, bounds, and edit/saved revisions. A document identity is `(project UUID, asset UUID, editor kind)`, so equal filenames and asset UUIDs in different projects remain distinct. Opening an existing identity activates its session and refreshes its displayed asset location without changing its revision. The Level Editor remains in the main host.

Document eligibility is explicit in [`EditorDocumentHostPolicy.h`](../PulseForgeEditor/Source/Public/Editor/EditorDocumentHostPolicy.h). A file extension, asset UUID, or panel name does not make an interface a document editor. Register an editor kind only when it provides a full workspace and document lifecycle. The shared document manager does not own an editor's model, preview state, or undo history.

The top document strip is the only document-transfer target inside editor windows. Utility panels and workspace bodies reject document tabs. Pressing a tab outside its close control starts a drag; releasing over a registered document strip transfers the same session. Releasing a tab outside all editor windows creates a native host, using the last valid drag position if the pointer leaves ImGui viewports. This applies to tabs from the main host and native hosts. Transfers are queued until panel traversal finishes and the destination is validated. Failed preparation leaves the source unchanged; successful transfers preserve document identity, revision, and dirty state. Empty native hosts are retired after successful transfers.

Opening preferences apply only to registered full editors:

| Preference | Behavior |
|---|---|
| New Native Window | Opens each new document in a native host. This is the default. |
| Main Document Strip | Opens each new document in the main host's top strip. |
| Remembered Placement | Reuses a valid remembered host for that editor kind; otherwise opens in a native host. |

Changing the preference affects documents opened later; it does not move existing documents. The preference is stored per user. Project workspace state stores host membership and document order, so remembered placement is restored only with a compatible host in that project. Material Details always follows utility-panel docking.

Panel descriptors in [`EditorLayout.h`](../PulseForgeEditor/Source/Public/Editor/EditorLayout.h) define the default workspace: Hierarchy on the left, Scene Viewport in the center, Inspector and Material Details in the right utility dock, and Content Browser, Console, and Scene in the bottom dock. Material Details starts hidden. First opening places it beside Inspector if no saved location exists; a valid user placement is retained. Recovery from a legacy material-document record places it beside Inspector once. **Window > Reset Layout** restores the default utility layout without closing documents or clearing material drafts.

## Native presentation and frame scheduling

The editor uses one ImGui context. Dear ImGui's GLFW platform backend owns secondary native windows; PulseForge supplies their renderer callbacks.

| Backend | Presentation path | Limitation |
|---|---|---|
| Vulkan | The custom ImGui renderer uses the PulseForge NVRHI device. Each secondary window has a Vulkan surface, swapchain, swapchain images, frame contexts, and acquire/present synchronization. Windows share the renderer's Vulkan instance, device, and queues. | No production full asset editor is registered. |
| OpenGL | GLFW secondary windows use shared OpenGL contexts and Dear ImGui's OpenGL3 renderer. The main context is restored after secondary rendering. | The editor shell and document-host windows render, but the Scene Viewport does not render a scene. Asset previews use type icons. |

The application submits the main Vulkan frame before rendering platform windows. It processes each secondary viewport in sequence: update the platform window, begin its renderer target, submit UI draw data, end the target, and swap buffers. NVRHI permits one immediate command list at a time. Rendering secondary windows before closing the main frame caused `Two or more immediate command lists cannot be open at the same time`, commands against an `INITIAL` list, and invalid present semaphores or image layouts. Sequential main and secondary submissions fix that ordering failure. `Application::Run()` continues rendering while secondary targets exist even when the main window cannot render.

The Vulkan renderer checks present-queue support, surface formats, FIFO presentation support, and nonzero framebuffer extent when creating a secondary target. It recreates out-of-date or suboptimal swapchains and skips minimized or zero-sized viewports. Resize and host retirement wait for that host's frame contexts and idle the present queue before releasing swapchain resources; graphics completion alone does not prove presentation has finished. These waits are limited to resize and retirement. If a native target cannot be created, the editor transfers its sessions to the main strip before retiring the host. A failed batch validation leaves the source host intact.

### Resource ownership

| Resource | Owner | Retirement condition |
|---|---|---|
| Main GLFW window | `Application` / `WindowsWindow` | Outlives editor layers and secondary platform windows. |
| Secondary GLFW windows | Dear ImGui GLFW platform backend | Renderer resources retire before the platform window is destroyed. |
| Vulkan instance, device, and queues | `VulkanRenderer` | Shared by native windows; queue submissions use the renderer's serialized path. |
| Secondary Vulkan surface and swapchain | `VulkanRenderer::CreatePlatformPresentationTarget` | Host frame work completes and the present queue is idle. |
| Swapchain image wrappers, depth targets, semaphores, frame contexts | Per-host Vulkan presentation record | Retained for a swapchain generation until graphics and presentation work completes. |
| ImGui context and UI texture bindings | `EditorLayer` / `EditorImGuiRenderer` | Remain valid while draw data or submitted frames can reference them; released before the device. |
| OpenGL shared contexts | GLFW platform backend and context callbacks | Restore the main context after each secondary viewport; destroy secondary viewports before the main context. |

Implementation references: [`VulkanRenderer.cpp`](../PulseForge/Source/Private/Renderer/Vulkan/VulkanRenderer.cpp), [`OpenGLRenderer.cpp`](../PulseForge/Source/Private/Renderer/OpenGLRenderer.cpp), [`EditorApplication.cpp`](../PulseForgeEditor/Source/Private/EditorApplication.cpp), and [`RendererBackend.h`](../PulseForge/Source/Public/Renderer/RendererBackend.h).

### Pinned dependencies

| Dependency | Repository pin | Editor use |
|---|---|---|
| Dear ImGui | `60d7fb207eeb46d6363dd4bde10b35991bae0ce7` (1.92.7 WIP, `IMGUI_VERSION_NUM 19265`) | Docking, platform viewports, GLFW integration, and OpenGL3 backend. |
| GLFW | 3.4.0 | Main and secondary native windows and OpenGL contexts. |
| NVRHI | `6b96fb03e07539f08327aea76c56d55f1de9d906` | Vulkan device and command submission. |
| Vulkan-Headers | `v1.4.352` | Vulkan API declarations fetched by NVRHI. |
| DXC | 1.9.2602.24 | Build-time HLSL-to-SPIR-V compilation. |

The repository pins these versions. ImGui and GLFW sources are under `PulseForge/Vendor`; the NVRHI, Vulkan-Headers, and DXC pins are declared in the root `CMakeLists.txt`.

## Focus, commands, and close behavior

Document commands resolve against the focused document identity and active project. With no focused document, commands use the Level Editor. Commands reject stale identities from another project. Policy tests cover Level, synthetic Material-kind and unsupported Texture-kind documents, and stale-project routing. A production full asset editor is needed to validate commands across multiple real asset workspaces.

Scene and history shortcuts are suppressed while text input, active ImGui controls, menus, popups, or modals own interaction. The Scene Viewport remains with the Level Editor; floating document hosts do not own its camera or gizmos.

Document-tab close, native-window close, project switch, and application shutdown use Save, Discard, or Cancel decisions. `EditorDocumentSession` tracks edit and saved revisions. Saving revision N marks the session clean only through N, so a later edit remains dirty. Failed saves and cancelled closes preserve the session. Host close resolves dirty documents before retiring the host; shutdown retains host state until the final workspace snapshot is written.

Material Details uses the application's dirty-state decision for its in-memory draft. Revert reloads the serialized material and resets its edit history. Migrating a legacy material document record selects that material for the utility without saving or discarding its draft.

## Workspace persistence and recovery

`EditorLayout` stores utility layout and panel state under `%LOCALAPPDATA%/PulseForge/Editor`. Set `PULSEFORGE_EDITOR_SETTINGS_DIR` to use isolated settings for tests. Isolated settings do not import the legacy working-directory `imgui.ini`.

Document state is stored in `documents-<project UUID>.json` beside the ImGui settings. Schema version 2 records project identity, document hosts and order, active documents, remembered placement by editor kind, host bounds, and the selected Material Details asset. Version 1 is migrated. Raw window, GPU, and viewport pointers are not persisted. Malformed, duplicate, foreign-project, missing-asset, or unregistered-editor records are rejected or skipped. A legacy Material-kind document record is migrated to the Material Details utility; obsolete empty hosts are removed without discarding unrelated valid state.

Saved native bounds include monitor name, position, size, DPI scale, and maximized state. Invalid geometry is clamped to a usable work area; valid negative desktop coordinates are accepted. If a saved monitor is unavailable, restoration uses an available monitor. Deterministic tests and a single-monitor runtime check cover this recovery. Physical monitor removal and mixed-DPI movement have not been verified.

Reset Layout rebuilds the utility dock without clearing document membership or edit state. A changed default-layout version does not replace a saved workspace. If the saved ImGui dockspace is missing or incomplete, the editor creates the default dockspace and retains valid panel state where possible.

## Asset moves and project dialogs

Content Browser moves use separate drag payloads for asset and folder moves. `AssetOperations::MoveFolder` moves a directory on the same volume, carries `.meta` sidecars, preserves UUIDs, and rebuilds the registry. It rejects traversal, reparse points, collisions, and moves into the source or its descendants. If registry rebuilding fails, the service attempts rollback and reports when recovery is still needed. Open document paths and labels refresh by UUID without changing identity or dirty revision.

The shared project-folder/name dialog supports asset Move/Rename, Duplicate, New Scene, Save Scene As, and Create Prefab. It provides folder search, breadcrumbs, filename and extension validation, collision feedback for files and sidecars, Enter to accept, and Escape to cancel. External image import and project-directory selection use native file dialogs. Asset changes made outside the editor are detected by explicit Refresh; continuous file watching is not implemented.

## Adding a full specialized editor

Future Material, Blueprint, and other specialized editors must register a document kind explicitly in `RegisteredAssetDocumentEditors`. Each integration should:

1. Add a stable `EditorDocumentKind` and update workspace schema validation or migration if persisted values change.
2. Own editable data, undo/redo, selection, preview state, and pending operations in a session keyed by project UUID, asset UUID, and editor kind.
3. Open and activate sessions through `EditorDocumentManager`; repeated opens should activate the existing session.
4. Render a complete editor workspace in its document host. Keep utility panels outside the top-strip transfer target.
5. Implement save, discard, dirty revisions, close decisions, and focused-document commands through the shared lifecycle policies.
6. Register stable utility and workspace layout identities. Detach, reattach, and regroup the same session without reopening it.
7. Assign preview targets and UI textures a generation and retirement policy that covers in-flight draw work from every consuming host.
8. Add deterministic registration, persistence, close, transfer, and failure tests, then validate the editor through the supported Vulkan and OpenGL presentation paths.

Material Details is not a document editor. A future full Material Editor is a separate workspace and opening operation.

## Validation and known limitations

Automated tests cover document identity, duplicate opens, host membership, transfer preparation, close decisions, save revisions, focused commands, layout migration and recovery, placement preferences, path bounds, asset operations, dialogs, and command-line parsing. The synthetic `--document-host-test-fixture` and related `--document-host-test-*` switches exercise the production host and tab-strip code with unregistered placeholder sessions or bounded renderer failures; they do not open editable asset content.

On 2026-10-14, the Vulkan and OpenGL Visual Studio 2022 x64 Debug, Release, and Dist editor/test configurations built successfully. Fresh `PulseForgeTests` runs passed 4,384 checks and CTest passed 2/2 in each configuration. Policy tests verify that a tab can become a native host only when released outside every editor window. Three-round Debug transfer fixtures ran on both backends. The Vulkan fixture used Khronos and NVRHI validation, rejected the bodies and four edges of Hierarchy, Inspector, Scene Viewport, Content Browser, and Console, and exercised reattachment, native detachment, and grouping. It also detached a tab from a grouped native host into a separate native host while retaining the other tab. The run reported no fixture failures or Vulkan/NVRHI validation errors. The OpenGL fixture confirmed backend startup and completed the same transfer sequence, including grouped-host tear-off. These fixtures queue ImGui events through the application path; they do not deliver operating-system pointer input. A Debug Vulkan capture confirms the native tab strip and close control render, but does not establish pointer-driven behavior.

Pointer-driven tab reordering, detachment, grouping, and reattachment still need manual testing. Physical multi-monitor movement, mixed-DPI transitions, and monitor-removal recovery are unverified. The OpenGL shell's visual output is not established by the automated fixture. No production full asset editor is registered, so multiple real asset sessions, their independent histories, and their dirty-close prompts remain unverified. OpenGL does not render scene snapshots or GPU asset previews.

### Manual document-host checks

Run the Debug-only fixture from the repository root to open synthetic document tabs without modifying project assets:

```powershell
cmake --build Build/CMake/VS2022-x64 --config Debug --target PulseForgeEditor --parallel 1

$repo = (Get-Location).Path
$previousSettingsDir = $env:PULSEFORGE_EDITOR_SETTINGS_DIR
$env:PULSEFORGE_EDITOR_SETTINGS_DIR = Join-Path $env:TEMP ("PulseForge-DocumentHost-Manual-" + [guid]::NewGuid())
Push-Location (Join-Path $repo 'Build/CMake/VS2022-x64/Debug')
try {
	$project = Join-Path $repo 'Samples/VisualValidation/EditorWorkflow/EditorWorkflow.pfproj'
	.\PulseForgeEditor.exe --project $project --document-host-test-fixture
}
finally {
	Pop-Location
	if ($null -eq $previousSettingsDir) {
		Remove-Item Env:\PULSEFORGE_EDITOR_SETTINGS_DIR -ErrorAction SilentlyContinue
	}
	else {
		$env:PULSEFORGE_EDITOR_SETTINGS_DIR = $previousSettingsDir
	}
}
```

Use a mouse to test tab reorder, grouping, detachment, and reattachment in both directions. Confirm that utility panels and workspace edges reject document drops, and that Level Editor remains in the main host. Test all three opening preferences with a registered full editor when one is available. Material Details should always open beside Inspector while the Scene Viewport remains available. Test dirty-close decisions and dialogs in a floating asset editor after a production full editor is registered.
