/*
    badBOX by Richard Gladson
    https://github.com/RichardGladson/badBOX
    Copyright (c) 2026 Richard Gladson

    Licensed under the GNU General Public License v3.0
    https://www.gnu.org/licenses/gpl-3.0.html

    SPDX-License-Identifier: GPL-3.0-only
*/

#ifndef analyzer_H
#define analyzer_H

#include <SPI.h>
#include <U8g2lib.h>
#include "esp_bt.h"
#include "esp_wifi.h"
#include "neopixel.h"

void analyzerSetup();
void analyzerLoop();

#endif
