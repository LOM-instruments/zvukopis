This is the `WebServer` library from the Arduino core for ESP32
(https://github.com/espressif/arduino-esp32, LGPL 2.1), version 2.0.0 as
shipped with core 2.0.17, with these changes (most are marked `zvukopis:` in
a comment where they sit):

- `src/WebServer.h`: `HTTP_MAX_DATA_WAIT` 5000 -> 500 ms and
  `HTTP_MAX_CLOSE_WAIT` 2000 -> 200 ms. The server handles one connection at a
  time, and browsers open spare connections that send nothing; with the stock
  waits a real request queued behind one for seconds.
- `src/WebServer.h`, `src/WebServer.cpp`: what the server sends is gathered
  into full TCP segments (`_currentClientWrite`, `_flushOut`) instead of going
  out piece by piece; `client()` and `streamFile()` flush first.
- `src/WebServer.cpp`: every response carries `Cache-Control: no-store`.
- `src/detail/RequestHandlersImpl.h`: no raw-body path. `canRaw()` answers
  false, so a POST that is not a form upload never reaches an upload callback
  (the stock code called it with no upload behind `upload()`: a null pointer).
- `src/Parsing.cpp`: a multipart boundary that is empty or longer than 70
  characters (`HTTP_MAX_BOUNDARY`) is refused before the form is parsed; the
  parser keeps the boundary in an array on the stack.
- `src/Parsing.cpp`: every wait in the multipart parser has a deadline,
  `HTTP_MAX_POST_WAIT`, and ends when the client has gone. Lines are read by
  `readFormLine()`, at most `HTTP_MAX_FORM_LINE` long, and `_uploadReadByte()`
  gives up on a stalled file. The stock loops went round for ever on a body
  that stopped short, and the server never answered again.
- `src/Parsing.cpp`: a body that is not a form upload may be
  `HTTP_MAX_PLAIN_BODY` (8192) bytes at most, and so may one field of a form;
  both are held in memory before any handler runs.
- `src/Parsing.cpp`: each request starts without the Host header and the form
  fields of the one before. A form that was cut off left its fields behind,
  and `arg()` then gave them to the requests that followed.

The examples and keywords file are left out.
