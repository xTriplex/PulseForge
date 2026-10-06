# Physics

PulseForge's initial scene physics runtime is backed by the pinned Jolt Physics dependency. The public API exposes engine-owned `RigidbodyComponent`, `BoxColliderComponent`, and `PhysicsSceneRuntime` types; Jolt types remain private to the implementation.

Attach both a rigidbody and a box collider to an entity before starting a physics runtime. Rigidbodies can be static or dynamic. Collider half-extents and entity scale are interpreted in meters; the current runtime uses gravity `(0, -9.81, 0)` by default. Only root entities are supported because hierarchy-to-body synchronization is not implemented yet.

`PhysicsSceneRuntime::Start` builds the runtime bodies from the scene's current components and transforms. Call `Advance(scene, timestep)` from the runtime update loop; it accumulates time and performs fixed-size steps (1/60 second by default), with a frame-delta cap and maximum substep count to bound catch-up work. Excess whole steps are discarded after the substep limit. Dynamic body positions and rotations are copied back to scene transforms after simulation steps. Entity deletion is reconciled on the next step.

The runtime does not automatically discover newly added bodies or rebuild bodies when physics components, collider dimensions, or static transforms change. Stop and start it again after those edits. Trigger events, other collider types, kinematic bodies, and automatic application-loop integration are not implemented yet. Scenes and prefabs serialize the supported component settings; see [SceneFormat.md](SceneFormat.md) and [PrefabFormat.md](PrefabFormat.md).
