#include <Wire.h>
#include <U8g2lib.h>
#include <Adafruit_NeoPixel.h>

// ============================================================
// PASS THE BOMB - XIAO RP2040
// 1.3" SH1106 128x64 OLED + 4 piezos + buzzer + NeoPixel
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
const uint16_t CAL_WINDOW = 45;
const uint16_t GAME_WINDOW = 18;

const unsigned long MIN_BOMB = 300;
const unsigned long MAX_BOMB = 5000;
const unsigned long PASS_TIME = 420;
const unsigned long STUN_TIME = 3000;
const unsigned long EXP_FRAME = 90;
const uint8_t EXP_FRAMES = 7;
const unsigned long TITLE_TIME = 2000;
const unsigned long ELIM_TIME = 1500;
const unsigned long WIN_TIME = 3500;
const unsigned long CAL_SWITCH_DELAY = 900; // pause before moving to the next player

// Calibration isolation: only the assigned player's piezo may advance calibration.
// All four piezos are sampled and the assigned channel must clearly dominate.
const uint16_t CAL_CROSS_MARGIN = 50;
const uint8_t CAL_DOM_NUM = 5;       // target >= 1.25x strongest other sensor
const uint8_t CAL_DOM_DEN = 4;
const unsigned long CAL_TAP_LOCKOUT = 300;

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

// ---------- declarations ----------
void showTitle();
void startCalibration();
void processCalibration();
void drawCalibration();
void showCalibrationSummary();

uint16_t peakRead(uint8_t p, unsigned long ms);
uint16_t baselineRead(uint8_t p);
void readAllCalibrationPeaks(uint16_t peaks[4], unsigned long ms);
bool calibrationTapIsForPlayer(const uint16_t peaks[4], uint8_t target);
int detectAnyTap();

