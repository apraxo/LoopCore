// loopcore_mouse.ino
//
// Receives movement packets from loopcore over USB serial and applies them
// as HID mouse movement. Optionally passes a physical mouse through, when a
// USB Host Shield is fitted.
//
// Boards: anything with native USB HID -- Leonardo, Micro, Pro Micro,
// Teensy. An Uno or Nano cannot do this without reflashing its 16U2.
//
// ---------------------------------------------------------------------------
// The packet, 7 bytes, fixed length so the receiver can resync after a
// dropped byte:
//
//   [0] 0xA5   header
//   [1] dx high    int16, big endian
//   [2] dx low
//   [3] dy high    int16, big endian
//   [4] dy low
//   [5] count      targets seen, saturated at 255
//   [6] checksum   XOR of bytes 1..5
//
// ---------------------------------------------------------------------------
// The one rule that matters: never let serial reading monopolise loop().
// USB HID and the host shield both need to be serviced constantly. Draining
// a full serial buffer in one pass is what makes a pass-through mouse feel
// like it is dragging through treacle.

// Output side. Arduino's stock Mouse library carries three buttons and no
// wheel, so passthrough uses HID-Project instead, which reports five buttons
// plus vertical and horizontal wheel.
#if ENABLE_PASSTHROUGH
  #include <HID-Project.h>
#else
  #include <Mouse.h>
#include <string.h>
#include <stdlib.h>
#endif

// Libraries. loopcore installs these for you when flashing from inside the
// app; by hand they are:
//     arduino-cli lib install Mouse                          (HID only)
//     arduino-cli lib install "USB Host Shield Library 2.0"  (passthrough)
//     arduino-cli lib install HID-Project                    (passthrough)
//
// HID-Project replaces the stock Mouse library when passthrough is on: the
// stock one has no wheel and only three buttons, so a scroll wheel and mouse
// 4 and 5 cannot pass through it whatever the host side reads.
// Defined on the command line by loopcore's flasher, so the toggle in the
// app works without editing this file. Edit the fallback if you flash by
// hand from the Arduino IDE.
#ifndef ENABLE_PASSTHROUGH
#define ENABLE_PASSTHROUGH 0
#endif

#if ENABLE_PASSTHROUGH
  // HIDUniversal, not HIDBoot. The boot protocol is a fixed three-byte
  // report -- three buttons, X, Y -- with no field for a wheel and no room
  // for buttons four and five. It exists so a mouse works in a BIOS, and
  // that is all it can do. Report protocol carries the whole device.
  #include <hidboot.h>
  #include <hiduniversal.h>
  #include <usbhub.h>
  USB          Usb;
  USBHub       Hub(&Usb);
  HIDUniversal Hid(&Usb);
#endif

// Set to 1 to print every raw HID report over serial. loopcore shows these
// in its log, which is how to work out an unusual mouse's report layout.
#ifndef DUMP_REPORTS
#define DUMP_REPORTS 0
#endif

// How to read the mouse's report.
//   0  guess from the report length
//   1  buttons, int8 X,  int8 Y,  int8 wheel
//   2  buttons, int16 X, int16 Y, int8 wheel
//   3  report id, buttons, int16 X, int16 Y, int8 wheel
// Guessing is right for most mice and wrong for some. If movement dashes
// across the screen or side-to-side motion scrolls, the guess is wrong: turn
// on DUMP_REPORTS and pick the layout that matches the bytes.
#ifndef MOUSE_LAYOUT
#define MOUSE_LAYOUT 0
#endif

// ---------------------------------------------------------------- config

static const uint8_t  HEADER          = 0xA5;
static const uint32_t BAUD            = 115200;   // ignored on native USB
// Never issue HID reports faster than the USB polling interval. Anything
// more is queued, not delivered, and the queue becomes latency.
static const uint16_t REPORT_INTERVAL_US = 1000;  // 1 kHz
// Bytes of serial drained per loop pass. Enough to keep up at 500 packets a
// second, small enough that HID never starves.
static const uint8_t  SERIAL_BUDGET   = 28;

// ------------------------------------------------------------ parser state

static uint8_t  buf[7];
static uint8_t  have = 0;

// Movement received but not yet reported. HID carries one signed byte per
// axis per report, so anything larger is spread over several.
static int32_t  pendingX = 0;
static int32_t  pendingY = 0;

static uint32_t lastReportUs = 0;
static uint32_t lastPacketMs = 0;

