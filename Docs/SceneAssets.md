# Scene assets

`SceneAssetService` loads and saves managed `.scene` assets by sidecar UUID. It resolves the current project-relative path from `AssetRegistry`, so moving a scene and its `.meta` sidecar does not change its identity. The scene document continues to serialize entity UUIDs and asset references by UUID; it never stores its own source path.

The service accepts existing scene assets only. Creating a new scene asset and editor-facing save workflows can build on `AssetMetadataSerializer` and `AssetOperations` later. Managed source paths are checked by `AssetPathResolver`; it rejects traversal, missing entries, and symbolic links under `Assets`.
