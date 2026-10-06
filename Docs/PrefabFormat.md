# PulseForge Prefab JSON, Version 1

`PrefabSerializer` stores a root entity and its descendants inside a versioned document. The embedded scene object reuses the scene serialization format:

```json
{
  "format": "PulseForgePrefab",
  "version": 1,
  "root": "00112233-4455-4677-8899-aabbccddeeff",
  "scene": {
    "format": "PulseForgeScene",
    "version": 2,
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

The root's parent outside the captured subtree is omitted. Instantiation creates new entity UUIDs and reconstructs hierarchy while retaining component asset UUIDs, so multiple instances can reference the same mesh asset. Asset paths and prefab inheritance/overrides are not stored in this format.

The current serializer preserves tag, transform, camera, and mesh-renderer components. New scene components must be added to scene and prefab serialization together.

Managed prefabs use the `.prefab` source extension and an adjacent `.prefab.meta` sidecar. `PrefabAssetService::Create` serializes a subtree to a new managed path, assigns a fresh sidecar UUID, and refreshes the supplied registry without overwriting existing files. The sidecar UUID is the prefab asset identity; the service resolves that UUID through the generated asset registry before saving or instantiating. Moving the prefab and sidecar changes its path, not its identity. Duplicating a prefab creates a new sidecar UUID.

Prefab documents are capped at 64 MiB when read or written. Unsupported prefab versions are rejected; scene and prefab versions are independent.
