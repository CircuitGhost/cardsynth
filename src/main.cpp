#include <M5Cardputer.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

// CardSynth — 4-voice polyphonic pad & chord synth for M5Stack Cardputer.
// Features:
// 1. 4-voice Polyphony with dual detuned PolyBLEP anti-aliased oscillators.
// 2. Dynamic Filter Envelope Modulation (VCF Attack/Decay) for plucks, brass & ambient sweeps.
// 3. Strummer & Multi-Pattern Arpeggiator with Tap Tempo (Spacebar).
// 4. 8 MPK-style chord pads (keys 1-8) with Fn-inversion control.
// 5. Flicker-free double-buffered M5Canvas UI.
//
// Future Roadmap (Saved for later):
// - Stereo Tape Delay / Reverb engine (PSRAM backed)
// - Preset Manager (Factory patches + flash saving)
// - USB & Bluetooth BLE MIDI Controller / Sound Module
// - Step Sequencer / Chord Progression Looper

static constexpr uint32_t kSampleRate = 44100;
static constexpr size_t kBufferFrames = 256;
static constexpr size_t kBufferCount = 4;
static constexpr int kMaxVoices = 4;
static constexpr int kMaxChordNotes = 4;
static constexpr int kPadCount = 8;

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
enum EditParam : uint8_t {
  EDIT_CUTOFF = 0,
  EDIT_RESO,
  EDIT_FILT_ENV,
  EDIT_FILT_DEC,
  EDIT_ATTACK,
  EDIT_RELEASE,
  EDIT_DETUNE,
  EDIT_WAVE,
  EDIT_BPM,
  EDIT_STRUM,
  EDIT_VOLUME,
  EDIT_COUNT
};

struct Voice {
  volatile bool active = false;
  volatile bool gate = false;
  float phase = 0.0f;
  float phase2 = 0.0f;
  float incr = 0.0f;
  float incr2 = 0.0f;
  float env = 0.0f;
  float fEnv = 0.0f;
  float velocity = 1.0f;
  uint32_t age = 0;
};

struct SynthParams {
  Waveform wave = WAVE_SAW;
  float cutoff = 0.65f;       // Base cutoff (0..1)
  float reso = 0.12f;         // Resonance (0..1)
  float filtEnv = 0.55f;      // Filter envelope modulation amount (0..1)
  float filtDecayMs = 280.0f; // Filter decay time (10..2000 ms)
  float attackMs = 25.0f;
  float releaseMs = 350.0f;
  float detuneCents = 6.0f;
  float volume = 0.80f;       // Master volume (0..1)
};

static Voice gVoices[kMaxVoices];
static SynthParams gParams;
static portMUX_TYPE gAudioMux = portMUX_INITIALIZER_UNLOCKED;

static int16_t gAudioBuf[kBufferCount][kBufferFrames];
static volatile int gWriteBuf = 0;
static volatile bool gAudioRunning = false;
static TaskHandle_t gAudioTask = nullptr;

static M5Canvas gCanvas(&M5Cardputer.Display);

static uint8_t gRootNote = 0;  // 0=C .. 11=B
static ScaleType gScale = SCALE_MAJOR;
static ChordType gChordType = CHORD_TRIAD;
static PlayMode gPlayMode = PLAY_NORMAL;
static int gOctave = 3;        // MIDI octave for pad root
static int gInversion = 1;     // 1st/2nd/3rd when Fn held
static EditParam gEdit = EDIT_CUTOFF;
static int gBpm = 120;         // Arpeggiator tempo (40..240 BPM)
static int gStrumSpeedMs = 35; // Strum interval between notes (10..100 ms)

