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
| OpenGL | GLFW secondary windows use shared OpenGL contexts and Dear ImGui's OpenGL3 renderer. The main context is restored after secondary rendering. | The Scene Viewport does not render a scene, and asset previews use type icons. |

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

On 2026-10-14, the Vulkan and OpenGL Visual Studio 2022 x64 Debug, Release, and Dist editor/test configurations built successfully. Fresh `PulseForgeTests` runs passed 4,384 checks and CTest passed 2/2 in each configuration.

Debug transfer fixtures ran on both backends. They exercised reattachment, native detachment, tab reordering and grouping, and rejected document drops over utility panels and workspace edges. A grouped native host could transfer one tab while retaining its other tab. These fixtures queue ImGui events through the application path; they do not deliver operating-system pointer input.

Separate Vulkan Debug fixtures recovered from presentation creation failure, swapchain image/depth allocation failure, and out-of-date image acquisition. In a bounded lifecycle run, the secondary host was resized four times, then resized again while the main window was minimized. Both secondary hosts remained visible. The secondary host also minimized and restored, then rendered a valid UI capture. A targeted native close retired one synthetic host while the main editor and sibling host remained open; the sibling continued to render. Khronos and NVRHI validation were enabled; these lifecycle runs reported no VUID or NVRHI validation errors, had empty standard error, and exited cleanly.

OpenGL Debug readbacks showed the editor shell, Content Browser type icons for images and materials, and the Vulkan-only viewport fallback. A bounded tear-off fixture rendered a secondary native document viewport through its shared OpenGL context. `PrintWindow` captures were blank, while internal readbacks taken after the OpenGL ImGui draw calls showed real UI pixels. A Vulkan runtime capture with Material Details selected confirmed that its tab shares Inspector's right-side stack, Level Editor remains active, and the Scene Viewport still displays the validation scene. A separate Debug fixture capture showed the document strip and close control.

Pointer-driven tab reordering, detachment, grouping, and reattachment still need manual testing. Physical multi-monitor movement, mixed-DPI transitions, and monitor-removal recovery are unverified. No production full asset editor is registered, so multiple real asset sessions, their independent histories, and their dirty-close prompts remain unverified. OpenGL does not render scene snapshots or GPU asset previews.

### Acceptance coverage

The scenario IDs below match the editor workflow and native-window acceptance matrix. **Passed** means the listed evidence covers the scenario's current scope. **Partial** means policy, synthetic, or bounded runtime coverage exists but a required real interaction or production editor path remains unverified. **Unverified** means the available evidence is insufficient. **Deferred** items require a production full asset editor, which is not registered.

#### Documents and docking

| ID | Status | Evidence and limits |
|---|---|---|
| D01 - Open Material Details with the default preference | Passed (policy and runtime layout) | The asset-open policy routes materials to Material Details. A Vulkan runtime capture showed it beside Inspector while Level Editor and Scene Viewport remained available. A real pointer-driven Content Browser double-click remains unverified. |
| D02 - Switch materials with unsaved drafts | Unverified (manual) | Drafts are keyed by project and asset identity in `EditorApplication.cpp`; tests cover key isolation, not editing and switching drafts in the UI. |
| D03 - Open an already-open asset document | Partial (policy) | `TestEditorDocumentManager` verifies duplicate identity activation and asset-location refresh. No production full asset document is registered. |
| D04 - Transfer a document to the main strip | Passed (synthetic application path) | Debug fixtures transfer the same session between native and main hosts. They queue ImGui events rather than operating-system pointer input. |
| D05 - Drop over utility panels | Passed (synthetic application path) | Fixtures reject drops over Hierarchy, Inspector, Scene Viewport, Content Browser, and Console. Pointer-driven rejection remains unverified. |
| D06 - Drop over workspace bodies or edges | Passed (synthetic application path) | Fixtures reject panel bodies and the tested edges; they do not prove behavior from physical mouse input. |
| D07 - Detach a dirty document with its editor state | Partial (policy) | Session transfer tests preserve dirty revisions and host membership. A production asset editor is needed to verify real undo, selection, preview, and model state through transfer. |
| D08 - Reattach repeatedly | Passed (synthetic application path) | Repeated native/main transfer fixtures preserve session identity and retire empty hosts. Physical reattachment remains unverified. |
| D09 - Reorder tabs and persist order | Passed (policy and synthetic application path) | Tab-strip fixtures reorder sessions; workspace codec tests cover persisted order. Physical pointer-driven reordering remains unverified. |
| D10 - Switch between Level Editor and asset workspaces | Partial (utility runtime) | Material Details leaves the Level Editor and Scene Viewport available. Switching among production full asset editors is deferred. |
| D11 - Group and regroup document tabs | Passed (synthetic application path) | Fixtures group sessions, move one tab from a grouped native host, and retain the other tab. Physical grouping remains unverified. |
| D12 - Rearrange utility panels | Partial (layout policy and runtime) | Default placement, visibility, persistence, and Reset Layout have coverage. Pointer-driven utility rearrangement has not been tested. |
| D13 - Distinguish same-basename assets | Passed (policy) | `TestEditorDocumentManager` distinguishes asset and project identities independently of displayed filenames. |
| D14 - Move or rename an open asset | Partial (service and policy) | Asset-operation tests preserve UUIDs; document-manager tests refresh paths without changing identity or dirty revision. An open production editor cannot be exercised until one is registered. |

