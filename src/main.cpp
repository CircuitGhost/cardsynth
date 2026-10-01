#include <M5Cardputer.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// CardSynth — 4-voice polyphonic pad synth, solo piano keyboard & wireless BLE MIDI controller for M5Cardputer.
// Features:
// 1. Dual Play Engine:
//    - Top row (1-8): 8 Smart Chord Pads with diatonic/locked harmony & Fn inversions.
//    - QWERTY Piano: 2-octave chromatic keyboard (A-S-D-F-G-H-J-K-L white keys, W-E-T-Y-U-O-P black keys).
// 2. Sound Design Engine:
//    - 4-Voice Polyphony with dual detuned PolyBLEP anti-aliased oscillators + Sub-Oscillator.
//    - Full 4-stage ADSR Amp Envelope (Attack, Decay, Sustain, Release).
//    - Dynamic Resonant Filter (VCF) with dedicated Filter Envelope Modulation.
//    - LFO Modulation Matrix (Cutoff Wah, Pitch Vibrato, Volume Tremolo).
// 3. Visual UI Overhaul (240x135 Double-Buffered M5Canvas):
//    - Real-Time Live Audio Oscilloscope.
//    - Animated Filter Response Curve with live VCF envelope sweep.
//    - 8-Pad Graphical Grid with active press & arpeggiator tracer.
// 4. Strummer & Multi-Pattern Arpeggiator with Tap-Tempo on Spacebar.
// 5. Wireless Bluetooth BLE MIDI Out with real-time CC automation.

static constexpr uint32_t kSampleRate = 44100;
static constexpr size_t kBufferFrames = 256;
static constexpr size_t kBufferCount = 4;
static constexpr int kMaxVoices = 4;
static constexpr int kMaxChordNotes = 4;
static constexpr int kPadCount = 8;

#define MIDI_SERVICE_UUID        "03b80e5a-ede8-4b33-a085-331652f1011c"
#define MIDI_CHARACTERISTIC_UUID "7772e5db-3868-4112-a1a9-f2669d106bf3"

static BLECharacteristic* pMidiCharacteristic = nullptr;
static volatile bool gBleConnected = false;

enum Waveform : uint8_t { WAVE_SAW = 0, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_COUNT };
enum ChordType : uint8_t {
  CHORD_TRIAD = 0,  // 1-3-5 in scale
  CHORD_ADD7,       // 1-3-5-7 in scale
  CHORD_ADD79,      // 1-3-5-7-9 (drop 5th for 4-voice jazz voicing)
  CHORD_MAJ7,       // locked Maj7
  CHORD_MIN7,       // locked Min7
  CHORD_DOM7,       // locked Dom7
  CHORD_TYPE_COUNT
};
enum ScaleType : uint8_t {
  SCALE_MAJOR = 0,
  SCALE_MINOR,
  SCALE_DORIAN,
  SCALE_MIXOLYDIAN,
  SCALE_PENT_MAJOR,
  SCALE_PENT_MINOR,
  SCALE_COUNT
};
enum PlayMode : uint8_t {
  PLAY_NORMAL = 0,
  PLAY_STRUM_UP,
  PLAY_STRUM_DN,
  PLAY_ARP_UP,
  PLAY_ARP_DN,
  PLAY_ARP_UPDN,
  PLAY_ARP_RAND,
  PLAY_MODE_COUNT
};
enum LfoTarget : uint8_t {
  LFO_OFF = 0,
  LFO_CUTOFF,
  LFO_PITCH,
  LFO_VOLUME,
  LFO_TARGET_COUNT
};
enum EditParam : uint8_t {
  EDIT_CUTOFF = 0,
  EDIT_RESO,
  EDIT_FILT_ENV,
  EDIT_FILT_DEC,
  EDIT_ATTACK,
  EDIT_DECAY,
  EDIT_SUSTAIN,
  EDIT_RELEASE,
  EDIT_LFO_TARGET,
  EDIT_LFO_RATE,
  EDIT_LFO_DEPTH,
  EDIT_DETUNE,
  EDIT_SUB,
  EDIT_WAVE,
  EDIT_BPM,
  EDIT_STRUM,
  EDIT_VOLUME,
  EDIT_COUNT
};

enum EnvStage : uint8_t {
  ENV_IDLE = 0,
  ENV_ATTACK,
  ENV_DECAY,
  ENV_SUSTAIN,
  ENV_RELEASE
};

struct Voice {
  volatile bool active = false;
  volatile bool gate = false;
  int midiNote = 60;
  EnvStage envStage = ENV_IDLE;
  float phase = 0.0f;
  float phase2 = 0.0f;
  float subPhase = 0.0f;
  float incr = 0.0f;
  float incr2 = 0.0f;
  float subIncr = 0.0f;
  float env = 0.0f;
  float fEnv = 0.0f;
  float velocity = 1.0f;
  uint32_t age = 0;
};

struct SynthParams {
  Waveform wave = WAVE_SAW;
  float cutoff = 0.65f;       // Base cutoff (0..1)
  float reso = 0.15f;         // Resonance (0..1)
  float filtEnv = 0.55f;      // Filter envelope modulation amount (0..1)
  float filtDecayMs = 280.0f; // Filter decay time (10..2000 ms)
  float attackMs = 20.0f;     // Amp Attack (1..2000 ms)
  float decayMs = 120.0f;     // Amp Decay (10..2000 ms)
  float sustain = 0.75f;      // Amp Sustain level (0..1)
  float releaseMs = 350.0f;   // Amp Release (10..4000 ms)
  float detuneCents = 6.0f;   // Dual-oscillator detune (0..40 cents)
  float subOsc = 0.25f;       // Sub-oscillator volume (0..1)
  LfoTarget lfoTarget = LFO_CUTOFF;
  float lfoRateHz = 2.5f;     // LFO Speed (0.1..15.0 Hz)
  float lfoDepth = 0.35f;     // LFO Intensity (0..1)
  float volume = 0.80f;       // Master volume (0..1)
};

static Voice gVoices[kMaxVoices];
static SynthParams gParams;
static portMUX_TYPE gAudioMux = portMUX_INITIALIZER_UNLOCKED;

static int16_t gAudioBuf[kBufferCount][kBufferFrames];
static int16_t gScopeBuf[kBufferFrames];
static volatile int gWriteBuf = 0;
static volatile bool gAudioRunning = false;
static TaskHandle_t gAudioTask = nullptr;

static M5Canvas gCanvas(&M5Cardputer.Display);

