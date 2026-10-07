# Editor

`PulseForgeEditor` is a separate application target. The engine and `PulseForgeGame` do not depend on ImGui; the editor currently owns its ImGui docking shell and explicitly uses the transitional OpenGL backend because the editor UI renderer has not yet been integrated with NVRHI/Vulkan.

The shell provides dockable Scene, Hierarchy, Inspector, Content Browser, and Console panels. These are presentation scaffolds only: project/scene operations, selection, and panel data are not connected yet. Build the target with the default `PULSEFORGE_BUILD_EDITOR=ON` CMake option, or set it to `OFF` for runtime-only builds.