// --------------------------------------------------------------- helpers

// Declared ahead of handlePacket, which uses it: the Arduino build
// generates prototypes for top-level functions but not reliably for statics.
static uint8_t buttonHid(uint8_t n);

static void handlePacket() {
    const uint8_t sum = buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5];
    if (sum != buf[6]) return;          // corrupt, drop it

    const int16_t dx = (int16_t)(((uint16_t)buf[1] << 8) | buf[2]);
    const int16_t dy = (int16_t)(((uint16_t)buf[3] << 8) | buf[4]);

    pendingX += dx;
    pendingY += dy;

    // The click byte, which was previously parsed and thrown away.
    //
    // The top bit marks a button command rather than a click count: no
    // ordinary count reaches 128, so the two cannot be confused, and a
    // button gets an exact press and release rather than a click that a
    // game may or may not see.
    if (buf[5] & 0x80) {
        const uint8_t which = (uint8_t)((buf[5] >> 1) & 0x03);
        const bool down = (buf[5] & 0x01) != 0;
        const uint8_t hid = buttonHid(which);
        if (down) Mouse.press(hid);
        else      Mouse.release(hid);
    } else {
        uint8_t clicks = buf[5];
        if (clicks > 4) clicks = 4;
        for (uint8_t i = 0; i < clicks; ++i) Mouse.click(MOUSE_LEFT);
    }
    lastPacketMs = millis();
}

// Text commands, which is what loopcore sends unless binary is selected.
//
// Two forms, distinguished by the first character:
//   x,y,clicks   movement, and a count of left clicks to emit
//   bN,S         button N (0 left, 1 right, 2 middle) down when S is 1
//
// A button has to be a separate command rather than a field on the movement
// line: presses and releases are events that must arrive exactly once and in
// order, whereas movement is a rate that is coalesced and re-sent.
static char    line[24];
static uint8_t lineLen = 0;
// Raised while loopcore is sketching a mouse layout: reports go out as fast
// as the link allows rather than at the rate that is merely readable.
static bool    sketchMode = false;

static uint8_t buttonHid(uint8_t n) {
    if (n == 1) return MOUSE_RIGHT;
    if (n == 2) return MOUSE_MIDDLE;
    return MOUSE_LEFT;
}

static void handleLine() {
    line[lineLen] = 0;
    if (lineLen == 0) return;

    // s1 / s0 -- sketch mode on or off.
    //
    // The length is tested before line[1] is read. It happens to be safe
    // without it -- the terminator above lands on index 1 when a single
    // character arrived, so the comparison sees '\0' -- but that is a
    // property of the line above rather than of this test, and it would stop
    // being true the moment the terminator moved.
    if (lineLen >= 2 && line[0] == 's' &&
        (line[1] == '0' || line[1] == '1')) {
        sketchMode = (line[1] == '1');
        Serial.print(F("#sketch "));
        Serial.println(sketchMode ? 1 : 0);
        return;
    }

    if (line[0] == 'b' || line[0] == 'B') {
        const char* comma = strchr(line, ',');
        if (!comma) return;
        const uint8_t which = (uint8_t)atoi(line + 1);
        const bool down = (atoi(comma + 1) != 0);
        const uint8_t hid = buttonHid(which);
        // press and release are idempotent in the library, so a repeated
        // command cannot leave the state inverted.
        if (down) Mouse.press(hid);
        else      Mouse.release(hid);
        lastPacketMs = millis();
        return;
    }

    // x,y[,clicks]
    const char* p1 = line;
    const char* c1 = strchr(p1, ',');
    if (!c1) return;
    const char* c2 = strchr(c1 + 1, ',');

    pendingX += atoi(p1);
    pendingY += atoi(c1 + 1);
    if (c2) {
        int clicks = atoi(c2 + 1);
        if (clicks > 4) clicks = 4;      // a runaway count must not lock up
        for (int i = 0; i < clicks; ++i) Mouse.click(MOUSE_LEFT);
    }
    lastPacketMs = millis();
}

