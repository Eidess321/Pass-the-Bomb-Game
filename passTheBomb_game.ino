#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>

// ============================================================
// PASS THE BOMB - XIAO RP2040 - DUAL CORE VERSION
// 1.3" SH1106 128x64 OLED + 4 piezos + buzzer + NeoPixel
//
// CORE 0 (setup/loop):   senses all 4 piezos continuously, at
//                        full speed, and pushes tap events
//                        (channel + peak strength) to Core 1
//                        over the RP2040's hardware FIFO. It
//                        NEVER touches the display, buzzer,
//                        NeoPixel, or game state.
//
// CORE 1 (setup1/loop1): owns the OLED, buzzer, onboard LED,
//                        NeoPixel, and the entire calibration
//                        + gameplay state machine. It reacts to
//                        tap events instead of reading the
//                        piezos itself.
//
// This split is the actual point of the assignment: one core
// dedicated to high-speed sensing, the other to display/logic,
// running truly in parallel rather than time-sliced in one loop.
// ============================================================

// XIAO RP2040 hardware I2C:
// D4 = GPIO6 = SDA
// D5 = GPIO7 = SCL
U8G2_SH1106_128X64_NONAME_F_HW_I2C oled(U8G2_R0, U8X8_PIN_NONE);

const uint8_t PIEZO[4] = {26,27,28,29};   // D0-D3
const uint8_t BUZZER = 0;   // XIAO RP2040 D6 = GPIO0; do NOT use GPIO6 (OLED SDA)

#ifndef PIN_NEOPIXEL
#define PIN_NEOPIXEL 12
#endif
#ifndef PIN_NEOPIXEL_POWER
#define PIN_NEOPIXEL_POWER 11
#endif

Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

const uint8_t PIXEL_DIM = 25;
const uint8_t PIXEL_BRIGHT = 65;

const uint8_t TAPS_REQUIRED = 3;
const uint16_t MIN_TAP_LEVEL = 75;
const uint16_t MAX_TAP_THRESHOLD = 450;
const uint16_t BASELINE_MARGIN = 35;
const uint16_t TAP_COOLDOWN = 120;

const unsigned long MIN_BOMB = 300;
const unsigned long MAX_BOMB = 5000;
const unsigned long PASS_TIME = 420;
const unsigned long STUN_TIME = 3000;
const unsigned long EXP_FRAME = 90;
const uint8_t EXP_FRAMES = 7;
const unsigned long TITLE_TIME = 2000;
const unsigned long ELIM_TIME = 1500;
const unsigned long WIN_TIME = 3500;
const unsigned long CAL_SWITCH_DELAY = 900;

// Calibration isolation: the assigned player's channel must
// clearly dominate any other channel that fired around the same
// time (real cross-talk from a shared mounting surface).
const uint16_t CAL_CROSS_MARGIN = 50;
const uint8_t CAL_DOM_NUM = 5;       // target >= 1.25x strongest other sensor
const uint8_t CAL_DOM_DEN = 4;
const unsigned long CAL_TAP_LOCKOUT = 300;
const unsigned long CROSS_TALK_WINDOW_MS = 80; // "around the same time" window

// ============================================================
// CORE 0 <-> CORE 1 EVENT FORMAT
// Packed into a uint32_t: [channel:8][peak strength:16][unused:8]
// ============================================================
uint32_t packEvent(uint8_t ch, uint16_t strength) {
  return ((uint32_t)ch << 24) | ((uint32_t)strength << 8);
}
void unpackEvent(uint32_t packed, uint8_t &ch, uint16_t &strength) {
  ch = (packed >> 24) & 0xFF;
  strength = (packed >> 8) & 0xFFFF;
}

// ============================================================
// CORE 0 - SENSING ONLY
// ============================================================
const int           CORE0_DETECT_FLOOR = 55; // low universal floor - opens a strike window
const unsigned long STRIKE_WINDOW_MS   = 5;  // peak-hold window per tap, for an accurate reading
const unsigned long CORE0_REFRACTORY_MS = 60; // short - real per-purpose cooldowns are enforced on Core 1

int           peak0[4]        = {0,0,0,0};
bool          windowOpen0[4]  = {false,false,false,false};
unsigned long windowStart0[4] = {0,0,0,0};
unsigned long refractory0[4]  = {0,0,0,0};

// Continuously-refreshed raw snapshot, for Core 1's baseline
// averaging and the initial random seed. Written only by Core 0,
// read-only from Core 1 - safe without a mutex since single
// 16-bit reads/writes are atomic on this chip and a slightly
// stale value here costs nothing (it's only used for averaging
// and seeding, never for tap decisions).
volatile uint16_t currentReading[4] = {0,0,0,0};

void setup() {
  for (uint8_t i=0;i<4;i++) pinMode(PIEZO[i], INPUT);
  analogReadResolution(10);
}

