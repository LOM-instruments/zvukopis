#pragma once

// USB card reader: the SD card shows up on the computer as a USB drive.
// The USB port then belongs to the card, so the serial port is gone until
// cardReaderLeave(), which restarts the board.

bool cardReaderStart(); // false when there's no card
bool cardReaderEjected(); // the computer ejected the drive
void cardReaderLeave();   // restarts; does not return
