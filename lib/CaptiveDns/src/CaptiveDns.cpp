#include "CaptiveDns.h"

#include <lwip/sockets.h>

static const uint16_t DNS_PORT = 53;
static const size_t HEADER_BYTES = 12;
// The size limits of RFC 1035, 2.3.4
static const size_t MAX_QUERY_BYTES = 512; // a message over UDP
static const size_t MAX_NAME_BYTES = 255;  // a whole name, with its length bytes and the closing zero
static const size_t MAX_LABEL_BYTES = 63;
static const size_t ANSWER_BYTES = 16;
static const uint32_t ANSWER_TTL_S = 60;

bool CaptiveDns::start(IPAddress ip) {
    stop();
    for (int i = 0; i < 4; i++) address[i] = ip[i];
    // Room for the longest query and the answer after it. On the heap, and
    // only while the hotspot is up.
    packet = (uint8_t *)malloc(MAX_QUERY_BYTES + ANSWER_BYTES);
    sock = packet ? lwip_socket(AF_INET, SOCK_DGRAM, 0) : -1;
    if (sock >= 0) {
        int yes = 1;
        lwip_setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        sockaddr_in local = {};
        local.sin_family = AF_INET;
        local.sin_port = htons(DNS_PORT);
        local.sin_addr.s_addr = htonl(INADDR_ANY);
        if (lwip_bind(sock, (sockaddr *)&local, sizeof(local)) == 0) return true;
    }
    stop();
    return false;
}

void CaptiveDns::stop() {
    if (sock >= 0) lwip_close(sock);
    sock = -1;
    free(packet);
    packet = nullptr;
}

void CaptiveDns::processNextRequest() {
    if (sock < 0) return;
    sockaddr_in from = {};
    socklen_t fromLen = sizeof(from);
    // One byte more than a query may have, so that a longer one shows as such
    // (the rest of it is thrown away by the network stack)
    int len = lwip_recvfrom(sock, packet, MAX_QUERY_BYTES + 1, MSG_DONTWAIT, (sockaddr *)&from, &fromLen);
    // Nothing waiting, or not the size of a query: a header, a name of at
    // least its closing zero, a type and a class
    if (len < (int)(HEADER_BYTES + 1 + 4) || len > (int)MAX_QUERY_BYTES) return;
    // Only a standard query with one question is answered; everything else is
    // dropped without a word, replies and other operations included
    bool standardQuery = (packet[2] & 0xF8) == 0;
    int questions = packet[4] << 8 | packet[5];
    if (!standardQuery || questions != 1) return;

    // The name: labels, each led by its length, up to a zero. Every step is
    // checked against the end of what arrived and against the longest name.
    size_t at = HEADER_BYTES;
    while (packet[at] != 0) {
        size_t label = packet[at];
        if (label > MAX_LABEL_BYTES) return; // too long, or a pointer to elsewhere, which a question has no use for
        at += 1 + label;
        if (at >= (size_t)len || at - HEADER_BYTES >= MAX_NAME_BYTES) return;
    }
    at++;
    if (at + 4 > (size_t)len) return;
    bool wantsAddress = packet[at] == 0 && packet[at + 1] == 1 && packet[at + 2] == 0 && packet[at + 3] == 1; // type A, class IN
    at += 4;

    // The reply is built where the query lies: its header and question, and
    // for an address question the answer. Other questions (AAAA, HTTPS) get a
    // reply with no answer, so that the asker moves on instead of waiting.
    // Whatever followed the question in the query is left out.
    packet[2] = 0x80 | (packet[2] & 0x01); // a response; "recursion desired" as it came
    packet[3] = 0;                         // no error
    packet[6] = 0;
    packet[7] = wantsAddress ? 1 : 0;      // answers
    memset(packet + 8, 0, 4);              // no authority or additional records
    if (wantsAddress) {
        const uint8_t answer[ANSWER_BYTES] = {
            0xC0, HEADER_BYTES, // the name: the one at the start of the question
            0, 1, 0, 1,         // type A, class IN
            (uint8_t)(ANSWER_TTL_S >> 24), (uint8_t)(ANSWER_TTL_S >> 16), (uint8_t)(ANSWER_TTL_S >> 8), (uint8_t)ANSWER_TTL_S,
            0, 4, address[0], address[1], address[2], address[3],
        };
        memcpy(packet + at, answer, ANSWER_BYTES);
        at += ANSWER_BYTES;
    }
    lwip_sendto(sock, packet, at, 0, (sockaddr *)&from, fromLen);
}
