# Ruben's Status

A live "where's Ruben" page. Anyone with the link sees the current status update instantly. Ruben changes it from the web page or from a touchscreen ESP32 on his desk.

## How it fits together

```
 Web page (GitHub Pages)          CYD touchscreen (ESP32)
   - everyone: read live            - shows current status
   - Ruben at /#admin: edit         - tap a button to change it
              \                       /
               \                     /
            Firebase Realtime Database
              /status  (one record)
              read: anyone   write: only Ruben's login
```

- **Firebase** holds one record: the current status. It's the single source of truth.
- **The web page** listens to that record and redraws the moment it changes. Visitors need no account.
- **Editing on the web**: go to `https://stonecoldos.github.io/status/#admin`, sign in, pick a status or write your own.
- **The CYD** checks the record every 3 seconds and redraws if it changed. Tapping a button writes a new status, which every open web page picks up within a second.
- **Security**: the database rules allow writes only from your Firebase login. The web page and the CYD sign in as that same user.

## Offline option: Status Terminal (CYD only)

`esp32/StatusTerminal/StatusTerminal.ino` is a standalone version with no Wi-Fi, Firebase or web page. It shows a terminal-style screen with HERE / AWAY / BUSY / LUNCH buttons, a blinking cursor and a "for 00:12:34" timer since the last change. The last status is saved to flash and restored on boot.

First boot asks you to tap 3 crosses to calibrate touch. Hold a finger on the screen for 5 seconds to recalibrate later.

To flash a prebuilt `.bin` with https://espressif.github.io/esptool-js/: connect, add the file at address `0x0`, Program. To build it yourself: steps 1-3 and 5 of the CYD section below (no `secrets.h` needed).

## One-time setup

### 1. Firebase (about 5 minutes)
1. Go to https://console.firebase.google.com and create a project (Analytics not needed).
2. **Build > Realtime Database > Create database**. Pick a US location, start in **locked mode**.
3. **Build > Authentication > Get started > Email/Password > Enable**.
4. **Authentication > Users > Add user**. Use your email and a password used nowhere else (the CYD stores it). Copy the **User UID**.
5. **Realtime Database > Rules**: paste `database.rules.json` from this repo, replace `PASTE_YOUR_UID` with your UID, **Publish**.
6. **Project settings (gear) > Your apps > Web (`</>`)**. Register an app (no hosting). Copy the config values into `firebase-config.js` and commit.

### 2. GitHub Pages
**Settings > Pages > Deploy from a branch > `main` / root.** The site is `https://stonecoldos.github.io/status/`.

### 3. CYD (ESP32-2432S028R)
1. Arduino IDE with the ESP32 board package. Board: **ESP32 Dev Module**.
2. Install libraries: **TFT_eSPI**, **XPT2046_Touchscreen** (Paul Stoffregen), **ArduinoJson** (v7).
3. Configure TFT_eSPI for the CYD: replace `User_Setup.h` in the TFT_eSPI library folder with the CYD version from https://github.com/witnessmenow/ESP32-Cheap-Yellow-Display (`DisplayConfig/User_Setup.h`).
4. Copy `esp32/StatusBoard/secrets.example.h` to `secrets.h` in the same folder and fill it in. It's git-ignored.
5. Upload `esp32/StatusBoard/StatusBoard.ino`.

If taps land on the wrong button, open Serial Monitor, print `p.x`/`p.y` in `handleTouch()`, and adjust the `TOUCH_*` values.

## Changing the presets
The six presets are listed in two places: `PRESETS` in `index.html` and `PRESETS` in `StatusBoard.ino`. Change both, and update the `key` list in `database.rules.json` if you add or rename one.

## Notes
- The CYD skips HTTPS certificate checks (`setInsecure()`) to keep things simple. Traffic is still encrypted. Fine for a status board on your own network.
- Firebase's free Spark plan covers this many times over.
