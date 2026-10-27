#pragma once

#include <stdint.h>

// File transfer over Wi-Fi: a small web page listing the moments, with
// downloads, deleting, and setting the clock from the browser's time.
// Only runs when started from Settings. It joins the network in
// wifi_config.h, or, when that isn't in reach, opens its own hotspot
// "zvukopis". Stops after 10 minutes without a request.

enum TransferState {
    TRANSFER_OFF,
    TRANSFER_CONNECTING,
    TRANSFER_READY,
};

struct TransferInfo {
    TransferState state;
    bool hotspot;       // its own network rather than the configured one
    char network[33];   // network name to join
    char password[33];  // hotspot password (empty when joining the configured network)
    char address[16];   // the page is at http://address/
    uint32_t requests;  // pages and files served so far
};

void transferStart();
void transferStop(); // returns once Wi-Fi is off
TransferInfo transferInfo();
