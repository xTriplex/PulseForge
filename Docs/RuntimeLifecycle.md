# Scene runtime lifecycle

`SceneRuntime` coordinates one loaded scene and one project. Both objects must outlive the runtime, and the scene must not move while it is started. Startup creates an audio device/cache, starts scripts so `OnCreate` can initialize scene transforms, creates physics bodies from the resulting scene state, and then starts scene audio. If any subsystem fails, already-started services are torn down and script `OnDestroy` callbacks run.

Each `Advance` runs script `OnUpdate` callbacks, advances Jolt by its configured fixed step and substep limit, then updates audio listener/source positions. Call it before preparing the scene renderer so physics-synchronized transforms are included in the frame snapshot. A runtime-level failure identifies the failing subsystem; script callback faults remain diagnostics and do not abort the remaining scene update.

`Stop` releases the coordinated services. Script shutdown runs while the scene and project are still alive. The current physics integration creates bodies at startup; adding or changing physics components during a running scene still requires a runtime restart. Script transform writes are not a physics teleport API for dynamic bodies.
