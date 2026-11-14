This is Espressif's ES8311 codec driver (the `es8311` component of
https://github.com/espressif/esp-bsp, Apache 2.0, see `LICENSE`), in the form
Waveshare ships it with the Arduino examples for this board
(https://github.com/waveshareteam/ESP32-S3-ePaper-3.97,
`Arduino/examples/01_Audio_Test`): there `es8311.cpp` reads and writes the
codec's registers through Arduino's `Wire` instead of the ESP-IDF I2C driver.

Changed for Zvukopis, in `es8311.cpp` and `es8311.h`:

- 2026-10-26: `es8311_power_down()` added. It powers the converters and the
  analog circuitry down while nothing records or plays; `es8311_init()`
  brings them back.
- 2026-10-27: `es8311_microphone_pga_set()` and `es8311_adc_volume_set()`
  added, for the analog preamp gain and the ADC's digital volume.
- 2026-11-14: `es8311_registers_default()` added. `es8311_init()` resets the
  codec's digital blocks, not its registers, and writes only some of them:
  the rest keep what the last firmware wrote for as long as the chip has
  power. Waveshare's factory firmware leaves the DAC muted (REG31) and five
  more registers changed, so a board flashed over it played nothing until it
  lost power. The function gives those registers their power-on values.