static uint8_t gRootNote = 0;    // 0=C .. 11=B
static ScaleType gScale = SCALE_MAJOR;
static ChordType gChordType = CHORD_TRIAD;
static PlayMode gPlayMode = PLAY_NORMAL;
static int gOctave = 3;          // MIDI octave for pad chords
static int gPianoOctave = 4;     // MIDI octave for solo QWERTY keyboard
static int gInversion = 1;       // 1st/2nd/3rd when Fn held
static EditParam gEdit = EDIT_CUTOFF;
static int gBpm = 120;           // Arpeggiator tempo (40..240 BPM)
static int gStrumSpeedMs = 35;   // Strum interval between notes (10..100 ms)

static char gLastChordName[24] = "Ready";
static char gLastNotes[32] = "";
static bool gUiDirty = true;
static bool gPadDown[kPadCount] = {};
static int gActivePad = -1;
static float gLiveFilterCutoffNorm = 0.65f;

// Strummer and Arpeggiator state
struct ScheduledNote {
  bool pending = false;
  uint32_t triggerTime = 0;
  int midi = 0;
  float velocity = 1.0f;
};
static ScheduledNote gStrumQueue[kMaxChordNotes];

static int gCurrentChordNotes[kMaxChordNotes] = {};
static int gCurrentChordCount = 0;
static uint32_t gLastArpStep = 0;
static int gArpIndex = 0;
static int gArpDir = 1;
static uint32_t gLastTapTime = 0;

// Piano key matrix tracking (QWERTY solo keys)
struct SoloKeyDef {
  char keyChar;
  int semitoneOffset; // Relative to gPianoOctave * 12 + gRootNote
};

static const SoloKeyDef kSoloKeys[] = {
  // White keys (Row 2)
  {'a', 0},   // C
  {'s', 2},   // D
  {'d', 4},   // E
  {'f', 5},   // F
  {'g', 7},   // G
  {'h', 9},   // A
  {'j', 11},  // B
  {'k', 12},  // C+1
  {'l', 14},  // D+1
  {';', 16},  // E+1
  {'\'', 17}, // F+1
  // Black keys (Row 1)
  {'w', 1},   // C#
  {'e', 3},   // D#
  {'t', 6},   // F#
  {'y', 8},   // G#
  {'u', 10},  // A#
  {'o', 13},  // C#+1
  {'p', 15},  // D#+1
  {'[', 18},  // F#+1
};
static constexpr size_t kSoloKeyCount = sizeof(kSoloKeys) / sizeof(kSoloKeys[0]);
static bool gSoloKeyDown[kSoloKeyCount] = {};

static const char* kNoteNames[] = {
  "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
static const char* kScaleNames[] = {
  "Major", "Minor", "Dorian", "Mixo", "MajPent", "MinPent"
};
static const char* kChordTypeNames[] = {
  "Triad", "+7", "+7+9", "Maj7", "Min7", "Dom7"
};
static const char* kWaveNames[] = { "Saw", "Square", "Tri" };
static const char* kPlayModeBadges[] = {
  "POLY", "STRUM+", "STRUM-", "ARP+", "ARP-", "ARP+-", "ARP?"
};
static const char* kLfoTargetNames[] = {
  "Off", "Cutoff", "Pitch", "Volume"
};
static const char* kEditNames[] = {
  "Cutoff", "Reso", "F-Env", "F-Dec", "Attack", "Decay", "Sustain", "Release",
  "LFO-Tgt", "LFO-Rate", "LFO-Dpth", "Detune", "Sub-Osc", "Wave", "BPM", "Strum", "Volume"
};

class BleServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) override {
    gBleConnected = true;
    gUiDirty = true;
  }
  void onDisconnect(BLEServer* pServer) override {
    gBleConnected = false;
    gUiDirty = true;
    BLEDevice::startAdvertising();
  }
};

static inline void midiSendNoteOn(uint8_t note, uint8_t vel = 100) {
  if (gBleConnected && pMidiCharacteristic) {
    uint32_t now = millis();
    uint8_t packet[5] = {
      (uint8_t)(0x80 | ((now >> 7) & 0x3F)),
      (uint8_t)(0x80 | (now & 0x7F)),
      0x90, // Note On channel 1
      note,
      vel
    };
    pMidiCharacteristic->setValue(packet, 5);
    pMidiCharacteristic->notify();
  }
}

static inline void midiSendNoteOff(uint8_t note) {
  if (gBleConnected && pMidiCharacteristic) {
    uint32_t now = millis();
    uint8_t packet[5] = {
      (uint8_t)(0x80 | ((now >> 7) & 0x3F)),
      (uint8_t)(0x80 | (now & 0x7F)),
      0x80, // Note Off channel 1
      note,
      0
    };
    pMidiCharacteristic->setValue(packet, 5);
    pMidiCharacteristic->notify();
  }
}

static inline void midiSendCC(uint8_t cc, uint8_t val) {
  if (gBleConnected && pMidiCharacteristic) {
    uint32_t now = millis();
    uint8_t packet[5] = {
      (uint8_t)(0x80 | ((now >> 7) & 0x3F)),
      (uint8_t)(0x80 | (now & 0x7F)),
      0xB0, // Control Change channel 1
      cc,
      val
    };
    pMidiCharacteristic->setValue(packet, 5);
    pMidiCharacteristic->notify();
  }
}

static inline const char* getNoteName(int midi) {
  int idx = ((midi % 12) + 12) % 12;
  return kNoteNames[idx];
}

static inline float softClip(float x) {
  if (x >= 1.5f) return 1.0f;
  if (x <= -1.5f) return -1.0f;
  return x - (x * x * x) * (1.0f / 3.0f);
}

// PolyBLEP anti-aliasing to remove digital edge harshness
static inline float polyBlep(float t, float dt) {
  if (t < dt) {
    t /= dt;
    return t + t - t * t - 1.0f;
  } else if (t > 1.0f - dt) {
    t = (t - 1.0f) / dt;
    return t * t + t + t + 1.0f;
  }
  return 0.0f;
}

static inline float oscSample(float phase, float dt, Waveform w) {
  switch (w) {
    case WAVE_SQUARE: {
      float naive = phase < 0.5f ? 1.0f : -1.0f;
      naive += polyBlep(phase, dt);
      float p2 = phase + 0.5f;
      if (p2 >= 1.0f) p2 -= 1.0f;
      naive -= polyBlep(p2, dt);
      return naive;
    }
    case WAVE_TRIANGLE:
      return 1.0f - 4.0f * fabsf(phase - 0.5f);
    case WAVE_SAW:
    default: {
      float naive = 2.0f * phase - 1.0f;
      naive -= polyBlep(phase, dt);
      return naive;
    }
  }
}