void startReady();
void processReady(int &tapResult);
void startGame(uint8_t starter);
void processGame();

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
// SETUP
// ============================================================
void setup() {
  Serial.begin(115200);

  Serial.println();
  Serial.println("=== PASS THE BOMB HARDWARE CHECK ===");
  Serial.println("OLED I2C: D4/GPIO6 SDA, D5/GPIO7 SCL");
  Serial.println("BUZZER:   D6/GPIO0");
  Serial.println("IMPORTANT: buzzer no longer shares OLED SDA");

  analogReadResolution(10);

  for (uint8_t i=0;i<4;i++) pinMode(PIEZO[i], INPUT);

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
// LOOP
// ============================================================
void loop() {
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
      int t = detectAnyTap();
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

uint16_t baselineRead(uint8_t p) {
  unsigned long st=millis();
  uint32_t total=0;
  uint16_t n=0;
  while (millis()-st < 220) {
    total += analogRead(PIEZO[p]);
    n++;
    delayMicroseconds(500);
  }
  return n ? total/n : 0;
}

uint16_t peakRead(uint8_t p, unsigned long ms) {
  uint16_t peak=0;
  unsigned long st=micros();
  while (micros()-st < ms*1000UL) {
    uint16_t v=analogRead(PIEZO[p]);
    if (v>peak) peak=v;
  }
  return peak;
}

// ============================================================
// CALIBRATION INPUT ISOLATION
// ============================================================
// Read ALL FOUR sensors during calibration. Reading only the requested
// player's sensor allows a different player's mechanical tap to appear
// as a valid tap through cross-coupling.
void readAllCalibrationPeaks(uint16_t peaks[4], unsigned long ms) {
  for (uint8_t p = 0; p < 4; p++) peaks[p] = 0;

  unsigned long st = micros();
  while (micros() - st < ms * 1000UL) {
    for (uint8_t p = 0; p < 4; p++) {
      uint16_t v = analogRead(PIEZO[p]);
      if (v > peaks[p]) peaks[p] = v;
    }
  }
}

// The assigned player's sensor must be both above its own threshold and
// clearly stronger than every other sensor.
bool calibrationTapIsForPlayer(const uint16_t peaks[4], uint8_t target) {
  uint16_t targetPeak = peaks[target];
  uint16_t strongestOther = 0;

  for (uint8_t p = 0; p < 4; p++) {
    if (p == target) continue;
    if (peaks[p] > strongestOther) strongestOther = peaks[p];
  }

  uint16_t required = baseline[target] + BASELINE_MARGIN;
  if (required < MIN_TAP_LEVEL) required = MIN_TAP_LEVEL;

  if (targetPeak < required) return false;
  if (targetPeak < strongestOther + CAL_CROSS_MARGIN) return false;

  return ((uint32_t)targetPeak * CAL_DOM_DEN >=
          (uint32_t)strongestOther * CAL_DOM_NUM);
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

  // ----------------------------------------------------------
  // KEY FIX: SAMPLE ALL FOUR PIEZOS, NOT JUST calPlayer.
  // ----------------------------------------------------------
  uint16_t peaks[4];
  readAllCalibrationPeaks(peaks, CAL_WINDOW);

  const uint8_t target = calPlayer;
  const uint16_t targetPeak = peaks[target];

  uint16_t strongestOther = 0;
  uint8_t strongestOtherPlayer = 255;

  for (uint8_t p = 0; p < 4; p++) {
    if (p == target) continue;
    if (peaks[p] > strongestOther) {
      strongestOther = peaks[p];
      strongestOtherPlayer = p;
    }
  }

  uint16_t required = baseline[target] + BASELINE_MARGIN;
  if (required < MIN_TAP_LEVEL) required = MIN_TAP_LEVEL;

  bool targetStrongEnough = targetPeak >= required;
  bool targetHasMargin = targetPeak >= strongestOther + CAL_CROSS_MARGIN;
  bool targetHasDominance =
    ((uint32_t)targetPeak * CAL_DOM_DEN >=
     (uint32_t)strongestOther * CAL_DOM_NUM);

  // Diagnostic output whenever any sensor sees a meaningful signal.
  if (targetPeak >= MIN_TAP_LEVEL || strongestOther >= MIN_TAP_LEVEL) {
    Serial.print("CAL P");
    Serial.print(target + 1);
    Serial.print(" peaks: ");

    for (uint8_t p = 0; p < 4; p++) {
      Serial.print("P"); Serial.print(p + 1);
      Serial.print("="); Serial.print(peaks[p]);
      if (p < 3) Serial.print("  ");
    }
    Serial.println();

    // If another player's sensor is stronger, this tap belongs to that
    // player, so it MUST NOT advance the currently requested player.
    if (strongestOtherPlayer != 255 && strongestOther > targetPeak) {
      Serial.print("REJECTED: tap belongs to P");
      Serial.print(strongestOtherPlayer + 1);
      Serial.print("; only P");
      Serial.print(target + 1);
      Serial.println(" may calibrate now.");
      return;
    }

    // If the target is strong but the source is ambiguous, reject it.
    if (targetStrongEnough && (!targetHasMargin || !targetHasDominance)) {
      Serial.print("REJECTED: P");
      Serial.print(target + 1);
      Serial.println(" signal is ambiguous/cross-coupled.");
      Serial.print("Target="); Serial.print(targetPeak);
      Serial.print(" OtherMax="); Serial.print(strongestOther);
      Serial.print(" Required="); Serial.println(required);
      return;
    }
  }

  // ----------------------------------------------------------
  // ACCEPT ONLY THE ASSIGNED PLAYER'S SENSOR.
  // ----------------------------------------------------------
  if (targetStrongEnough && targetHasMargin && targetHasDominance) {
    lastTap = millis();
    tapPeaks[target][calTap] = targetPeak;
    calTap++;

    Serial.print("ACCEPTED: PLAYER ");
    Serial.print(target + 1);
    Serial.print(" TAP ");
    Serial.print(calTap);
    Serial.print("/");
    Serial.print(TAPS_REQUIRED);
    Serial.print(" peak=");
    Serial.println(targetPeak);

    flashPixel(
      pixel.Color(PIXEL_BRIGHT, PIXEL_BRIGHT, PIXEL_BRIGHT),
      70,
      pixel.Color(0, 0, 0)
    );

    // --------------------------------------------------------
    // PLAYER FINISHED ALL CALIBRATION TAPS
    // --------------------------------------------------------
    if (calTap >= TAPS_REQUIRED) {
      uint32_t sum = 0;
      for (uint8_t i = 0; i < TAPS_REQUIRED; i++) {
        sum += tapPeaks[target][i];
      }

      uint16_t avg = sum / TAPS_REQUIRED;

      uint16_t th = (uint16_t)(avg * 0.30f);
      if (th < MIN_TAP_LEVEL) th = MIN_TAP_LEVEL;

      uint16_t baselineRequired = baseline[target] + BASELINE_MARGIN;
      if (th < baselineRequired) th = baselineRequired;
      if (th > MAX_TAP_THRESHOLD) th = MAX_TAP_THRESHOLD;

      threshold[target] = th;

      Serial.println();
      Serial.print("P"); Serial.print(target + 1);
      Serial.println(" CALIBRATION COMPLETE");
      Serial.print("  Peaks: ");
      for (uint8_t i = 0; i < TAPS_REQUIRED; i++) {
        Serial.print(tapPeaks[target][i]);
        if (i < TAPS_REQUIRED - 1) Serial.print(", ");
      }
      Serial.println();
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

        Serial.print("Next: ONLY P");
        Serial.print(calPlayer + 1);
        Serial.println(" may tap.");
      } else {
        state = CAL_WAIT;
        calSwitchAt = millis() + 250;
        Serial.println("P4 FINISHED.");
        Serial.println("Calibration input CLOSED.");
      }
    } else {
      drawCalibration();
    }
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

  oled.setFont(u8g2_font_6x10_tf);
  drawCentered("TAP ANY SENSOR",62);
  oled.sendBuffer();

  Serial.println("=== READY: TAP ANY PLAYER ===");
}

int detectAnyTap() {
  if (millis()-lastTap<TAP_COOLDOWN) return -1;

  uint16_t peaks[4]={0,0,0,0};
  unsigned long st=micros();

  while (micros()-st < GAME_WINDOW*1000UL) {
    for (uint8_t p=0;p<4;p++) {
      uint16_t v=analogRead(PIEZO[p]);
      if (v>peaks[p]) peaks[p]=v;
    }
  }

  int best=-1;
  uint16_t bestMargin=0;

  for (uint8_t p=0;p<4;p++) {
    uint16_t needed=threshold[p];
    if (peaks[p]>=needed) {
      uint16_t margin=peaks[p]-needed;
      if (best<0 || margin>bestMargin) {
        best=p;
        bestMargin=margin;
      }
    }
  }

  if (best>=0) {
    lastTap=millis();
    Serial.print("TAP P"); Serial.print(best+1);
    Serial.print(" peak="); Serial.print(peaks[best]);
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

  // IMPORTANT: the player who tapped READY becomes the first holder.
  currentPlayer=starter;
  previousPlayer=starter;
  bombDuration=random(MIN_BOMB,MAX_BOMB+1);
  bombStart=millis();
  lastTap=millis(); // prevents the same physical tap from passing immediately

  state=PLAYING;
  stateStart=millis();
  nextBeep=0;

  Serial.println("OLED -> PLAYING screen");
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

  int tap=detectAnyTap();

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
      // small "Z" marker inside the player's own corner
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
  if (millis()>=pulse) {
    static bool bright=false;
    bright=!bright;
    setPixel(playerColor(winnerPlayer,bright?PIXEL_BRIGHT:PIXEL_DIM));
    pulse=millis()+250;
  }

  if (millis()-stateStart>=WIN_TIME) {
    showTitle();
  }
}

// ============================================================
// GRAPHICS - deliberately separated into non-overlapping regions
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

  // compact pixel-art person: 11x15, fits corner slots
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

  // label sits at the outer edge, icon stays inside the slot
  if (p==0 || p==3) oled.drawStr(PX[p],PY[p]+6,label);
  else {
    int w=oled.getStrWidth(label);
    oled.drawStr(PX[p]+28-w,PY[p]+6,label);
  }

  drawPlayerIcon(cx,cy,elim,holder);
}

void drawBomb(int x,int y,bool burst) {
  // compact bomb: no text around it, so it stays inside the center zone
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

void seedRandom() {
  uint32_t seed=micros();
  for (uint8_t i=0;i<4;i++) {
    seed ^= ((uint32_t)analogRead(PIEZO[i]) << (i*8));
  }
  randomSeed(seed);
}
