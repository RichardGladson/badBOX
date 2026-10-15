/*
    badBOX — shared mode switch
    Combines doomBOX (security tools) and flipBOX (game launcher) into one
    firmware image. Exactly one of the two subsystems runs at a time; the
    active one is chosen at boot from a flag stored in its own dedicated NVS
    namespace, kept separate from doomBOX's EEPROM layout and flipBOX's own
    "flipbox" Preferences namespace so neither existing storage is touched.

    Licensed under the MIT License
    SPDX-License-Identifier: MIT
*/

#ifndef BADBOX_MODE_H
#define BADBOX_MODE_H

#include <Arduino.h>
#include <Preferences.h>

enum BadboxMode : uint8_t {
  BADBOX_MODE_TOOLS = 0,   // doomBOX — WiFi/BLE security tools
  BADBOX_MODE_GAMES = 1    // flipBOX — racing game launcher
};

// Dedicated namespace/key — isolated from both subsystems' own settings.
static inline const char* badboxPrefsNamespace() { return "badbox"; }
static inline const char* badboxModeKey()        { return "mode"; }

// Reads the persisted boot mode. Defaults to BADBOX_MODE_TOOLS (doomBOX)
// the very first time the device boots, before anything has been saved.
static inline BadboxMode badboxGetMode() {
  Preferences prefs;
  prefs.begin(badboxPrefsNamespace(), true /* read-only */);
  uint8_t raw = prefs.getUChar(badboxModeKey(), (uint8_t)BADBOX_MODE_TOOLS);
  prefs.end();
  return (raw == (uint8_t)BADBOX_MODE_GAMES) ? BADBOX_MODE_GAMES : BADBOX_MODE_TOOLS;
}

// Persists a new boot mode without rebooting.
static inline void badboxSetMode(BadboxMode mode) {
  Preferences prefs;
  prefs.begin(badboxPrefsNamespace(), false /* read-write */);
  prefs.putUChar(badboxModeKey(), (uint8_t)mode);
  prefs.end();
}

// Returns "the other" mode relative to the one currently running.
static inline BadboxMode badboxOtherMode(BadboxMode current) {
  return (current == BADBOX_MODE_TOOLS) ? BADBOX_MODE_GAMES : BADBOX_MODE_TOOLS;
}

// Persists newMode then restarts the MCU. The new main.cpp dispatcher reads
// the flag again on the way back up and boots straight into the other
// subsystem. A clean reboot — rather than a live in-place teardown of
// whichever radios/tasks/buffers the current subsystem has set up — is what
// keeps this swap reliable: doomBOX and flipBOX manage very different
// peripheral/task state and were never designed to hand off to one another
// without a fresh boot.
static inline void badboxSwitchAndReboot(BadboxMode newMode) {
  badboxSetMode(newMode);
  delay(60);           // let the NVS write flush before reset
  ESP.restart();
}

#endif // BADBOX_MODE_H