static const int8_t kScaleIntervals[SCALE_COUNT][8] = {
  {0, 2, 4, 5, 7, 9, 11, -1},  // major
  {0, 2, 3, 5, 7, 8, 10, -1},  // natural minor
  {0, 2, 3, 5, 7, 9, 10, -1},  // dorian
  {0, 2, 4, 5, 7, 9, 10, -1},  // mixolydian
  {0, 2, 4, 7, 9, -1},         // major pent
  {0, 3, 5, 7, 10, -1},        // minor pent
};

static int scaleLength(ScaleType s) {
  int n = 0;
  while (kScaleIntervals[s][n] >= 0) n++;
  return n;
}

static int degreeToMidi(int degree, int octaveOffset = 0) {
  const int len = scaleLength(gScale);
  int oct = gOctave + octaveOffset;
  int d = degree;
  while (d < 0) {
    d += len;
    oct--;
  }
  while (d >= len) {
    d -= len;
    oct++;
  }
  const int midi = 12 * (oct + 1) + gRootNote + kScaleIntervals[gScale][d];
  return midi;
}

static float midiToHz(int midi) {
  return 440.0f * powf(2.0f, (midi - 69) / 12.0f);
}

static float clampf(float v, float lo, float hi) {
  if (v < lo) return lo;
  if (v > hi) return hi;
  return v;
}

static void buildChordNotes(int degree, bool invert, int* outNotes, int* outCount, char* nameOut, size_t nameLen) {
  int notes[5];
  int count = 0;
  const int rootMidi = degreeToMidi(degree);

  auto push = [&](int midi) {
    if (count < 5) notes[count++] = midi;
  };

  const bool locked = (gChordType == CHORD_MAJ7 || gChordType == CHORD_MIN7 || gChordType == CHORD_DOM7);

  if (!locked) {
    push(degreeToMidi(degree));
    push(degreeToMidi(degree + 2));
    push(degreeToMidi(degree + 4));
    if (gChordType == CHORD_ADD7 || gChordType == CHORD_ADD79) {
      push(degreeToMidi(degree + 6));
    }
    if (gChordType == CHORD_ADD79) {
      if (count == 5) {
        notes[2] = notes[3];
        notes[3] = notes[4];
        count = 4;
      }
    }
  } else {
    int third = 4, seventh = 11;
    if (gChordType == CHORD_MIN7) {
      third = 3;
      seventh = 10;
    } else if (gChordType == CHORD_DOM7) {
      third = 4;
      seventh = 10;
    } else {  // Maj7
      third = 4;
      seventh = 11;
    }
    push(rootMidi);
    push(rootMidi + third);
    push(rootMidi + 7);
    push(rootMidi + seventh);
  }

  // Inversion: rotate bottom notes up an octave
  int invSteps = invert ? gInversion : 0;
  if (invSteps > count - 1) invSteps = count - 1;
  for (int i = 0; i < invSteps; i++) {
    const int n = notes[0];
    for (int j = 0; j < count - 1; j++) notes[j] = notes[j + 1];
    notes[count - 1] = n + 12;
  }

  if (count > kMaxChordNotes) count = kMaxChordNotes;
  *outCount = count;
  for (int i = 0; i < count; i++) outNotes[i] = notes[i];

  static const char* quality[] = {"", "7", "9", "maj7", "m7", "7"};
  const char* q = quality[gChordType];
  if (!locked) {
    const int thirdInterval = (degreeToMidi(degree + 2) - rootMidi + 120) % 12;
    if (gChordType == CHORD_TRIAD) {
      q = (thirdInterval == 3) ? "m" : "";
      if ((degreeToMidi(degree + 4) - rootMidi + 120) % 12 == 6) q = "dim";
    } else if (gChordType == CHORD_ADD7) {
      q = (thirdInterval == 3) ? "m7" : "7";
    } else if (gChordType == CHORD_ADD79) {
      q = (thirdInterval == 3) ? "m9" : "9";
    }
  }
  snprintf(nameOut, nameLen, "%s%s%s", getNoteName(rootMidi), q, invert ? " inv" : "");
}

static int allocVoice() {
  int best = -1;
  uint32_t bestAge = 0;
  for (int i = 0; i < kMaxVoices; i++) {
    if (!gVoices[i].active) return i;
    if (gVoices[i].age >= bestAge) {
      bestAge = gVoices[i].age;
      best = i;
    }
  }
  return best;
}

static void noteOn(int midi, float velocity = 1.0f) {
  const float hz = midiToHz(midi);
  const float det = powf(2.0f, gParams.detuneCents / 1200.0f);
  portENTER_CRITICAL(&gAudioMux);
  const int idx = allocVoice();
  Voice& v = gVoices[idx];
  for (int i = 0; i < kMaxVoices; i++) {
    if (gVoices[i].active) gVoices[i].age++;
  }
  v.active = true;
  v.gate = true;
  v.midiNote = midi;
  v.envStage = ENV_ATTACK;
  v.phase = (float)(rand() % 1000) * 0.001f;
  v.phase2 = fmodf(v.phase + 0.37f, 1.0f);
  v.subPhase = fmodf(v.phase + 0.5f, 1.0f);
  v.incr = hz / kSampleRate;
  v.incr2 = (hz * det) / kSampleRate;
  v.subIncr = (hz * 0.5f) / kSampleRate; // 1 octave below
  v.env = 0.0f;
  v.fEnv = 1.0f; // Trigger filter envelope
  v.velocity = velocity;
  v.age = 0;
  portEXIT_CRITICAL(&gAudioMux);

  midiSendNoteOn((uint8_t)midi, (uint8_t)(velocity * 127.0f));
}

static void noteOff(int midi) {
  portENTER_CRITICAL(&gAudioMux);
  for (int i = 0; i < kMaxVoices; i++) {
    if (gVoices[i].active && gVoices[i].midiNote == midi) {
      gVoices[i].gate = false;
      gVoices[i].envStage = ENV_RELEASE;
      midiSendNoteOff((uint8_t)midi);
    }
  }
  portEXIT_CRITICAL(&gAudioMux);
}

