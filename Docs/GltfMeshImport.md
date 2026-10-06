# Static glTF mesh import

`GltfMeshImporter::ImportStaticPrimitive` resolves a managed glTF asset by its sidecar UUID through `AssetRegistry`, then returns owned CPU vertex and index arrays that can be uploaded with `Application::CreateMesh`. Source paths are used only to locate the bytes after identity resolution.

The initial importer supports glTF 2.0 JSON (`.gltf`) and binary glTF (`.glb`), external relative buffers and base64 `application/octet-stream` / `application/gltf-buffer` data URIs. External buffers must remain under the project `Assets` directory and cannot traverse symbolic links. Each source file managed by the project, including external buffers, requires its own `.meta` sidecar.

This is intentionally a static triangle-geometry subset: dense float `POSITION`, optional dense float `TEXCOORD_0` and `COLOR_0`, and optional unsigned scalar indices. Missing UVs default to zero and missing colors to white. It imports the selected mesh primitive's local geometry; node transforms, materials, images, animation, morph targets, sparse accessors, normalized integer attributes, Draco compression, and meshopt compression are not imported. Assets declaring any required glTF extension are rejected because this importer implements none of them. Unsupported or malformed data returns a diagnostic rather than partially producing geometry.

Container files are limited to 512 MiB, JSON to 64 MiB, total decoded buffers to 512 MiB, vertices to 10 million, and indices to 30 million. These limits bound allocations for the current importer and can be revisited alongside a broader import pipeline.
