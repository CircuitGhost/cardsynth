# CardSynth slice plan

Pocket chord synth for the [M5Stack Cardputer ADV](https://docs.m5stack.com/en/core/Cardputer-Adv) (ESP32-S3, ES8311 codec, speaker + 3.5 mm jack, 56-key keyboard).

Playing model follows Akai MPK Mini Chords mode: one key = one full chord in a key and scale.

## Locked product decisions

| Decision | Choice |
| --- | --- |
| Chord pads | Top row `1`–`8` (scale degrees I → I↑) |
| Inversions | `Fn` + pad (not Shift) |
| Polyphony | 4 voices |
| Audio start | Silent until a chord pad is pressed |
| Output | ES8311 → built-in speaker and 3.5 mm jack (jack mutes amp in hardware) |
| Voice | Editable pad synth (cutoff, reso, attack, release, detune, wave, volume) |

### Pad map (C major triad example)

| Key | Degree | Chord |
| --- | --- | --- |
| 1 | I | C |
| 2 | ii | Dm |
| 3 | iii | Em |
| 4 | IV | F |
| 5 | V | G |
| 6 | vi | Am |
| 7 | vii | Bdim |
| 8 | I↑ | C (octave up) |

### Chord types

- **Scale chords:** triad (1-3-5), +7, +7+9 — quality follows the scale
- **Locked chords:** Maj7, Min7, Dom7 — same quality on every degree; roots still from the scale

### Performance controls

- `-` / `=` — octave
- `[` / `]` or `Tab` — key
- `'` — scale
- `;` — chord type
- `/` — cycle edit parameter
- `z` `x` `c` `v` `b` `n` `m` — jump to cutoff / reso / attack / release / detune / wave / volume
- `,` / `.` — decrease / increase selected parameter

---

## Slice 1 — Chord engine

**Status:** done

- Key, scale, chord type, inversion
- Top-row pad → MIDI note list for the chord
- Spread voicing (root down an octave) for clarity on the small speaker
- Chord name + note list on the 240×135 display

## Slice 2 — Pad synth

**Status:** done

- 4-voice polyphonic saw/square/triangle pad through ES8311
- Speaker and headphone jack on the same codec path
- Continuous silent I2S stream when idle (no boot tone / no music until a pad)
- On-screen status: key, scale, type, octave, edit target

## Slice 3 — Feel & editability

**Status:** done

- Attack / release envelopes so chords can overlap on release within the 4-voice limit
- Live edit of filter, envelope, detune, wave, and volume from the keyboard
- ADV board detection shown at boot (`Ready ADV`)

---

## Later slices (not started)

Pick any of these when extending beyond the chord instrument:

| Slice | Idea |
| --- | --- |
| 4 | Melody layer on the letter rows (single notes alongside chord pads) |
| 5 | Saved presets (NVS or microSD) |
| 6 | BMI270 as filter / mod source |
| 7 | Bluetooth MIDI out to a DAW / soft synth |
| 8 | Mic sampling / one-shot pads |

---

## Build & flash

```bash
cd cardsynth
source .venv/bin/activate   # if using the project venv
pio run -t upload
```

Board in `platformio.ini`: `m5stack-stamps3` with M5Cardputer / M5Unified / M5GFX. Upload port defaults to `/dev/cu.usbmodem101` (adjust if needed).
