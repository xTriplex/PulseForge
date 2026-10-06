# Scene assets

`SceneAssetService` creates, loads, and saves managed `.scene` assets. `Create` serializes the supplied scene to a new project-relative path below `Assets`, generates a sidecar UUID, and refreshes the supplied registry. It refuses to overwrite an existing source or sidecar. `Load` and `Save` address existing scenes by sidecar UUID and resolve their current project-relative path from `AssetRegistry`, so moving a scene and its `.meta` sidecar does not change its identity. The scene document continues to serialize entity UUIDs and asset references by UUID; it never stores its own source path.

Managed source paths are checked by `AssetPathResolver` and the asset-operation layer; traversal and symbolic links under `Assets` are rejected. Editor-facing project and scene workflows can build on these services.