#### Close and focus

| ID | Status | Evidence and limits |
|---|---|---|
| C01 - Cancel closing a dirty tab | Partial (policy) | `TestEditorDocumentManager` verifies Cancel preserves the session and revision. A real dirty asset-tab close prompt is unavailable without a production editor. |
| C02 - Cancel native close or Alt-F4 for a dirty host | Unverified (production editor) | Native close routing is present, but dirty-close interaction in a real asset workspace has not been exercised. |
| C03 - Keep a document dirty after save failure | Passed (policy) | `TestEditorDocumentManager` injects a failed save callback and verifies the session remains open and dirty. |
| C04 - Resolve several dirty documents as a batch | Partial (policy) | Dirty-session filtering and batch close policies are tested; a real multi-document host dialog is not available. |
| C05 - Close the application with dirty secondary documents | Unverified (production editor) | Shutdown ordering is implemented, but there are no registered production asset documents to exercise a batch dirty shutdown. |
| C06 - Save or undo Material Details while Level Editor remains active | Unverified (manual interaction) | Material Details is a utility, not a floating document. Save/Revert and focus routing are implemented, but this interaction has not been tested directly. |
| C07 - Prevent text fields and dialogs from leaking shortcuts | Passed (policy) | `TestEditorInteractionPolicy` covers text, popup, menu, and modal ownership. Real typing workflows still need manual checking. |
| C08 - Release viewport capture on focus loss | Partial (policy) | Viewport capture and focus-loss policies are tested; physical focus transitions among native windows remain unverified. |
| C09 - Preserve later edits after saving an earlier revision | Passed (policy) | Document revision tests verify that saving revision N leaves a subsequent revision dirty. |
| C10 - Reset layout without discarding edits | Passed (synthetic application path) | The Debug reset-layout fixture verifies dirty synthetic sessions and host membership survive. |

#### Renderer and platform

