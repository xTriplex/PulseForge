# Asset identity metadata, version 1

Managed source assets under a project's `Assets` directory have an adjacent sidecar named by appending `.meta` to the complete source filename. The `.meta` suffix is reserved for sidecars. For example, `Assets/Shaders/Triangle.hlsl` is identified by `Assets/Shaders/Triangle.hlsl.meta`.

```json
{
  "format": "PulseForgeAssetMeta",
  "version": 1,
  "uuid": "62cf4556-0d8c-4ac5-96f5-df2bc4a1f6eb",
  "importSettings": {}
}
```

The UUID is the asset identity. Asset paths may change; serialized references store the UUID, never the source path. Moving or renaming an asset must move or rename its sidecar with it. Copying an asset for duplication must generate a new sidecar UUID rather than copying the original sidecar.

`AssetRegistry` scans `<ProjectRoot>/Assets` and builds an in-memory UUID-to-project-relative-path index. The index is derived data and can be reconstructed from sidecars. A scan fails with diagnostics for missing, malformed, orphaned, or duplicate metadata and leaves the previous registry contents unchanged. It does not create or repair sidecars automatically; missing or damaged metadata must be resolved without silently changing an existing asset identity.

`AssetOperations` provides the first managed file operations for project-relative `Assets/...` paths. Moving an asset moves its UUID identity with the sidecar; duplicating it copies import settings but creates a new UUID; deleting it removes both source and sidecar. Operations reject invalid project state rather than hiding duplicate or missing metadata. Delete is permanent, so confirmation/undo policy belongs to the caller. Rebuild any in-memory `AssetRegistry` after a successful operation; the sidecars remain authoritative.

`AssetReferenceValidator` checks current scene mesh references against a rebuilt registry and returns structured entity/asset UUID diagnostics for unresolved references. It does not discard or rewrite missing references; scenes can still be loaded and repaired when assets are restored.

Symbolic links inside or in place of `Assets` are reported and not followed, keeping registry paths inside the project tree and avoiding platform-dependent traversal behavior.

Metadata versions are independent of scene-format versions. Version 1 requires an `importSettings` object, currently empty; future importers can add settings under that object while retaining the asset UUID. Unsupported metadata versions are reported rather than interpreted as version 1. Keep `.meta` files with their source assets in version control. Project-generated registry caches and imported data belong under `.pulseforge/cache/` and `.pulseforge/imported/` and are ignored by default.