static void allNotesOff() {
  portENTER_CRITICAL(&gAudioMux);
  for (int i = 0; i < kMaxVoices; i++) {
    if (gVoices[i].active) {
      midiSendNoteOff((uint8_t)gVoices[i].midiNote);
    }
    gVoices[i].gate = false;
    gVoices[i].envStage = ENV_RELEASE;
  }
  portEXIT_CRITICAL(&gAudioMux);
}

static void clearStrumQueue() {
  for (int i = 0; i < kMaxChordNotes; i++) {
    gStrumQueue[i].pending = false;
  }
}

static void playChordPad(int padIndex, bool invert) {
  int notes[kMaxChordNotes];
  int count = 0;
  char name[24];
  buildChordNotes(padIndex, invert, notes, &count, name, sizeof(name));

  gCurrentChordCount = count;
  for (int i = 0; i < count; i++) {
    gCurrentChordNotes[i] = notes[i];
  }

  allNotesOff();
  clearStrumQueue();

  if (gPlayMode == PLAY_NORMAL) {
    for (int i = 0; i < count; i++) {
      noteOn(notes[i], 1.0f - i * 0.03f);
    }
  } else if (gPlayMode == PLAY_STRUM_UP) {
    uint32_t now = millis();
    for (int i = 0; i < count; i++) {
      gStrumQueue[i].pending = true;
      gStrumQueue[i].triggerTime = now + (uint32_t)(i * gStrumSpeedMs);
      gStrumQueue[i].midi = notes[i];
      gStrumQueue[i].velocity = 1.0f - i * 0.03f;
    }
  } else if (gPlayMode == PLAY_STRUM_DN) {
    uint32_t now = millis();
    for (int i = 0; i < count; i++) {
      int noteIdx = count - 1 - i;
      gStrumQueue[i].pending = true;
      gStrumQueue[i].triggerTime = now + (uint32_t)(i * gStrumSpeedMs);
      gStrumQueue[i].midi = notes[noteIdx];
      gStrumQueue[i].velocity = 1.0f - i * 0.03f;
    }
  } else {
    // Arpeggiator mode
    gArpIndex = (gPlayMode == PLAY_ARP_DN) ? (count - 1) : 0;
    gArpDir = 1;
    gLastArpStep = millis();
    noteOn(gCurrentChordNotes[gArpIndex], 1.0f);
  }

  strncpy(gLastChordName, name, sizeof(gLastChordName) - 1);
  gLastChordName[sizeof(gLastChordName) - 1] = 0;

  gLastNotes[0] = 0;
  for (int i = 0; i < count; i++) {
    char piece[8];
    snprintf(piece, sizeof(piece), "%s%s", i ? " " : "", getNoteName(notes[i]));
    strncat(gLastNotes, piece, sizeof(gLastNotes) - strlen(gLastNotes) - 1);
  }
  gActivePad = padIndex;
  gUiDirty = true;
}

static void updateStrumQueue() {
  uint32_t now = millis();
  for (int i = 0; i < kMaxChordNotes; i++) {
    if (gStrumQueue[i].pending && now >= gStrumQueue[i].triggerTime) {
      gStrumQueue[i].pending = false;
      noteOn(gStrumQueue[i].midi, gStrumQueue[i].velocity);
    }
  }
}

static void updateArpeggiator() {
  if (gActivePad < 0 || gPlayMode < PLAY_ARP_UP || gCurrentChordCount == 0) return;

  uint32_t stepIntervalMs = 15000 / (uint32_t)gBpm; // 1/16th note rate
  uint32_t now = millis();
  if (now - gLastArpStep >= stepIntervalMs) {
    gLastArpStep = now;

    if (gPlayMode == PLAY_ARP_UP) {
      gArpIndex = (gArpIndex + 1) % gCurrentChordCount;
    } else if (gPlayMode == PLAY_ARP_DN) {
      gArpIndex = (gArpIndex + gCurrentChordCount - 1) % gCurrentChordCount;
    } else if (gPlayMode == PLAY_ARP_UPDN) {
      if (gCurrentChordCount > 1) {
        gArpIndex += gArpDir;
        if (gArpIndex >= gCurrentChordCount - 1) {
          gArpIndex = gCurrentChordCount - 1;
          gArpDir = -1;
        } else if (gArpIndex <= 0) {
          gArpIndex = 0;
          gArpDir = 1;
        }
      }
    } else if (gPlayMode == PLAY_ARP_RAND) {
      gArpIndex = rand() % gCurrentChordCount;
    }

    allNotesOff();
    noteOn(gCurrentChordNotes[gArpIndex], 1.0f);
  }
}

static bool anyVoiceSounding() {
  for (int i = 0; i < kMaxVoices; i++) {
    if (gVoices[i].active) return true;
  }
  return false;
}

