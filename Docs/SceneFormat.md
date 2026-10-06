# PulseForge Scene JSON, Version 1

`SceneSerializer` writes JSON with this top-level structure:

```json
{
  "format": "PulseForgeScene",
  "version": 1,
  "entities": [
    {
      "uuid": "00112233-4455-4677-8899-aabbccddeeff",
      "tag": { "name": "Example" },
      "transform": {
        "translation": [0.0, 0.0, 0.0],
        "rotation": [1.0, 0.0, 0.0, 0.0],
        "scale": [1.0, 1.0, 1.0]
      },
      "camera": {
        "verticalFovRadians": 0.785398163,
        "nearClipPlane": 0.1,
        "farClipPlane": 1000.0
      },
      "parent": null
    }
  ]
}
```

UUIDs use the canonical 8-4-4-4-12 hexadecimal spelling. Generated IDs are random version-4 UUIDs. Rotation values are quaternions in `[w, x, y, z]` order. Transform values are local to the entity's parent; `parent` is either `null` or another entity UUID in the same document.

Every entity must have a UUID, tag name, transform, and parent field. The `camera` object is optional; when present it describes a perspective camera with a vertical field of view in radians and finite near/far clip planes. PulseForge uses a right-handed, -Z-forward projection with zero-to-one clip depth and a vertical flip for framebuffer coordinates. Parent references must resolve, and the hierarchy must be acyclic. The serializer emits entities in UUID order. Child lists are derived from parent references rather than stored separately. Unknown fields are ignored by version 1 readers; incompatible formats or versions are rejected.

Deserialization stages and validates the full scene before replacing the destination. On failure the destination and its entity handles remain unchanged; on success, handles referring to the replaced scene state become invalid. Incompatible format changes require a new version and an explicit migration path.

`SaveToFile` writes a temporary sibling file and replaces the destination only after the complete document has been written. `LoadFromFile` leaves the destination scene unchanged if opening, reading, or parsing fails. File paths are supplied by the caller and are not stored in the scene document.
