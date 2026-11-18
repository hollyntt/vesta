# Hitsound assets

The embedded sounds in `hitsounds.hpp` are selected from **Kenney Interface
Sounds**, copyright Kenney (2020), dedicated to the public domain under the
Creative Commons CC0 1.0 Universal license.

Source: https://github.com/Calinou/kenney-interface-sounds

Selected originals:

- `confirmation_001.wav` (`soft`)
- `glass_002.wav` (`glass`)
- `pluck_001.wav` (`pluck`)
- `select_007.wav` (`crisp`)

The four Kenney sounds are embedded as their original PCM WAV bytes.

`flesh` is derived from **Gore (grapefruit) #6**, a grapefruit gore foley
recording published by LaSonotheque / BigSoundBank under CC0 (public domain).
It is cropped to the initial impact, normalized, faded out, and converted to
mono 44.1 kHz 16-bit PCM for low-latency hitsound playback.

Source: https://bigsoundbank.com/gore-grapefruit-6-s2600.html

The kill-sound bank in `killsounds.hpp` is a separate selection from the same
Kenney Interface Sounds pack, so a kill does not reuse a hit clip:

- `bong_001.wav` (`bell`)
- `close_002.wav` (`shut`)
- `maximize_008.wav` (`rise`)
- `error_008.wav` (`buzz`)
- `toggle_002.wav` (`snap`)
