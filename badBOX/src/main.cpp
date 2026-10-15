/*
    badBOX
    Combines doomBOX (WiFi/BLE security tools) and flipBOX (racing game
    launcher) into a single firmware image.

    This file is intentionally thin: it owns no menu, radio, or display
    logic of its own. At boot it reads which subsystem should be active
    (persisted in include/badBOX/badbox_mode.h's dedicated NVS namespace) and
    hands off entirely to that subsystem's setup()/loop(). The in-menu
    switch button (added to both subsystems' options screens) flips that
    flag and reboots, so exactly one subsystem's peripherals, radios and
    background tasks are ever initialized in a given run — the thing that
    keeps the swap bug-free.

    Licensed under the MIT License
    SPDX-License-Identifier: MIT
*/

#include <Arduino.h>
#include "badbox_mode.h"

// doomBOX (src/doomBOX/doombox_app.cpp)
void doomboxAppSetup();
void doomboxAppLoop();

// flipBOX (src/flipBOX/flipbox_app.cpp)
void flipboxAppSetup();
void flipboxAppLoop();

static BadboxMode activeMode = BADBOX_MODE_TOOLS;

void setup() {
  // Read once at boot; the switch button changes this via a fresh reboot,
  // so loop() never needs to re-check it mid-run.
  activeMode = badboxGetMode();

  if (activeMode == BADBOX_MODE_GAMES) {
    flipboxAppSetup();
  } else {
    doomboxAppSetup();
  }
}

void loop() {
  if (activeMode == BADBOX_MODE_GAMES) {
    flipboxAppLoop();
  } else {
    doomboxAppLoop();
  }
}
