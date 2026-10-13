# PulseForge Editor

`PulseForgeEditor` is a separate Windows application target. The engine and `PulseForgeGame` do not depend on ImGui. The editor owns the ImGui shell and uses Vulkan/NVRHI by default. The transitional OpenGL configuration renders the editor shell and native UI windows, but does not render scene content. See [Editor Documents and Native Windows](EditorWorkflowMultiWindow.md) for document hosts, native presentation, and persistence.

## Workspace and settings

The default workspace has a dedicated document strip above the utility dockspace. Hierarchy opens on the left, Scene Viewport in the center, Inspector and Material Details in the right utility dock, and Content Browser, Console, and Scene in the bottom dock. Material Details and Scene start hidden. The Window menu controls panel visibility; **Window > Reset Layout** restores this default without closing documents or discarding edit state.

Dear ImGui stores user docking and window geometry in `%LOCALAPPDATA%/PulseForge/Editor/imgui.ini`. PulseForge stores panel visibility, layout metadata, and document-opening preference alongside it. Project document-host state is stored separately by project UUID. Use `PULSEFORGE_EDITOR_SETTINGS_DIR` to run the editor with an isolated workspace; isolated settings do not import the legacy working-directory `imgui.ini`.

The default utility layout has version 4. Updating the default does not overwrite a saved custom layout. A new layout is created when no restorable dockspace exists or when the user requests Reset Layout. Material Details docks beside Inspector when it has no saved location. A user-moved utility panel keeps its saved placement. Reset Layout and legacy material-document recovery place Material Details beside Inspector.

The document-opening preference applies to registered full asset editors. **New Native Window** is the default; **Main Document Strip** and **Remembered Placement** are also available. The current Material Details property form is a utility panel, not a full asset editor, and always follows utility docking regardless of this preference. No full asset editor is currently registered.

## Appearance and accessibility

`EditorStyle` centralizes the slate/blue palette, spacing, property layouts, and control metrics. The editor bundles Inter 4.1 Regular and SemiBold for interface text and JetBrains Mono 2.304 Regular for Console messages. The font atlas includes Latin, Greek, Cyrillic, IPA extensions, combining marks, general punctuation, and currency symbols; CJK glyph ranges are not included. Selected Font Awesome Free Solid 7.3.0 icons are merged into interface fonts. The editor uses FreeType for OTF icon outlines. Licenses and provenance notices are in `PulseForgeEditor/Assets/ThirdParty/`.

Missing custom fonts fall back to Dear ImGui's built-in font; missing icons leave text labels available. Font sizes are startup settings. Dynamic per-monitor font-atlas rebuilding is not implemented. New panels should use `EditorStyle` and `EditorIcon` helpers and preserve stable ImGui window names so saved layouts remain valid.

Palette values are authored in display-space sRGB. Vulkan linearizes UI draw colors for sRGB composition; a UNORM swapchain uses an sRGB intermediate and a final output-transfer pass. OpenGL requests a non-sRGB framebuffer and retains direct display values. These conversions affect editor UI colors, not scene/material data. See [Scene Rendering](SceneRendering.md) for scene HDR and display conversion.

The menu, global toolbar, and status strip reserve space around the dockspace. The toolbar keeps Play/Stop centered and Save on the right; it adapts at narrow widths and moves secondary viewport controls into a More menu when needed. The status strip shows operation feedback and compact scene, runtime, and backend state. The native title follows the current project or scene and marks unsaved scene edits. On Windows, the editor requests dark caption styling while retaining the system title-bar controls.

## Workspace panels

### Hierarchy and Inspector

Hierarchy supports an ASCII case-insensitive name filter. Matching entities and their ancestors remain visible without changing the scene. Visibility eyes change editor-only visibility; hidden rows remain selectable. `H` hides the selection, `Ctrl+H` shows all, and `Shift+H` isolates the selection. Isolation state is transient, scene-local, and undoable; it is not serialized into runtime scene data.

The Inspector shows the selected entity, UUID, and component properties. It edits entity names, local transforms, cameras, lights, mesh renderers, physics, audio, and scripts. Asset selectors show a filename in the closed control and the full project-relative path in their popup/tooltip. Multi-selection property editing is not implemented. Point and spot lights expose opt-in shadows; see [Scene Rendering](SceneRendering.md) for their budgets and renderer limits.

### Content Browser

The Content Browser builds its folder tree from registered project assets. Empty directories without registered assets are not shown. With no search query, the current folder displays immediate child folders and assets. Search matches registered asset names and paths recursively under the current folder. The root and current folder remain visible in the breadcrumb; intermediate ancestors are available from the ellipsis menu.

Grid and List modes share selection and asset operations. On Vulkan, visible supported image assets in Grid mode receive bounded asynchronous previews. Pending, unsupported, and failed previews retain the type icon. Images preserve aspect ratio and alpha. Material tiles show a saved base-color-factor swatch; it does not render the base-color map, metallic/roughness response, normal detail, or scene lighting. The OpenGL shell uses type icons rather than GPU previews. External file changes are discovered by **Refresh**; continuous file watching is not implemented.

Import copies external files into `Assets`. Rename/Move preserves the asset UUID and sidecar, Duplicate creates a new UUID, and Delete removes the asset and sidecar after confirmation. Assets and folders can be dragged onto managed folders to move within the project; operations preserve sidecars and validate project boundaries, collisions, reparse points, and self/descendant moves. Deleting the open or configured startup scene is blocked; other serialized UUID references may become unresolved after deletion.