| ID | Status | Evidence and limits |
|---|---|---|
| R01 - Resize a secondary host repeatedly | Passed (bounded Vulkan runtime) | A Debug runtime resized a secondary host four times and recorded new swapchain depth-target extents. |
| R02 - Minimize and restore a secondary host | Passed (bounded Vulkan runtime) | The secondary host minimized and restored, then rendered a valid capture without a validation error. |
| R03 - Keep secondary rendering active when main is minimized | Partial (bounded Vulkan runtime) | Both secondary hosts remained visible; one was resized and captured while the main window was minimized. Pointer response while minimized was not tested. |
| R04 - Move windows between mixed-DPI monitors | Unverified (hardware) | DPI and geometry policies are tested, but physical mixed-DPI monitor movement is unavailable in this validation environment. |
| R05 - Restore a window on negative desktop coordinates | Partial (policy) | `TestEditorNativeWindowBoundsPolicy` covers negative coordinates with a simulated monitor work area. Physical placement on a negative-coordinate monitor is unverified. |
| R06 - Retire resources during close and transfer | Passed (bounded Vulkan runtime) | Native close retired one host while the main and sibling host remained rendered. Khronos and NVRHI validation reported no VUID or NVRHI errors. |
| R07 - Show scene and multiple asset previews together | Partial (visual) | The Vulkan capture showed the Scene Viewport with material swatches in the Content Browser. Simultaneous full asset-editor previews are deferred until such editors exist. |
| R08 - Run Vulkan Debug with validation enabled | Passed (bounded Vulkan Debug) | Khronos and NVRHI validation were active during document-host, resize, minimize, close, and failure-recovery runs; no VUID or NVRHI validation errors were reported. |
| R09 - Run native-host flows on OpenGL | Passed (bounded OpenGL runtime) | Internal framebuffer readbacks showed the main shell, material and image type icons, and a secondary native host. OpenGL does not render the Scene Viewport or Vulkan previews. |
| R10 - Recover from presentation creation failure | Passed (failure injection) | The Debug fixture injected native presentation and swapchain-allocation failures and retained the source session in the main host. |
| R11 - Recover from out-of-date acquisition | Passed (failure injection) | The Debug fixture injected `VK_ERROR_OUT_OF_DATE_KHR` before acquisition and presented a subsequent frame. |
| R12 - Recover from swapchain allocation failure | Passed (failure injection) | Partial swapchain image/depth allocation was injected; setup rolled back and the session remained usable. Allocator-level reclamation counters were not measured. |

#### Persistence and assets

| ID | Status | Evidence and limits |
|---|---|---|
| P01 - Restore main and secondary hosts after restart | Passed (bounded synthetic-host restart) | Two synthetic document hosts restored across an editor restart with isolated settings. Production asset-editor restoration and unsaved crash recovery are not covered. |
| P02 - Recover after removing a monitor | Partial (policy) | Missing-monitor placement recovery is tested against simulated monitor data. Physical monitor removal and restart are unverified. |
| P03 - Recover from corrupt or obsolete workspace data | Passed (policy and runtime) | Workspace codec tests and bounded recovery runs reject corrupt state while retaining valid document state. |
| P04 - Skip missing asset records | Passed (runtime) | Missing-asset workspace restoration skips invalid records while preserving valid state. |
| P05 - Keep Material Details independent of opening preference | Passed (policy and runtime layout) | Material opening resolves to the utility operation regardless of full-editor preference; the runtime capture confirmed Inspector-side placement. No full editor is registered to exercise the other half of the preference. |
| P06 - Resolve remembered placement by editor kind | Passed (policy) | Manager and workspace tests cover remembered-host reuse and deterministic fallback. |
| P07 - Move a referenced asset while its editor is open | Partial (service and identity policy) | Asset operations and stable-identity path updates are tested; end-to-end editor and reference behavior needs a production asset editor. |
| P08 - Reject collisions and recover from move failures | Passed (service tests) | `TestAssetOperations` covers collision, invalid-path, registry-refresh, and rollback cases. |
| P09 - Reject moving a folder into itself or a descendant | Passed (service tests) | `TestAssetOperations` verifies rejection before filesystem mutation. |
| P10 - Route asset dialogs from a floating full editor | Deferred | Shared dialogs work in the main workspace. No production full asset editor exists to test native-host ownership and result routing. |

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

Use a mouse to test tab reorder, grouping, and reattachment in both directions. Drag tabs from the main strip and a grouped native host, then release outside every PulseForge window to create a new native host. Confirm that utility panels and workspace areas reject document drops, and that Level Editor remains in the main host. Test all three opening preferences with a registered full editor when one is available. Material Details should always open beside Inspector while the Scene Viewport remains available. Test dirty-close decisions and dialogs in a floating asset editor after a production full editor is registered.
