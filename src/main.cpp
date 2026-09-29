#include <M5Cardputer.h>
#include <math.h>
#include <string.h>

// CardSynth — 4-voice editable pad synth with MPK-style chord pads.
// Silent until a chord pad is pressed. Audio goes through ES8311 → speaker
// and 3.5mm jack (jack insertion mutes the speaker amp in hardware).

static constexpr uint32_t kSampleRate = 22050;
static constexpr size_t kBufferFrames = 256;
static constexpr size_t kBufferCount = 3;
static constexpr int kMaxVoices = 4;
static constexpr int kMaxChordNotes = 4;
static constexpr int kPadCount = 8;

enum Waveform : uint8_t { WAVE_SAW = 0, WAVE_SQUARE, WAVE_TRIANGLE, WAVE_COUNT };
enum ChordType : uint8_t {
  CHORD_TRIAD = 0,  // 1-3-5 in scale
  CHORD_ADD7,       // 1-3-5-7 in scale
  CHORD_ADD79,      // 1-3-5-7-9 (uses 4 notes: drop 5th for voice limit)
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
enum EditParam : uint8_t {
  EDIT_CUTOFF = 0,
  EDIT_RESO,
  EDIT_ATTACK,
  EDIT_RELEASE,
  EDIT_DETUNE,
  EDIT_WAVE,
  EDIT_VOLUME,
  EDIT_COUNT
};

struct Voice {
  bool active = false;
  bool gate = false;
  float phase = 0.0f;
  float phase2 = 0.0f;
  float incr = 0.0f;
  float incr2 = 0.0f;
  float env = 0.0f;
  float velocity = 1.0f;
  uint32_t age = 0;
};

struct SynthParams {
  Waveform wave = WAVE_SAW;
  float cutoff = 0.35f;   // 0..1
  float reso = 0.20f;     // 0..1
  float attackMs = 40.0f;
  float releaseMs = 320.0f;
  float detuneCents = 8.0f;
  float volume = 0.55f;   // 0..1 master
};

static Voice gVoices[kMaxVoices];
static SynthParams gParams;
static portMUX_TYPE gAudioMux = portMUX_INITIALIZER_UNLOCKED;

static int16_t gAudioBuf[kBufferCount][kBufferFrames];
static volatile int gWriteBuf = 0;
static volatile bool gAudioRunning = false;
static TaskHandle_t gAudioTask = nullptr;

static uint8_t gRootNote = 0;  // 0=C .. 11=B
static ScaleType gScale = SCALE_MAJOR;
static ChordType gChordType = CHORD_TRIAD;
static int gOctave = 3;       // MIDI octave for pad root
static int gInversion = 1;    // 1st/2nd/3rd when Fn held
static EditParam gEdit = EDIT_CUTOFF;
static char gLastChordName[24] = "-";
static char gLastNotes[32] = "";
static bool gUiDirty = true;
static bool gPadDown[kPadCount] = {};
static int gActivePad = -1;

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
static const char* kEditNames[] = {
  "Cutoff", "Reso", "Attack", "Release", "Detune", "Wave", "Volume"
};

// Scale degree intervals from root (semitones), null-terminated by -1
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
      // Prefer 1-3-7-9 under the 4-voice limit
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

