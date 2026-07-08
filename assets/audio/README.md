# Fishtank Audio Assets

`fishtank_ambience.webm` is the default quiet aquarium loop used by
`kilix-fishtank` in interactive mode. The `.wav` file is the same loop in an
uncompressed format for inspection or replacement.

Playback:

```sh
ffplay -hide_banner -nostats -loglevel quiet -nodisp -autoexit \
  -volume 18 -loop 0 assets/audio/fishtank_ambience.webm
```

Runtime controls:

```sh
KILIX_FISHTANK_NO_AUDIO=1 ./kilix-fishtank
KILIX_FISHTANK_AUDIO_VOLUME=12 ./kilix-fishtank
KILIX_FISHTANK_AUDIO=/path/to/loop.webm ./kilix-fishtank
```