static void renderBlock(int16_t* out, size_t frames) {
  const float attackInc = 1.0f / fmaxf(1.0f, gParams.attackMs * 0.001f * kSampleRate);
  const float decayInc = 1.0f / fmaxf(1.0f, gParams.decayMs * 0.001f * kSampleRate);
  const float releaseInc = 1.0f / fmaxf(1.0f, gParams.releaseMs * 0.001f * kSampleRate);
  const float filtDecInc = 1.0f / fmaxf(1.0f, gParams.filtDecayMs * 0.001f * kSampleRate);
  const float lfoIncr = gParams.lfoRateHz / kSampleRate;

  static float lp = 0.0f;
  static float bp = 0.0f;
  static float hp_x = 0.0f;
  static float hp_y = 0.0f;
  static float lfoPhase = 0.0f;

  const float wave = gParams.wave;
  const float hp_r = 0.985f; // ~105 Hz highpass roll-off

  for (size_t n = 0; n < frames; n++) {
    float mix = 0.0f;
    float maxFenv = 0.0f;

    // Advance LFO
    lfoPhase += lfoIncr;
    if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
    float lfoVal = (lfoPhase < 0.5f) ? (4.0f * lfoPhase - 1.0f) : (3.0f - 4.0f * lfoPhase); // Triangle LFO (-1..+1)

    float pitchMod = 1.0f;
    if (gParams.lfoTarget == LFO_PITCH) {
      pitchMod = powf(2.0f, (lfoVal * gParams.lfoDepth * 35.0f) / 1200.0f);
    }

    for (int i = 0; i < kMaxVoices; i++) {
      Voice& v = gVoices[i];
      if (!v.active) continue;

      // Full 4-Stage ADSR Engine
      if (v.gate) {
        if (v.envStage == ENV_ATTACK) {
          v.env += attackInc;
          if (v.env >= 1.0f) {
            v.env = 1.0f;
            v.envStage = ENV_DECAY;
          }
        } else if (v.envStage == ENV_DECAY) {
          v.env -= decayInc;
          if (v.env <= gParams.sustain) {
            v.env = gParams.sustain;
            v.envStage = ENV_SUSTAIN;
          }
        }
      } else {
        v.env -= releaseInc;
        if (v.env <= 0.0f) {
          v.env = 0.0f;
          v.active = false;
          v.envStage = ENV_IDLE;
          continue;
        }
      }

      // Filter envelope decay
      v.fEnv -= filtDecInc;
      if (v.fEnv < 0.0f) v.fEnv = 0.0f;
      if (v.fEnv > maxFenv) maxFenv = v.fEnv;

      // Anti-aliased dual-oscillator voice + Sub-Oscillator
      float s = oscSample(v.phase, v.incr * pitchMod, (Waveform)wave);
      s += oscSample(v.phase2, v.incr2 * pitchMod, (Waveform)wave) * 0.85f;
      // Warm Sub-Oscillator (Square wave 1 octave down)
      s += (v.subPhase < 0.5f ? 1.0f : -1.0f) * gParams.subOsc * 0.6f;

      mix += s * 0.22f * v.env * v.velocity;

      v.phase += v.incr * pitchMod;
      if (v.phase >= 1.0f) v.phase -= 1.0f;
      v.phase2 += v.incr2 * pitchMod;
      if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
      v.subPhase += v.subIncr * pitchMod;
      if (v.subPhase >= 1.0f) v.subPhase -= 1.0f;
    }

    // Dynamic Filter Envelope & LFO Cutoff Modulation
    float dynCutoff = gParams.cutoff + maxFenv * gParams.filtEnv * 0.40f;
    if (gParams.lfoTarget == LFO_CUTOFF) {
      dynCutoff += lfoVal * gParams.lfoDepth * 0.25f;
    }
    dynCutoff = clampf(dynCutoff, 0.02f, 1.0f);
    gLiveFilterCutoffNorm = dynCutoff;

    float cutoffHz = 180.0f * powf(80.0f, dynCutoff);
    float f = clampf(2.0f * sinf(3.14159265f * cutoffHz / (kSampleRate * 2.0f)), 0.01f, 0.70f);
    float q = clampf(1.0f - gParams.reso * 0.85f, 0.15f, 1.0f);

    // 2x oversampled Chamberlin filter
    for (int step = 0; step < 2; step++) {
      lp += f * (mix - lp - q * bp);
      bp += f * (lp - bp);
    }

    // Highpass DC & sub-bass roll-off (~105 Hz)
    float hp_in = lp;
    hp_y = hp_in - hp_x + hp_r * hp_y;
    hp_x = hp_in;

    // Master volume & Tremolo LFO
    float master = gParams.volume * 0.80f;
    if (gParams.lfoTarget == LFO_VOLUME) {
      master *= clampf(1.0f + lfoVal * gParams.lfoDepth * 0.5f, 0.0f, 1.0f);
    }

    // Warm analog saturation soft-clipper
    float sample = softClip(hp_y * master);
    out[n] = (int16_t)(sample * 28000.0f);
    gScopeBuf[n] = out[n];
  }
}

static void audioTask(void*) {
  while (true) {
    const int idx = gWriteBuf;
    if (anyVoiceSounding()) {
      gAudioRunning = true;
      renderBlock(gAudioBuf[idx], kBufferFrames);
    } else {
      gAudioRunning = false;
      memset(gAudioBuf[idx], 0, sizeof(gAudioBuf[idx]));
      memset(gScopeBuf, 0, sizeof(gScopeBuf));
    }

    while (!M5Cardputer.Speaker.playRaw(
             gAudioBuf[idx], kBufferFrames, kSampleRate, false, 1, 0, false)) {
      vTaskDelay(1);
    }
    gWriteBuf = (gWriteBuf + 1) % kBufferCount;
  }
}

