#include "usbport.h"

#include <Arduino.h>
#include <soc/rtc_cntl_reg.h>

// Hand the USB pins back to the serial/JTAG controller, as at power-up.
// These bits live in the RTC domain and survive a restart: the PHY selection
// goes back to the hardware default, and the pads stay enabled (with them
// off, the board was not on the bus at all after restarting).
void usbPortRestoreAtBoot() {
    CLEAR_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_SW_HW_USB_PHY_SEL | RTC_CNTL_SW_USB_PHY_SEL);
    SET_PERI_REG_MASK(RTC_CNTL_USB_CONF_REG, RTC_CNTL_USB_PAD_ENABLE);
}

void usbPortRestart() {
    usbPortRestoreAtBoot();
    delay(100);
    esp_restart();
}
