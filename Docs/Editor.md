# Editor

`PulseForgeEditor` is a separate application target. The engine and `PulseForgeGame` do not depend on ImGui; the editor owns its ImGui docking shell and uses the transitional OpenGL backend because the editor UI renderer has not yet been integrated with NVRHI/Vulkan.

The File menu can create and open `.pfproj` projects and close the active project. New projects include an empty `Assets/Main.scene`. The Scene menu creates, opens, and saves managed `.scene` assets; the project start scene is updated when a new scene is created or saved under a new name. Native file dialogs are provided by the vendored NativeFileDialog dependency.

The Content Browser lists registered assets in project-relative path order and displays their UUIDs on hover. Double-clicking a scene asset opens it by UUID. The Hierarchy displays parent/child entities and supports selection; the Inspector currently shows the selected entity's tag, UUID, and transform read-only. Entity editing, viewport rendering, and play/stop controls are not connected yet.

Build the target with the default `PULSEFORGE_BUILD_EDITOR=ON` CMake option, or set it to `OFF` for runtime-only builds. The editor currently targets Windows and uses OpenGL for ImGui even though the game runtime defaults to Vulkan.
