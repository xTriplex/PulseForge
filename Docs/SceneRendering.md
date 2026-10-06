# Scene Rendering

`SceneRenderSnapshotBuilder` converts a scene, a selected camera, and a framebuffer aspect ratio into immutable per-frame render data. Callers may select a camera entity explicitly or let the builder resolve the scene's sole camera marked `primary`; missing or multiple primary cameras produce clear errors. It resolves entity hierarchy transforms but performs no GPU work.

`SceneRenderer` consumes that snapshot through PulseForge's renderer and asset-cache APIs. Call `PrepareScene` during `Layer::OnUpdate`, before `Application` opens the frame command list; it resolves meshes and unlit material textures and lazily creates the compatible pipeline. Call `RenderPreparedScene` from `Layer::OnRender` to update per-object constants and submit indexed draws. The application and project must outlive the renderer, and the project must not be moved while the renderer exists because asset caches refer to its registry.

The initial scene renderer supports one vertex layout per renderer instance and the current unlit textured material format. It reports missing or unsupported assets/layouts through typed errors. Multi-layout pipeline caching, broader materials, lighting, and camera selection policy remain later work. The transitional OpenGL backend continues to provide its bootstrap/ImGui path and does not render scene snapshots.
