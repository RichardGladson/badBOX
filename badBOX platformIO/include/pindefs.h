/*
    badBOX by Richard Gladson
    https://github.com/RichardGladson/badBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/

// pindefs.h
#ifndef PINDEFS_H
#define PINDEFS_H

// Button Pin Definitions
#define BUTTON_PIN_UP      26
#define BUTTON_PIN_DOWN    33
#define BUTTON_PIN_CENTER  32  // Exit
#define BUTTON_PIN_LEFT    25  // Back
#define BUTTON_PIN_RIGHT   27  // Select

// Radio Pins (single NRF24 module)
#define RADIO_CE_PIN_1      5
#define RADIO_CSN_PIN_1    17

// NeoPixel
#define NEOPIXEL_PIN       14

// RC522 RFID (shared SPI bus: SCK=18, MISO=19, MOSI=23)
// Moved off 21/22 to free I2C for the OLED. Do not use GPIO2.
// Free GPIOs chosen: 15 (SS), 16 (RST) — not used by buttons, NRF24, NeoPixel, or SPI.
#define RFID_SS_PIN        15
#define RFID_RST_PIN       16

#endif // PINDEFS_H
