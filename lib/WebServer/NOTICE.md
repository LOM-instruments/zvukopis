This is the `WebServer` library from the Arduino core for ESP32
(https://github.com/espressif/arduino-esp32, LGPL 2.1), version 2.0.0 as
shipped with core 2.0.17, with these changes (most are marked `zvukopis:` in
a comment where they sit):

- 2026-10-28, `src/WebServer.h`: `HTTP_MAX_DATA_WAIT` 5000 -> 500 ms and
  `HTTP_MAX_CLOSE_WAIT` 2000 -> 200 ms. The server handles one connection at a
  time, and browsers open spare connections that send nothing; with the stock
  waits a real request queued behind one for seconds.
- 2026-11-02, `src/WebServer.h`, `src/WebServer.cpp`: what the server sends is gathered
  into full TCP segments (`_currentClientWrite`, `_flushOut`) instead of going
  out piece by piece; `client()` and `streamFile()` flush first.
- 2026-11-11, `src/WebServer.cpp`: every response carries `Cache-Control: no-store`.
- 2026-11-11, `src/detail/RequestHandlersImpl.h`: no raw-body path. `canRaw()` answers
  false, so a POST that is not a form upload never reaches an upload callback
  (the stock code called it with no upload behind `upload()`: a null pointer).
- 2026-11-11, `src/Parsing.cpp`: a multipart boundary that is empty or longer than 70
  characters (`HTTP_MAX_BOUNDARY`) is refused before the form is parsed; the
  parser keeps the boundary in an array on the stack.
- 2026-11-11, `src/Parsing.cpp`: every wait in the multipart parser has a deadline,
  `HTTP_MAX_POST_WAIT`, and ends when the client has gone. Lines are read by
  `readFormLine()`, at most `HTTP_MAX_FORM_LINE` long, and `_uploadReadByte()`
  gives up on a stalled file. The stock loops went round for ever on a body
  that stopped short, and the server never answered again.
- 2026-11-11, `src/Parsing.cpp`: a body that is not a form upload may be
  `HTTP_MAX_PLAIN_BODY` (8192) bytes at most, and so may one field of a form;
  both are held in memory before any handler runs.
- 2026-11-11, `src/Parsing.cpp`: each request starts without the Host header and the form
  fields of the one before. A form that was cut off left its fields behind,
  and `arg()` then gave them to the requests that followed.
- 2026-11-14, `src/Parsing.cpp`, `src/WebServer.h`: the request line and the
  header lines are read with a longest length (`HTTP_MAX_REQUEST_LINE`,
  `HTTP_MAX_HEADER_LINE`, at most `HTTP_MAX_HEADER_LINES` of them) and a
  deadline, and so is the first line of a form. The stock reads took a line of
  any length, megabytes of it, before any handler could ask who was sending.
- 2026-11-14, `src/Parsing.cpp`, `src/WebServer.h`: a request may have
  `HTTP_MAX_ARGS` (48) arguments at most, and the arrays for arguments and
  uploads are made with a `new` that can fail; such a request is dropped. The
  stock code sized the array by the number of `&` in the request, and its
  `new` ended the program when memory ran out: one request restarted the
  device.
- 2026-11-14, `src/WebServer.cpp`: the arguments and headers of a request are
  freed when its connection is done, not when the next request is parsed.
- 2026-11-14, `src/Parsing.cpp`: after a complete file, a closing line that
  never comes ends the form, so that the upload is answered.

The examples and keywords file are left out.