void loop() {
  unsigned long now = millis();

  for (uint8_t ch=0; ch<4; ch++) {
    int v = analogRead(PIEZO[ch]);
    currentReading[ch] = v;

    if (now < refractory0[ch]) continue;

    if (!windowOpen0[ch]) {
      if (v > CORE0_DETECT_FLOOR) {
        windowOpen0[ch] = true;
        windowStart0[ch] = now;
        peak0[ch] = v;
      }
    } else {
      if (v > peak0[ch]) peak0[ch] = v;
      if (now - windowStart0[ch] >= STRIKE_WINDOW_MS) {
        rp2040.fifo.push(packEvent(ch, peak0[ch]));
        refractory0[ch] = now + CORE0_REFRACTORY_MS;
        windowOpen0[ch] = false;
        peak0[ch] = 0;
      }
    }
  }
}

// ============================================================
// CORE 1 - CALIBRATION, GAMEPLAY, DISPLAY, SOUND, LIGHTS
// ============================================================

enum State { TITLE, CAL_BASE, CAL_TAPS, CAL_WAIT, READY, PLAYING, PASSING,
             EXPLOSION, ELIMINATED, WIN };
State state = TITLE;

bool displayOK = false;
bool alive[4] = {true,true,true,true};
bool stunned[4] = {false,false,false,false};
unsigned long stunUntil[4] = {0,0,0,0};

uint16_t baseline[4] = {0,0,0,0};
uint16_t threshold[4] = {MIN_TAP_LEVEL,MIN_TAP_LEVEL,MIN_TAP_LEVEL,MIN_TAP_LEVEL};
uint16_t tapPeaks[4][TAPS_REQUIRED];

uint8_t calPlayer = 0;
uint8_t calTap = 0;

uint8_t currentPlayer = 0;
uint8_t previousPlayer = 0;
uint8_t winnerPlayer = 0;

unsigned long stateStart = 0;
unsigned long bombStart = 0;
unsigned long bombDuration = 0;
unsigned long lastTap = 0;
unsigned long explosionStart = 0;
unsigned long nextBeep = 0;
unsigned long calSwitchAt = 0;
unsigned long pixelOff = 0;
bool pixelFlash = false;
uint32_t pixelRestore = 0;

const int PX[4] = {2, 98, 98, 2};
const int PY[4] = {12, 12, 44, 44};

// Most recent event seen per channel, and when - fed by draining
// the FIFO once per loop1() pass. This replaces the old "sample
// all 4 piezos live" approach with recent event history, which is
// the natural fit once sensing lives on the other core.
uint16_t lastEventPeak[4] = {0,0,0,0};
unsigned long lastEventTime[4] = {0,0,0,0};

struct TapEvent { uint8_t ch; uint16_t strength; };
TapEvent frameEvents[8];
uint8_t frameEventCount = 0;

// ---------- declarations ----------
void showTitle();
void startCalibration();
void processCalibration();
void drawCalibration();
void showCalibrationSummary();

void drainFifo();
uint16_t baselineRead(uint8_t p);
bool evaluateCalibrationTap(uint8_t target, uint16_t targetPeak);
int detectAnyTapFromEvents(uint16_t *outPeak);

void startReady();
void startGame(uint8_t starter);
void processGame();
void enterPlayingEffects();

void startPassing(uint8_t next);
void processPassing();
void startExplosion();
void processExplosion();
void startEliminated();
void processEliminated();
void startWin();
void processWin();

void drawPlayerSlot(uint8_t p, bool holder, bool elim);
void drawPlayerIcon(int cx, int cy, bool elim, bool active);
void drawBomb(int x, int y, bool burst);
void drawGameplay();
void drawCentered(const char *s, int y);
void drawX(int cx, int cy, int s);
void drawBurst(int cx, int cy, uint8_t frame);

void ledSet(bool on);
void setPixel(uint32_t c);
void flashPixel(uint32_t c, unsigned long ms, uint32_t restore);
void updatePixel();
uint32_t playerColor(uint8_t p, uint8_t b);

void beepTap();
void beepPass();
void beepStun();
void beepExplosion();
void beepNext();
void beepWin();

bool onlyOneAlive();
uint8_t lastAlive();
uint8_t randomOtherAlive(uint8_t from);
void seedRandom();

// ============================================================
// SETUP1
// ============================================================
void setup1() {
  Serial.begin(115200);
  unsigned long t0 = millis();
  while (!Serial && millis() - t0 < 2000) { /* wait briefly for USB serial */ }

  Serial.println();
  Serial.println("=== PASS THE BOMB - DUAL CORE ===");
  Serial.println("Core 0: senses all 4 piezos continuously");
  Serial.println("Core 1: display, buzzer, LEDs, game logic");
  Serial.println("OLED I2C: D4/GPIO6 SDA, D5/GPIO7 SCL");
  Serial.println("BUZZER:   D6/GPIO0");

  pinMode(BUZZER, OUTPUT);
  digitalWrite(BUZZER, LOW);

  pinMode(LED_BUILTIN, OUTPUT);
  ledSet(false);

  pinMode(PIN_NEOPIXEL_POWER, OUTPUT);
  digitalWrite(PIN_NEOPIXEL_POWER, HIGH);
  pixel.begin();
  pixel.clear();
  pixel.show();

  displayOK = oled.begin();
  Serial.println(displayOK ? "OLED init OK." : "OLED init FAILED.");
  if (displayOK) {
    oled.setPowerSave(0);
    oled.clearBuffer();
    oled.setFont(u8g2_font_6x10_tf);
    drawCentered("Starting...", 35);
    oled.sendBuffer();
  }

  seedRandom();
  delay(400);
  showTitle();
}

