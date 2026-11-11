#pragma once

#include <Arduino.h>

// The name server of Zvukopis's own hotspot. Whatever name a phone or laptop
// asks for, the answer is the device's address, so their check for a sign-in
// page lands on the web interface and they open it by themselves.
// It answers queries and nothing else: see README.md for what it leaves out.

class CaptiveDns {
public:
    // Listens on UDP port 53 and answers with `address` from now on
    bool start(IPAddress address);
    void stop();
    // Answers one waiting query, if there is one. Never waits.
    void processNextRequest();

private:
    int sock = -1;
    uint8_t *packet = nullptr; // one query, and the reply built in its place
    uint8_t address[4] = {};
};
