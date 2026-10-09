# Material Assets

PulseForge material assets provide an opaque metallic/roughness material description. Each managed `.material` file has the usual adjacent `.meta` sidecar; scenes refer to the material by the sidecar UUID, not its path.

Version 1 documents use this shape:

```json
{
  "format": "PulseForgeMaterial",
  "version": 1,
  "baseColorTexture": "4c9b0a26-5ca1-4d37-b451-f23231002f92",
  "baseColorFactor": [1.0, 1.0, 1.0, 1.0]
}
```

Version 2 adds linear scalar factors:

```json
{
  "format": "PulseForgeMaterial",
  "version": 2,
  "baseColorTexture": "4c9b0a26-5ca1-4d37-b451-f23231002f92",
  "baseColorFactor": [1.0, 1.0, 1.0, 1.0],
  "metallicFactor": 0.0,
  "roughnessFactor": 1.0
}
```

`baseColorTexture` is a stable asset UUID resolved through the project registry. The factor is linear RGBA in `[0, 1]`; metallic and roughness are finite linear factors in `[0, 1]`. Version 1 remains readable and maps to `metallicFactor: 0` and `roughnessFactor: 1`. Saving writes version 2. Base-color textures use an sRGB GPU format and are decoded by texture sampling before linear-light shading. Vertex color multiplies base color. Materials remain opaque; alpha is retained in the asset but does not enable blending. The renderer clamps roughness internally for stable BRDF evaluation without changing the authored value.

Material references are optional on a scene mesh renderer for compatibility with older scenes. Material descriptions are cached for the lifetime of the cache and are reloaded after `Clear()`; hot reload and editor import settings are not implemented yet. The renderer supports HDR image-based lighting and directional cascaded soft shadows. Metallic/roughness textures, normal maps, material occlusion maps, emissive, and transparency remain deferred.
