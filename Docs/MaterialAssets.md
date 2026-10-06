# Material Assets

PulseForge material assets currently provide a small unlit base-color description for renderer validation. Each managed `.material` file has the usual adjacent `.meta` sidecar; scenes refer to the material by the sidecar UUID, not its path.

Version 1 documents use this shape:

```json
{
  "format": "PulseForgeMaterial",
  "version": 1,
  "baseColorTexture": "4c9b0a26-5ca1-4d37-b451-f23231002f92",
  "baseColorFactor": [1.0, 1.0, 1.0, 1.0]
}
```

`baseColorTexture` is a stable asset UUID resolved through the project registry. The factor is linear RGBA in the range `[0, 1]`. The current sample shader multiplies it by the sampled sRGB texture and vertex color; this is not a lighting or PBR material model.

Material references are optional on a scene mesh renderer for compatibility with older scenes. The sample validation scene assigns a material to each mesh instance. Material descriptions are cached for the lifetime of the cache and are reloaded after `Clear()`; hot reload and editor import settings are not implemented yet.