// ============================================================
// LOOP1
// ============================================================
void loop1() {
  drainFifo();
  updatePixel();

  switch (state) {
    case TITLE:
      if (millis()-stateStart >= TITLE_TIME) startCalibration();
      break;

    case CAL_BASE:
    case CAL_TAPS:
    case CAL_WAIT:
      processCalibration();
      break;

    case READY: {
      uint16_t peak;
      int t = detectAnyTapFromEvents(&peak);
      if (t >= 0) startGame((uint8_t)t);
      break;
    }

    case PLAYING:
      processGame();
      break;

    case PASSING:
      processPassing();
      break;

    case EXPLOSION:
      processExplosion();
      break;

    case ELIMINATED:
      processEliminated();
      break;

    case WIN:
      processWin();
      break;
  }
}

// Drain every event Core 0 has queued since the last pass. Keeping
// ALL of them (not just the latest) matters: two events can arrive
// in the same pass (a real tap plus a cross-talk blip), and only
// looking at the last one popped can silently discard the real tap.
void drainFifo() {
  frameEventCount = 0;
  while (rp2040.fifo.available() && frameEventCount < 8) {
    uint32_t packed = rp2040.fifo.pop();
    uint8_t ch; uint16_t strength;
    unpackEvent(packed, ch, strength);

    lastEventPeak[ch] = strength;
    lastEventTime[ch] = millis();

    frameEvents[frameEventCount].ch = ch;
    frameEvents[frameEventCount].strength = strength;
    frameEventCount++;
  }
}

// ============================================================
// TITLE / CALIBRATION
// ============================================================
void showTitle() {
  state = TITLE;
  stateStart = millis();
  ledSet(false);
  setPixel(pixel.Color(0,0,0));

  if (!displayOK) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_7x14B_tf);
  drawCentered("PASS THE BOMB", 22);
  oled.setFont(u8g2_font_6x10_tf);
  drawCentered("4 PLAYER GAME", 39);
  drawCentered("GET READY", 55);
  oled.sendBuffer();
}

void startCalibration() {
  state = CAL_BASE;
  calPlayer = 0;
  calTap = 0;
  lastTap = 0;

  for (uint8_t p=0;p<4;p++) {
    baseline[p]=0;
    threshold[p]=MIN_TAP_LEVEL;
    for (uint8_t t=0;t<TAPS_REQUIRED;t++) tapPeaks[p][t]=0;
  }

  if (displayOK) {
    oled.clearBuffer();
    oled.setFont(u8g2_font_7x14B_tf);
    drawCentered("CALIBRATION", 14);
    oled.setFont(u8g2_font_6x10_tf);
    drawCentered("Measuring sensors...", 32);
    drawCentered("Don't tap yet", 48);
    drawCentered("Please wait", 61);
    oled.sendBuffer();
  }
  delay(350);
}

// Averages Core 0's continuously-refreshed snapshot instead of
// reading the ADC directly - Core 1 never touches the piezo pins.
uint16_t baselineRead(uint8_t p) {
  unsigned long st=millis();
  uint32_t total=0;
  uint16_t n=0;
  while (millis()-st < 220) {
    total += currentReading[p];
    n++;
    delayMicroseconds(500);
  }
  return n ? total/n : 0;
}

// The assigned player's tap must be both above its own threshold
// and clearly stronger than anything else that fired recently
// (the "recently" substitutes for the old live 4-channel snapshot,
// since sensing now happens on the other core).
bool evaluateCalibrationTap(uint8_t target, uint16_t targetPeak) {
  unsigned long now = millis();
  uint16_t strongestOther = 0;
  uint8_t strongestOtherPlayer = 255;

  for (uint8_t p = 0; p < 4; p++) {
    if (p == target) continue;
    if (now - lastEventTime[p] <= CROSS_TALK_WINDOW_MS && lastEventPeak[p] > strongestOther) {
      strongestOther = lastEventPeak[p];
      strongestOtherPlayer = p;
    }
  }

  uint16_t required = baseline[target] + BASELINE_MARGIN;
  if (required < MIN_TAP_LEVEL) required = MIN_TAP_LEVEL;

  bool strongEnough = targetPeak >= required;
  bool hasMargin = targetPeak >= (uint16_t)(strongestOther + CAL_CROSS_MARGIN);
  bool hasDominance = ((uint32_t)targetPeak * CAL_DOM_DEN >=
                        (uint32_t)strongestOther * CAL_DOM_NUM);

  if (targetPeak >= MIN_TAP_LEVEL || strongestOther >= MIN_TAP_LEVEL) {
    Serial.print("CAL P"); Serial.print(target + 1);
    Serial.print(" target="); Serial.print(targetPeak);
    Serial.print(" strongestOther=");
    if (strongestOtherPlayer != 255) {
      Serial.print("P"); Serial.print(strongestOtherPlayer + 1);
      Serial.print("="); Serial.print(strongestOther);
    } else {
      Serial.print("none");
    }
    Serial.println();

    if (strongestOtherPlayer != 255 && strongestOther > targetPeak) {
      Serial.print("REJECTED: likely cross-talk from P");
      Serial.println(strongestOtherPlayer + 1);
      return false;
    }
    if (strongEnough && (!hasMargin || !hasDominance)) {
      Serial.println("REJECTED: ambiguous/cross-coupled signal.");
      return false;
    }
  }

  return strongEnough && hasMargin && hasDominance;
}

