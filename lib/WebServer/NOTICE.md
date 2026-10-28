This is the `WebServer` library from the Arduino core for ESP32
(https://github.com/espressif/arduino-esp32, LGPL 2.1), version 2.0.0 as
shipped with core 2.0.17, with one change in `src/WebServer.h`:

- `HTTP_MAX_DATA_WAIT` 5000 -> 500 ms and `HTTP_MAX_CLOSE_WAIT` 2000 -> 200 ms.
  The server handles one connection at a time, and browsers open spare
  connections that send nothing; with the stock waits a real request queued
  behind one for seconds.

The examples and keywords file are left out.
