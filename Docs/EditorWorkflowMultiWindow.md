# Editor Workflow and Native Multi-Window Architecture

## Scope and progress

This record tracks the implementation authorized by `C:\Users\kaan_\Desktop\Pulseforge\PulseForge_Editor_UI_Architecture_Dossier.md` (read in full on 2026-10-13). It is a feature-branch implementation record; the dossier remains the product contract. The completed UI-polish and Phase 5 asset-preview work was merged to local `main` and validated before this branch was created.

| Milestone point | Status | Evidence / next action |
|---|---|---|
| Prior-work integration | Complete locally | `main` base `e702e44c1a7e653c0af45dd3a398ee8d3931070e`; Debug/Release/Dist each 4,058 checks and CTest 2/2. Push to `origin/main` was rejected by automatic review; remote publication was not requested. |
| Fresh feature branch | Complete | `feat/editor-workflow-multi-window` starts at the integrated main SHA above. |
| Source and dependency audit | In progress | Current single-window and renderer ownership are recorded below. Finish focused feasibility prototypes before broad integration. |
| Asset/folder move transactions | Implemented and validated | `AssetOperations::MoveFolder` performs same-volume staged rename, rejects traversal/reparse points/self-descendants/collisions, rebuilds registry with rollback; Content Browser has distinct asset/folder drag payloads and defers the move until after item traversal. `TestAssetFolderOperations` passes in Debug/Release/Dist (4,081 checks); editor target builds in all three. |
| Document sessions and top strip | Not started | Replace the single material-editor utility panel with stable multi-asset sessions and an application-managed document strip. |
| Native hosts and backend presentation | Not started | Demonstrate shared-device Vulkan and OpenGL host presentation, independent lifecycle and close requests. |
| Transfer/preferences/persistence/dialogs | Not started | Implement validated versioned host/workspace restoration and scoped dialogs. |
| Final regression and handoff | Not started | Track dossier D01-D14, C01-C10, R01-R12, P01-P10 individually with evidence. |

## Repository and protected-state baseline

- Repository: `C:\Dev\PulseForge`.
- Starting source branch: `feature/editor-asset-previews`, HEAD `41d7560b94550907e134a4ba14e0a3e3ab052c13`.
- Integrated branch: `feat/editor-workflow-multi-window`, based on local `main` at `e702e44c1a7e653c0af45dd3a398ee8d3931070e`.
- Remote `origin/main` remains at `43e68fb0b77f06d365f460b377762d87f73f8cf8`. A normal push of local `main` was rejected by the automatic approval reviewer; no remote main update was made.
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

This decision is source-grounded, but the required native behavior prototype remains a gate before adopting any Vulkan presentation implementation. The next prototype must demonstrate a real second OS window, preserve a document session during host transfer, keep the window top-level while overlapping the main window, and reject non-strip targets. If the renderer cannot present that window safely through the current NVRHI/device ownership, stop that presentation path and document the exact gap rather than claiming multi-window support.

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

The audit/prototype phase is still active for document hosting and rendering. No native-window or document-session implementation has been committed yet.