void drawCalibration() {
  if (!displayOK) return;
  oled.clearBuffer();

  oled.setFont(u8g2_font_7x14B_tf);
  drawCentered("CALIBRATION", 11);

  drawPlayerIcon(20,25,false,false);

  oled.setFont(u8g2_font_6x10_tf);
  char name[12];
  snprintf(name,sizeof(name),"PLAYER %d",calPlayer+1);
  oled.drawStr(39,25,name);

  char count[8];
  snprintf(count,sizeof(count),"%d / %d",calTap,TAPS_REQUIRED);
  oled.drawStr(39,38,count);

  for (uint8_t i=0;i<TAPS_REQUIRED;i++) {
    int x=39+i*13;
    oled.drawFrame(x,43,9,9);
    if (i<calTap) oled.drawBox(x+2,45,5,5);
  }

  oled.setFont(u8g2_font_5x7_tf);
  char instruction[22];
  snprintf(instruction, sizeof(instruction), "P%d ONLY - TAP SENSOR", calPlayer + 1);
  drawCentered(instruction,62);
  oled.sendBuffer();
}

void showCalibrationSummary() {
  if (!displayOK) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  drawCentered("CALIBRATION OK",10);

  for (uint8_t p=0;p<4;p++) {
    int x=16+p*32;
    drawPlayerIcon(x,25,false,false);
    oled.setFont(u8g2_font_5x7_tf);
    char s[4];
    snprintf(s,sizeof(s),"P%d",p+1);
    int w=oled.getStrWidth(s);
    oled.drawStr(x-w/2,39,s);
    oled.drawFrame(x-4,44,8,8);
    oled.drawLine(x-2,48,x,50);
    oled.drawLine(x,50,x+3,46);
  }
  oled.setFont(u8g2_font_6x10_tf);
  drawCentered("ALL SET!",63);
  oled.sendBuffer();
  delay(900);
}

