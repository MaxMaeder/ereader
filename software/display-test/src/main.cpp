#include "EPD_5in0.h"
#include "Debug.h"

static const UDOUBLE FRAME_SIZE = ((EPD_5in0_WIDTH + 7) / 8) * EPD_5in0_HEIGHT; // 66240
static const char MAGIC[] = "EPDI";
static const UBYTE MAGIC_LEN = 4;
static const unsigned long READ_TIMEOUT_MS = 10000;

static UBYTE *frameBuffer = nullptr;

static bool waitForMagic() {
    UBYTE matched = 0;
    while (true) {
        if (Serial.available()) {
            UBYTE b = Serial.read();
            if (b == MAGIC[matched]) {
                matched++;
                if (matched == MAGIC_LEN) return true;
            } else {
                matched = (b == MAGIC[0]) ? 1 : 0;
            }
        }
    }
}

static bool readFrame() {
    UDOUBLE received = 0;
    unsigned long lastData = millis();

    while (received < FRAME_SIZE) {
        int avail = Serial.available();
        if (avail > 0) {
            UDOUBLE toRead = min((UDOUBLE)avail, FRAME_SIZE - received);
            Serial.readBytes((char *)(frameBuffer + received), toRead);
            received += toRead;
            lastData = millis();
        } else if (millis() - lastData > READ_TIMEOUT_MS) {
            return false;
        }
    }
    return true;
}

void setup() {
    if (DEV_Module_Init() != 0) {
        return;
    }

    frameBuffer = (UBYTE *)malloc(FRAME_SIZE);
    if (!frameBuffer) {
        Debug("Failed to allocate frame buffer\r\n");
        return;
    }

    EPD_5in0_Init();
    EPD_5in0_Clear();

    Serial.println("READY");
}

void loop() {
    waitForMagic();

    if (readFrame()) {
        EPD_5in0_Display(frameBuffer);
        Serial.println("OK");
    } else {
        Serial.println("ERR");
    }
}
