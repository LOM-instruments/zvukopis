/*****************************************************************************
* | File      	:   DEV_Config.c
* | Author      :   Waveshare team
* | Function    :   Hardware underlying interface
* | Info        :
*----------------
* |	This version:   V1.0
* | Date        :   2020-02-19
* | Info        :
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documnetation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to  whom the Software is
# furished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS OR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
# THE SOFTWARE.
#
******************************************************************************/
#include "DEV_Config.h"

#include <SPI.h>

// Hardware SPI instead of the original bit-banging, which took about half a
// second per screen at 80 MHz. The panel takes up to 20 MHz.
static const SPISettings EPD_SPI(10000000, MSBFIRST, SPI_MODE0);

void GPIO_Config(void)
{
#if D_9PIN
    pinMode(EPD_PWR_PIN,  OUTPUT);
    digitalWrite(EPD_PWR_PIN , HIGH);
#endif

    pinMode(EPD_BUSY_PIN,  INPUT);
    pinMode(EPD_RST_PIN , OUTPUT);
    pinMode(EPD_DC_PIN  , OUTPUT);
    
    pinMode(EPD_SCK_PIN, OUTPUT);
    pinMode(EPD_MOSI_PIN, OUTPUT);
    pinMode(EPD_CS_PIN , OUTPUT);

    digitalWrite(EPD_CS_PIN , HIGH);
    digitalWrite(EPD_SCK_PIN, LOW);
}

void GPIO_Mode(UWORD GPIO_Pin, UWORD Mode)
{
    if(Mode == 0) {
        pinMode(GPIO_Pin , INPUT);
	} else {
		pinMode(GPIO_Pin , OUTPUT);
	}
}
/******************************************************************************
function:	Module Initialize, the BCM2835 library and initialize the pins, SPI protocol
parameter:
Info:
******************************************************************************/
UBYTE DEV_Module_Init(void)
{
	//gpio
	GPIO_Config();

	//serial printf
	Serial.begin(115200);

	// spi (chip select stays a plain GPIO, driven by the callers)
	SPI.begin(EPD_SCK_PIN, -1, EPD_MOSI_PIN, -1);

	return 0;
}

/******************************************************************************
function:
			SPI read and write
******************************************************************************/
void DEV_SPI_WriteByte(UBYTE data)
{
    digitalWrite(EPD_CS_PIN, GPIO_PIN_RESET);
    SPI.beginTransaction(EPD_SPI);
    SPI.transfer(data);
    SPI.endTransaction();
    digitalWrite(EPD_CS_PIN, GPIO_PIN_SET);
}

UBYTE DEV_SPI_ReadByte()
{
    UBYTE j=0xff;
    GPIO_Mode(EPD_MOSI_PIN, 0);
    digitalWrite(EPD_CS_PIN, GPIO_PIN_RESET);
    for (int i = 0; i < 8; i++)
    {
        j = j << 1;
        if (digitalRead(EPD_MOSI_PIN))  j = j | 0x01;
        else                            j = j & 0xfe;
        
        digitalWrite(EPD_SCK_PIN, GPIO_PIN_SET);     
        digitalWrite(EPD_SCK_PIN, GPIO_PIN_RESET);
    }
    digitalWrite(EPD_CS_PIN, GPIO_PIN_SET);
    GPIO_Mode(EPD_MOSI_PIN, 1);
    return j;
}

void DEV_SPI_Write_nByte(const UBYTE *pData, UDOUBLE len)
{
    digitalWrite(EPD_CS_PIN, GPIO_PIN_RESET);
    SPI.beginTransaction(EPD_SPI);
    SPI.writeBytes(pData, len);
    SPI.endTransaction();
    digitalWrite(EPD_CS_PIN, GPIO_PIN_SET);
}


void DEV_Module_Exit(void)
{
#if D_9PIN
    digitalWrite(EPD_PWR_PIN , LOW);
#endif
    digitalWrite(EPD_RST_PIN , LOW);
}
