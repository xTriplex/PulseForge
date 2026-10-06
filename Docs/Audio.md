# Audio

`AudioEngine` wraps miniaudio and owns the output device. Create clips from encoded audio bytes; each
playback gets an independent decoder, so one clip can be played more than once at different positions
or volumes. `AudioPlayback` is move-only and keeps its sound, clip data, and engine device alive until
it is destroyed.

`AudioPlayback::Pause()` stops without rewinding; `Resume()` continues from the current cursor. `Stop()`
rewinds to the beginning. Playback descriptions support volume, looping, and optional 3D positioning.
The engine currently exposes one listener position and direction.

`AudioAssetCache` resolves managed audio assets by their stable UUID through the project asset registry.
It retains encoded source data in memory and currently limits an audio source to 256 MiB. Clear the
cache when project asset contents change. Streaming and editor controls are not implemented yet.

`AudioSourceComponent` stores an asset UUID and playback settings; `AudioListenerComponent` marks a
primary listener. `AudioSceneRuntime` starts sources marked `PlayOnStart`, follows spatial source and
listener transforms on `Advance()`, and exposes play, pause, resume, and stop operations by entity UUID.
There may be at most one primary listener. Sources added after runtime start can be played explicitly;
removing an entity or its source releases its playback. The scene must outlive the runtime, and the
audio engine and asset cache must outlive it as well.

Use `AudioOutputBackend::Null` for headless tests. The default backend selects miniaudio's normal platform output device.
