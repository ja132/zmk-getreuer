/*
 * Orbital Mouse behavior parameters.
 * ZMK port of https://getreuer.info/posts/keyboards/orbital-mouse/index.html
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#pragma once

/* Polar (tank) movement. */
#define OM_U 0 /* Move forward along the heading.       */
#define OM_D 1 /* Move backward.                        */
#define OM_L 2 /* Steer left (counter-clockwise).       */
#define OM_R 3 /* Steer right (clockwise).              */

/* Cardinal snapping: conventional up/down/left/right (diagonals combine). */
#define OM_CS_U 4
#define OM_CS_D 5
#define OM_CS_L 6
#define OM_CS_R 7

/* Mouse wheel. */
#define OM_W_U 8
#define OM_W_D 9
#define OM_W_L 10
#define OM_W_R 11

/* Speed modes (momentary while held). */
#define OM_SLOW 12
#define OM_FAST 13

/* Selected-button actions. */
#define OM_BTNS 14 /* Press the selected button while held. */
#define OM_DBLS 15 /* Double-click the selected button.     */
#define OM_HLDS 16 /* Hold the selected button down.        */
#define OM_RELS 17 /* Release the selected button.          */

/* Direct mouse buttons. ZMK's HID report exposes 5 buttons. */
#define OM_BTN1 32
#define OM_BTN2 33
#define OM_BTN3 34
#define OM_BTN4 35
#define OM_BTN5 36

/* Select which button OM_BTNS / OM_DBLS / OM_HLDS / OM_RELS act on. */
#define OM_SEL1 48
#define OM_SEL2 49
#define OM_SEL3 50
#define OM_SEL4 51
#define OM_SEL5 52
