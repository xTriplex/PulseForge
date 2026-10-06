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

Prefab JSON is source content and should receive its own adjacent `.meta` sidecar when managed by a project. Its stable asset identity is not embedded in the document. Unsupported prefab versions are rejected; scene and prefab versions are independent.