void processCalibration() {

  // ----------------------------------------------------------
  // CAL_WAIT: calibration input is completely disabled.
  // ----------------------------------------------------------
  if (state == CAL_WAIT) {
    if (millis() < calSwitchAt) return;

    if (calPlayer >= 4) {
      Serial.println();
      Serial.println("================================");
      Serial.println("=== CALIBRATION COMPLETE ===");
      Serial.println("Calibration input CLOSED.");
      Serial.println("================================");

      for (uint8_t p = 0; p < 4; p++) {
        Serial.print("P"); Serial.print(p + 1);
        Serial.print(" baseline="); Serial.print(baseline[p]);
        Serial.print(" threshold="); Serial.println(threshold[p]);
      }

      showCalibrationSummary();
      startReady();
      return;
    }

    state = CAL_TAPS;
    calTap = 0;
    lastTap = millis();
    drawCalibration();

    Serial.println();
    Serial.println("--------------------------------");
    Serial.print("CALIBRATING PLAYER ");
    Serial.println(calPlayer + 1);
    Serial.print("ONLY PLAYER ");
    Serial.print(calPlayer + 1);
    Serial.println(" SHOULD TAP.");
    Serial.println("Other player taps will be rejected.");
    Serial.println("--------------------------------");
    return;
  }

  // ----------------------------------------------------------
  // BASELINE
  // ----------------------------------------------------------
  if (state == CAL_BASE) {
    Serial.println();
    Serial.println("=== BASELINE ===");
    Serial.println("Do not tap any piezo.");

    for (uint8_t p = 0; p < 4; p++) {
      baseline[p] = baselineRead(p);
      Serial.print("P"); Serial.print(p + 1);
      Serial.print(" baseline="); Serial.println(baseline[p]);
    }

    state = CAL_TAPS;
    calPlayer = 0;
    calTap = 0;
    lastTap = millis();
    drawCalibration();

    Serial.println();
    Serial.println("================================");
    Serial.println("CALIBRATING PLAYER 1");
    Serial.println("ONLY PLAYER 1 SHOULD TAP.");
    Serial.println("================================");
    return;
  }

  if (calPlayer >= 4) {
    showCalibrationSummary();
    startReady();
    return;
  }

  // One physical tap cannot count twice.
  if (millis() - lastTap < CAL_TAP_LOCKOUT) return;

  // Look for a fresh event on the player currently being calibrated.
  const uint8_t target = calPlayer;
  bool found = false;
  uint16_t targetPeak = 0;

  for (uint8_t i = 0; i < frameEventCount; i++) {
    if (frameEvents[i].ch == target) {
      targetPeak = frameEvents[i].strength;
      found = true;
      break;
    }
  }

  if (!found) return;

  if (!evaluateCalibrationTap(target, targetPeak)) return;

  // ----------------------------------------------------------
  // ACCEPTED
  // ----------------------------------------------------------
  lastTap = millis();
  tapPeaks[target][calTap] = targetPeak;
  calTap++;

  Serial.print("ACCEPTED: PLAYER "); Serial.print(target + 1);
  Serial.print(" TAP "); Serial.print(calTap);
  Serial.print("/"); Serial.print(TAPS_REQUIRED);
  Serial.print(" peak="); Serial.println(targetPeak);

  flashPixel(pixel.Color(PIXEL_BRIGHT, PIXEL_BRIGHT, PIXEL_BRIGHT), 70, pixel.Color(0,0,0));

  if (calTap >= TAPS_REQUIRED) {
    uint32_t sum = 0;
    for (uint8_t i = 0; i < TAPS_REQUIRED; i++) sum += tapPeaks[target][i];
    uint16_t avg = sum / TAPS_REQUIRED;

    uint16_t th = (uint16_t)(avg * 0.30f);
    if (th < MIN_TAP_LEVEL) th = MIN_TAP_LEVEL;

    uint16_t baselineRequired = baseline[target] + BASELINE_MARGIN;
    if (th < baselineRequired) th = baselineRequired;
    if (th > MAX_TAP_THRESHOLD) th = MAX_TAP_THRESHOLD;

    threshold[target] = th;

    Serial.println();
    Serial.print("P"); Serial.print(target + 1); Serial.println(" CALIBRATION COMPLETE");
    Serial.print("  Average="); Serial.println(avg);
    Serial.print("  Final threshold="); Serial.println(th);

    calPlayer++;
    calTap = 0;

    if (calPlayer < 4) {
      state = CAL_WAIT;
      calSwitchAt = millis() + CAL_SWITCH_DELAY;

      if (displayOK) {
        oled.clearBuffer();
        oled.setFont(u8g2_font_7x14B_tf);
        drawCentered("GOOD!", 18);
        oled.setFont(u8g2_font_6x10_tf);
        char msg[22];
        snprintf(msg, sizeof(msg), "PLAYER %d NEXT", calPlayer + 1);
        drawCentered(msg, 38);
        drawCentered("GET READY...", 53);
        oled.sendBuffer();
      }

      Serial.print("Next: ONLY P"); Serial.print(calPlayer + 1); Serial.println(" may tap.");
    } else {
      state = CAL_WAIT;
      calSwitchAt = millis() + 250;
      Serial.println("P4 FINISHED. Calibration input CLOSED.");
    }
  } else {
    drawCalibration();
  }
}

// ============================================================
// READY / GAME START
// ============================================================
void startReady() {
  state=READY;
  stateStart=millis();
  lastTap=0;
  ledSet(false);
  setPixel(pixel.Color(0,0,PIXEL_DIM));

  if (!displayOK) return;
  oled.clearBuffer();
  oled.setFont(u8g2_font_6x10_tf);
  drawCentered("READY",9);
  for (uint8_t p=0;p<4;p++) drawPlayerSlot(p,false,false);
  drawBomb(64,34,true);
  drawCentered("TAP ANY SENSOR",62);
  oled.sendBuffer();

  Serial.println("=== READY: TAP ANY PLAYER ===");
}

// Looks at this frame's freshly-arrived events (already drained
// into frameEvents by drainFifo()) rather than sampling the
// piezos itself - Core 1 never touches the ADC.
int detectAnyTapFromEvents(uint16_t *outPeak) {
  if (millis()-lastTap < TAP_COOLDOWN) return -1;

  int best=-1;
  uint16_t bestMargin=0;
  uint16_t bestPeak=0;

  for (uint8_t i=0;i<frameEventCount;i++) {
    uint8_t ch = frameEvents[i].ch;
    uint16_t v = frameEvents[i].strength;
    if (v >= threshold[ch]) {
      uint16_t margin = v - threshold[ch];
      if (best<0 || margin>bestMargin) {
        best = ch;
        bestMargin = margin;
        bestPeak = v;
      }
    }
  }

  if (best>=0) {
    lastTap=millis();
    if (outPeak) *outPeak = bestPeak;
    Serial.print("TAP P"); Serial.print(best+1);
    Serial.print(" peak="); Serial.print(bestPeak);
    Serial.print(" threshold="); Serial.println(threshold[best]);
  }
  return best;
}

