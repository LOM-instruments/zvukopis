#pragma once

#include <stdint.h>

// The web interface, over Wi-Fi: the moments (download, delete), scheduled
// recordings, the categories, and setting the clock from the browser's time.
// Only runs when started from Settings. It joins the strongest network it
// knows, or opens its own hotspot (named like the unit, "zvukopis-ab12") when
// asked to or when none is in reach. The page opens only from the address on
// the screen, which carries a key made anew at each start. Stops after
// 10 minutes without a request that has the key, counted from the end of a
// recording if one is running.

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
    char address[16];   // the page is at http://address/key
    char host[24];      // ... and, on a home network, at http://host.local/key
    char key[12];       // the access key: without it in the address the page stays shut (empty: no key)
    uint32_t requests;  // pages and files served so far
};

void transferStart(bool hotspot); // hotspot: its own network even if the usual one is near
// Returns once Wi-Fi is off, or after 5 s when the server is still held by a
// request (a name lookup, a card that takes long); Wi-Fi then goes off as
// soon as that is over, and transferStart() does nothing until it has
void transferStop();
TransferInfo transferInfo();