static char gLastChordName[24] = "-";
static char gLastNotes[32] = "";
static bool gUiDirty = true;
static bool gPadDown[kPadCount] = {};
static int gActivePad = -1;

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
static const char* kPlayModeNames[] = {
  "Poly", "Strum Up", "Strum Dn", "Arp Up", "Arp Dn", "Arp UpDn", "Arp Rand"
};
static const char* kPlayModeBadges[] = {
  "POLY", "STRUM+", "STRUM-", "ARP+", "ARP-", "ARP+-", "ARP?"
};
static const char* kEditNames[] = {
  "Cutoff", "Reso", "F-Env", "F-Decay", "Attack", "Release", "Detune", "Wave", "BPM", "Strum", "Volume"
};

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
    // Scale-relative: degrees 1,3,5,(7),(9)
    push(degreeToMidi(degree));
    push(degreeToMidi(degree + 2));
    push(degreeToMidi(degree + 4));
    if (gChordType == CHORD_ADD7 || gChordType == CHORD_ADD79) {
      push(degreeToMidi(degree + 6));
    }
    if (gChordType == CHORD_ADD79) {
      // 1-3-7-9 voicing under 4-voice limit
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

  // Cap to voice count
  if (count > kMaxChordNotes) count = kMaxChordNotes;
  *outCount = count;
  for (int i = 0; i < count; i++) outNotes[i] = notes[i];

  // Name
  static const char* quality[] = {"", "7", "9", "maj7", "m7", "7"};
  const char* q = quality[gChordType];
  if (!locked) {
    // Guess quality from 3rd in scale
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
  // Prefer free voice, else steal oldest
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
  // Phase randomization prevents constructive wave stacking
  v.phase = (float)(rand() % 1000) * 0.001f;
  v.phase2 = fmodf(v.phase + 0.37f, 1.0f);
  v.incr = hz / kSampleRate;
  v.incr2 = (hz * det) / kSampleRate;
  v.env = 0.0f;
  v.fEnv = 1.0f; // Trigger filter envelope
  v.velocity = velocity;
  v.age = 0;
  portEXIT_CRITICAL(&gAudioMux);
}

static void allNotesOff() {
  portENTER_CRITICAL(&gAudioMux);
  for (int i = 0; i < kMaxVoices; i++) {
    gVoices[i].gate = false;
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
    // Arpeggiator mode: reset step counter and trigger immediately
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

    // Advance note index
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
  const float releaseInc = 1.0f / fmaxf(1.0f, gParams.releaseMs * 0.001f * kSampleRate);
  const float filtDecInc = 1.0f / fmaxf(1.0f, gParams.filtDecayMs * 0.001f * kSampleRate);
  
  static float lp = 0.0f;
  static float bp = 0.0f;
  static float hp_x = 0.0f;
  static float hp_y = 0.0f;

  const float master = gParams.volume * 0.80f;
  const Waveform wave = gParams.wave;
  const float hp_r = 0.985f; // ~105 Hz highpass roll-off

  for (size_t n = 0; n < frames; n++) {
    float mix = 0.0f;
    float maxFenv = 0.0f;

    for (int i = 0; i < kMaxVoices; i++) {
      Voice& v = gVoices[i];
      if (!v.active) continue;

      if (v.gate) {
        v.env += attackInc;
        if (v.env > 1.0f) v.env = 1.0f;
      } else {
        v.env -= releaseInc;
        if (v.env <= 0.0f) {
          v.env = 0.0f;
          v.active = false;
          continue;
        }
      }

      // Filter envelope decay
      v.fEnv -= filtDecInc;
      if (v.fEnv < 0.0f) v.fEnv = 0.0f;
      if (v.fEnv > maxFenv) maxFenv = v.fEnv;

      // Anti-aliased dual-oscillator voice
      float s = oscSample(v.phase, v.incr, wave);
      s += oscSample(v.phase2, v.incr2, wave) * 0.85f;
      mix += s * 0.25f * v.env * v.velocity;

      v.phase += v.incr;
      if (v.phase >= 1.0f) v.phase -= 1.0f;
      v.phase2 += v.incr2;
      if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
    }

    // Dynamic Filter Envelope calculation
    float dynCutoff = clampf(gParams.cutoff + maxFenv * gParams.filtEnv * 0.40f, 0.02f, 1.0f);
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

    // Warm analog saturation soft-clipper
    float sample = softClip(hp_y * master);
    out[n] = (int16_t)(sample * 28000.0f);
  }
}

static void audioTask(void*) {
  // Feed I2S continuously via dedicated virtual channel 0.
  while (true) {
    const int idx = gWriteBuf;
    if (anyVoiceSounding()) {
      gAudioRunning = true;
      renderBlock(gAudioBuf[idx], kBufferFrames);
    } else {
      gAudioRunning = false;
      memset(gAudioBuf[idx], 0, sizeof(gAudioBuf[idx]));
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

  // Header Title & Play Mode Badge
  d.setTextColor(TFT_ORANGE);
  d.setTextSize(1);
  d.drawString("CardSynth", 4, 2);

  d.setTextColor(gPlayMode == PLAY_NORMAL ? TFT_DARKGREY : TFT_GREEN);
  char modeBadge[20];
  snprintf(modeBadge, sizeof(modeBadge), "[%s %dBPM]", kPlayModeBadges[gPlayMode], gBpm);
  d.drawString(modeBadge, 130, 2);

  // Large Chord Name
  d.setTextColor(TFT_WHITE);
  d.setTextSize(2);
  d.drawString(gLastChordName, 4, 16);

  // Chord Notes
  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(gLastNotes, 4, 38);

  // Scale & Transposition info
  char line[56];
  snprintf(line, sizeof(line), "%s %s | %s | oct%d | inv%d",
           getNoteName(gRootNote), kScaleNames[gScale], kChordTypeNames[gChordType], gOctave, gInversion);
  d.setTextColor(TFT_CYAN);
  d.drawString(line, 4, 52);

  // Active Edit Parameter & Value
  snprintf(line, sizeof(line), "Edit %s", kEditNames[gEdit]);
  d.setTextColor(TFT_YELLOW);
  d.drawString(line, 4, 68);

  switch (gEdit) {
    case EDIT_CUTOFF:
      snprintf(line, sizeof(line), "%.0f%%", gParams.cutoff * 100.0f);
      break;
    case EDIT_RESO:
      snprintf(line, sizeof(line), "%.0f%%", gParams.reso * 100.0f);
      break;
    case EDIT_FILT_ENV:
      snprintf(line, sizeof(line), "%.0f%% (Mod Amt)", gParams.filtEnv * 100.0f);
      break;
    case EDIT_FILT_DEC:
      snprintf(line, sizeof(line), "%.0f ms (F-Decay)", gParams.filtDecayMs);
      break;
    case EDIT_ATTACK:
      snprintf(line, sizeof(line), "%.0f ms", gParams.attackMs);
      break;
    case EDIT_RELEASE:
      snprintf(line, sizeof(line), "%.0f ms", gParams.releaseMs);
      break;
    case EDIT_DETUNE:
      snprintf(line, sizeof(line), "%.1f ct", gParams.detuneCents);
      break;
    case EDIT_WAVE:
      snprintf(line, sizeof(line), "%s", kWaveNames[gParams.wave]);
      break;
    case EDIT_BPM:
      snprintf(line, sizeof(line), "%d BPM [Space: Tap]", gBpm);
      break;
    case EDIT_STRUM:
      snprintf(line, sizeof(line), "%d ms (Strum Speed)", gStrumSpeedMs);
      break;
    case EDIT_VOLUME:
      snprintf(line, sizeof(line), "%.0f%%", gParams.volume * 100.0f);
      break;
    default:
      line[0] = 0;
      break;
  }
  d.setTextColor(TFT_WHITE);
  d.drawString(line, 4, 82);

  // Quick Help Footer
  d.setTextColor(TFT_DARKGREY);
  d.drawString("1-8 chord  a:mode  spc:tap  ,/. edit", 4, 104);
  d.drawString("; type  ' scale  [] key  i inv", 4, 118);

  gCanvas.pushSprite(0, 0);
}

static void nudgeEdit(int dir) {
  switch (gEdit) {
    case EDIT_CUTOFF:
      gParams.cutoff = clampf(gParams.cutoff + dir * 0.03f, 0.02f, 1.0f);
      break;
    case EDIT_RESO:
      gParams.reso = clampf(gParams.reso + dir * 0.03f, 0.0f, 0.95f);
      break;
    case EDIT_FILT_ENV:
      gParams.filtEnv = clampf(gParams.filtEnv + dir * 0.05f, 0.0f, 1.0f);
      break;
    case EDIT_FILT_DEC:
      gParams.filtDecayMs = clampf(gParams.filtDecayMs + dir * 20.0f, 10.0f, 2000.0f);
      break;
    case EDIT_ATTACK:
      gParams.attackMs = clampf(gParams.attackMs + dir * 6.0f, 1.0f, 2000.0f);
      break;
    case EDIT_RELEASE:
      gParams.releaseMs = clampf(gParams.releaseMs + dir * 20.0f, 10.0f, 4000.0f);
      break;
    case EDIT_DETUNE:
      gParams.detuneCents = clampf(gParams.detuneCents + dir * 1.0f, 0.0f, 40.0f);
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

  // Track pad presses via matrix so Fn+digit still maps to the physical key.
  bool nowDown[kPadCount] = {};
  for (const auto& p : M5Cardputer.Keyboard.keyList()) {
    if (p.y == 0 && p.x >= 1 && p.x <= 8) {
      nowDown[p.x - 1] = true;
    }
  }

  for (int i = 0; i < kPadCount; i++) {
    if (nowDown[i] && !gPadDown[i]) {
      playChordPad(i, fn);
    }
    if (!nowDown[i] && gPadDown[i]) {
      // Release pad
      bool any = false;
      for (int j = 0; j < kPadCount; j++) {
        if (j != i && nowDown[j]) any = true;
      }
      if (!any) {
        allNotesOff();
        clearStrumQueue();
        gActivePad = -1;
        gUiDirty = true;
      }
    }
    gPadDown[i] = nowDown[i];
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

  // One-shot controls from character layer
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
      case 'a':
      case 'A':
        gPlayMode = (PlayMode)((gPlayMode + 1) % PLAY_MODE_COUNT);
        gUiDirty = true;
        break;
      case ';':
      case ':':
        gChordType = (ChordType)((gChordType + 1) % CHORD_TYPE_COUNT);
        gUiDirty = true;
        break;
      case '\'':
      case '"':
        gScale = (ScaleType)((gScale + 1) % SCALE_COUNT);
        gUiDirty = true;
        break;
      case '[':
      case '{':
        gRootNote = (gRootNote + 11) % 12;
        gUiDirty = true;
        break;
      case ']':
      case '}':
        gRootNote = (gRootNote + 1) % 12;
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
      case '/':
      case '?':
        gEdit = (EditParam)((gEdit + 1) % EDIT_COUNT);
        gUiDirty = true;
        break;
      case 'z':
      case 'Z':
        gEdit = EDIT_CUTOFF;
        gUiDirty = true;
        break;
      case 'x':
      case 'X':
        gEdit = EDIT_RESO;
        gUiDirty = true;
        break;
      case 'f':
      case 'F':
        gEdit = EDIT_FILT_ENV;
        gUiDirty = true;
        break;
      case 'd':
      case 'D':
        gEdit = EDIT_FILT_DEC;
        gUiDirty = true;
        break;
      case 'c':
      case 'C':
        gEdit = EDIT_ATTACK;
        gUiDirty = true;
        break;
      case 'v':
      case 'V':
        gEdit = EDIT_RELEASE;
        gUiDirty = true;
        break;
      case 'b':
      case 'B':
        gEdit = EDIT_DETUNE;
        gUiDirty = true;
        break;
      case 'n':
      case 'N':
        gEdit = EDIT_WAVE;
        gUiDirty = true;
        break;
      case 't':
      case 'T':
        gEdit = EDIT_BPM;
        gUiDirty = true;
        break;
      case 's':
      case 'S':
        gEdit = EDIT_STRUM;
        gUiDirty = true;
        break;
      case 'm':
      case 'M':
        gEdit = EDIT_VOLUME;
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

  const auto board = M5.getBoard();
  if (board == m5::board_t::board_M5CardputerADV) {
    strncpy(gLastChordName, "Ready ADV", sizeof(gLastChordName) - 1);
  } else if (board == m5::board_t::board_M5Cardputer) {
    strncpy(gLastChordName, "Ready (v1)", sizeof(gLastChordName) - 1);
  } else {
    snprintf(gLastChordName, sizeof(gLastChordName), "Board %d", (int)board);
  }
  strncpy(gLastNotes, "press 1-8", sizeof(gLastNotes) - 1);
  drawUi();

  // Speaker task is priority 5; keep synth feed slightly below it on core 1.
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 4, &gAudioTask, 1);
}

void loop() {
  M5Cardputer.update();
  handleKeyboard();
  updateStrumQueue();
  updateArpeggiator();
  if (gUiDirty) {
    drawUi();
    gUiDirty = false;
  }
  delay(2);
}