static void readSerial() {
    uint8_t budget = SERIAL_BUDGET;
    while (Serial.available() && budget--) {
        const uint8_t b = (uint8_t)Serial.read();

        // The binary framing and the text one coexist: a header byte starts
        // a packet, anything else is treated as text. That way one build of
        // this sketch works whichever protocol loopcore is set to, and a
        // mismatched pair is no longer silently dead.
        if (have == 0 && b != HEADER) {
            if (b == '\n' || b == '\r') {
                handleLine();
                lineLen = 0;
            } else if (lineLen < sizeof(line) - 1) {
                line[lineLen++] = (char)b;
            } else {
                lineLen = 0;      // overlong: drop it rather than wrap
            }
            continue;
        }

        buf[have++] = b;
        if (have == 7) {
            handlePacket();
            have = 0;
        }
    }
}

static void emitMovement() {
    if (pendingX == 0 && pendingY == 0) return;

    const uint32_t now = micros();
    if ((uint32_t)(now - lastReportUs) < REPORT_INTERVAL_US) return;
    lastReportUs = now;

    // Clamp to what a single HID report can carry; the remainder rides along
    // to the next one, so large moves arrive smoothly rather than being lost.
    int8_t sx = 0, sy = 0;
    if (pendingX >  127) sx =  127; else if (pendingX < -127) sx = -127; else sx = (int8_t)pendingX;
    if (pendingY >  127) sy =  127; else if (pendingY < -127) sy = -127; else sy = (int8_t)pendingY;

    pendingX -= sx;
    pendingY -= sy;

    // Both libraries expose the same three-argument form, so this needs no
    // conditional.
    Mouse.move(sx, sy, 0);
}

// ------------------------------------------------------------ passthrough

#if ENABLE_PASSTHROUGH

// Report layouts vary between mice, so the parser adapts to the length of
// the report rather than assuming one shape. The common cases:
//
//   4 bytes   buttons, int8 X, int8 Y, int8 wheel
//   6         buttons, int16 X, int16 Y, int8 wheel
//   7+        buttons, int16 X, int16 Y, int8 wheel, int8 pan
//
// If a mouse does something else, set DUMP_REPORTS to 1 and read the raw
// bytes out of loopcore's log.
class MouseFwd : public HIDReportParser {
public:
    void Parse(USBHID* /*hid*/, bool is_rpt_id, uint8_t len, uint8_t* buf) override {
        if (is_rpt_id && len > 0) { ++buf; --len; }   // strip the report id
        if (len < 3) return;

#if DUMP_REPORTS
        // A gaming mouse reports up to a thousand times a second. Printing
        // every one would starve the USB work this depends on, so it is
        // throttled -- except while loopcore is working out the layout, when
        // the opposite is wanted.
        //
        // Sketching a layout means watching which byte moves during a
        // deliberate action, and at sixteen reports a second a flick of the
        // wrist produces three or four samples. That is too few to tell a
        // real axis from noise. In sketch mode the throttle drops to the
        // point where the serial link is the only limit, and the analysis
        // gets hundreds of samples per prompt instead.
        {
            static uint32_t lastDump = 0;
            const uint32_t gap = sketchMode ? 2 : 60;
            if ((uint32_t)(millis() - lastDump) > gap) {
                lastDump = millis();
                Serial.print("#rpt ");
                Serial.print(len);
                Serial.print(":");
                for (uint8_t i = 0; i < len; ++i) {
                    Serial.print(' ');
                    if (buf[i] < 0x10) Serial.print('0');
                    Serial.print(buf[i], HEX);
                }
                Serial.println();
            }
        }
#endif

        const uint8_t btns = buf[0];

        int16_t x = 0, y = 0;
        int8_t  wheel = 0;

#if MOUSE_LAYOUT == 1
        x = (int8_t)buf[1];
        y = (int8_t)buf[2];
        if (len >= 4) wheel = (int8_t)buf[3];
#elif MOUSE_LAYOUT == 2
        if (len >= 6) {
            x = (int16_t)((uint16_t)buf[1] | ((uint16_t)buf[2] << 8));
            y = (int16_t)((uint16_t)buf[3] | ((uint16_t)buf[4] << 8));
            wheel = (int8_t)buf[5];
        }
#elif MOUSE_LAYOUT == 3
        if (len >= 7) {
            x = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));
            y = (int16_t)((uint16_t)buf[4] | ((uint16_t)buf[5] << 8));
            wheel = (int8_t)buf[6];
        }
