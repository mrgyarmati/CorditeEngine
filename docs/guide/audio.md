# Sound

Sounds are assets: WAV, FLAC and MP3 files in your project, read whole when they are first
played (up to ten minutes at 48 kHz). [miniaudio](https://miniaud.io) mixes and plays them.

## What an entity sounds like: AudioSource

An **AudioSource** component plays a `sound` from where its entity is:

- `playing` - set it to true to play the sound; the engine sets it back to false when a sound
  that does not `loop` has ended. A source that is `playing` when the scene starts plays
  from the start.
- `loop`, `volume`, `pitch` (2 is an octave up and twice as fast).
- `bus` - which volume control it answers to: `effects`, `music`, `ambience`, `voice` or
  `interface`.
- `spatial` - heard from where the entity is (on), or the same in both ears (off, for music
  and the interface).
- `rolloff` - how it gets quieter with distance: `inverse` halves at twice `minDistance`
  and never reaches silence; `linear` reaches silence at `maxDistance`.

## Who hears it: AudioListener

An **AudioListener** component - usually on the camera, or on the player - is where the
ears are. A source straight ahead is heard in both ears; one to the side mostly in that ear.
`volume` is the volume of everything.

## Playing sounds from scripts

What belongs to an entity is its AudioSource: `entity:set("AudioSource", "playing", true)`.
What belongs to no entity - an explosion where something hit, a click of the interface - is
played with `cordite.audio.play`:

```lua
cordite.audio.play("sounds/explosion.wav", { position = hit.position, volume = 0.8, pitch = 0.9 })
cordite.audio.setVolume("music", 0.5)
```

`play` returns nothing when every voice is busy (a game has 64): too much sound at once is
nothing to stop the game for.

## In the editor

Nothing of the scene is heard while you edit. A scene that is played is heard; pausing it
pauses its sounds, and stopping it stops them. To hear a sound file before you use it,
choose it in the Assets panel and press **Play** beside the list (`asset.listen` for agents):
it plays once, as it was made, also while the scene is not played. A batch run of the editor
and the tests mix sound into memory instead of a device, the same on every run.
