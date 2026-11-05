#include "cardreader.h"

#include <Arduino.h>
#include <USB.h>
#include <USBMSC.h>

#include "storage.h"
#include "usbport.h"

// Made when the mode starts: the object's constructor already puts a drive
// into the USB descriptor, which the microphone mode must not carry
static USBMSC *msc = nullptr;
static sdmmc_card_t *card = nullptr;
static volatile bool ejected = false;

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
    // TinyUSB asks for whole sectors (offset 0)
    uint32_t sectorSize = card->csd.sector_size;
    if (offset != 0 || size % sectorSize) return -1;
    return sdmmc_read_sectors(card, buffer, lba, size / sectorSize) == ESP_OK ? size : -1;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
    uint32_t sectorSize = card->csd.sector_size;
    if (offset != 0 || size % sectorSize) return -1;
    return sdmmc_write_sectors(card, buffer, lba, size / sectorSize) == ESP_OK ? size : -1;
}

static bool onStartStop(uint8_t powerCondition, bool start, bool loadEject) {
    if (loadEject && !start) ejected = true;
    return true;
}

bool cardReaderStart() {
    card = storageOpenRawCard(); // unmounts the card from this side first
    if (!card) return false;
    Serial.printf("[card reader] %llu MB card, switching USB to a drive; serial ends here\n",
                  (uint64_t)card->csd.capacity * card->csd.sector_size >> 20);
    Serial.flush();
    delay(50);

    if (!msc) msc = new USBMSC();
    msc->vendorID("zvukopis");
    msc->productID("SD card");
    msc->productRevision("1.0");
    msc->onRead(onRead);
    msc->onWrite(onWrite);
    msc->onStartStop(onStartStop);
    msc->mediaPresent(true);
    msc->begin(card->csd.capacity, card->csd.sector_size);
    USB.productName("Zvukopis SD card");
    USB.manufacturerName("Zvukopis");
    USB.begin(); // takes the USB port over from the serial/JTAG controller
    return true;
}

bool cardReaderEjected() { return ejected; }

void cardReaderLeave() { usbPortRestart(); }