  // Spread: drop root an octave for clearer speaker tone
  notes[0] -= 12;

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
  snprintf(nameOut, nameLen, "%s%s%s", kNoteNames[rootMidi % 12], q, invert ? " inv" : "");
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
  v.active = true;
  v.gate = true;
  v.phase = 0.0f;
  v.phase2 = 0.37f;
  v.incr = hz / kSampleRate;
  v.incr2 = (hz * det) / kSampleRate;
  v.env = 0.0f;
  v.velocity = velocity;
  v.age = 0;
  for (int i = 0; i < kMaxVoices; i++) {
    if (gVoices[i].active) gVoices[i].age++;
  }
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

static void playChordPad(int padIndex, bool invert) {
  int notes[kMaxChordNotes];
  int count = 0;
  char name[24];
  buildChordNotes(padIndex, invert, notes, &count, name, sizeof(name));

  allNotesOff();
  for (int i = 0; i < count; i++) {
    noteOn(notes[i], 1.0f - i * 0.04f);
  }

  strncpy(gLastChordName, name, sizeof(gLastChordName) - 1);
  gLastChordName[sizeof(gLastChordName) - 1] = 0;

  gLastNotes[0] = 0;
  for (int i = 0; i < count; i++) {
    char piece[8];
    snprintf(piece, sizeof(piece), "%s%s", i ? " " : "", kNoteNames[notes[i] % 12]);
    strncat(gLastNotes, piece, sizeof(gLastNotes) - strlen(gLastNotes) - 1);
  }
  gActivePad = padIndex;
  gUiDirty = true;
}

static float oscSample(float phase, Waveform w) {
  switch (w) {
    case WAVE_SQUARE:
      return phase < 0.5f ? 1.0f : -1.0f;
    case WAVE_TRIANGLE:
      return 1.0f - 4.0f * fabsf(phase - 0.5f);
    case WAVE_SAW:
    default:
      return 2.0f * phase - 1.0f;
  }
}

static bool anyVoiceSounding() {
  for (int i = 0; i < kMaxVoices; i++) {
    if (gVoices[i].active) return true;
  }
  return false;
}

static void renderBlock(int16_t* out, size_t frames) {
  const float attack = fmaxf(1.0f, gParams.attackMs) * 0.001f * kSampleRate;
  const float release = fmaxf(1.0f, gParams.releaseMs) * 0.001f * kSampleRate;
  const float attackInc = 1.0f / attack;
  const float releaseInc = 1.0f / release;

  // One-pole lowpass with mild reso feedback (shared for CPU)
  static float lp = 0.0f;
  static float bp = 0.0f;
  const float f = clampf(gParams.cutoff * gParams.cutoff, 0.002f, 0.95f);
  const float q = clampf(gParams.reso * 0.95f, 0.0f, 0.92f);
  const float master = gParams.volume * 0.22f;

  for (size_t n = 0; n < frames; n++) {
    float mix = 0.0f;
    portENTER_CRITICAL(&gAudioMux);
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

      float s = oscSample(v.phase, gParams.wave);
      s += oscSample(v.phase2, gParams.wave) * 0.85f;
      mix += s * v.env * v.velocity;

      v.phase += v.incr;
      if (v.phase >= 1.0f) v.phase -= 1.0f;
      v.phase2 += v.incr2;
      if (v.phase2 >= 1.0f) v.phase2 -= 1.0f;
    }
    portEXIT_CRITICAL(&gAudioMux);

    // State-variable-ish lowpass
    lp += f * (mix - lp - q * bp);
    bp += f * (lp - bp);
    float sample = bp;
    sample *= master;

    if (sample > 1.0f) sample = 1.0f;
    if (sample < -1.0f) sample = -1.0f;
    out[n] = (int16_t)(sample * 30000.0f);
  }
}

static void audioTask(void*) {
  // Wait until a chord is actually sounding — no idle stream, no boot tone.
  while (true) {
    if (!anyVoiceSounding()) {
      gAudioRunning = false;
      vTaskDelay(pdMS_TO_TICKS(5));
      continue;
    }

    gAudioRunning = true;
    const int idx = gWriteBuf;
    renderBlock(gAudioBuf[idx], kBufferFrames);

    // Queue until the speaker accepts the buffer (non-blocking API).
    while (!M5Cardputer.Speaker.playRaw(
             gAudioBuf[idx], kBufferFrames, kSampleRate, false, 1, 0, false)) {
      vTaskDelay(1);
      if (!anyVoiceSounding() && !gAudioRunning) break;
    }
    gWriteBuf = (gWriteBuf + 1) % kBufferCount;
  }
}