static void drawUi() {
  auto& d = gCanvas;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(top_left);

  // --- Top Header ---
  d.setTextColor(TFT_ORANGE);
  d.setTextSize(1);
  d.drawString("CardSynth", 4, 2);

  d.setTextColor(gBleConnected ? TFT_GREEN : TFT_DARKGREY);
  d.drawString(gBleConnected ? "BLE:ON" : "BLE:OFF", 70, 2);

  d.setTextColor(gPlayMode == PLAY_NORMAL ? TFT_DARKGREY : TFT_CYAN);
  char modeBadge[20];
  snprintf(modeBadge, sizeof(modeBadge), "[%s %d]", kPlayModeBadges[gPlayMode], gBpm);
  d.drawString(modeBadge, 126, 2);

  d.setTextColor(TFT_LIGHTGREY);
  char pianoOctStr[12];
  snprintf(pianoOctStr, sizeof(pianoOctStr), "P:C%d", gPianoOctave);
  d.drawString(pianoOctStr, 204, 2);

  // --- Left Side: Chord / Solo Note Info ---
  d.setTextColor(TFT_WHITE);
  d.setTextSize(2);
  d.drawString(gLastChordName, 4, 15);

  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(gLastNotes, 4, 34);

  char line[56];
  snprintf(line, sizeof(line), "%s %s | oct%d | inv%d",
           getNoteName(gRootNote), kScaleNames[gScale], gOctave, gInversion);
  d.setTextColor(TFT_CYAN);
  d.drawString(line, 4, 46);

  // Edit Parameter line
  snprintf(line, sizeof(line), "Edit %s", kEditNames[gEdit]);
  d.setTextColor(TFT_YELLOW);
  d.drawString(line, 4, 59);

  switch (gEdit) {
    case EDIT_CUTOFF:
      snprintf(line, sizeof(line), "%.0f%% [CC#74]", gParams.cutoff * 100.0f);
      break;
    case EDIT_RESO:
      snprintf(line, sizeof(line), "%.0f%% [CC#71]", gParams.reso * 100.0f);
      break;
    case EDIT_FILT_ENV:
      snprintf(line, sizeof(line), "%.0f%% (VCF Mod)", gParams.filtEnv * 100.0f);
      break;
    case EDIT_FILT_DEC:
      snprintf(line, sizeof(line), "%.0f ms (F-Dec)", gParams.filtDecayMs);
      break;
    case EDIT_ATTACK:
      snprintf(line, sizeof(line), "%.0f ms (Atk)", gParams.attackMs);
      break;
    case EDIT_DECAY:
      snprintf(line, sizeof(line), "%.0f ms (Dec)", gParams.decayMs);
      break;
    case EDIT_SUSTAIN:
      snprintf(line, sizeof(line), "%.0f%% (Sus)", gParams.sustain * 100.0f);
      break;
    case EDIT_RELEASE:
      snprintf(line, sizeof(line), "%.0f ms (Rel)", gParams.releaseMs);
      break;
    case EDIT_LFO_TARGET:
      snprintf(line, sizeof(line), "%s", kLfoTargetNames[gParams.lfoTarget]);
      break;
    case EDIT_LFO_RATE:
      snprintf(line, sizeof(line), "%.1f Hz", gParams.lfoRateHz);
      break;
    case EDIT_LFO_DEPTH:
      snprintf(line, sizeof(line), "%.0f%%", gParams.lfoDepth * 100.0f);
      break;
    case EDIT_DETUNE:
      snprintf(line, sizeof(line), "%.1f ct", gParams.detuneCents);
      break;
    case EDIT_SUB:
      snprintf(line, sizeof(line), "%.0f%% (Sub-Osc)", gParams.subOsc * 100.0f);
      break;
    case EDIT_WAVE:
      snprintf(line, sizeof(line), "%s", kWaveNames[gParams.wave]);
      break;
    case EDIT_BPM:
      snprintf(line, sizeof(line), "%d BPM", gBpm);
      break;
    case EDIT_STRUM:
      snprintf(line, sizeof(line), "%d ms", gStrumSpeedMs);
      break;
    case EDIT_VOLUME:
      snprintf(line, sizeof(line), "%.0f%% [CC#7]", gParams.volume * 100.0f);
      break;
    default:
      line[0] = 0;
      break;
  }
  d.setTextColor(TFT_WHITE);
  d.drawString(line, 4, 71);

  // --- Right Side: Real-Time Live Oscilloscope ---
  const int scopeX = 144;
  const int scopeY = 15;
  const int scopeW = 92;
  const int scopeH = 46;
  d.drawRect(scopeX, scopeY, scopeW, scopeH, 0x2124); // Dark border
  d.fillRect(scopeX + 1, scopeY + 1, scopeW - 2, scopeH - 2, 0x0821); // Dark navy background
  d.drawFastHLine(scopeX + 1, scopeY + scopeH / 2, scopeW - 2, 0x18C3); // Center grid line

  // Plot live waveform trace
  int lastPy = scopeY + scopeH / 2;
  for (int x = 0; x < scopeW - 2; x++) {
    int sIdx = (x * kBufferFrames) / (scopeW - 2);
    int sample = gScopeBuf[sIdx];
    int py = (scopeY + scopeH / 2) - (sample * (scopeH / 2 - 2)) / 30000;
    py = (int)clampf((float)py, (float)(scopeY + 2), (float)(scopeY + scopeH - 3));
    if (x > 0) {
      d.drawLine(scopeX + x, lastPy, scopeX + 1 + x, py, TFT_GREEN);
    }
    lastPy = py;
  }

  // --- Right Side Lower: Animated Filter Curve Visualizer ---
  const int fltX = 144;
  const int fltY = 64;
  const int fltW = 92;
  const int fltH = 22;
  d.drawRect(fltX, fltY, fltW, fltH, 0x2124);
  d.fillRect(fltX + 1, fltY + 1, fltW - 2, fltH - 2, 0x0800);

  // Draw filter curve
  int cutPx = (int)(gLiveFilterCutoffNorm * (fltW - 6));
  for (int x = 0; x < fltW - 2; x++) {
    float normX = (float)x / (fltW - 2);
    float gain = 1.0f;
    if (normX > gLiveFilterCutoffNorm) {
      float delta = normX - gLiveFilterCutoffNorm;
      gain = expf(-delta * (5.0f + gParams.reso * 8.0f));
    } else if (fabsf(normX - gLiveFilterCutoffNorm) < 0.1f) {
      gain += gParams.reso * 0.8f * (1.0f - fabsf(normX - gLiveFilterCutoffNorm) / 0.1f);
    }
    int cy = (fltY + fltH - 2) - (int)(gain * (fltH - 4));
    cy = (int)clampf((float)cy, (float)(fltY + 2), (float)(fltY + fltH - 2));
    d.drawPixel(fltX + 1 + x, cy, TFT_CYAN);
  }
  // Cutoff dot indicator
  d.fillCircle(fltX + 3 + cutPx, fltY + 8, 2, TFT_YELLOW);

  // --- Bottom: 8-Pad Graphical Grid Visualizer ---
  const int padY = 89;
  const int padH = 30;
  const int padW = 27;
  for (int i = 0; i < kPadCount; i++) {
    int px = 4 + i * 29;
    bool isDown = gPadDown[i];
    bool isArpStep = (gPlayMode >= PLAY_ARP_UP && gActivePad == i);

    uint16_t padBorder = isDown ? TFT_ORANGE : (isArpStep ? TFT_CYAN : 0x4208);
    uint16_t padBg = isDown ? TFT_ORANGE : 0x10A2;
    d.fillRect(px, padY, padW, padH, padBg);
    d.drawRect(px, padY, padW, padH, padBorder);

    // Number & chord label
    char pNum[4];
    snprintf(pNum, sizeof(pNum), "%d", i + 1);
    d.setTextColor(isDown ? TFT_BLACK : TFT_DARKGREY);
    d.drawString(pNum, px + 2, padY + 2);

    int chordNotes[kMaxChordNotes];
    int cCount = 0;
    char cName[16];
    buildChordNotes(i, false, chordNotes, &cCount, cName, sizeof(cName));
    cName[4] = 0; // Shorten
    d.setTextColor(isDown ? TFT_BLACK : TFT_WHITE);
    d.drawString(cName, px + 2, padY + 16);
  }

  // --- Footer Help ---
  d.setTextColor(TFT_DARKGREY);
  d.drawString("1-8:chord  QWERTY:solo  spc:tap  tab:key  ent:mode", 4, 124);

  gCanvas.pushSprite(0, 0);
}