Double-clicking a scene opens it by asset UUID. A `.prefab` asset can be instantiated in the open scene. For `.material` assets, single-click selects the asset and double-click or **Show Material Details** reveals the utility beside Inspector. Material Details shows the filename and project-relative path and edits base color, alpha mode/cutoff, double-sided state, metallic/roughness, optional texture maps, normal scale, occlusion strength, and emissive factor. Its property list scrolls independently; Save and Revert stay available in the footer. Drafts are keyed by project and asset UUID, survive material switching and panel hiding, and are not saved automatically. Draft values are session-only; the selected Material Details asset and panel layout persist.

### Console

The Console retains up to 2,000 core and client entries, with each payload limited to 8,192 bytes plus a truncation marker. Its default table aligns severity, source, and message columns on one row; hovering any part of a row highlights the whole row. Clipped messages expose the full text on hover. **Select text** switches to a read-only multiline view with mouse selection and Ctrl+C. **Copy All** copies the retained snapshot as `[severity] [logger] message` lines; the entry context menu offers **Copy Message** and **Copy Entry**. Clear removes retained entries. Auto-scroll follows new messages at the bottom; in selectable mode, scrolling upward pauses following until Auto-scroll is toggled off and on again.

## Scene Viewport and scene editing

The Vulkan Scene Viewport uses a transient editor camera in Edit mode; camera movement does not dirty the scene. Click the rendered image to focus it. WASD moves, Q/E move vertically, Shift accelerates, and the mouse wheel adjusts fly speed during right-button camera capture. Alt+left orbits the current focus pivot, Alt+middle pans, and Alt+right dollies. Without viewport keyboard focus, Q/W/E/R select Select/Move/Rotate/Scale. With viewport focus, use 1/2/3/4 so Q/E remain camera controls. Text entry, active ImGui items, popups, menus, and gizmo interaction suppress camera shortcuts. The orientation widget is informational and excluded from picking.

Click a rendered mesh to select it; click empty space to clear selection. Ctrl-click toggles and Shift-click adds selection. The Hierarchy's Shift-click selects a visible row range. `F` frames selected mesh bounds or an entity's location when it has no geometry. Picking uses CPU triangles from the static glTF importer. Gizmos transform selected top-level roots so selected parents do not transform a child twice. Multi-selection uses a shared bounds-center pivot. Alt+Move and Alt+Rotate duplicate after a drag begins. Nonrepresentable shear is rejected and rolled back. Translation snapping uses world units, rotation uses degrees, and scale snapping applies to the relative scale delta.

Press `End` in Edit mode to place selected top-level roots on the nearest eligible static mesh support below their lowest geometry. The operation preserves rotation and hierarchy, is recorded as one undo command, and does not use physics. Hidden surfaces and the selected subtrees are excluded. Support surfaces must face upward with a world-up normal dot product of at least 0.5.

Play serializes the authored scene into a runtime copy and starts scripts, fixed-step physics, and spatial audio. Stop discards the runtime copy and returns to the editor camera. A separate game viewport is not implemented. The viewport resizes its offscreen target in quantized steps to avoid rebuilding render resources for every small panel change.

Scene history is local to the open scene and disabled during Play. It stores before/after snapshots at operation boundaries, not every drag frame. It retains at most 128 commands and 64 MiB of snapshot data, with a 32 MiB per-command limit; an edit that exceeds the budget is rolled back. Create, duplicate, delete, reparent, prefab instantiation, rename, transforms, and Inspector component edits can be undone. A gizmo drag or continuous Inspector gesture is one command. Saving records the current history state as the clean checkpoint without clearing earlier undo history. Undo/redo can recreate entity handles, so the editor preserves selection by UUID.

Ctrl+Z undoes; Ctrl+Y and Ctrl+Shift+Z redo. Ctrl+C/X/V copy, cut, and paste selected entity subtrees; Ctrl+D duplicates; Delete removes; F2 renames; Ctrl+Shift+N creates an entity; Ctrl+S saves; Ctrl+Shift+S opens Save Scene As. Scene clipboard paste retains internal hierarchy and asset UUID references and rejects unavailable or cross-project references. Help > Keyboard Shortcuts lists the editor's primary bindings. The [Editor Workflow sample](../Samples/VisualValidation/EditorWorkflow/README.md) provides a scene for testing these interactions.

## Projects and asset dialogs

The File menu opens, creates, and closes projects. New projects include an empty `Assets/Main.scene`. The Scene menu creates, opens, and saves managed `.scene` assets; the Content Browser can select the project startup scene. Native file dialogs handle external file import and project-directory selection.

The shared project-folder/name dialog handles asset Move/Rename, Duplicate, New Scene, Save Scene As, and Create Prefab. It provides folder search, breadcrumbs, extension/name validation, destination and sidecar collision feedback, Enter to submit, and Escape to cancel. Inspector and Material Details asset-reference selectors filter by compatible type and search project-relative paths without changing UUID-based assignment semantics.

## Renderer and platform limits

Vulkan renders the prepared scene into a persistent RGBA16F HDR target and converts it to the SDR Scene Viewport target before composing editor UI. The transient `Exposure (EV)` control ranges from -8 to +8 EV and does not dirty or serialize the scene. ImGui overlays are composed after scene tonemapping. A UNORM swapchain uses the sRGB UI intermediate described above. Exposure and scene tonemapping do not affect editor controls.

The OpenGL fallback provides the editor shell, utility panels, docking, and native UI windows. It does not render scene snapshots or HDR output; the Scene Viewport shows its fallback message. Vulkan image thumbnails and material swatches are also disabled on this backend. PulseForge currently targets Windows; Linux and macOS builds are not supported.

For implementation details and validation limits, see [Editor Documents and Native Windows](EditorWorkflowMultiWindow.md), [Scene Rendering](SceneRendering.md), and [Visual Validation Projects](VisualValidationProjects.md).
