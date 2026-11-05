#pragma once

// The board's USB port is either the serial/JTAG console or TinyUSB's device
// (the card reader, the microphone); TinyUSB cannot hand it back, so leaving
// one of those modes restarts the board.

void usbPortRestart();       // restarts into the app with the serial port back; does not return
void usbPortRestoreAtBoot(); // first thing at boot: the port back on the console, should a USB mode have crashed