void startGame(uint8_t starter) {
  for (uint8_t i=0;i<4;i++) {
    alive[i]=true;
    stunned[i]=false;
    stunUntil[i]=0;
  }

  currentPlayer=starter;
  previousPlayer=starter;
  bombDuration=random(MIN_BOMB,MAX_BOMB+1);
  bombStart=millis();
  lastTap=millis();

  state=PLAYING;
  stateStart=millis();
  nextBeep=0;

  enterPlayingEffects();

  Serial.println("\n=== GAME START ===");
  Serial.print("Starting Player: P"); Serial.println(currentPlayer+1);
  Serial.print("Hidden bomb time: "); Serial.println(bombDuration);
  beepTap();

  if (displayOK) {
    oled.clearBuffer();
    for (uint8_t p=0;p<4;p++) drawPlayerSlot(p,p==currentPlayer,false);
    drawBomb(64,34,true);
    oled.sendBuffer();
  }
}

void enterPlayingEffects() {
  ledSet(true);
  setPixel(playerColor(currentPlayer,PIXEL_DIM));
}

// ============================================================
// GAMEPLAY
// ============================================================
void processGame() {
  unsigned long now=millis();

  if (now-bombStart>=bombDuration) {
    startExplosion();
    return;
  }

  for (uint8_t p=0;p<4;p++) {
    if (stunned[p] && now>=stunUntil[p]) stunned[p]=false;
  }

  uint16_t tapPeak;
  int tap = detectAnyTapFromEvents(&tapPeak);

  if (tap>=0 && alive[tap]) {
    if (tap==currentPlayer) {
      uint8_t next=randomOtherAlive(currentPlayer);
      flashPixel(pixel.Color(PIXEL_BRIGHT,PIXEL_BRIGHT,PIXEL_BRIGHT),
                 70,playerColor(next,PIXEL_DIM));
      startPassing(next);
      return;
    } else {
      // Wrong player touched the sensor: brief stun, bomb does not move.
      stunned[tap]=true;
      stunUntil[tap]=now+STUN_TIME;
      beepStun();
      flashPixel(pixel.Color(PIXEL_BRIGHT,0,0),90,playerColor(currentPlayer,PIXEL_DIM));
      Serial.print("P"); Serial.print(tap+1);
      Serial.println(" stunned - not the bomb holder.");
    }
  }

  // Near the hidden explosion, increase sound urgency only.
  unsigned long elapsed=now-bombStart;
  unsigned long remaining=(elapsed>=bombDuration)?0:(bombDuration-elapsed);
  if (remaining<=3000 && now>=nextBeep) {
    tone(BUZZER,1700,35);
    flashPixel(pixel.Color(PIXEL_BRIGHT,0,0),45,playerColor(currentPlayer,PIXEL_DIM));
    unsigned long interval=map(remaining,0,3000,75,260);
    nextBeep=now+interval;
  }

  drawGameplay();
}

void drawGameplay() {
  if (!displayOK) return;
  oled.clearBuffer();

  for (uint8_t p=0;p<4;p++) {
    drawPlayerSlot(p,p==currentPlayer,!alive[p]);
    if (stunned[p] && alive[p]) {
      oled.setFont(u8g2_font_5x7_tf);
      oled.drawStr(PX[p]+20,PY[p]+7,"Z");
    }
  }

  drawBomb(64,34,true);
  oled.sendBuffer();
}

// ============================================================
// PASSING
// ============================================================
void startPassing(uint8_t next) {
  previousPlayer=currentPlayer;
  currentPlayer=next;
  state=PASSING;
  stateStart=millis();
  beepPass();
}

int arcY(int y1,int y2,float t) {
  int base=y1+(int)((y2-y1)*t);
  return base-(int)(13.0f*4.0f*t*(1.0f-t));
}

void processPassing() {
  unsigned long e=millis()-stateStart;
  float t=(float)e/(float)PASS_TIME;
  if (t>1.0f) t=1.0f;

  int x1=PX[previousPlayer]+14;
  int y1=PY[previousPlayer]+9;
  int x2=PX[currentPlayer]+14;
  int y2=PY[currentPlayer]+9;

  int x=x1+(int)((x2-x1)*t);
  int y=arcY(y1,y2,t);

  if (displayOK) {
    oled.clearBuffer();
    for (uint8_t p=0;p<4;p++) drawPlayerSlot(p,false,!alive[p]);

    for (uint8_t i=0;i<7;i++) {
      float q=t*i/7.0f;
      if (q>=t) break;
      oled.drawPixel(x1+(int)((x2-x1)*q),arcY(y1,y2,q));
    }

    drawBomb(x,y,false);
    oled.sendBuffer();
  }

  if (e>=PASS_TIME) {
    bombStart=millis();
    bombDuration=random(MIN_BOMB,MAX_BOMB+1);
    lastTap=millis();
    nextBeep=0;
    state=PLAYING;
    enterPlayingEffects();

    Serial.print("Bomb passed to P"); Serial.println(currentPlayer+1);
  }
}

