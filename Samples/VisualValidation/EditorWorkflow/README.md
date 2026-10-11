# PulseForge Editor Workflow

Open `EditorWorkflow.pfproj` in PulseForgeEditor. Its startup scene, `Assets/Scenes/EditorWorkflowOverview.scene`, is a compact workspace for testing hierarchy editing, selection, duplication, viewport navigation, and surface placement. The project owns its cube mesh and materials and requires no external assets or network access.

The scene includes a ground plane, an elevated platform, a sloped platform, centered-pivot cubes at different heights, a small and a large focus target, a rotated prop, and a parent with a mesh child. Use the Hierarchy names to locate each example; use `F` to frame one or more selected objects.

## Manual checks

- Click rows, Ctrl-click to toggle selection, and Shift-click Hierarchy rows to select a visible range. Select the parent and child together, then move them once: the child should not receive the parent's motion twice.
- Use Ctrl+C/Ctrl+V, Ctrl+X/Ctrl+V, and Ctrl+D. Pasted or duplicated entities receive new UUIDs and retain the copied hierarchy. Ctrl+Z and Ctrl+Y should undo and redo each operation.
- Select a cube and use Alt+drag on a Move axis or Rotate ring. A duplicate should appear only once the drag moves; the source stays in place, and the gesture is one undo step.
- Click a Hierarchy eye to hide an entity. A hidden parent also hides its child in the viewport, while both remain in Hierarchy. H hides selection, Shift+H isolates it, Ctrl+H shows all. Undo restores visibility; these are editor-only states and do not alter the runtime scene.
- Press `F` on the small cube, large object, parent, and multiselection. The editor camera should frame bounds without changing transforms.
- Select `Floating cube - centered pivot` and press `End`; it should land on the ground with its bottom at the surface. Repeat on `Floating cube - platform test`; it should land on the raised platform. The slope prop exercises an inclined support. Repeating End while resting should not drift. Each drop is undoable.
- In the viewport toolbar, switch Local/World and compare axes on `Rotated prop`. Toggle Snap or hold Ctrl during Move/Rotate/Scale drags; use the adjacent increment menu to select translation units, rotation degrees, or scale-factor increments.
- Right-drag + WASD navigates, Q/E moves vertically while navigating, Shift accelerates, and the wheel adjusts fly speed. Alt+left/middle/right drag orbits/pans/dollies when no gizmo handle is active.
- Save, close and reopen the scene, then test Play/Stop. Editor-only visibility and camera motion should not change the authored/runtime scene data.

## Notes and limitations

Surface snapping queries the supported static mesh triangles and accepts upward-facing support planes whose normal has a world-up dot product of at least 0.5. It moves selected top-level roots vertically, samples a bounded set of their world-space mesh vertices, and stops at the first support contact; it preserves orientation and hierarchy. Transparent/hidden objects are excluded as support targets. It does not use physics simulation or snap to arbitrary unsupported procedural geometry. World-space nonuniform scaling can imply shear for rotated objects; such transforms are rejected instead of silently corrupting the hierarchy. Multiselection Inspector property editing remains unavailable; use the shared viewport gizmo for grouped transforms.

The model is a local copy of PulseForge's own procedural cube validation mesh, assigned a project-specific asset UUID. No third-party assets or runtime dependencies are used.
