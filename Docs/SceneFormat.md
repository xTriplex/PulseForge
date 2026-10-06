# PulseForge Scene JSON

`SceneSerializer` writes JSON with this top-level structure:

```json
{
  "format": "PulseForgeScene",
  "version": 3,
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
        "farClipPlane": 1000.0,
        "primary": true
      },
      "meshRenderer": {
        "meshAsset": "62cf4556-0d8c-4ac5-96f5-df2bc4a1f6eb",
        "materialAsset": "c81dd968-a9ac-42e7-9c31-1d37072dfb66"
      },
      "parent": null
    }
  ]
}
```

UUIDs use the canonical 8-4-4-4-12 hexadecimal spelling. Generated IDs are random version-4 UUIDs. Rotation values are quaternions in `[w, x, y, z]` order. Transform values are local to the entity's parent; `parent` is either `null` or another entity UUID in the same document.

Every entity must have a UUID, tag name, transform, and parent field. The `camera` object is optional; when present it describes a perspective camera with a vertical field of view in radians, finite near/far clip planes, and an optional `primary` boolean. Older scenes without `primary` load with it set to `false`. Scene snapshot construction can select a camera by UUID or resolve the single primary camera; zero or multiple primary cameras are reported as errors. PulseForge uses a right-handed, -Z-forward projection with zero-to-one clip depth and a vertical flip for framebuffer coordinates. `meshRenderer.meshAsset` is a stable mesh UUID; `materialAsset` is an optional stable material UUID. References are never filesystem paths and are retained when an asset is temporarily unresolved. Versions 1 through 3 remain readable; scenes written by the current serializer use version 4. Parent references must resolve, and the hierarchy must be acyclic. The serializer emits entities in UUID order. Child lists are derived from parent references rather than stored separately. Unknown fields are ignored; incompatible formats or versions are rejected.

Version 2 added the optional mesh-renderer reference. Version 3 added the optional material UUID. Version 4 added the optional primary-camera flag. Version 1 and 2 documents remain loadable; older mesh renderers have no material assignment, and older cameras are not primary unless explicitly marked after migration. The serializer writes version 4. A version change is required when a new schema cannot be safely interpreted by older readers.

Deserialization stages and validates the full scene before replacing the destination. On failure the destination and its entity handles remain unchanged; on success, handles referring to the replaced scene state become invalid. Incompatible format changes require a new version and an explicit migration path.

`SaveToFile` writes a temporary sibling file and replaces the destination only after the complete document has been written. `LoadFromFile` leaves the destination scene unchanged if opening, reading, or parsing fails. File paths are supplied by the caller and are not stored in the scene document.
