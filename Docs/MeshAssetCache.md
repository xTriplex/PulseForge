# Runtime mesh asset cache

`MeshAssetCache` resolves `MeshRendererComponent` asset UUIDs through an `AssetRegistry`, imports the current static glTF primitive, uploads it through `Application::CreateMesh`, and reuses the resulting engine-owned `Mesh` for later lookups. It currently loads the first mesh's first primitive; broader glTF scene/material import remains separate work.

The cache is scoped to one project and renderer. Its `Application` and registry references must outlive it, and returned mesh references are borrowed until `Clear()` or cache destruction. Call `Clear()` after changing source content or import settings when the cached GPU representation should be rebuilt. The cache is not internally synchronized.
