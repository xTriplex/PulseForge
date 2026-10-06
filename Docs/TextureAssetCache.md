# Runtime texture asset cache

`TextureAssetCache` resolves image UUIDs through an `AssetRegistry`, decodes them through `ImageAssetImporter`, uploads an engine-owned texture, and reuses it for later requests. The requested RGBA8 format is part of the cache key, so the same source can be used as sRGB color or linear data without conflating GPU resources.

The cache is scoped to one project and renderer. Its `Application` and registry references must outlive it, and returned texture references are borrowed until `Clear()` or cache destruction. Call `Clear()` after changing source content or import settings. The cache is not internally synchronized.