static void drawUi() {
  auto& d = M5Cardputer.Display;
  d.fillScreen(TFT_BLACK);
  d.setTextDatum(top_left);

  d.setTextColor(TFT_ORANGE);
  d.setTextSize(1);
  d.drawString("CardSynth", 4, 2);

  d.setTextColor(TFT_WHITE);
  d.setTextSize(2);
  d.drawString(gLastChordName, 4, 18);

  d.setTextSize(1);
  d.setTextColor(TFT_LIGHTGREY);
  d.drawString(gLastNotes, 4, 42);

  char line[48];
  snprintf(line, sizeof(line), "%s %s | %s | oct%d",
           kNoteNames[gRootNote], kScaleNames[gScale], kChordTypeNames[gChordType], gOctave);
  d.setTextColor(TFT_CYAN);
  d.drawString(line, 4, 58);

  snprintf(line, sizeof(line), "Edit %s", kEditNames[gEdit]);
  d.setTextColor(TFT_YELLOW);
  d.drawString(line, 4, 74);

  switch (gEdit) {
    case EDIT_CUTOFF:
      snprintf(line, sizeof(line), "%.0f%%", gParams.cutoff * 100.0f);
      break;
    case EDIT_RESO:
      snprintf(line, sizeof(line), "%.0f%%", gParams.reso * 100.0f);
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
    case EDIT_VOLUME:
      snprintf(line, sizeof(line), "%.0f%%", gParams.volume * 100.0f);
      break;
    default:
      line[0] = 0;
      break;
  }
  d.setTextColor(TFT_WHITE);
  d.drawString(line, 4, 90);

  d.setTextColor(TFT_DARKGREY);
  d.drawString("1-8 chords  Fn=inv  ,/. edit", 4, 110);
  d.drawString("; type  ' scale  [] key", 4, 122);
}

static void nudgeEdit(int dir) {
  switch (gEdit) {
    case EDIT_CUTOFF:
      gParams.cutoff = clampf(gParams.cutoff + dir * 0.03f, 0.02f, 1.0f);
      break;
    case EDIT_RESO:
      gParams.reso = clampf(gParams.reso + dir * 0.03f, 0.0f, 0.95f);
      break;
    case EDIT_ATTACK:
      gParams.attackMs = clampf(gParams.attackMs + dir * 8.0f, 1.0f, 2000.0f);
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
      // Release this pad — if no other pad held, release notes.
      bool any = false;
      for (int j = 0; j < kPadCount; j++) {
        if (j != i && nowDown[j]) any = true;
      }
      if (!any) {
        allNotesOff();
        gActivePad = -1;
        gUiDirty = true;
      }
    }
    gPadDown[i] = nowDown[i];
  }

  if (!M5Cardputer.Keyboard.isPressed()) return;

  // One-shot controls from character layer (ignore while only Fn held)
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
  M5Cardputer.begin(cfg, true);

  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Display.setBrightness(80);

  // Speaker + ES8311 path also drives the 3.5mm jack.
  auto spk = M5Cardputer.Speaker.config();
  spk.sample_rate = kSampleRate;
  spk.stereo = false;
  spk.task_priority = 5;
  M5Cardputer.Speaker.config(spk);
  M5Cardputer.Speaker.begin();
  M5Cardputer.Speaker.setVolume((uint8_t)(gParams.volume * 200.0f));
  // Do NOT tone() or playRaw on boot — stay silent until a pad is pressed.

  strncpy(gLastChordName, "Ready", sizeof(gLastChordName) - 1);
  strncpy(gLastNotes, "press 1-8", sizeof(gLastNotes) - 1);
  drawUi();

  xTaskCreatePinnedToCore(audioTask, "audio", 8192, nullptr, 4, &gAudioTask, 1);
}

void loop() {
  M5Cardputer.update();
  handleKeyboard();
  if (gUiDirty) {
    drawUi();
    gUiDirty = false;
  }
  delay(2);
}
