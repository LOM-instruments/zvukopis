# CaptiveDns

The name server of Zvukopis's own hotspot: it answers every address question
with the device's own address, so that phones and laptops, which test a new
network for a sign-in page, open the web interface by themselves.

Written for Zvukopis (MIT, like the rest of the firmware). It takes the place
of the `DNSServer` library of the Arduino core for ESP32 (core 2.0.17,
`libraries/DNSServer`), which `src/transfer.cpp` used before, and does the one
thing that was used of it: `start()`, `processNextRequest()`, `stop()`, with
every name leading to one address. No code is copied from it.

Why not the core's: its `processNextRequest()` copies the question's name into
a 256-byte field without looking at the name's length or at the size of the
packet, so one over-long or cut-off query from a device on the hotspot wrote
past the field, and the reply sent memory back.

What is different here:

- A query is read into one buffer of fixed size and the reply is built in the
  same place. The name is never copied; it is only walked, and every step is
  checked against the end of the packet.
- Dropped without a reply: anything shorter than a question can be or longer
  than 512 bytes, a name over 255 bytes or a label over 63, a message that is
  not a standard query, and one with more or fewer than one question.
- A type A question in class IN gets the device's address (60 s to live). Any
  other question (AAAA, HTTPS) gets a reply with no answer, where the core's
  server answered those with an address record too.
- Records after the question, such as the EDNS record newer resolvers add,
  are ignored and left out of the reply. The core's server answered such
  queries with "no such name".
- Nothing is allocated per packet. The buffer exists while the server runs.
