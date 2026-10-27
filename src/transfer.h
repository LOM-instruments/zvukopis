#pragma once

#include <stdint.h>

// The web page, over Wi-Fi: the moments (download, delete), scheduled
// recordings, the categories, and setting the clock from the browser's time.
// Only runs when started from Settings. It joins the network in
// wifi_config.h, or opens its own hotspot "zvukopis" when asked to or when that
// network isn't in reach. Stops after 10 minutes without a request.

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

void transferStart(bool hotspot); // hotspot: its own network even if the usual one is near
void transferStop(); // returns once Wi-Fi is off
TransferInfo transferInfo();
