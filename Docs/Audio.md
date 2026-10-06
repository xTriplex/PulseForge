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
cache when project asset contents change. Scene audio components, streaming, voice management, and
editor controls are not part of this initial runtime API.

Use `AudioOutputBackend::Null` for headless tests. The default backend selects miniaudio's normal platform output device.