#else
        // Guessed from the length. Reports of 6 or more carry 16-bit axes;
        // shorter ones are 8-bit. Five bytes is the ambiguous case and is
        // treated as 8-bit with a wheel and a tilt byte, which is the more
        // common of the two shapes.
        if (len >= 6) {
            x = (int16_t)((uint16_t)buf[1] | ((uint16_t)buf[2] << 8));
            y = (int16_t)((uint16_t)buf[3] | ((uint16_t)buf[4] << 8));
            wheel = (int8_t)buf[5];
        } else {
            x = (int8_t)buf[1];
            y = (int8_t)buf[2];
            if (len >= 4) wheel = (int8_t)buf[3];
        }
#endif

        // Movement and wheel go straight out, deliberately not through the
        // rate limiter: the user's own input must never wait on ours.
        if (x || y || wheel) {
            const int8_t sx = x >  127 ? 127 : (x < -127 ? -127 : (int8_t)x);
            const int8_t sy = y >  127 ? 127 : (y < -127 ? -127 : (int8_t)y);
            Mouse.move(sx, sy, wheel);
        }

        // Buttons are edge-triggered off the previous report, so a held
        // button is pressed once rather than every few milliseconds.
        applyButton(btns, prev_, 0x01, MOUSE_LEFT);
        applyButton(btns, prev_, 0x02, MOUSE_RIGHT);
        applyButton(btns, prev_, 0x04, MOUSE_MIDDLE);
        applyButton(btns, prev_, 0x08, MOUSE_PREV);    // mouse 4, back
        applyButton(btns, prev_, 0x10, MOUSE_NEXT);    // mouse 5, forward
        prev_ = btns;
    }

private:
    static void applyButton(uint8_t now, uint8_t before, uint8_t mask, uint8_t hid) {
        const bool isDown  = (now    & mask) != 0;
        const bool wasDown = (before & mask) != 0;
        if (isDown == wasDown) return;
        if (isDown) Mouse.press(hid);
        else        Mouse.release(hid);
    }

    uint8_t prev_ = 0;
};
MouseFwd fwd;
#endif

// ------------------------------------------------------------------ main

#if ENABLE_PASSTHROUGH
static bool     usbReady    = false;
static uint32_t lastUsbTry  = 0;
#endif

void setup() {
    Serial.begin(BAUD);
    Mouse.begin();

#if ENABLE_PASSTHROUGH
    pinMode(LED_BUILTIN, OUTPUT);
    // Usb.Init() is what brings the MAX3421E up, and bringing the MAX3421E
    // up is what switches VBUS through to the downstream port. Without this
    // call a mouse plugged into the shield gets no power at all.
    usbReady = (Usb.Init() != -1);
    if (usbReady) {
        delay(200);
        Hid.SetReportParser(0, &fwd);
    }
    digitalWrite(LED_BUILTIN, usbReady ? LOW : HIGH);
    lastUsbTry = millis();
#endif
}

void loop() {
#if ENABLE_PASSTHROUGH
    if (usbReady) {
        // First and often. Everything else in this loop is bounded so that
        // this call is never starved.
        Usb.Task();
    } else if ((uint32_t)(millis() - lastUsbTry) > 1000) {
        // Keep trying rather than giving up for good: the shield may simply
        // have been powered up after the board was. The LED stays lit while
        // the shield is not responding, which is usually wiring or a missing
        // VBUS jumper.
        lastUsbTry = millis();
        usbReady = (Usb.Init() != -1);
        if (usbReady) {
            delay(200);
            Hid.SetReportParser(0, &fwd);
            digitalWrite(LED_BUILTIN, LOW);
        }
    }
#endif

    readSerial();
    emitMovement();

#if DUMP_REPORTS
    // A Leonardo discards anything printed before the host opens the port,
    // so a one-off banner in setup() is usually never seen. Repeating it
    // proves the return channel works even if the mouse is never touched.
    {
        static uint32_t lastHello = 0;
        if ((uint32_t)(millis() - lastHello) > 3000) {
            lastHello = millis();
            Serial.print("#hello passthrough=");
            Serial.print(ENABLE_PASSTHROUGH);
            Serial.print(" layout=");
            Serial.print(MOUSE_LAYOUT);
#if ENABLE_PASSTHROUGH
            Serial.print(" shield=");
            Serial.print(usbReady ? "up" : "DOWN");
#endif
            Serial.println();
        }
    }
#endif

    // If loopcore goes quiet mid-movement, drop whatever is queued rather
    // than continuing to drift after the program has stopped asking.
    if (pendingX || pendingY) {
        if ((uint32_t)(millis() - lastPacketMs) > 250) {
            pendingX = 0;
            pendingY = 0;
        }
    }
}