static void nudgeEdit(int dir) {
  switch (gEdit) {
    case EDIT_CUTOFF:
      gParams.cutoff = clampf(gParams.cutoff + dir * 0.03f, 0.02f, 1.0f);
      midiSendCC(74, (uint8_t)(gParams.cutoff * 127.0f));
      break;
    case EDIT_RESO:
      gParams.reso = clampf(gParams.reso + dir * 0.03f, 0.0f, 0.95f);
      midiSendCC(71, (uint8_t)(gParams.reso * 127.0f));
      break;
    case EDIT_FILT_ENV:
      gParams.filtEnv = clampf(gParams.filtEnv + dir * 0.05f, 0.0f, 1.0f);
      break;
    case EDIT_FILT_DEC:
      gParams.filtDecayMs = clampf(gParams.filtDecayMs + dir * 20.0f, 10.0f, 2000.0f);
      break;
    case EDIT_ATTACK:
      gParams.attackMs = clampf(gParams.attackMs + dir * 5.0f, 1.0f, 2000.0f);
      break;
    case EDIT_DECAY:
      gParams.decayMs = clampf(gParams.decayMs + dir * 10.0f, 10.0f, 2000.0f);
      break;
    case EDIT_SUSTAIN:
      gParams.sustain = clampf(gParams.sustain + dir * 0.05f, 0.0f, 1.0f);
      break;
    case EDIT_RELEASE:
      gParams.releaseMs = clampf(gParams.releaseMs + dir * 20.0f, 10.0f, 4000.0f);
      break;
    case EDIT_LFO_TARGET:
      gParams.lfoTarget = (LfoTarget)((gParams.lfoTarget + dir + LFO_TARGET_COUNT) % LFO_TARGET_COUNT);
      break;
    case EDIT_LFO_RATE:
      gParams.lfoRateHz = clampf(gParams.lfoRateHz + dir * 0.2f, 0.1f, 15.0f);
      break;
    case EDIT_LFO_DEPTH:
      gParams.lfoDepth = clampf(gParams.lfoDepth + dir * 0.05f, 0.0f, 1.0f);
      break;
    case EDIT_DETUNE:
      gParams.detuneCents = clampf(gParams.detuneCents + dir * 1.0f, 0.0f, 40.0f);
      break;
    case EDIT_SUB:
      gParams.subOsc = clampf(gParams.subOsc + dir * 0.05f, 0.0f, 1.0f);
      break;
    case EDIT_WAVE:
      gParams.wave = (Waveform)((gParams.wave + dir + WAVE_COUNT) % WAVE_COUNT);
      break;
    case EDIT_BPM:
      gBpm = (int)clampf((float)(gBpm + dir * 4), 40.0f, 240.0f);
      break;
    case EDIT_STRUM:
      gStrumSpeedMs = (int)clampf((float)(gStrumSpeedMs + dir * 5), 10.0f, 100.0f);
      break;
    case EDIT_VOLUME: {
      gParams.volume = clampf(gParams.volume + dir * 0.04f, 0.0f, 1.0f);
      M5Cardputer.Speaker.setVolume((uint8_t)(gParams.volume * 200.0f));
      midiSendCC(7, (uint8_t)(gParams.volume * 127.0f));
      break;
    }
    default:
      break;
  }
  gUiDirty = true;
}

static void handleKeyboard() {
  if (!M5Cardputer.Keyboard.isChange()) return;

  auto st = M5Cardputer.Keyboard.keysState();
  const bool fn = st.fn;

  // --- 1. Top Row Pad Matrix Scan (Keys 1-8) ---
  bool nowPadDown[kPadCount] = {};
  for (const auto& p : M5Cardputer.Keyboard.keyList()) {
    if (p.y == 0 && p.x >= 1 && p.x <= 8) {
      nowPadDown[p.x - 1] = true;
    }
  }

  for (int i = 0; i < kPadCount; i++) {
    if (nowPadDown[i] && !gPadDown[i]) {
      playChordPad(i, fn);
    }
    if (!nowPadDown[i] && gPadDown[i]) {
      bool any = false;
      for (int j = 0; j < kPadCount; j++) {
        if (j != i && nowPadDown[j]) any = true;
      }
      if (!any && gActivePad == i) {
        allNotesOff();
        clearStrumQueue();
        gActivePad = -1;
        gUiDirty = true;
      }
    }
    gPadDown[i] = nowPadDown[i];
  }

  // --- 2. QWERTY Solo Piano Keyboard Matrix Scan (Rows 1 & 2) ---
  bool nowSoloDown[kSoloKeyCount] = {};
  for (const auto& p : M5Cardputer.Keyboard.keyList()) {
    for (size_t k = 0; k < kSoloKeyCount; k++) {
      char targetChar = kSoloKeys[k].keyChar;
      // Match physical matrix coordinates
      if (p.y == 1) { // Row 1: q,w,e,r,t,y,u,i,o,p,[,]
        if (targetChar == 'w' && p.x == 2) nowSoloDown[k] = true;
        if (targetChar == 'e' && p.x == 3) nowSoloDown[k] = true;
        if (targetChar == 't' && p.x == 5) nowSoloDown[k] = true;
        if (targetChar == 'y' && p.x == 6) nowSoloDown[k] = true;
        if (targetChar == 'u' && p.x == 7) nowSoloDown[k] = true;
        if (targetChar == 'o' && p.x == 9) nowSoloDown[k] = true;
        if (targetChar == 'p' && p.x == 10) nowSoloDown[k] = true;
        if (targetChar == '[' && p.x == 11) nowSoloDown[k] = true;
      } else if (p.y == 2) { // Row 2: a,s,d,f,g,h,j,k,l,;,'
        if (targetChar == 'a' && p.x == 2) nowSoloDown[k] = true;
        if (targetChar == 's' && p.x == 3) nowSoloDown[k] = true;
        if (targetChar == 'd' && p.x == 4) nowSoloDown[k] = true;
        if (targetChar == 'f' && p.x == 5) nowSoloDown[k] = true;
        if (targetChar == 'g' && p.x == 6) nowSoloDown[k] = true;
        if (targetChar == 'h' && p.x == 7) nowSoloDown[k] = true;
        if (targetChar == 'j' && p.x == 8) nowSoloDown[k] = true;
        if (targetChar == 'k' && p.x == 9) nowSoloDown[k] = true;
        if (targetChar == 'l' && p.x == 10) nowSoloDown[k] = true;
        if (targetChar == ';' && p.x == 11) nowSoloDown[k] = true;
        if (targetChar == '\'' && p.x == 12) nowSoloDown[k] = true;
      }
    }
  }

  for (size_t k = 0; k < kSoloKeyCount; k++) {
    int soloMidi = 12 * (gPianoOctave + 1) + gRootNote + kSoloKeys[k].semitoneOffset;
    if (nowSoloDown[k] && !gSoloKeyDown[k]) {
      noteOn(soloMidi, 1.0f);
      snprintf(gLastChordName, sizeof(gLastChordName), "Solo %s%d", getNoteName(soloMidi), gPianoOctave + (kSoloKeys[k].semitoneOffset / 12));
      snprintf(gLastNotes, sizeof(gLastNotes), "%s", getNoteName(soloMidi));
      gUiDirty = true;
    }
    if (!nowSoloDown[k] && gSoloKeyDown[k]) {
      noteOff(soloMidi);
      gUiDirty = true;
    }
    gSoloKeyDown[k] = nowSoloDown[k];
  }

  if (!M5Cardputer.Keyboard.isPressed()) return;

  // Spacebar Tap-Tempo
  if (st.space) {
    uint32_t now = millis();
    if (gLastTapTime > 0) {
      uint32_t diff = now - gLastTapTime;
      if (diff >= 200 && diff <= 2000) {
        int tappedBpm = (int)(60000 / diff);
        gBpm = (int)clampf((float)tappedBpm, 40.0f, 240.0f);
        gEdit = EDIT_BPM;
        gUiDirty = true;
      }
    }
    gLastTapTime = now;
  }

  // Enter toggles Play Mode (Poly / Strum / Arp)
  if (st.enter) {
    gPlayMode = (PlayMode)((gPlayMode + 1) % PLAY_MODE_COUNT);
    gUiDirty = true;
  }

  // Character Layer controls
  for (auto c : st.word) {
    switch (c) {
      case ',':
      case '<':
        nudgeEdit(-1);
        break;
      case '.':
      case '>':
        nudgeEdit(1);
        break;
      case '/':
      case '?':
        gEdit = (EditParam)((gEdit + 1) % EDIT_COUNT);
        gUiDirty = true;
        break;
      case 'z':
      case 'Z':
        if (gPianoOctave > 1) {
          gPianoOctave--;
          gUiDirty = true;
        }
        break;
      case 'x':
      case 'X':
        if (gPianoOctave < 6) {
          gPianoOctave++;
          gUiDirty = true;
        }
        break;
      case 'c':
      case 'C':
        gEdit = (EditParam)((gEdit + 1) % EDIT_COUNT);
        gUiDirty = true;
        break;
      case 'v':
      case 'V':
        nudgeEdit(-1);
        break;
      case 'b':
      case 'B':
        nudgeEdit(1);
        break;
      case 'n':
      case 'N':
        gParams.wave = (Waveform)((gParams.wave + 1) % WAVE_COUNT);
        gUiDirty = true;
        break;
      case 'm':
      case 'M':
        gParams.lfoTarget = (LfoTarget)((gParams.lfoTarget + 1) % LFO_TARGET_COUNT);
        gUiDirty = true;
        break;
      case '-':
      case '_':
        if (gOctave > 1) {
          gOctave--;
          gUiDirty = true;
        }
        break;
      case '=':
      case '+':
        if (gOctave < 6) {
          gOctave++;
          gUiDirty = true;
        }
        break;
      case 'i':
      case 'I':
        gInversion = (gInversion % 3) + 1;
        gUiDirty = true;
        break;
      default:
        break;
    }
  }

  if (st.tab) {
    gRootNote = (gRootNote + 1) % 12;
    gUiDirty = true;
  }
}

