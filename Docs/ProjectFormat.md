# PulseForge Project Files

A project is opened from a `.pfproj` JSON document in its root directory. Version 1 stores a stable project UUID, display name, and optional startup-scene asset UUID:

```json
{
  "format": "PulseForgeProject",
  "version": 1,
  "projectId": "77e2f64b-3305-498e-a271-8b11f4c4ad22",
  "name": "My Game",
  "startScene": "b101a2e7-582d-4dfb-ae1e-8ce41fb375ce"
}
```

The startup scene is an asset UUID resolved through the rebuilt sidecar registry; no machine-specific or project-relative asset path is stored in the manifest. Projects conventionally contain an `Assets` directory. `Project::Create` creates it when needed, while `Project::Open` rejects a malformed manifest, an invalid asset registry, or a missing/wrong-type startup scene. Older readers must reject unknown project versions rather than guess their meaning.

`Project::Save` replaces the manifest through a temporary sibling file. `SetStartScene` validates the UUID against the current registry and restores the prior in-memory value if saving fails. Asset files and sidecar metadata remain the authoritative asset identity records.
