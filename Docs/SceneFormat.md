# PulseForge Scene JSON

`SceneSerializer` writes JSON with this top-level structure:

```json
{
  "format": "PulseForgeScene",
  "version": 7,
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
      "rigidbody": {
        "motionType": "dynamic",
        "mass": 1.0,
        "friction": 0.2,
        "restitution": 0.0,
        "allowSleeping": true
      },
      "boxCollider": { "halfExtents": [0.5, 0.5, 0.5] },
      "audioSource": {
        "asset": "d90d2efb-5a80-42f7-9c85-44d55f95dc71",
        "volume": 0.8,
        "looping": true,
        "playOnStart": true,
        "spatialized": true
      },
      "audioListener": { "primary": true },
      "script": {
        "asset": "84d2d353-1c24-4b65-901b-47b2aa3421e3",
        "enabled": true
      },
      "parent": null
    }
  ]
}
```

UUIDs use the canonical 8-4-4-4-12 hexadecimal spelling. Generated IDs are random version-4 UUIDs. Rotation values are quaternions in `[w, x, y, z]` order. Transform values are local to the entity's parent; `parent` is either `null` or another entity UUID in the same document.

Every entity must have a UUID, tag name, transform, and parent field. Camera, mesh-renderer, physics, audio, and script component objects are optional. Asset fields contain stable UUIDs, never filesystem paths; unresolved references are preserved for later validation. Script components store a `.lua` asset UUID and an `enabled` flag. An optional audio listener marks whether that entity is the primary listener; at runtime, at most one primary listener is allowed. Parent references must resolve, and the hierarchy must be acyclic. The serializer emits entities in UUID order. Child lists are derived from parent references rather than stored separately. Unknown fields are ignored; incompatible formats or versions are rejected.

Versions 1 through 7 are readable; scenes written by the current serializer use version 7. Version 2 added mesh-renderer references, version 3 added material UUIDs, version 4 added the primary-camera flag, version 5 added physics components, version 6 added audio components, and version 7 added script attachments. Missing camera `primary` values default to false. A version change is required when a new schema cannot be safely interpreted by older readers.

Version 2 added the optional mesh-renderer reference. Version 3 added the optional material UUID. Version 4 added the optional primary-camera flag. Version 5 added optional `rigidbody` and `boxCollider` components. Version 6 added optional audio source and listener components. Version 7 added optional script attachments. The current version 7 also accepts optional `directionalLight` (`color` linear RGB, non-negative unitless `intensity`, and optional `castShadows`, `shadowDistance`, `shadowBias`, `shadowNormalBias`, and `shadowSoftness`) and `environmentLight` (`hdrImage` managed HDR asset UUID and non-negative unitless `intensity`) objects. Missing shadow fields use component defaults, so existing version-7 documents remain readable without a format bump. Directional-light ray direction derives from entity local -Z transformed by its hierarchy. Shadow softness is a PCF filter radius in map texels, not a physical emitter size. Environment rotation derives from parent-world * local rotation, while translation and scale are ignored. Older mesh renderers have no material assignment, and older cameras are not primary unless explicitly marked after migration.

Deserialization stages and validates the full scene before replacing the destination. On failure the destination and its entity handles remain unchanged; on success, handles referring to the replaced scene state become invalid. Incompatible format changes require a new version and an explicit migration path.

`SaveToFile` writes a temporary sibling file and replaces the destination only after the complete document has been written. `LoadFromFile` leaves the destination scene unchanged if opening, reading, or parsing fails. File paths are supplied by the caller and are not stored in the scene document.
