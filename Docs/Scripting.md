# Lua scripting

`ScriptComponent` stores a managed script asset UUID and an enabled flag. Script identity resolves through the project's sidecar-backed asset registry; the runtime accepts managed `.lua` source files only. Scene serialization writes the component as a UUID reference, and prefab serialization preserves it.

Create a `ScriptRuntime` with a live `Project`, call `Start` for a `Scene`, then call `Advance(scene, timestep)` once per frame. The project and scene must outlive the runtime and the scene must remain at the same address while it is running. `Stop` invokes `OnDestroy` on active scripts. A script may define these optional global functions:

- `OnCreate()` when its component is enabled and its asset is loaded.
- `OnUpdate(deltaSeconds)` once per `Advance`.
- `OnDestroy()` when disabled, removed, replaced, or when the runtime stops.

Each scripted entity receives its own Lua state, so script-local variables are not shared between entities using the same source. A failed compile or initialization is reported as a diagnostic and does not prevent other scripts from starting. An update failure is reported once and disables further updates for that script until it is reattached or the runtime restarts.

The initial host API is deliberately small:

- `entity:uuid()` returns the entity UUID.
- `entity:get_translation()` returns `x, y, z`.
- `entity:set_translation(x, y, z)` returns `true`, or `nil, message` if the entity or values are invalid.
- `scene.find_entity(uuid)` returns an entity handle or `nil` when no entity has that UUID; invalid UUID input returns `nil, message`.

The runtime opens only the base, table, string, math, and UTF-8 libraries. File, operating-system, package, debug, and coroutine libraries are not opened. `load`, `loadfile`, `dofile`, `collectgarbage`, `pcall`, `xpcall`, and `string.dump` are disabled so scripts cannot catch the instruction-budget hook's callback error and continue running. These restrictions and execution limits are intended for gameplay scripts, not as a security boundary for hostile code.

By default, a source file is limited to 2 MiB, each Lua state to 16 MiB, all states in one runtime to 256 MiB, and each lifecycle callback to at most 100,000 Lua instructions. Instruction checks run in 1,000-instruction quanta and round the effective limit down to a quantum. `ScriptRuntimeDesc` can lower these limits for tests or projects. Source code is read at runtime; there is no bytecode cache or hot reload yet.

Lua 5.5.1 is fetched from the official Lua release archive and built into PulseForge. It is a build-time dependency; end-user builds do not require a Lua installation or the Vulkan SDK. The editor, project script authoring, hot reload, entity creation/destruction, component access beyond transforms, and physics/audio/input bindings remain future work.
