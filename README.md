# CardSynth

A 4-voice polyphonic synthesizer, chord machine, and wireless Bluetooth BLE-MIDI controller built for the **M5Stack Cardputer** (ESP32-S3).

---

## About

**CardSynth** transforms the M5Stack Cardputer into a standalone, portable pocket groove synthesizer and wireless MIDI controller. 

Designed for quick musical sketching and tactile live jamming, it pairs an MPK-style chord pad engine on the top row with a full 2-octave chromatic QWERTY keyboard for soloing. The audio engine runs at **44.1 kHz** using dual detuned PolyBLEP anti-aliased oscillators, a resonant low-pass filter with dedicated envelope modulation, a 4-stage ADSR amp envelope, an LFO matrix, and a multi-pattern arpeggiator/strummer.

When paired over Bluetooth, CardSynth acts as a wireless BLE-MIDI controller with real-time parameter CC automation for DAWs and mobile synths on Mac, iOS, iPad, Windows, and Android.

---

## Features

- **Dual Playing Modes**:
  - **Top Row (`1–8`)**: 8 diatonic chord pads (Triads, 7ths, 9ths) with instant `Fn` chord inversions.
  - **QWERTY Piano Roll**: 2-octave chromatic keyboard (`A–L` white keys, `W–P` black keys) for solo leads and basslines.
- **Sound Engine**:
  - 4-voice polyphony with dual detuned PolyBLEP anti-aliased oscillators + dedicated sub-oscillator.
  - 44.1 kHz 16-bit audio output with high-pass DC filtering (tuned for both headphones and the built-in speaker).
  - Resonant low-pass filter (Chamberlin SVF) with dedicated **VCF Filter Envelope** for plucks and brass swells.
  - Full **4-Stage ADSR** amplitude envelope (Attack, Decay, Sustain, Release).
  - **LFO Modulation Matrix** (routes to Filter Cutoff, Pitch Vibrato, or Volume Tremolo).
- **Performance Tools**:
  - **Strummer** (Up / Down with adjustable ms strum speed).
  - **Multi-Pattern Arpeggiator** (Up, Down, Up/Down, Random).
  - **Tap-Tempo** on `Spacebar`.
- **Wireless Bluetooth BLE MIDI**:
  - Pairs instantly with macOS, iOS (iPad/iPhone), Windows, and Android as a wireless MIDI controller.
  - Real-time MIDI CC parameter transmission (Cutoff CC#74, Resonance CC#71, Volume CC#7).
- **Display**:
  - Flicker-free 240x135 UI with real-time audio oscilloscope, animated filter response curve, and an 8-pad status grid.

---

## Keyboard Controls

```
[1] [2] [3] [4] [5] [6] [7] [8]  -> Chord Pads (Fn + 1-8 for inversions)
  [W] [E]     [T] [Y] [U]     [O] [P] [[] -> Black Keys (C#, D#, F#, G#, A#...)
[A] [S] [D] [F] [G] [H] [J] [K] [L] [;] ['] -> White Keys (C, D, E, F, G, A, B, C5...)
```

### Performance & Playing

| Key | Action |
|---|---|
| **`1` – `8`** | Trigger Chord Pads (scale degrees 1–8) |
| **`Fn` + `1` – `8`** | Trigger Inverted Chords |
| **`A` – `'`** | Play Solo White Keys (C through F5) |
| **`W` – `[`** | Play Solo Black Keys (C# through F#5) |
| **`Enter`** | Cycle Play Mode (`Poly` → `Strum ↑` → `Strum ↓` → `Arp ↑` → `Arp ↓` → `Arp ↑↓` → `Arp ?`) |
| **`Space`** | Tap-Tempo (automatically detects & sets BPM) |
| **`Tab`** | Transpose Root Key (C → C# → D → ...) |
| **`i` / `I`** | Cycle Inversion Step (1st, 2nd, 3rd) |
| **`z` / `x`** | Shift Solo Piano Octave Down / Up |

### Sound Design & Navigation

| Key | Action |
|---|---|
| **`c` / `C`** | Cycle Active Edit Parameter |
| **`v` / `b`** or **`,` / `.`** | Decrement / Increment Selected Parameter Value |
| **`n` / `N`** | Cycle Oscillator Waveform (`Saw` → `Square` → `Tri`) |
| **`m` / `M`** | Cycle LFO Target (`Off` → `Cutoff` → `Pitch` → `Volume`) |
| **`;` / `'`** | Cycle Chord Types (`Triad`, `+7`, `+7+9`, `Maj7`, `Min7`, `Dom7`) & Scales |
| **`-` / `=`** | Shift Chord Pad Octave Down / Up |

---

## Edit Parameters

| Parameter | Range | Description |
|---|---|---|
| **Cutoff** | 2% – 100% | Low-pass filter cutoff frequency (Transmits MIDI CC #74) |
| **Reso** | 0% – 95% | Filter resonance / Q peak (Transmits MIDI CC #71) |
| **F-Env** | 0% – 100% | Filter envelope modulation depth (snappy plucks to swells) |
| **F-Dec** | 10 – 2000 ms | Filter envelope decay time |
| **Attack** | 1 – 2000 ms | Amp attack time |
| **Decay** | 10 – 2000 ms | Amp decay time |
| **Sustain** | 0% – 100% | Amp sustain level |
| **Release** | 10 – 4000 ms | Amp release time |
| **LFO-Tgt** | Off / Cut / Pit / Vol | Modulation destination |
| **LFO-Rate** | 0.1 – 15.0 Hz | LFO speed |
| **LFO-Dpth** | 0% – 100% | LFO intensity |
| **Detune** | 0 – 40 cents | Oscillator 2 pitch offset |
| **Sub-Osc** | 0% – 100% | -1 octave sub-oscillator level |
| **Wave** | Saw / Square / Tri | Primary oscillator shape |
| **BPM** | 40 – 240 BPM | Arpeggiator clock speed |
| **Strum** | 10 – 100 ms | Note onset delay in strum mode |
| **Volume** | 0% – 100% | Master volume (Transmits MIDI CC #7) |

---

## Connecting via Bluetooth BLE MIDI

CardSynth advertises automatically as **`CardSynth MIDI`**.

- **Mac**: Open **Audio MIDI Setup** → **Window** → **MIDI Studio** → click the **Bluetooth** icon → click **Connect** next to `CardSynth MIDI`.
- **iOS / iPadOS**: Open GarageBand, AUM, or Koala → **Settings** → **Bluetooth MIDI Devices** → connect to `CardSynth MIDI`.
- **Windows**: Pair in Bluetooth Settings, then route via **MIDIberry** or your DAW's Bluetooth MIDI settings.

---

## Build & Flash

Built using [PlatformIO](https://platformio.org/).

1. Clone the repository:
   ```bash
   git clone https://github.com/CircuitGhost/cardsynth.git
   cd cardsynth
   ```

2. Compile and upload to your Cardputer:
   ```bash
   pio run -t upload
   ```

---

## License

MIT License

Copyright (c) 2026 CircuitGhost

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