void setup() {
  auto cfg = M5.config();
  cfg.internal_spk = true;
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(80);
  gCanvas.createSprite(M5Cardputer.Display.width(), M5Cardputer.Display.height());

  // Re-apply rate on the board-configured ADV pins (ES8311 → speaker + 3.5mm).
  {
    auto spk = M5Cardputer.Speaker.config();
    M5Cardputer.Speaker.end();
    spk.sample_rate = kSampleRate;
    spk.stereo = false;
    spk.task_priority = 5;
    spk.dma_buf_count = 8;
    spk.dma_buf_len = 256;
    M5Cardputer.Speaker.config(spk);
    M5Cardputer.Speaker.begin();
    M5Cardputer.Speaker.setVolume(200);
  }

  // Initialize Wireless Bluetooth BLE MIDI
  BLEDevice::init("CardSynth MIDI");
  BLEServer* pServer = BLEDevice::createServer();
  pServer->setCallbacks(new BleServerCallbacks());
  BLEService* pService = pServer->createService(BLEUUID(MIDI_SERVICE_UUID));
  pMidiCharacteristic = pService->createCharacteristic(
    BLEUUID(MIDI_CHARACTERISTIC_UUID),
    BLECharacteristic::PROPERTY_READ |
    BLECharacteristic::PROPERTY_NOTIFY |
    BLECharacteristic::PROPERTY_WRITE_NR
  );
  pMidiCharacteristic->addDescriptor(new BLE2902());
  pService->start();

  BLEAdvertising* pAdvertising = BLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(MIDI_SERVICE_UUID);
  pAdvertising->setScanResponse(true);
  pAdvertising->setMinPreferred(0x06); // Fast iPhone/Mac connection interval
  pAdvertising->setMinPreferred(0x12);
  BLEDevice::startAdvertising();

  const auto board = M5.getBoard();
  if (board == m5::board_t::board_M5CardputerADV) {
    strncpy(gLastChordName, "Ready ADV", sizeof(gLastChordName) - 1);
  } else if (board == m5::board_t::board_M5Cardputer) {
    strncpy(gLastChordName, "Ready (v1)", sizeof(gLastChordName) - 1);
  } else {
    snprintf(gLastChordName, sizeof(gLastChordName), "Board %d", (int)board);
  }
  strncpy(gLastNotes, "Play 1-8 or QWERTY", sizeof(gLastNotes) - 1);
  drawUi();

  // Speaker task is priority 5; keep synth feed slightly below it on core 1.
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 4, &gAudioTask, 1);
}

void loop() {
  static uint32_t lastScopeRedraw = 0;
  M5Cardputer.update();
  handleKeyboard();
  updateStrumQueue();
  updateArpeggiator();

  uint32_t now = millis();
  // Redraw UI when dirty or 30 FPS oscilloscope refresh when audio is sounding
  if (gUiDirty || (gAudioRunning && (now - lastScopeRedraw >= 33))) {
    drawUi();
    gUiDirty = false;
    lastScopeRedraw = now;
  }
  delay(2);
}