// ============================================================
// EXPLOSION / ELIMINATION / WIN
// ============================================================
void startExplosion() {
  state=EXPLOSION;
  explosionStart=millis();
  beepExplosion();
}

void processExplosion() {
  unsigned long e=millis()-explosionStart;
  uint8_t frame=e/EXP_FRAME;

  if (frame>=EXP_FRAMES) {
    alive[currentPlayer]=false;
    stunned[currentPlayer]=false;

    Serial.print("P"); Serial.print(currentPlayer+1);
    Serial.println(" ELIMINATED");

    ledSet(false);
    setPixel(pixel.Color(0,0,0));

    if (onlyOneAlive()) {
      winnerPlayer=lastAlive();
      startWin();
    } else {
      startEliminated();
    }
    return;
  }

  ledSet((frame&1)==0);
  setPixel((frame&1)==0 ? pixel.Color(PIXEL_BRIGHT,0,0)
                         : pixel.Color(PIXEL_BRIGHT,PIXEL_BRIGHT,PIXEL_BRIGHT));

  if (displayOK) {
    oled.clearBuffer();
    for (uint8_t p=0;p<4;p++)
      if (p!=currentPlayer) drawPlayerSlot(p,false,!alive[p]);

    drawBurst(64,34,frame);

    if (frame>=4) {
      oled.setFont(u8g2_font_7x14B_tf);
      drawCentered("BOOM!",57);
    }
    oled.sendBuffer();
  }
}

void startEliminated() {
  state=ELIMINATED;
  stateStart=millis();

  if (!displayOK) return;
  oled.clearBuffer();

  for (uint8_t p=0;p<4;p++)
    if (p!=currentPlayer) drawPlayerSlot(p,false,!alive[p]);

  oled.drawCircle(64,24,11);
  drawX(60,21,2);
  drawX(68,21,2);
  oled.drawLine(60,29,68,29);

  oled.setFont(u8g2_font_6x10_tf);
  char s[18];
  snprintf(s,sizeof(s),"P%d OUT",currentPlayer+1);
  drawCentered(s,46);

  oled.setFont(u8g2_font_5x7_tf);
  drawCentered("PASS CONTINUES",61);
  oled.sendBuffer();
}

void processEliminated() {
  if (millis()-stateStart>=ELIM_TIME) {
    currentPlayer=randomOtherAlive(255);
    bombStart=millis();
    bombDuration=random(MIN_BOMB,MAX_BOMB+1);
    lastTap=millis();
    nextBeep=0;
    state=PLAYING;
    enterPlayingEffects();
    beepNext();
  }
}

void startWin() {
  state=WIN;
  stateStart=millis();
  ledSet(true);
  beepWin();

  if (!displayOK) return;
  oled.clearBuffer();

  for (uint8_t p=0;p<4;p++)
    if (p!=winnerPlayer) drawPlayerSlot(p,false,!alive[p]);

  drawPlayerIcon(64,23,false,true);

  oled.setFont(u8g2_font_7x14B_tf);
  char s[14];
  snprintf(s,sizeof(s),"P%d WINS!",winnerPlayer+1);
  drawCentered(s,46);

  oled.setFont(u8g2_font_5x7_tf);
  drawCentered("* * *",60);
  oled.sendBuffer();
}

void processWin() {
  static unsigned long pulse=0;
  static bool bright=false;
  if (millis()>=pulse) {
    bright=!bright;
    setPixel(playerColor(winnerPlayer,bright?PIXEL_BRIGHT:PIXEL_DIM));
    pulse=millis()+250;
  }

  if (millis()-stateStart>=WIN_TIME) {
    showTitle();
  }
}

// ============================================================
// GRAPHICS
// ============================================================
void drawPlayerIcon(int cx,int cy,bool elim,bool active) {
  if (active) {
    oled.drawFrame(cx-11,cy-9,22,19);
  }

  if (elim) {
    oled.drawCircle(cx,cy,5);
    drawX(cx,cy,3);
    return;
  }

  oled.drawBox(cx-3,cy-7,7,6);
  oled.drawBox(cx-5,cy,11,8);
  oled.drawBox(cx-7,cy+2,2,5);
  oled.drawBox(cx+5,cy+2,2,5);
  oled.drawBox(cx-5,cy+8,4,5);
  oled.drawBox(cx+1,cy+8,4,5);

  if (!active) {
    oled.drawPixel(cx-1,cy-5);
    oled.drawPixel(cx+2,cy-5);
  }
}

void drawPlayerSlot(uint8_t p,bool holder,bool elim) {
  int cx=PX[p]+14;
  int cy=PY[p]+8;

  oled.setFont(u8g2_font_5x7_tf);
  char label[4];
  snprintf(label,sizeof(label),"P%d",p+1);

  if (p==0 || p==3) oled.drawStr(PX[p],PY[p]+6,label);
  else {
    int w=oled.getStrWidth(label);
    oled.drawStr(PX[p]+28-w,PY[p]+6,label);
  }

  drawPlayerIcon(cx,cy,elim,holder);
}

