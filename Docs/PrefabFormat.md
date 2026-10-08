# PulseForge Prefab JSON, Version 3

`PrefabSerializer` stores a root entity and its descendants inside a versioned document. The embedded scene object reuses the scene serialization format:

```json
{
  "format": "PulseForgePrefab",
  "version": 3,
  "root": "00112233-4455-4677-8899-aabbccddeeff",
  "scene": {
    "format": "PulseForgeScene",
    "version": 7,
    "entities": [
      {
        "uuid": "00112233-4455-4677-8899-aabbccddeeff",
        "tag": { "name": "Prefab root" },
        "transform": {
          "translation": [0.0, 0.0, 0.0],
          "rotation": [1.0, 0.0, 0.0, 0.0],
          "scale": [1.0, 1.0, 1.0]
        },
        "parent": null
      }
    ]
  }
}
```

The root's parent outside the captured subtree is omitted. Instantiation creates new entity UUIDs and reconstructs hierarchy while retaining component asset UUIDs, so multiple instances can reference the same mesh and optional material assets. Primary camera and audio-listener flags are scene-local roles: prefab capture and instantiation clear those flags so a prefab instance cannot silently create another primary. Asset paths and prefab inheritance/overrides are not stored in this format.

The current serializer preserves tag, transform, camera, directional-light, mesh-renderer, rigidbody, box-collider, audio, and script components. Embedded scene versions 1 through 7 are readable; optional directional-light records are supported without changing the scene version. Primary camera and audio-listener flags are scene-local roles and are cleared on prefab capture/instantiation; directional lights are ordinary authored entity components and are retained. New scene components must be added to scene and prefab serialization together.

Managed prefabs use the `.prefab` source extension and an adjacent `.prefab.meta` sidecar. `PrefabAssetService::Create` serializes a subtree to a new managed path, assigns a fresh sidecar UUID, and refreshes the supplied registry without overwriting existing files. The sidecar UUID is the prefab asset identity; the service resolves that UUID through the generated asset registry before saving or instantiating. Moving the prefab and sidecar changes its path, not its identity. Duplicating a prefab creates a new sidecar UUID.

Prefab documents are capped at 64 MiB when read or written. Prefab versions 1 through 3 are readable; version 3 writes the updated scene serialization. Unsupported versions are rejected, and scene/prefab versions are tracked independently.
