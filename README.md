# Pass-the-Bomb-Game
A 4‑player interactive bomb‑passing game using Seeed XIAO RP2040, piezo sensors, OLED, and buzzer.

## Hardware
- Seeed XIAO RP2040
- 1.3" SH1106 OLED (128x64)
- 4 Piezoelectric sensors
- Buzzer
- NeoPixel LED

## Features
- Player calibration
- Randomized bomb timer
- Passing animation
- Stun mechanic for incorrect taps
- Explosion & elimination sequence
- Win detection

## Setup
1. Install Arduino IDE with RP2040 board support.
2. Add required libraries:
   - `Wire.h`
   - `U8g2lib.h`
   - `Adafruit_NeoPixel.h`
3. Upload the code to your XIAO RP2040.
4. Connect hardware as described in the code comments.

## How to Play
- Each player taps their sensor to pass the bomb.
- Wrong taps cause a stun.
- The bomb explodes randomly — eliminating the holder.
- Last player standing wins!