void drawBomb(int x,int y,bool burst) {
  oled.drawDisc(x,y,5);
  oled.drawLine(x+3,y-5,x+6,y-9);
  oled.drawPixel(x+7,y-10);

  if (burst) {
    oled.drawLine(x-10,y,x-7,y);
    oled.drawLine(x+7,y,x+10,y);
    oled.drawLine(x,y-10,x,y-7);
    oled.drawLine(x,y+7,x,y+10);
    oled.drawPixel(x-7,y-7);
    oled.drawPixel(x+7,y-7);
    oled.drawPixel(x-7,y+7);
    oled.drawPixel(x+7,y+7);
  }
}

void drawBurst(int cx,int cy,uint8_t frame) {
  if (frame<2) {
    int r=6+frame*7;
    oled.drawCircle(cx,cy,r);
    oled.drawCircle(cx,cy,r/2);
  } else {
    uint8_t rays=12;
    int len=8+(frame-2)*4;
    for (uint8_t i=0;i<rays;i++) {
      int dx=0,dy=0;
      switch(i) {
        case 0: dx=len; break;
        case 1: dx=len*3/4; dy=len*3/4; break;
        case 2: dy=len; break;
        case 3: dx=-len*3/4; dy=len*3/4; break;
        case 4: dx=-len; break;
        case 5: dx=-len*3/4; dy=-len*3/4; break;
        case 6: dy=-len; break;
        case 7: dx=len*3/4; dy=-len*3/4; break;
        case 8: dx=len*2/3; dy=len/3; break;
        case 9: dx=-len*2/3; dy=len/3; break;
        case 10: dx=-len*2/3; dy=-len/3; break;
        default: dx=len*2/3; dy=-len/3; break;
      }
      oled.drawLine(cx,cy,cx+dx,cy+dy);
    }
  }
}

void drawX(int cx,int cy,int s) {
  oled.drawLine(cx-s,cy-s,cx+s,cy+s);
  oled.drawLine(cx-s,cy+s,cx+s,cy-s);
}

void drawCentered(const char *s,int y) {
  int w=oled.getStrWidth(s);
  oled.drawStr((128-w)/2,y,s);
}

// ============================================================
// LED / NEOPIXEL
// ============================================================
void ledSet(bool on) {
  digitalWrite(LED_BUILTIN,on?LOW:HIGH);
}

uint32_t playerColor(uint8_t p,uint8_t b) {
  switch(p) {
    case 0: return pixel.Color(b,0,0);
    case 1: return pixel.Color(0,b,0);
    case 2: return pixel.Color(0,0,b);
    default:return pixel.Color(b,b,0);
  }
}

void setPixel(uint32_t c) {
  pixel.setPixelColor(0,c);
  pixel.show();
}

void flashPixel(uint32_t c,unsigned long ms,uint32_t restore) {
  setPixel(c);
  pixelFlash=true;
  pixelOff=millis()+ms;
  pixelRestore=restore;
}

void updatePixel() {
  if (pixelFlash && millis()>=pixelOff) {
    setPixel(pixelRestore);
    pixelFlash=false;
  }
}

// ============================================================
// SOUND
// ============================================================
void beepTap()       { tone(BUZZER,1200,55); }
void beepPass()      { tone(BUZZER,900,70); }
void beepStun()      { tone(BUZZER,350,90); }
void beepExplosion() {
  tone(BUZZER,1800,70); delay(80);
  tone(BUZZER,1000,80); delay(90);
  tone(BUZZER,400,160);
}
void beepNext() {
  tone(BUZZER,1000,50); delay(70);
  tone(BUZZER,1300,70);
}
void beepWin() {
  tone(BUZZER,1000,80); delay(90);
  tone(BUZZER,1400,90); delay(100);
  tone(BUZZER,1800,160);
}

// ============================================================
// PLAYER HELPERS / RANDOM
// ============================================================
bool onlyOneAlive() {
  uint8_t n=0;
  for (uint8_t i=0;i<4;i++) if (alive[i]) n++;
  return n==1;
}

uint8_t lastAlive() {
  for (uint8_t i=0;i<4;i++) if (alive[i]) return i;
  return 0;
}

uint8_t randomOtherAlive(uint8_t from) {
  uint8_t list[4];
  uint8_t n=0;
  for (uint8_t i=0;i<4;i++)
    if (alive[i] && i!=from) list[n++]=i;

  if (n==0) return from;
  return list[random(0,n)];
}

// Seeds from Core 0's continuously-refreshed snapshot plus micros() -
// no direct ADC access from Core 1.
void seedRandom() {
  uint32_t seed=micros();
  for (uint8_t i=0;i<4;i++) {
    seed ^= ((uint32_t)currentReading[i] << (i*8));
  }
  randomSeed(seed);
}
