#include "usbport.h"

#include <Arduino.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/usb_serial_jtag_reg.h>
#include "tusb.h"

// Hand the USB pins back to the serial/JTAG controller, as at power-up.
// These bits live in the RTC domain and survive a restart: the PHY selection
// goes back to the hardware default, and the pads stay enabled (with them
// off, the board was not on the bus at all after restarting).
void usbPortRestoreAtBoot() {
    // Still set at boot only when a USB mode ended without usbPortRestart(): a crash or a watchdog
    bool leftInUsbMode = (READ_PERI_REG(RTC_CNTL_USB_CONF_REG) & RTC_CNTL_SW_USB_PHY_SEL) != 0;
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_PAD_ENABLE);
    if (leftInUsbMode) {
        // The computer was never told the device went away: it would keep the
        // dead microphone or drive in its list, and this serial port would not
        // show until a replug. D+ low for a moment looks like one.
        SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
        CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
        delay(150);
        SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_DP_PULLUP);
        CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_PAD_PULL_OVERRIDE);
    }
}

void usbPortRestart() {
    // Unplug from the computer's point of view first. The restart resets only
    // the processor, not the USB lines, so without this the computer keeps
    // the microphone or the drive in its list with nobody answering, and a
    // second visit to the mode finds a computer that never saw the first end.
    tud_disconnect();
    delay(250);
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_PAD_ENABLE);
    delay(100);
    esp_restart();
}
