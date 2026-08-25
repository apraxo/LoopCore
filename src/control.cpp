// control.cpp
#include "control.h"

#include "paths.h"

#include <windows.h>

#include "rawinput.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>

namespace lc {

// ------------------------------------------------------------ mouse sink

namespace {

class MouseSink : public OutputSink {
public:
    bool open(const Config& cfg, Log& log) override {
        // Derived from the control rate but firmly capped: this is what goes
        // into the system input queue, and nothing downstream benefits from
        // more than a couple of hundred events a second.
        setRate(cfg.controlRateHz);
        blocked_ = false;
        rejected_ = 0;
        accX_ = accY_ = 0.0f;
        lastSend_ = 0;
        log.info("Output: relative mouse movement via SendInput, coalesced to " +
                 std::to_string((int)(1'000'000'000LL / intervalNs_)) + " Hz.");
        ready_ = true;
        return true;
    }

    // Whether Windows is refusing the injected input, and why.
    //
    // SendInput returns the number of events it accepted and sets a last
    // error when it accepts none. Nothing was reading either, so the one
    // failure that matters here -- a target window running elevated while
    // loopcore does not, which makes Windows discard every event silently --
    // looked identical to the model simply not producing any movement.
    uint64_t rejectedCount() const override { return rejected_; }
    bool     accessBlocked() const override { return blocked_; }

    void button(int b, bool down) override {
        INPUT in{};
        in.type = INPUT_MOUSE;
        switch (b) {
        case 1: in.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN
                                     : MOUSEEVENTF_RIGHTUP; break;
        case 2: in.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN
                                     : MOUSEEVENTF_MIDDLEUP; break;
        default: in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN
                                      : MOUSEEVENTF_LEFTUP; break;
        }
        in.mi.dwExtraInfo = kLoopcoreInputTag;
        note(SendInput(1, &in, sizeof(INPUT)));
    }

    void setRate(int hz) {
        // Capped well below the control rate on purpose; see send().
        const int use = std::clamp(hz, 30, 250);
        intervalNs_ = 1'000'000'000LL / use;
    }

    void send(float dx, float dy, int) override {
        if (!ready_) return;

        // Accumulated and emitted at a limited rate, rather than once per
        // control tick.
        //
        // This used to call SendInput on every tick -- up to a thousand times
        // a second. Injected input goes into the same queue as real hardware
        // input and Windows serialises the two, so at that rate genuine key
        // presses and releases queue behind the flood. The result is a
        // system-wide stutter: a keystroke arriving a second late, or a
        // release being missed so the key reads as still held. It only
        // appeared while actually tracking a target, because that is the only
        // time movement is produced continuously.
        //
        // Coalescing costs nothing in accuracy. The plan is a position, not a
        // sequence of impulses, so ten ticks' worth of movement sent as one
        // event lands in exactly the same place -- and no game samples input
        // faster than this anyway.
        accX_ += dx;
        accY_ += dy;

        const int64_t now = now_ns();
        if (now - lastSend_ < intervalNs_) return;
        lastSend_ = now;

        const int ix = (int)accX_;
        const int iy = (int)accY_;
        // The fraction stays behind rather than being discarded, so slow
        // movement still accumulates into whole pixels instead of vanishing.
        accX_ -= (float)ix;
        accY_ -= (float)iy;
        if (ix == 0 && iy == 0) return;

        INPUT in{};
        in.type = INPUT_MOUSE;
        in.mi.dx = ix;
        in.mi.dy = iy;
        in.mi.dwFlags = MOUSEEVENTF_MOVE;   // relative, no absolute flag
        // Tagged so our own raw input handler can throw it away immediately.
        in.mi.dwExtraInfo = kLoopcoreInputTag;
        note(SendInput(1, &in, sizeof(INPUT)));
    }

    void flush() override { accX_ = accY_ = 0.0f; }

    const char* name() const override { return "Mouse"; }
    bool ready() const override { return ready_; }

private:
    void note(UINT sent) {
        if (sent != 0) {
            blocked_ = false;
            return;
        }
        // Counted rather than logged: this runs on the control thread at up
        // to a kilohertz, and a log line per rejected event would be worse
        // than the fault it reports.
        ++rejected_;
        if (GetLastError() == ERROR_ACCESS_DENIED) blocked_ = true;
    }

    bool     ready_ = false;
    bool     blocked_ = false;
    uint64_t rejected_ = 0;
    float    accX_ = 0.0f, accY_ = 0.0f;
    int64_t  lastSend_ = 0;
    int64_t  intervalNs_ = 1'000'000'000LL / 200;   // 200 Hz
};

// --------------------------------------------------------- arduino sink

// Fixed-size binary packet. ASCII would cost parsing time on a 16 MHz part,
// and a variable length frame cannot resync after a dropped byte.
//
//   0  0xA5   header
//   1  dx high
//   2  dx low     int16, big endian
//   3  dy high
//   4  dy low     int16
//   5  count      targets seen, saturated at 255
//   6  xor of 1..5
// A Makcu, driven over serial from a second machine.
//
// The device is a mouse as far as the machine being played on is concerned:
// it plugs in there and enumerates as real hardware, while this machine only
// writes text commands to a COM port. That split is the point -- capture and
// inference happen here, and nothing is injected locally at all.
//
// Its protocol is line-based: "km.move(x,y)", "km.left(1)" and so on, each
// terminated by carriage return and newline. The device answers only when
// asked, so movement is fire-and-forget.
class MakcuSink : public OutputSink {
public:
    ~MakcuSink() override { close(); }

    bool open(const Config& cfg, Log& log) override {
        close();
        if (cfg.serialPort <= 0) {
            log.error("No COM port is set for the Makcu.");
            return false;
        }

        const std::string name = "\\\\.\\COM" + std::to_string(cfg.serialPort);
        h_ = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = nullptr;
            log.error("Could not open COM" + std::to_string(cfg.serialPort) +
                      " for the Makcu. Check the port number, and that no "
                      "other program has it open.");
            return false;
        }

        // Opened at the rate the device starts on; the switch to its fast
        // rate happens below.
        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h_, &dcb)) { close(); return false; }
        dcb.BaudRate = 115200;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;
        dcb.fBinary  = TRUE;
        dcb.fDtrControl = DTR_CONTROL_DISABLE;
        dcb.fRtsControl = RTS_CONTROL_DISABLE;
        if (!SetCommState(h_, &dcb)) { close(); return false; }

        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout = MAXDWORD;
        SetCommTimeouts(h_, &to);

        ov_ = OVERLAPPED{};
        ov_.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        pending_ = false;

        // The device is told to switch to four megabaud, and this end
        // follows. Sent as raw bytes rather than a command, because until it
        // has been accepted the device is still listening at the old rate.
        static const unsigned char kBaudCmd[] =
            { 0xDE, 0xAD, 0x05, 0x00, 0xA5, 0x00, 0x09, 0x3D, 0x00 };
        DWORD wrote = 0;
        WriteFile(h_, kBaudCmd, sizeof(kBaudCmd), &wrote, nullptr);
        Sleep(150);

        if (GetCommState(h_, &dcb)) {
            dcb.BaudRate = 4000000;
            if (!SetCommState(h_, &dcb)) {
                // Not fatal: some serial drivers refuse the rate outright,
                // and the device keeps answering at the old one. Worth
                // saying, because the symptom is movement that works but
                // arrives in bursts.
                log.warn("The port would not accept four megabaud, so the "
                         "Makcu is being driven at 115200. Movement still "
                         "works; it is just coarser.");
            }
        }

        // Button reporting on, so the device tells us about presses made on
        // the real mouse plugged into it.
        writeLine("km.buttons(1)");

        log.info("Output: Makcu on COM" + std::to_string(cfg.serialPort) +
                 ". Movement is sent to the other machine; nothing is "
                 "injected here.");
        ready_ = true;
        return true;
    }

    void send(float dx, float dy, int) override {
        if (!ready_) return;

        // Whole counts only, with the fraction carried, for the same reason
        // the other sinks do it: the device takes integers and repeatedly
        // discarding a fraction is a systematic under-travel.
        accX_ += dx;
        accY_ += dy;
        const int ix = (int)accX_;
        const int iy = (int)accY_;
        accX_ -= (float)ix;
        accY_ -= (float)iy;
        if (!ix && !iy) return;

        char line[48];
        int n = snprintf(line, sizeof(line), "km.move(%d,%d)\r\n", ix, iy);
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        if (n <= 0) return;
        writeRaw(line, n);
    }

    void button(int b, bool down) override {
        if (!ready_) return;
        const char* nm = (b == 1) ? "right" : (b == 2) ? "middle" : "left";
        char line[32];
        int n = snprintf(line, sizeof(line), "km.%s(%d)\r\n", nm, down ? 1 : 0);
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        if (n > 0) writeRaw(line, n);
    }

    void flush() override { accX_ = accY_ = 0.0f; }

    void poll() override {
        // Only to retire a completed write; the device is not read from.
        if (!pending_ || !h_ || !ov_.hEvent) return;
        DWORD done = 0;
        if (GetOverlappedResult(h_, &ov_, &done, FALSE) ||
            GetLastError() != ERROR_IO_INCOMPLETE) {
            pending_ = false;
            ResetEvent(ov_.hEvent);
        }
    }

    void close() override {
        ready_ = false;
        if (h_) { CancelIo(h_); CloseHandle(h_); h_ = nullptr; }
        if (ov_.hEvent) { CloseHandle(ov_.hEvent); ov_.hEvent = nullptr; }
        pending_ = false;
    }

    const char* name() const override { return "Makcu"; }
    bool ready() const override { return ready_; }

private:
    void writeLine(const char* text) {
        char line[64];
        int n = snprintf(line, sizeof(line), "%s\r\n", text);
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        if (n > 0) writeRaw(line, n);
    }

    // One pending write at a time. A move that cannot go out now is dropped
    // rather than queued: the next tick carries the accumulated position
    // anyway, so a stale packet would only put the aim somewhere it has
    // already left.
    void writeRaw(const char* data, int n) {
        if (!h_ || !ov_.hEvent) return;
        poll();
        if (pending_) return;

        if ((size_t)n > sizeof(txBuf_)) n = (int)sizeof(txBuf_);
        memcpy(txBuf_, data, (size_t)n);

        DWORD wrote = 0;
        if (!WriteFile(h_, txBuf_, (DWORD)n, &wrote, &ov_)) {
            if (GetLastError() == ERROR_IO_PENDING) pending_ = true;
        }
    }

    HANDLE     h_ = nullptr;
    OVERLAPPED ov_{};
    bool       pending_ = false;
    bool       ready_ = false;
    char       txBuf_[64]{};
    float      accX_ = 0.0f, accY_ = 0.0f;
};

class ArduinoSink : public OutputSink {
public:
    // Closed on destruction as well as on request.
    //
    // Every path today calls close() first, but the sink is held in a
    // unique_ptr and a future one need only reset it to leak a COM port and
    // three event handles. A destructor costs nothing and removes the
    // question.
    ~ArduinoSink() override { close(); }

    bool open(const Config& cfg, Log& log) override {
        close();

        if (cfg.serialPort <= 0) {
            log.warn("No serial port selected. Pick one on the Settings tab.");
            return false;
        }

        char name[32];
        // The \\.\ prefix is required for COM10 and above; harmless below it.
        snprintf(name, sizeof(name), "\\\\.\\COM%d", cfg.serialPort);

        // Read as well as write: the sketch reports back on this line, and
        // its raw HID dumps are the only way to work out an unusual mouse's
        // report layout.
        // Overlapped, so a write can never block the control thread. A
        // synchronous handle returns only once the driver has taken the
        // bytes, and if the device stalls that stall lands directly on a
        // loop that is supposed to tick every millisecond or two.
        h_ = CreateFileA(name, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) {
            h_ = nullptr;
            log.error("Could not open COM" + std::to_string(cfg.serialPort) +
                      ". Check the port number in Device Manager, and that no "
                      "other program (Arduino IDE serial monitor) has it open.");
            return false;
        }

        DCB dcb{};
        dcb.DCBlength = sizeof(dcb);
        if (!GetCommState(h_, &dcb)) { close(); log.error("GetCommState failed."); return false; }
        dcb.BaudRate = cfg.baudRate;
        dcb.ByteSize = 8;
        dcb.Parity   = NOPARITY;
        dcb.StopBits = ONESTOPBIT;

        // Do NOT assert DTR. On a 32u4 board -- Leonardo, Micro, Pro Micro --
        // asserting DTR on open resets the sketch, exactly as the Arduino IDE
        // does when it uploads. If that board is also acting as a USB mouse,
        // the reset takes the HID device down with it and any pass-through
        // mouse stops responding until the sketch comes back.
        //
        // Nothing here needs hardware flow control, so both lines stay low.
        dcb.fDtrControl  = DTR_CONTROL_DISABLE;
        dcb.fRtsControl  = RTS_CONTROL_DISABLE;
        dcb.fOutxCtsFlow = FALSE;
        dcb.fOutxDsrFlow = FALSE;
        dcb.fDsrSensitivity = FALSE;
        if (!SetCommState(h_, &dcb)) { close(); log.error("SetCommState failed."); return false; }

        // Belt and braces: clear the lines explicitly in case the driver had
        // them asserted already.
        EscapeCommFunction(h_, CLRDTR);
        EscapeCommFunction(h_, CLRRTS);

        // Every timeout zero: neither reads nor writes may ever block the
        // control thread.
        // Reads: MAXDWORD interval with both totals zero means "return
        // whatever is already buffered, immediately".
        //
        // Writes: zero for both total fields does NOT mean no waiting, it
        // means no timeout at all -- an unbounded wait. That was the
        // opposite of what was wanted here. A bound is set as a second line
        // of defence behind the overlapped handle.
        COMMTIMEOUTS to{};
        to.ReadIntervalTimeout         = MAXDWORD;
        to.ReadTotalTimeoutConstant    = 0;
        to.ReadTotalTimeoutMultiplier  = 0;
        to.WriteTotalTimeoutConstant   = 20;
        to.WriteTotalTimeoutMultiplier = 0;
        SetCommTimeouts(h_, &to);

        readPending_ = false;
        wrOv_ = OVERLAPPED{};
        wrOv_.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        rdOv_ = OVERLAPPED{};
        rdOv_.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        btnOv_ = OVERLAPPED{};
        btnOv_.hEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
        btnPending_ = false;
        btnQueue_.clear();
        writePending_ = false;

        buf_[0] = 0xA5;
        accX_ = accY_ = 0.0f;
        lastSend_ = 0;
        proto_ = cfg.serialProtocol;
        setRate(cfg.serialRateHz);
        ready_ = true;
        log.info(std::string("Output: Arduino on COM") +
                 std::to_string(cfg.serialPort) + " at " +
                 std::to_string(cfg.baudRate) + " baud, " +
                 (cfg.serialProtocol == ProtoAscii ? "ASCII x,y,click"
                                                   : "binary packet") + ".");
        log.info("DTR is held low, so opening the port does not reset the "
                 "board or interrupt any HID pass-through.");
        return true;
    }

    void setRate(int hz) { intervalNs_ = 1'000'000'000LL / std::max(30, hz); }

    void flush() override {
        // The accumulator is the queue here: movement is gathered between
        // packets and sent at the configured rate, so whatever is sitting in
        // it would go out on the next tick regardless of the key. Buttons
        // are deliberately left alone -- a queued release must still happen.
        accX_ = accY_ = 0.0f;
    }

    // Returns without waiting, always. If the previous write has not
    // finished, this one is skipped and its movement stays in the
    // accumulator to go out with the next packet -- which is why the
    // accumulator exists.
    bool writeAsync(const void* data, DWORD len) {
        if (!h_ || !wrOv_.hEvent) return false;

        if (writePending_) {
            DWORD done = 0;
            if (!GetOverlappedResult(h_, &wrOv_, &done, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return false;
            }
            writePending_ = false;
            ResetEvent(wrOv_.hEvent);
        }

        DWORD written = 0;
        if (WriteFile(h_, data, len, &written, &wrOv_)) return true;
        if (GetLastError() == ERROR_IO_PENDING) { writePending_ = true; return true; }
        return false;
    }

    void send(float dx, float dy, int count) override {
        if (!ready_ || !h_) return;

        // Accumulate and flush on a timer rather than writing per control
        // tick. At 1 kHz the board spends all its time draining the serial
        // buffer and its own USB work starves -- which shows up as the
        // pass-through mouse going sluggish the moment output engages.
        accX_ += dx;
        accY_ += dy;
        lastCount_ = count;

        const int64_t now = now_ns();
        if (now - lastSend_ < intervalNs_) return;
        lastSend_ = now;

        if (accX_ == 0.0f && accY_ == 0.0f) return;

        const int16_t ix = (int16_t)std::clamp(accX_, -32000.0f, 32000.0f);
        const int16_t iy = (int16_t)std::clamp(accY_, -32000.0f, 32000.0f);
        accX_ -= (float)ix;
        accY_ -= (float)iy;
        count = lastCount_;

        if (proto_ == ProtoAscii) {
            // Text, because that is what the firmware on the other end
            // expects. Costs a few bytes and an atoi per packet, which is
            // nothing next to being able to use a known-good binary.
            char line[32];
            int n = snprintf(line, sizeof(line), "%d,%d,0\n", (int)ix, (int)iy);
            // snprintf reports the length it *would* have written, not the
            // length it did. On truncation that value exceeds the buffer, and
            // using it as a copy length reads past the end of one array and
            // writes past the end of another. The values here cannot overflow
            // thirty-two characters today, but the guard costs nothing and
            // the next format string might.
            if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
            if (n > (int)sizeof(txBuf_)) n = (int)sizeof(txBuf_);
            if (n > 0) {
                // The staging copy has to outlive the call: an overlapped
                // write reads from this buffer after returning.
                memcpy(txBuf_, line, (size_t)n);
                if (!writeAsync(txBuf_, (DWORD)n)) {
                    // Still busy. Put the movement back so it goes out with
                    // the next packet rather than being silently dropped.
                    accX_ += (float)ix;
                    accY_ += (float)iy;
                }
            }
            drainIncoming();
            return;
        }

        buf_[1] = (uint8_t)((ix >> 8) & 0xFF);
        buf_[2] = (uint8_t)(ix & 0xFF);
        buf_[3] = (uint8_t)((iy >> 8) & 0xFF);
        buf_[4] = (uint8_t)(iy & 0xFF);
        buf_[5] = (uint8_t)std::min(count, 255);
        buf_[6] = buf_[1] ^ buf_[2] ^ buf_[3] ^ buf_[4] ^ buf_[5];

        if (!writeAsync(buf_, sizeof(buf_))) {
            accX_ += (float)ix;
            accY_ += (float)iy;
        }

        drainIncoming();
    }

    void poll() override {
        flushButtons();
        drainIncoming();
    }

    // One queued press per call, so a burst cannot monopolise the port.
    void flushButtons() {
        if (btnQueue_.empty() || !h_ || !btnOv_.hEvent) return;

        if (btnPending_) {
            DWORD done = 0;
            if (!GetOverlappedResult(h_, &btnOv_, &done, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return;
            }
            btnPending_ = false;
            ResetEvent(btnOv_.hEvent);
        }

        const auto ev = btnQueue_.front();
        int n = 0;
        if (proto_ == ProtoBinary) {
            // A framed packet the sketch can tell apart from movement: the
            // same header, then a marker byte in the click field that no
            // movement ever sets, followed by the button and its state.
            btnBuf_[0] = (char)0xA5;
            btnBuf_[1] = (char)0x00;
            btnBuf_[2] = (char)0x00;
            btnBuf_[3] = (char)0x00;
            btnBuf_[4] = (char)0x00;
            btnBuf_[5] = (char)(0x80 | ((ev.first & 0x03) << 1) |
                                (ev.second ? 1 : 0));
            btnBuf_[6] = (char)(btnBuf_[1] ^ btnBuf_[2] ^ btnBuf_[3] ^
                                btnBuf_[4] ^ btnBuf_[5]);
            n = 7;
        } else {
            char line[16];
            n = snprintf(line, sizeof(line), "b%d,%d\n",
                         ev.first, ev.second ? 1 : 0);
            // Same reasoning as the movement packet: a truncating snprintf
            // returns more than it wrote.
            if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
            if (n > (int)sizeof(btnBuf_)) n = (int)sizeof(btnBuf_);
            if (n <= 0) { btnQueue_.pop_front(); return; }
            memcpy(btnBuf_, line, (size_t)n);
        }

        DWORD written = 0;
        if (WriteFile(h_, btnBuf_, (DWORD)n, &written, &btnOv_)) {
            btnQueue_.pop_front();
        } else if (GetLastError() == ERROR_IO_PENDING) {
            btnPending_ = true;
            btnQueue_.pop_front();
        }
    }

    // Anything the board sends back, a line at a time.
    //
    // Deliberately does not touch the log. Logging takes a mutex the UI
    // thread also holds and, until recently, wrote to disk synchronously --
    // neither belongs on a loop that is supposed to tick every millisecond.
    // Lines are queued here and the UI drains them.
    void drainIncoming() {
        if (!h_ || !rdOv_.hEvent) return;

        // A proper pending-read state machine, with the buffer as a member.
        //
        // Issuing an overlapped read into a local array and then abandoning
        // it leaves the driver writing into a stack frame that no longer
        // exists. Waiting for it instead would block the control thread.
        // Neither is acceptable, so the read is left outstanding between
        // calls and collected when it completes.
        DWORD got = 0;

        if (readPending_) {
            if (!GetOverlappedResult(h_, &rdOv_, &got, FALSE)) {
                if (GetLastError() == ERROR_IO_INCOMPLETE) return;
                readPending_ = false;
                ResetEvent(rdOv_.hEvent);
                return;
            }
            readPending_ = false;
            ResetEvent(rdOv_.hEvent);
        } else {
            if (!ReadFile(h_, rxBuf_, sizeof(rxBuf_), &got, &rdOv_)) {
                if (GetLastError() == ERROR_IO_PENDING) { readPending_ = true; }
                return;
            }
        }

        if (got == 0) return;

        for (DWORD i = 0; i < got; ++i) {
            const char c = rxBuf_[i];
            if (c == '\n' || c == '\r') {
                if (!rxLine_.empty()) {
                    if (lines_ && lineMutex_) {
                        std::lock_guard<std::mutex> lk(*lineMutex_);
                        // Bounded: a sketch printing constantly must not be
                        // able to grow this without limit.
                        if (lines_->size() < 64) lines_->push_back(rxLine_);
                    }
                    rxLine_.clear();
                }
            } else if (rxLine_.size() < 200) {
                rxLine_.push_back(c);
            }
        }
    }

    void button(int b, bool down) override {
        // Sent as its own line, immediately, outside the coalescing that
        // movement goes through.
        //
        // A press is an event: it has to arrive once, in order, and not be
        // merged with the next one. The firmware reads a leading 'b' as a
        // button command; anything older that does not understand it will
        // ignore the line rather than misinterpret a movement.
        if (!h_) return;
        // Queued rather than written here.
        //
        //
        // The movement channel has a single pending write, and a button that
        // arrived while one was in flight would simply be dropped -- which
        // for a press is not a small loss, since a missed release leaves the
        // button held. The queue is drained from poll(), which runs every
        // tick, so nothing is lost and order is preserved.
        if (btnQueue_.size() < 32) btnQueue_.push_back({std::clamp(b, 0, 2), down});
    }

    void sendLine(const char* text) override {
        if (!h_ || !text) return;
        // Queued on the button channel, which exists precisely because a
        // command must arrive exactly once and in order rather than being
        // merged with the movement stream.
        char line[24];
        int n = snprintf(line, sizeof(line), "%s\n", text);
        if (n >= (int)sizeof(line)) n = (int)sizeof(line) - 1;
        if (n <= 0) return;
        DWORD wrote = 0;
        WriteFile(h_, line, (DWORD)n, &wrote, nullptr);
    }

    void setLineSink(std::vector<std::string>* v, std::mutex* mtx) {
        lines_ = v;
        lineMutex_ = mtx;
    }

    void close() override {
        if (h_) {
            CancelIoEx(h_, nullptr);
            // Both outstanding operations are collected before the handle
            // and its buffers go away.
            DWORD done = 0;
            if (writePending_) {
                GetOverlappedResult(h_, &wrOv_, &done, TRUE);
                writePending_ = false;
            }
            if (readPending_) {
                GetOverlappedResult(h_, &rdOv_, &done, TRUE);
                readPending_ = false;
            }
            CloseHandle(h_);
            h_ = nullptr;
        }
        if (wrOv_.hEvent) { CloseHandle(wrOv_.hEvent); wrOv_.hEvent = nullptr; }
        if (rdOv_.hEvent) { CloseHandle(rdOv_.hEvent); rdOv_.hEvent = nullptr; }
        if (btnOv_.hEvent) { CloseHandle(btnOv_.hEvent); btnOv_.hEvent = nullptr; }
        ready_ = false;
        rxLine_.clear();
    }

    const char* name() const override { return "Arduino"; }
    bool ready() const override { return ready_; }

private:
    HANDLE  h_ = nullptr;
    bool    ready_ = false;
    uint8_t buf_[7]{};
    char    txBuf_[40]{};
    char    btnBuf_[16]{};
    OVERLAPPED btnOv_{};
    bool    btnPending_ = false;
    std::deque<std::pair<int, bool>> btnQueue_;
    OVERLAPPED wrOv_{};
    OVERLAPPED rdOv_{};
    bool    writePending_ = false;
    bool    readPending_  = false;
    char    rxBuf_[256]{};
    float   accX_ = 0.0f, accY_ = 0.0f;
    int     lastCount_ = 0;
    int64_t lastSend_ = 0;
    int64_t intervalNs_ = 1'000'000'000LL / 500;
    std::string rxLine_;
    std::vector<std::string>* lines_ = nullptr;
    std::mutex* lineMutex_ = nullptr;
    int         proto_ = ProtoBinary;
};

} // namespace

// ------------------------------------------------------------ controller

Controller::~Controller() { close(); }

void Controller::close() {
    // Last chance: a button still down when the program exits stays down for
    // the rest of the session, and nothing in loopcore will be running to
    // put it back up.
    releaseActionButton();
    if (running_.exchange(false) && thread_.joinable()) thread_.join();

    // No lock needed below, and taking one would be misleading.
    //
    // The control thread has been joined on the line above, so this is the
    // only thread left that can reach the sink. sinkMutex_ exists to order
    // the UI against the control loop, and there is no longer a control loop
    // to order against.
    if (sink_) { sink_->close(); sink_.reset(); }
    sinkMethod_ = -1;
    sinkPort_   = -1;
    reset();
}

void Controller::setTuneGain(bool on, float sensitivity) {
    tuneGain_ = sensitivity;
    tuneOn_ = on;
}

void Controller::setForceEngage(bool on) { forceEngage_ = on; }
void Controller::setTunePlain(bool on) { tunePlain_ = on; }

void Controller::nudgeRaw(float dx, float dy) {
    std::lock_guard<std::mutex> lk(sinkMutex_);
    if (sink_) sink_->send(dx, dy, 0);
}

void Controller::sendBoardLine(const char* text) {
    std::lock_guard<std::mutex> lk(sinkMutex_);
    if (sink_) sink_->sendLine(text);
}

void Controller::releaseOutput() {
    // Before the sink goes away, not after.
    releaseActionButton();
    {
        std::lock_guard<std::mutex> lk(sinkMutex_);
        if (sink_) { sink_->close(); sink_.reset(); }
    }
    sinkMethod_ = -1;
    sinkPort_   = -1;
    std::lock_guard<std::mutex> lk(m_);
    st_.sinkReady = false;
    st_.sinkName  = "none";
}

void Controller::reset() {
    // Not called for the action state: reset() runs whenever tracking is
    // interrupted, which happens constantly, and dropping a held button
    // every time the target blinked would make Hold unusable. The action has
    // its own release paths.
    haveSmooth_ = false;
    smoothX_ = smoothY_ = 0;
    prevX_ = prevY_ = 0;
    velX_ = velY_ = 0;
    rawVelX_ = rawVelY_ = 0;
    accX_ = accY_ = 0;
    abPosX_ = abPosY_ = 0;
    haveAb_ = false;
    abPathX_ = abPathY_ = 0.0f;
    havePrevBox_ = false;
    haveStab_ = false;
    haveKalman_ = false;
    kalmanPosX_ = kalmanPosY_ = 0.0f;
    haveBox_ = false;
    lastLeadX_ = lastLeadY_ = 0.0f;
    leadVelX_ = leadVelY_ = 0.0f;
    turnedNow_ = false;
    userVelX_ = 0.0f; userVelY_ = 0.0f;
    velMeanX_ = velMeanY_ = 0.0f;
    velVarX_ = velVarY_ = 0.0f;
    leadTrust_ = 1.0f;
    edgeVarTop_ = edgeVarBottom_ = edgeVarLeft_ = edgeVarRight_ = 0.0f;
    jitterRatio_ = 0.0f;
    // The learned scale deliberately survives a target change: it is a
    // property of the game, not of whatever is being aimed at.
    prevNs_ = 0;
    carryX_ = carryY_ = 0;
    remainX_ = remainY_ = 0;
    havePlan_ = false;
    sent_.clear();
    // The path restarts with the plan. reset() runs whenever tracking is
    // interrupted, and a curve resumed from its middle would begin a fresh
    // move at whatever speed the last one happened to end at.
    pathState_.clear();
    traceCount_ = 0; traceHead_ = 0; traceX_ = traceY_ = 0.0f;
    // toggleOn_ deliberately survives: losing the target should not silently
    // disarm a toggle the user switched on.
}

void Controller::configure(const Config& cfg, Log& log) {
    if (!running_.load()) {
        running_ = true;
        thread_ = std::thread(&Controller::threadMain, this);
        log.info("Control thread started at " +
                 std::to_string(cfg.controlRateHz) + " Hz.");
    }
    {
        std::lock_guard<std::mutex> lk(obsMutex_);
        obsCfg_ = cfg;
    }

    const bool needReopen = !sink_ ||
                            sinkMethod_ != cfg.inputMethod ||
                            // Both serial methods care about the port; only
                            // the Arduino has a protocol setting.
                            ((cfg.inputMethod == InputArduino ||
                              cfg.inputMethod == InputMakcu) &&
                             sinkPort_ != cfg.serialPort) ||
                            (cfg.inputMethod == InputArduino &&
                             sinkProto_ != cfg.serialProtocol);
    if (!needReopen) return;

    // A button held through a sink swap would never be released, because the
    // object that could release it is about to be destroyed.
    releaseActionButton();

    {
        // Swapped under the lock, so a send already running on the control
        // thread finishes against the old sink instead of a freed one.
        std::lock_guard<std::mutex> lk(sinkMutex_);
        if (sink_) { sink_->close(); sink_.reset(); }

        if (cfg.inputMethod == InputArduino) {
            auto a = std::make_unique<ArduinoSink>();
            a->setLineSink(&boardLines_, &lineMutex_);
            sink_ = std::move(a);
        } else if (cfg.inputMethod == InputMakcu) {
            sink_ = std::make_unique<MakcuSink>();
        } else {
            sink_ = std::make_unique<MouseSink>();
        }
        sink_->open(cfg, log);
    }
    sinkMethod_ = cfg.inputMethod;
    sinkPort_   = cfg.serialPort;
    sinkProto_  = cfg.serialProtocol;

    std::lock_guard<std::mutex> lk(sinkMutex_);
    std::lock_guard<std::mutex> lk2(m_);
    st_.sinkName  = sink_ ? sink_->name() : "none";
    st_.sinkReady = sink_ && sink_->ready();
}

// Called from the capture thread. Picks a target and records it; all the
// motion happens on the control thread.
void Controller::submit(const std::vector<Det>& dets, float originX,
                        float originY, const Config& cfg, int64_t stampNs)
{
    if (!cfg.controlEnabled || !cfg.aiEnabled) {
        std::lock_guard<std::mutex> lk(obsMutex_);
        obsValid_ = false;
        obsCfg_ = cfg;
        return;
    }

    // Prefer whatever we were already tracking. A detection far from the
    // last one, while the last one is still fresh, is a different object or
    // a false positive -- and following it is how the aim ends up thousands
    // of pixels away.
    const int64_t nowNs = now_ns();
    const bool trackFresh = haveTrack_ &&
                            (nowNs - trackNs_) < (int64_t)cfg.staleMs * 1'000'000LL;
    const float assocR = std::max(80.0f, cfg.jumpRejectPx);

    // Sticky drops its hold the moment the key is released, so the next
    // press is free to pick something new.
    const bool keyHeldNow = cfg.activationMode == ActAlways ||
                            (cfg.activationKey > 0 &&
                             (GetAsyncKeyState(cfg.activationKey) & 0x8000) != 0) ||
                            (cfg.activationKey2 > 0 &&
                             (GetAsyncKeyState(cfg.activationKey2) & 0x8000) != 0);
    if (cfg.targetPriority == PrioSticky && !keyHeldNow && stickyHeld_) {
        stickyHeld_ = false;
        haveTrack_ = false;
    }

    // Sticky lowers the bar a little for the box it is already on. A target
    // whose confidence dips for a frame or two is still the same target, and
    // dropping it there is what makes tracking feel brittle.
    const float stickyFloor = std::max(0.05f, cfg.confThresh * 0.6f);

    int best = -1;
    float bestKey = 0.0f;

    // The action region, when enabled: detection still covers the whole
    // field of view, but nothing moves until a box actually reaches here.
    const float ah = cfg.actionFovOn ? cfg.actionFov * 0.5f : 0.0f;
    const float aL = originX - ah, aR = originX + ah;
    const float aT = originY - ah, aB = originY + ah;

    // Class rank: earlier in the order wins outright, whatever the geometry
    // says. A class the user put first should never lose to a nearer box of
    // a class they put last.
    auto classRank = [&cfg](int cls) -> int {
        if (!cfg.classFilterOn) return 0;
        for (int i = 0; i < cfg.classOrderCount && i < 32; ++i)
            if (cfg.classOrder[i] == cls) return i;
        return 1000;   // unranked classes come last
    };

    int bestRank = 1 << 30;
    // Counted so the reason for an empty selection can be reported.
    int rejectedOutside = 0;

    for (size_t i = 0; i < dets.size(); ++i) {
        const Det& d = dets[i];

        // The relaxed floor applies only to the box already being held.
        float floorHere = cfg.confThresh;
        if ((cfg.stickyTarget || cfg.targetPriority == PrioSticky) && trackFresh) {
            const float cx0 = (d.x1 + d.x2) * 0.5f;
            const float cy0 = (d.y1 + d.y2) * 0.5f;
            const float ddx0 = cx0 - trackX_, ddy0 = cy0 - trackY_;
            if (ddx0 * ddx0 + ddy0 * ddy0 < assocR * assocR)
                floorHere = stickyFloor;
        }
        if (d.score < floorHere) continue;

        if (cfg.classFilterOn && d.cls >= 0 && d.cls < 32 &&
            !(cfg.classMask & (1u << d.cls)))
            continue;

        // Association is a preference, not a filter.
        //
        // This used to skip every detection outside the radius whenever a
        // track was fresh, so the moment the tracked target disappeared
        // nothing else could be selected at all -- not even a target sitting
        // in plain view -- until the track went stale, which is up to
        // staleMs of doing nothing for no reason.
        //
        // Ranking instead keeps the stickiness that association is for while
        // letting the search fall through to whatever else is there. An
        // associated candidate outranks an unassociated one whatever their
        // scores, so a held target is still held.
        bool associated = true;
        if (trackFresh) {
            const float tcx = (d.x1 + d.x2) * 0.5f;
            const float tcy = (d.y1 + d.y2) * 0.5f;
            const float ddx = tcx - trackX_, ddy = tcy - trackY_;
            associated = (ddx * ddx + ddy * ddy <= assocR * assocR);
        }

        // Touching counts, not just containing: a target entering the edge
        // of the region should already be acted on.
        if (cfg.actionFovOn &&
            (d.x2 < aL || d.x1 > aR || d.y2 < aT || d.y1 > aB)) {
            ++rejectedOutside;
            continue;
        }

        const float cx = (d.x1 + d.x2) * 0.5f;
        const float cy = (d.y1 + d.y2) * 0.5f;
        const float w  = d.x2 - d.x1;
        const float h  = d.y2 - d.y1;
        const float dx = cx - originX, dy = cy - originY;

        float key = 0.0f;
        switch (cfg.targetPriority) {
        case PrioLargest:    key = w * h; break;
        case PrioSmallest:   key = -(w * h); break;
        case PrioConfidence: key = d.score; break;
        case PrioOverlap:
            // Prefer a box the origin already sits inside; among those,
            // the tightest. Anything else loses to all of them.
            if (originX >= d.x1 && originX <= d.x2 &&
                originY >= d.y1 && originY <= d.y2)
                key = 1e6f - w * h;
            else
                key = -(dx * dx + dy * dy);
            break;
        case PrioClosestSmallest: {
            // Distance decides between separate targets. Among boxes that
            // overlap each other, the smaller one wins outright: an enclosing
            // box is almost never the thing worth aiming at.
            bool enclosedByOther = false;
            for (size_t j = 0; j < dets.size(); ++j) {
                if (j == i) continue;
                const Det& o = dets[j];
                if (o.score < cfg.confThresh) continue;
                const float ow = o.x2 - o.x1, oh = o.y2 - o.y1;
                if (ow * oh >= w * h) continue;          // not smaller
                // Overlapping at all is enough; containment is too strict
                // when boxes are jittering.
                const float ix = std::min(d.x2, o.x2) - std::max(d.x1, o.x1);
                const float iy = std::min(d.y2, o.y2) - std::max(d.y1, o.y1);
                if (ix > 0.0f && iy > 0.0f &&
                    (ix * iy) > 0.35f * std::min(w * h, ow * oh)) {
                    enclosedByOther = true;
                    break;
                }
            }
            key = -(dx * dx + dy * dy);
            if (enclosedByOther) key -= 1e7f;    // a smaller one is available
            break;
        }
        case PrioIntent: {
            // Distance still matters, but agreement with the hand's
            // direction matters more: a target being moved toward is
            // preferred over a nearer one being moved away from.
            const float ux = userVelX_.load(), uy = userVelY_.load();
            const float hand = std::sqrt(ux * ux + uy * uy);
            key = -(dx * dx + dy * dy);
            if (hand > 25.0f) {
                const float dist = std::sqrt(dx * dx + dy * dy);
                if (dist > 1.0f) {
                    // dx and dy point from the target to the origin, so the
                    // direction to the target is their negation.
                    const float agree = ((-dx) * ux + (-dy) * uy) /
                                        (dist * hand);
                    // Scaled by distance so the bonus means the same thing
                    // across the region rather than favouring far targets.
                    key += agree * dist * 2.0f;
                }
            }
            break;
        }
        case PrioSticky:
            // Stay put. Anything already associated scores far above the
            // rest, so a newcomer cannot take the target away.
            key = -(dx * dx + dy * dy);
            if (trackFresh) {
                const float sdx = ((d.x1 + d.x2) * 0.5f) - trackX_;
                const float sdy = ((d.y1 + d.y2) * 0.5f) - trackY_;
                if (sdx * sdx + sdy * sdy < assocR * assocR) key += 1e7f;
            }
            break;
        case PrioClosest:
        default:             key = -(dx * dx + dy * dy); break;
        }

        // Association dominates class order, which in turn dominates the
        // per-priority key. Holding the target already being tracked matters
        // more than which class it is, and both matter more than whichever
        // geometric measure the priority happens to use.
        // Sticky as a modifier rather than a mode.
        //
        // Rather than a rank of its own -- which would make it beat class
        // order and turn every other priority off -- a held target simply
        // looks nearer than it is. The chosen rule still decides between
        // candidates; it just needs a margin to overcome before it swaps.
        float keyAdj = key;
        if (cfg.stickyTarget && trackFresh && associated)
            keyAdj += cfg.stickyBiasPx * cfg.stickyBiasPx;

        const int rank = classRank(d.cls) + (associated ? 0 : 1000);
        if (best < 0 || rank < bestRank || (rank == bestRank && keyAdj > bestKey)) {
            bestRank = rank;
            bestKey  = keyAdj;
            best     = (int)i;
        }
    }

    std::lock_guard<std::mutex> lk(obsMutex_);
    obsCfg_   = cfg;
    // Targets worth reporting, not everything the decoder produced. Since
    // decoding stopped being limited by the display setting this would
    // otherwise always read as the decode cap.
    outsideFov_ = rejectedOutside;

    int usableCount = 0;
    for (const Det& dd : dets) if (dd.score >= cfg.confThresh) ++usableCount;

    if (best < 0) {
        obsOx_ = originX;
        obsOy_ = originY;
        obsCount_ = usableCount;
        obsValid_ = false;
        // Nothing matched the track. Let it go stale so the next frame can
        // acquire freely rather than being gated forever by a track that no
        // longer exists.
        if (trackFresh && dets.empty()) haveTrack_ = false;
        return;
    }

    const Det& d = dets[best];
    trackX_ = (d.x1 + d.x2) * 0.5f;
    trackY_ = (d.y1 + d.y2) * 0.5f;
    trackW_ = d.x2 - d.x1;
    trackH_ = d.y2 - d.y1;
    trackNs_ = nowNs;
    haveTrack_ = true;
    if (cfg.targetPriority == PrioSticky && keyHeldNow) stickyHeld_ = true;

    float scaleSizePx = 0.0f;
    // The size distance-scaling should use, which is not always this box.
    //
    // Detectors often emit a small box inside a large one -- a head within a
    // body, or a tightly cropped part of a target that also has a full
    // detection. Both describe the same thing at the same distance, but the
    // small one implies a target far further away than it is, so the aim
    // slows to a crawl on something standing right in front of the user.
    //
    // So: if another detection largely contains the chosen one, its size is
    // used instead. Containment is measured as the share of the chosen box
    // that lies inside the other, which is the question being asked -- a
    // plain overlap test would also match two targets standing shoulder to
    // shoulder, which are genuinely separate and should not pool their size.
    {
        const float cx1 = d.x1, cy1 = d.y1, cx2 = d.x2, cy2 = d.y2;
        const float chosenArea = std::max(1.0f, (cx2 - cx1) * (cy2 - cy1));
        float bestArea = chosenArea;

        for (const auto& o : dets) {
            if (&o == &d) continue;
            if (o.score < cfg.confThresh) continue;

            const float ix1 = std::max(cx1, o.x1);
            const float iy1 = std::max(cy1, o.y1);
            const float ix2 = std::min(cx2, o.x2);
            const float iy2 = std::min(cy2, o.y2);
            if (ix2 <= ix1 || iy2 <= iy1) continue;

            const float inside = (ix2 - ix1) * (iy2 - iy1);
            // Three quarters of the chosen box inside the other one. Below
            // that the two are neighbours rather than one within the other.
            if (inside < chosenArea * 0.75f) continue;

            const float oArea = (o.x2 - o.x1) * (o.y2 - o.y1);
            if (oArea > bestArea) bestArea = oArea;
        }
        scaleSizePx = std::sqrt(bestArea);
    }

    // Written without taking obsMutex_, as the version that works does.
    //
    // Publishing these under the lock is correct in principle: the reader
    // copies them out under it, so a torn observation is possible without it.
    // In practice adding the lock stopped the aim moving at all, and a
    // correctness argument that breaks the program is worth less than the
    // program. The writer is a single thread, the fields are word-sized, and
    // the reader tolerates a mismatched pair far better than it tolerates
    // never being told anything.
    //
    // If this is revisited, use a sequence counter rather than a mutex. The
    // capture thread must never block behind the control loop, and a lock
    // here lets it do exactly that.
    obsOx_    = originX;
    obsOy_    = originY;
    obsCount_ = usableCount;
    obsX_  = trackX_;
    obsY_  = trackY_;
    obsX1_ = d.x1; obsY1_ = d.y1;
    obsX2_ = d.x2; obsY2_ = d.y2;
    obsScaleSize_ = scaleSizePx;
    // The frame's own arrival time. Using the current time here reported
    // every detection as brand new, so the age term in the prediction was
    // short by the entire capture-to-decode latency and the lead came up
    // correspondingly short.
    obsScore_ = d.score;
    obsNs_ = stampNs ? stampNs : now_ns();
    obsValid_ = true;
}

void Controller::threadMain() {
    // Above the interface, deliberately.
    //
    // This loop has a deadline: a tick it does not get is movement that
    // arrives late and then arrives all at once. The panel has no deadline
    // at all -- a frame it misses is a frame nobody notices. Leaving both at
    // normal priority let a report build or a file write push this thread
    // aside, which is exactly what produced a lurch after copying a timing
    // report.
    //
    // Time critical is deliberately not used: this thread runs at a
    // kilohertz and starving the rest of the system would be a worse fault
    // than the one being fixed.
    // Above the interface, but not above the rate at which this becomes a
    // problem in itself.
    //
    // At a kilohertz a raised thread priority competes with the parts of
    // Windows that deliver keyboard and mouse events, and the two together --
    // a fast loop and a priority boost -- are how a program makes the whole
    // desktop feel sluggish rather than just itself. The boost is worth
    // having at ordinary rates, where the loop sleeps most of the time.
    {
        int startRate = 500;
        {
            std::lock_guard<std::mutex> lk(obsMutex_);
            startRate = std::clamp(obsCfg_.controlRateHz, 60, 2000);
        }
        SetThreadPriority(GetCurrentThread(),
                          startRate > 1000 ? THREAD_PRIORITY_NORMAL
                                           : THREAD_PRIORITY_ABOVE_NORMAL);
    }

    // Note: the warning below is checked even when the model is off, since
    // that is exactly the case it exists to report.
    // A high-resolution waitable timer holds a tight period far better than
    // sleep_for, which rounds to the system tick.
    HANDLE timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (!timer)     // pre-1803 fallback
        timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);

    int64_t last = now_ns();

    while (running_.load()) {
        Config cfg;
        {
            std::lock_guard<std::mutex> lk(obsMutex_);
            cfg = obsCfg_;
        }
        const int rate = std::clamp(cfg.controlRateHz, 60, 2000);

        // Re-armed as a one-shot every iteration rather than left running as
        // a periodic timer.
        //
        // SetWaitableTimer's period argument is a whole number of
        // milliseconds, so 1000/rate is integer division: at 2000 Hz it came
        // out as zero, which makes the timer one-shot. The wait then fell
        // through to its 50 ms timeout on every iteration and the loop ran
        // at about 20 Hz -- fifty times slower than the setting asked for,
        // and slowest at the fastest setting. The due time is in 100 ns
        // units, so re-arming each pass expresses any rate exactly.
        if (timer) {
            LARGE_INTEGER due{};
            due.QuadPart = -(LONGLONG)(10'000'000LL / rate);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 50);
        } else {
            Sleep(1);
        }

        const int64_t now = now_ns();
        float dt = (float)((now - last) / 1e9);
        last = now;
        if (dt <= 0.0f) continue;

        // A stall is not elapsed time that may be paid out.
        //
        // The old ceiling was a tenth of a second, which sounds cautious and
        // is not: the amount dispensed is 1 - exp(-gain * dt), so at a
        // typical gain a hundred-millisecond gap releases about nine tenths
        // of the entire remaining plan in one tick. That is the aim crossing
        // the screen, and it is why it happened right after copying a timing
        // report -- sorting thirteen rings of four thousand samples, or
        // writing the session file, is long enough to starve this thread.
        //
        // Time the loop did not get is not time the target spent moving in
        // any way this plan knows about. Capping at a few ticks' worth means
        // a stall costs a little tracking rather than producing a lurch.
        const float period = 1.0f / (float)std::max(60, rate);
        const float ceiling = std::min(0.02f, period * 4.0f);
        const bool stalled = (dt > ceiling * 3.0f);
        if (dt > ceiling) dt = ceiling;

        // A long gap also means the plan describes where the target was
        // before the gap, which is no longer useful. Dropping it costs one
        // detection's worth of tracking and avoids steering confidently
        // toward a stale position.
        if (stalled) {
            remainX_ = 0.0f;
            remainY_ = 0.0f;
            sent_.clear();
        }

        tick(cfg, dt);
    }

    if (timer) { CancelWaitableTimer(timer); CloseHandle(timer); }
}

void Controller::tick(const Config& cfg, float dt) {
    // The lock is held only while the sink is touched, not across the whole
    // tick. The arithmetic in between does not need it, and holding it there
    // makes the UI thread wait to change output method for no reason.
    //
    // The return channel is serviced unconditionally: reading only while
    // sending leaves a board that is talking to us unheard for the entire
    // time output is idle, which is most of the time.
    State out;
    {
        std::lock_guard<std::mutex> lk(sinkMutex_);
        if (sink_) {
            static int64_t lastPoll = 0;
            const int64_t now = now_ns();
            if (now - lastPoll > 10'000'000LL) {   // 100 Hz is ample for text
                lastPoll = now;
                sink_->poll();
            }
        }
        out.sinkName  = sink_ ? sink_->name() : "none";
        out.sinkReady = sink_ && sink_->ready();
        if (sink_) {
            out.rejected      = sink_->rejectedCount();
            out.outputBlocked = sink_->accessBlocked();
        }
    }

    bool valid;
    float rawX, rawY, ox, oy;
    float bx1, by1, bx2, by2;
    int64_t obsNs;
    int count;
    float scaleSize;
    {
        std::lock_guard<std::mutex> lk(obsMutex_);
        valid = obsValid_;
        rawX = obsX_; rawY = obsY_;
        bx1 = obsX1_; by1 = obsY1_; bx2 = obsX2_; by2 = obsY2_;
        ox = obsOx_;  oy = obsOy_;
        obsNs = obsNs_;
        count = obsCount_;
        // Copied with the rest of the observation rather than read later.
        // It is written by the capture thread, so reading it outside this
        // block would be a race for no benefit.
        scaleSize = obsScaleSize_;
    }

    // --- engage -------------------------------------------------------------
    // Mouse buttons are read through Raw Input when the output goes to a
    // board and filtering is on, so a second mouse on the desk cannot
    // trigger it. Keyboard keys are unaffected: they do not pass through the
    // shield, so there is nothing to distinguish.
    // Either key engages.
    //
    // Written as one routine applied to both rather than duplicated, because
    // the board-filtering rule below is subtle enough that two copies would
    // drift apart the first time one of them was corrected.
    auto keyIsDown = [&](int vk) -> bool {
        if (vk <= 0) return false;

        const bool isMouseButton =
            vk == VK_LBUTTON  || vk == VK_RBUTTON ||
            vk == VK_MBUTTON  || vk == VK_XBUTTON1 ||
            vk == VK_XBUTTON2;

        if (isMouseButton && cfg.inputMethod == InputArduino &&
            cfg.arduinoOnlyInput && rawin::BoardSeen() &&
            rawin::BoardResponsive()) {
            // Filtered only while the chosen device is still answering.
            //
            // Two conditions, not one. The first is that a device has ever
            // identified itself as the board: filtering before that rejects
            // every mouse and leaves nothing working at all. The second is
            // that it has answered recently.
            //
            // Reflashing the board changes the path it enumerates under, so
            // the saved identity stops matching anything. BoardSeen stays
            // true from earlier in the session, the filter stays on, and
            // every press is attributed to some other device and discarded.
            return rawin::BoardButtonDown(vk);
        }
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };

    const bool k1 = keyIsDown(cfg.activationKey);
    const bool k2 = keyIsDown(cfg.activationKey2);
    const bool keyDown = k1 || k2;
    key1Down_ = k1;
    key2Down_ = k2;

    bool engaged = false;
    switch (cfg.activationMode) {
    case ActAlways: engaged = true; break;
    case ActToggle:
        if (keyDown && !keyWasDown_) toggleOn_ = !toggleOn_;
        engaged = toggleOn_;
        break;
    case ActHold:
    default: engaged = keyDown; break;
    }
    keyWasDown_ = keyDown;
    if (forceEngage_.load()) engaged = true;

    if (keyDown && !cfg.aiEnabled) blockedByDisabled_ = true;

    // Deliberately above the "no target" early-out below.
    //
    // Reading the key after that return meant a toggle press with nothing on
    // screen was swallowed -- the next press just undid it -- and the
    // warning about pressing the key while the model is off could never fire
    // at all, because that case returns first.

    // A detection that has not been refreshed recently is not a target any
    // more. Extrapolating an old one forward is how a brief dropout turns
    // into a flick hundreds of pixels away: age keeps growing, and velocity
    // times age grows with it.
    const float obsAge = valid ? (float)((now_ns() - obsNs) / 1e9) : 0.0f;
    if (cfg.safetyOn && valid && obsAge > cfg.staleMs * 0.001f) valid = false;

    // The condition, and the action, are evaluated even on the paths that
    // return early -- otherwise a button held when the target vanished would
    // stay held.
    if (!cfg.aiEnabled || !cfg.controlEnabled || !valid) {
        // Acquired and lost are edges, so losing the target is a real event
        // rather than merely an absence.
        const bool lostNow = (cfg.actionTrigger == TrigLost) && actWasValid_;
        runAction(cfg, lostNow, now_ns());
        actWasValid_ = false;

        // The release edge is handled here as well as on the normal path.
        //
        // Letting go while no target is visible -- which is most releases --
        // returned through here without ever reaching the code that drops
        // the plan and flushes the sink, so whatever the board had
        // accumulated still went out afterwards.
        if (!engaged && wasEngaged_) {
            std::lock_guard<std::mutex> lk(sinkMutex_);
            if (sink_) sink_->flush();
        }
        wasEngaged_ = engaged;

        reset();
        // Reported truthfully even with nothing to act on, so the readout
        // distinguishes "not holding the key" from "holding it, no target".
        out.engaged = engaged;
        out.key1Down = key1Down_.load();
        out.key2Down = key2Down_.load();
        out.boxScaleMult = boxScaleMult_.load();
        out.boxScaleSize = boxScaleSize_.load();
        out.outsideActionFov = outsideFov_.load();
    {
        // Oldest first, so the consumer can draw it as a polyline without
        // knowing where the ring wraps.
        out.pathCount = traceCount_;
        for (int i = 0; i < traceCount_; ++i) {
            const int idx = (traceHead_ - traceCount_ + i + State::kPathMax * 2)
                          % State::kPathMax;
            out.pathPts[i] = { trace_[idx].x, trace_[idx].y, trace_[idx].speed };
        }
    }
        std::lock_guard<std::mutex> lk(m_);
        st_ = out;
        return;
    }

    // --- velocity, from successive distinct observations -------------------
    if (obsNs != prevNs_) {
        if (prevNs_ != 0) {
            // A very short interval is the real danger here: dividing a
            // displacement by a fraction of a millisecond produces a
            // velocity in the tens of thousands of pixels per second, and
            // the lead time then multiplies that into a huge jump. Floor it.
            float odt = (float)((obsNs - prevNs_) / 1e9);
            if (odt < 0.004f) odt = 0.004f;

            // A target that appears to have teleported is a different
            // object, or a false positive, not something moving fast. Take
            // the new position but do not infer a velocity from the gap.
            const float jump = std::sqrt((rawX - prevX_) * (rawX - prevX_) +
                                         (rawY - prevY_) * (rawY - prevY_));
            if (cfg.safetyOn && jump > cfg.jumpRejectPx) {
                velX_ = velY_ = 0.0f;
                accX_ = accY_ = 0.0f;
                haveAb_ = false;
    abPathX_ = abPathY_ = 0.0f;
                haveStab_ = false;
            } else if (odt < 0.5f) {
                // Subtract the displacement we caused between the two
                // observations. Otherwise the estimator reads our own
                // correction as target motion and predicts further in the
                // direction we just moved, which compounds the overshoot.
                float selfX = 0.0f, selfY = 0.0f;
                for (const Sent& m : sent_)
                    if (m.ns >= prevNs_ && m.ns < obsNs) { selfX += m.dx; selfY += m.dy; }

                // Learn how far the world moves per unit of output, by
                // least squares rather than by averaging ratios.
                //
                // This number carries far more weight than it looks. Once the
                // controller is tracking a moving target well, the target
                // barely moves on screen -- the output is cancelling its
                // motion -- so the raw frame-to-frame displacement is close
                // to zero and essentially all of the velocity estimate comes
                // from adding our own movement back. Get the scale wrong and
                // the recovered velocity is wrong by the same proportion,
                // which reads as a target that is plainly moving but has no
                // measured speed and therefore gets no lead.
                //
                // Averaging per-frame ratios was a poor way to find it: each
                // ratio divides by our own movement, so the frames that
                // carry least information produce the wildest numbers and
                // count equally. A decaying least-squares slope weights each
                // sample by how much it actually says.
                // Only samples that can actually say something.
                //
                // A moving target being tracked well travels with the view,
                // so our output produces almost no relative displacement --
                // and a regression fed those frames concludes that output
                // moves the world hardly at all. The estimate collapses, the
                // plan divides by it, and the aim flies across the screen.
                // That is not a hypothetical: it reached 0.021 against a
                // true value near 0.4, a factor of twenty.
                //
                // Two gates keep it honest. Our own movement has to dominate,
                // and the world has to have moved the way our own movement
                // implies. A target travelling with us fails the second and
                // is discarded rather than averaged in.
                if (std::fabs(selfX) > 4.0f) {
                    const float dx = -(rawX - prevX_);   // world moves opposite

                    // The sample has to be consistent with the estimate it
                    // is meant to refine, not merely pointing the right way.
                    //
                    // A target being tracked well travels with the view, so
                    // the world barely displaces however hard we push -- and
                    // a regression fed those frames concludes that output
                    // moves the world hardly at all. That is what drove the
                    // estimate down to a sixth of its true value and made
                    // the aim fling across the screen. Requiring the
                    // observed displacement to be within a factor of a few
                    // of the predicted one excludes exactly those frames,
                    // and excludes the opposite failure too.
                    const float predicted = selfX * effectiveScale_.load();
                    const float ratio = (std::fabs(predicted) > 0.5f)
                                      ? dx / predicted : 0.0f;
                    // Wide while nothing is known, narrow once it is.
                    //
                    // The gate compares against the current estimate, so a
                    // strict one applied from the first frame could never
                    // reach a true value far from where it started. It opens
                    // for the first few dozen samples and closes as the
                    // figure earns confidence.
                    const bool early = scaleSamples_.load() < 40;
                    const bool agrees = early ? (ratio > 0.08f && ratio < 12.0f)
                                              : (ratio > 0.35f && ratio < 2.8f);
                    if (agrees) {
                        sxx_ = sxx_ * 0.995f + selfX * selfX;
                        sxy_ = sxy_ * 0.995f + selfX * dx;
                        if (sxx_ > 16.0f) {
                            const float slope = sxy_ / sxx_;
                            // A plausible range. Outside it the estimate is
                            // wrong rather than surprising, and acting on it
                            // is worse than keeping the last good one.
                            // Moved toward, never jumped to.
                            //
                            // A single bad batch should not be able to
                            // relocate a figure the whole loop divides by.
                            // Creeping means a genuine change still arrives
                            // within a second or two, while a burst of
                            // misleading frames barely registers.
                            if (slope > 0.08f && slope < 6.0f) {
                                const float cur = scaleEst_.load();
                                const float lo2 = early ? cur * 0.5f : cur * 0.85f;
                                const float hi2 = early ? cur * 2.0f : cur * 1.18f;
                                const float capped = std::clamp(slope, lo2, hi2);
                                scaleEst_ = capped;
                                const int n = scaleSamples_.load();
                                if (n < 100000) scaleSamples_ = n + 1;
                            }

                        }
                    }
                }

                // What the loop actually divides by, decided here and
                // nowhere else, on every observation.
                //
                // A half-converged estimate was being used the moment it
                // existed, and a wrong one is not a small error: the plan is
                // divided by it, so a value ten times too high makes every
                // move ten times too small. Sensitivity cannot recover that,
                // because it scales the payout of a plan that is already
                // short. It reads as the aim being sluggish for several
                // seconds and then snapping right once the estimate settles,
                // which is exactly what was reported.
                //
                // So the configured value is used until the estimate has
                // earned trust, and the changeover is eased rather than
                // stepped.
                {
                    const float manual = std::clamp(cfg.responseScale, 0.05f, 8.0f);
                    if (!cfg.autoResponse && !forceAuto_.load()) {
                        effectiveScale_ = manual;
                    } else {
                        const int n2 = scaleSamples_.load();
                        if (n2 < 30) {
                            effectiveScale_ = manual;
                        } else {
                            const float w = std::min(1.0f, (n2 - 30) / 60.0f);
                            effectiveScale_ = manual * (1.0f - w) +
                                              scaleEst_.load() * w;
                        }
                    }
                }

                const float k = effectiveScale_.load();

                float dispX = (rawX - prevX_) + selfX * k;
                float dispY = (rawY - prevY_) + selfY * k;

                if (cfg.predictionMethod == PredEdgeConsensus && havePrevBox_) {
                    // Compare the two edges of each axis. Moving together is
                    // travel; moving apart is the box breathing on an object
                    // that has not gone anywhere. The centre averages the two
                    // and so cannot tell the difference -- which is exactly
                    // why a static target appears to have velocity.
                    const float dx1 = (bx1 - pX1_) + selfX * k;
                    const float dx2 = (bx2 - pX2_) + selfX * k;
                    const float dy1 = (by1 - pY1_) + selfY * k;
                    const float dy2 = (by2 - pY2_) + selfY * k;

                    // Jitter scales with box size, so the deadband does too.
                    const float bw = std::max(1.0f, bx2 - bx1);
                    const float bh = std::max(1.0f, by2 - by1);
                    const float dbX = bw * cfg.jitterReject * 0.01f;
                    const float dbY = bh * cfg.jitterReject * 0.01f;

                    auto consensus = [](float a, float b, float dead) -> float {
                        // Disagreeing signs mean pure resize: no translation.
                        if ((a > 0.0f) != (b > 0.0f)) return 0.0f;
                        // Agreeing: average them.
                        //
                        // Taking the smaller magnitude was deliberately
                        // conservative, but it is biased: box noise makes one
                        // edge move less than the other on almost every
                        // frame, so the smaller one systematically
                        // under-reports real travel -- by around a quarter in
                        // ordinary conditions, which is a quarter of the lead
                        // thrown away. Averaging is unbiased for symmetric
                        // noise and still rejects a pure resize outright.
                        const float m = (a + b) * 0.5f;
                        return std::fabs(m) < dead ? 0.0f : m;
                    };

                    const float tdx = consensus(dx1, dx2, dbX);
                    const float tdy = consensus(dy1, dy2, dbY);

                    const float rawMag = std::sqrt(dispX * dispX + dispY * dispY);
                    const float conMag = std::sqrt(tdx * tdx + tdy * tdy);
                    jitterRatio_ = rawMag > 0.01f
                                 ? std::clamp(1.0f - conMag / rawMag, 0.0f, 1.0f)
                                 : 0.0f;

                    dispX = tdx;
                    dispY = tdy;
                }

                // Uncompensated, purely for the readout: what the box looks
                // like it is doing before our own movement is accounted for.
                screenVelX_ = (rawX - prevX_) / odt;
                screenVelY_ = (rawY - prevY_) / odt;

                // The compensation carries its own uncertainty, and it is
                // proportional to its own size.
                //
                // Adding our movement back is a subtraction of two similar
                // numbers, so what survives is dominated by the error in the
                // scale rather than by anything the target did. On a
                // stationary target the residual is selfX*(estimated minus
                // true scale) -- which points the same way we are moving
                // whenever the estimate is high. Lead follows it, the aim
                // moves further, the residual grows: the aim walks off a
                // still target in the direction it approached from.
                //
                // Below that error bar there is no measurement to be had, so
                // none is claimed. Genuine motion clears the floor easily,
                // because it is not proportional to our own output.
                // Wider while the scale is still being learned, because the
                // error bar genuinely is wider then.
                const float rel = std::max(0.01f, cfg.selfMotionFloor * 0.01f) *
                                  (responseLearned() ? 1.0f : 2.5f);

                // Two sources of noise, not one.
                //
                // The compensation term has error proportional to its own
                // size, which the first part covers. But the detector's own
                // wobble does not care whether we were moving: on an axis we
                // are not moving along at all, the first part is nearly zero
                // and box jitter passes straight through as velocity. That is
                // what produces vertical twitching on a target crossing
                // horizontally -- there is no real vertical motion to
                // measure, only the box breathing.
                //
                // Box jitter scales with box size, so the floor does too.
                const float boxNoiseX = std::max(1.0f, bx2 - bx1) *
                                        cfg.jitterReject * 0.01f;
                const float boxNoiseY = std::max(1.0f, by2 - by1) *
                                        cfg.jitterReject * 0.01f;

                const float floorX = std::fabs(selfX * k) * rel + boxNoiseX + 0.4f;
                const float floorY = std::fabs(selfY * k) * rel + boxNoiseY + 0.4f;
                if (std::fabs(dispX) < floorX) dispX = 0.0f;
                if (std::fabs(dispY) < floorY) dispY = 0.0f;

                // Velocity decays when nothing confirms it.
                //
                // Every estimator here is a filter with memory, so a reading
                // that was once real outlives the motion that produced it.
                // On a still target that shows up as the aim continuing to
                // push one way after it has arrived. A frame that produced
                // no usable displacement should pull the estimate down
                // rather than leave it standing.
                if (dispX == 0.0f && dispY == 0.0f) {
                    const float decay = expf(-odt / 0.12f);
                    velX_ *= decay;  velY_ *= decay;
                    accX_ *= decay;  accY_ *= decay;
                    if (haveKalman_) { kx_[1] *= decay; ky_[1] *= decay; }
                }

                float instX = dispX / odt;
                float instY = dispY / odt;

                // Nothing on screen moves faster than this. Anything that
                // claims to is a measurement artefact.
                const float mag = std::sqrt(instX * instX + instY * instY);
                if (cfg.safetyOn && mag > cfg.maxSpeedPx) {
                    const float k2 = cfg.maxSpeedPx / mag;
                    instX *= k2;
                    instY *= k2;
                }
                const float prevVX = velX_, prevVY = velY_;
                rawVelX_ = instX; rawVelY_ = instY;

                switch (cfg.predictionMethod) {
                case PredLinear:
                    velX_ = instX; velY_ = instY;
                    break;
                case PredAccel:
                    velX_ = velX_ * 0.6f + instX * 0.4f;
                    velY_ = velY_ * 0.6f + instY * 0.4f;
                    accX_ = accX_ * 0.8f + ((velX_ - prevVX) / odt) * 0.2f;
                    accY_ = accY_ * 0.8f + ((velY_ - prevVY) / odt) * 0.2f;
                    break;
                case PredAlphaBeta: {
                    // Tracks the compensated path, not the screen position.
                    //
                    // This measured rawX directly, which is where the box
                    // sits on screen -- and while the aim is keeping up, the
                    // box barely moves however fast the target is going. The
                    // filter therefore reported almost no velocity exactly
                    // when there was most to report. Kalman had the same
                    // fault and was fixed; this one was missed.
                    abPathX_ += dispX;
                    abPathY_ += dispY;

                    const float alpha = 0.35f, beta = 0.08f;
                    if (!haveAb_) {
                        abPathX_ = rawX; abPathY_ = rawY;
                        abPosX_ = rawX; abPosY_ = rawY;
                        velX_ = instX;  velY_ = instY;
                        haveAb_ = true;
                    } else {
                        const float pX = abPosX_ + velX_ * odt;
                        const float pY = abPosY_ + velY_ * odt;
                        const float rx = abPathX_ - pX, ry = abPathY_ - pY;
                        abPosX_ = pX + alpha * rx;
                        abPosY_ = pY + alpha * ry;
                        // Dividing a residual by the frame interval amplifies
                        // it: at five milliseconds a two-pixel disagreement
                        // becomes three hundred pixels a second of velocity
                        // correction. Bounded so a single noisy frame cannot
                        // throw the estimate across the screen.
                        const float bumpX = std::clamp((beta * rx) / odt,
                                                       -400.0f, 400.0f);
                        const float bumpY = std::clamp((beta * ry) / odt,
                                                       -400.0f, 400.0f);
                        velX_ += bumpX;
                        velY_ += bumpY;
                    }
                    break;
                }
                case PredKalman: {
                    // Constant-velocity model. Process noise says how much
                    // the target may accelerate between frames; measurement
                    // noise says how much the detector wobbles. The ratio of
                    // the two is what decides whether it trusts the model or
                    // the measurement, and it adapts as the covariance grows.
                    const float q = cfg.kalmanProcess;
                    const float r = cfg.kalmanMeasure;
                    auto step = [&](float* x, float P[2][2], float z) {
                        // Predict.
                        x[0] += x[1] * odt;
                        const float dt2 = odt * odt;
                        P[0][0] += odt * (P[1][0] + P[0][1]) + dt2 * P[1][1]
                                 + q * dt2 * dt2 * 0.25f;
                        P[0][1] += odt * P[1][1] + q * dt2 * odt * 0.5f;
                        P[1][0] += odt * P[1][1] + q * dt2 * odt * 0.5f;
                        P[1][1] += q * dt2;

                        // Update.
                        const float S = P[0][0] + r;
                        const float k0 = P[0][0] / S;
                        const float k1 = P[1][0] / S;
                        const float y = z - x[0];
                        x[0] += k0 * y;
                        x[1] += k1 * y;
                        const float p00 = P[0][0], p01 = P[0][1];
                        P[0][0] -= k0 * p00;
                        P[0][1] -= k0 * p01;
                        P[1][0] -= k1 * p00;
                        P[1][1] -= k1 * p01;
                    };

                    // Fed the compensated position, not the raw one.
                    //
                    // Every other estimator works from dispX, which has our
                    // own movement added back. This one was measuring rawX
                    // directly, so it tracked where the box sat on screen
                    // rather than where the target was going -- and while the
                    // aim is keeping up, the box barely moves on screen. It
                    // therefore reported almost no velocity and led almost
                    // nothing, which is exactly what was seen.
                    //
                    // The measurement is a virtual position that accumulates
                    // compensated displacement, so the filter sees the
                    // target's real path.
                    kalmanPosX_ += dispX;
                    kalmanPosY_ += dispY;

                    if (!haveKalman_) {
                        kalmanPosX_ = rawX; kalmanPosY_ = rawY;
                        kx_[0] = rawX; kx_[1] = 0.0f;
                        ky_[0] = rawY; ky_[1] = 0.0f;
                        kPx_[0][0] = kPy_[0][0] = 100.0f;
                        kPx_[1][1] = kPy_[1][1] = 100.0f;
                        kPx_[0][1] = kPx_[1][0] = 0.0f;
                        kPy_[0][1] = kPy_[1][0] = 0.0f;
                        haveKalman_ = true;
                    } else {
                        // A reversal widens the covariance before the
                        // update.
                        //
                        // A constant-velocity filter treats a turn as a
                        // measurement that disagrees with a confident model,
                        // and a confident model wins -- so it crosses over
                        // slowly and its velocity output lags the target by
                        // several frames. Inflating the uncertainty tells it
                        // the model is no longer to be trusted, which is
                        // exactly true at that moment, and it re-acquires
                        // within a frame or two instead.
                        if ((kx_[1] > 0.0f) != (instX > 0.0f) &&
                            std::fabs(kx_[1]) > 30.0f &&
                            std::fabs(instX) > 30.0f) {
                            kPx_[0][0] += 400.0f;
                            kPx_[1][1] += 40000.0f;
                        }
                        if ((ky_[1] > 0.0f) != (instY > 0.0f) &&
                            std::fabs(ky_[1]) > 30.0f &&
                            std::fabs(instY) > 30.0f) {
                            kPy_[0][0] += 400.0f;
                            kPy_[1][1] += 40000.0f;
                        }

                        step(kx_, kPx_, kalmanPosX_);
                        step(ky_, kPy_, kalmanPosY_);
                    }
                    velX_ = kx_[1];
                    velY_ = ky_[1];
                    break;
                }
                case PredEdgeConsensus:
                    // The consensus step has already rejected the noise, so
                    // this can filter gently and stay responsive.
                    velX_ = velX_ * 0.55f + instX * 0.45f;
                    velY_ = velY_ * 0.55f + instY * 0.45f;
                    break;
                case PredSmoothed:
                default:
                    velX_ = velX_ * 0.7f + instX * 0.3f;
                    velY_ = velY_ * 0.7f + instY * 0.3f;
                    break;
                }
            }
        }
        // --- denoise the measurement, not the control signal --------------
        if (cfg.emaOn) {
            // Shift the previous estimate by the movement we made in the
            // meantime first. Without that the filter is averaging positions
            // taken in different frames of reference -- the camera moved
            // between them -- so it drags against every correction and the
            // output hunts. Shifted, it only ever averages detector noise.
            const float odt2 = std::max(0.004f, (float)((obsNs - prevNs_) / 1e9));
            // Per axis: sideways movement is usually the target actually
            // moving and wants little filtering, while vertical is mostly
            // the box breathing and takes more without costing anything.
            const float tauX = 0.005f + cfg.emaIntensity  * 0.12f;
            const float tauY = 0.005f + cfg.emaIntensityY * 0.12f;
            const float ax2 = 1.0f - expf(-odt2 / tauX);
            const float ay2 = 1.0f - expf(-odt2 / tauY);

            if (!haveSmooth_) {
                smoothX_ = rawX; smoothY_ = rawY; haveSmooth_ = true;
            } else {
                float shiftX = 0.0f, shiftY = 0.0f;
                for (const Sent& m : sent_)
                    if (m.ns >= prevNs_ && m.ns < obsNs) { shiftX += m.dx; shiftY += m.dy; }
                const float kk = effectiveScale_.load();
                smoothX_ -= shiftX * kk;
                smoothY_ -= shiftY * kk;

                smoothX_ += ax2 * (rawX - smoothX_);
                smoothY_ += ay2 * (rawY - smoothY_);
            }
        } else {
            smoothX_ = rawX; smoothY_ = rawY; haveSmooth_ = true;
        }

        if (cfg.predictionMethod == PredEdgeConsensus) {
            // Move the aim point by the translation we believe in, then pull
            // it weakly toward the raw centre. The pull stops it drifting
            // away over time; being weak stops per-frame wobble reaching it.
            const float tdx = velX_ * (float)((obsNs - prevNs_) / 1e9);
            const float tdy = velY_ * (float)((obsNs - prevNs_) / 1e9);
            if (!haveStab_) { stabX_ = rawX; stabY_ = rawY; haveStab_ = true; }
            else {
                stabX_ += tdx + 0.06f * (rawX - stabX_);
                stabY_ += tdy + 0.06f * (rawY - stabY_);
            }
        }

        // Track how much each edge moves. Squared frame-to-frame deltas,
        // filtered, which is enough to say which edge is the quieter one.
        if (havePrevBox_) {
            const float dT = by1 - pY1_, dB = by2 - pY2_;
            const float dL = bx1 - pX1_, dR = bx2 - pX2_;
            edgeVarTop_    = edgeVarTop_    * 0.9f + dT * dT * 0.1f;
            edgeVarBottom_ = edgeVarBottom_ * 0.9f + dB * dB * 0.1f;
            edgeVarLeft_   = edgeVarLeft_   * 0.9f + dL * dL * 0.1f;
            edgeVarRight_  = edgeVarRight_  * 0.9f + dR * dR * 0.1f;
        }

        // Size is smoothed separately. Anchoring to an edge only helps if
        // the height used with it is not itself jumping around.
        {
            const float w = bx2 - bx1, h = by2 - by1;
            if (!haveBox_) { boxW_ = w; boxH_ = h; haveBox_ = true; }
            else { boxW_ = boxW_ * 0.8f + w * 0.2f; boxH_ = boxH_ * 0.8f + h * 0.2f; }
        }

        pX1_ = bx1; pY1_ = by1; pX2_ = bx2; pY2_ = by2;
        havePrevBox_ = true;
        prevX_ = rawX; prevY_ = rawY; prevNs_ = obsNs;

        // --- plan the move ------------------------------------------------
        // Everything the loop will do until the next detection is decided
        // here, once, from fresh information.
        float baseX0, baseY0;
        if (cfg.predictionMethod == PredKalman && haveKalman_) {
            // The filter now tracks a virtual path, so its position is not a
            // screen coordinate any more. Only its velocity is used, and the
            // aim comes from the smoothed measurement like everything else.
            baseX0 = haveSmooth_ ? smoothX_ : rawX;
            baseY0 = haveSmooth_ ? smoothY_ : rawY;
        } else if (cfg.predictionMethod == PredEdgeConsensus && haveStab_) {
            baseX0 = stabX_;
            baseY0 = stabY_;
        } else {
            baseX0 = haveSmooth_ ? smoothX_ : rawX;
            baseY0 = haveSmooth_ ? smoothY_ : rawY;
        }

        // Aim offsets are a share of the box, not of the screen: half way up
        // a distant target and a near one should mean the same thing. The
        // smoothed size is used so the offset does not jump with the box.
        const float bw0 = std::max(1.0f, haveBox_ ? boxW_ : bx2 - bx1);
        const float bh0 = std::max(1.0f, haveBox_ ? boxH_ : by2 - by1);

        // The aim point, worked out from the box actually being reported.
        //
        // The smoothed size was being combined with the current box edges,
        // and the two do not describe the same rectangle. When the smoothed
        // height was larger than the real one, an edge anchor placed the aim
        // outside the box -- and the clamp meant to catch that was built from
        // the same mismatched pair, so its lower bound could exceed its upper
        // bound and it produced nonsense rather than a correction. That is
        // the aim sitting above the top of the box.
        //
        // Everything here now comes from one rectangle. The smoothed size is
        // still used, but only to steady the reference point, never mixed
        // with a different box's edges.
        // One centre and one size, and they belong together.
        //
        // The centre is the filtered one, so the aim does not inherit box
        // jitter. The size is the current box's, because that is what an
        // offset expressed as a share of the box has to mean. The edges are
        // then derived from those two rather than taken from a different
        // rectangle, which is what made the bounds cross.
        const float cw = std::max(1.0f, bx2 - bx1);
        const float ch = std::max(1.0f, by2 - by1);

        // The filtered centre, but pulled back to the box it is meant to be
        // describing.
        //
        // Every filter here drifts: the alpha-beta and Kalman states
        // integrate velocity, and the consensus stabiliser is only weakly
        // tied to the measurement on purpose. Building the aim point AND the
        // bounds that were supposed to contain it from that same drifting
        // centre meant nothing could ever catch the drift -- the box moved
        // with it. That is the aim sitting well above the real box.
        //
        // Anchoring the centre to the reported box first makes the filtering
        // a way to steady the aim rather than a way to lose it.
        const float realCx = (bx1 + bx2) * 0.5f;
        const float realCy = (by1 + by2) * 0.5f;
        const float ccx = std::clamp(baseX0, realCx - cw * 0.35f,
                                             realCx + cw * 0.35f);
        const float ccy = std::clamp(baseY0, realCy - ch * 0.35f,
                                             realCy + ch * 0.35f);
        const float top = ccy - ch * 0.5f;
        const float bot = ccy + ch * 0.5f;

        int anchor = cfg.boxAnchor;
        if (anchor == 3)
            anchor = (edgeVarTop_ <= edgeVarBottom_) ? 1 : 2;

        // Positive Y is upward, and the offset is a share of this box.
        const float offY = std::clamp(cfg.aimOffYPct, -50.0f, 50.0f) * 0.01f;
        const float offX = std::clamp(cfg.aimOffXPct, -50.0f, 50.0f) * 0.01f;

        // All three forms reduce to the same point at zero offset, which is
        // what makes the anchor setting a choice about noise rather than a
        // change of aim.
        float aimY;
        if (anchor == 1)      aimY = top + ch * (0.5f - offY);
        else if (anchor == 2) aimY = bot - ch * (0.5f + offY);
        else                  aimY = ccy - ch * offY;

        float aimX = ccx + cw * offX;

        // Bounded by the box the detector actually reported, which is the
        // only rectangle that cannot have drifted. Written with explicit
        // ordering because a clamp whose lower bound exceeds its upper is
        // undefined, and that is how a bad aim point escaped last time.
        {
            const float loX = std::min(bx1 + 1.0f, bx2 - 1.0f);
            const float hiX = std::max(bx1 + 1.0f, bx2 - 1.0f);
            const float loY = std::min(by1 + 1.0f, by2 - 1.0f);
            const float hiY = std::max(by1 + 1.0f, by2 - 1.0f);
            aimX = std::clamp(aimX, loX, hiX);
            aimY = std::clamp(aimY, loY, hiY);
        }

        if (cfg.predictionOn && !tunePlain_.load()) {
            // The velocity that feeds the lead is filtered separately from
            // the one the estimators produce.
            //
            // A lead is an offset proportional to velocity, so noise in the
            // velocity lands directly on where the aim points, and an aim
            // chasing its own jitter is exactly what orbiting is. This is
            // deliberately slower than the estimators: it is better for the
            // lead to arrive a fraction late than to shake.
            {
                // Scaled with the horizon, then abandoned on a real turn.
                //
                // A long lead needs a steady velocity to multiply, so the
                // window that produces one is proportional to how far ahead
                // it is being projected. That is right while a target holds
                // its course and badly wrong the moment it changes: a filter
                // wide enough to smooth a second of noise takes most of a
                // second to notice a reversal, by which time the target has
                // turned again and the lead has never once pointed the right
                // way.
                //
                // So the window is wide only while the new measurement
                // agrees with the filtered one. A large disagreement is not
                // noise -- noise is small by definition -- and the filter
                // gives way to it almost immediately.
                const float base = std::max(0.01f, cfg.leadSmoothMs * 0.001f);
                float tau = cfg.leadSmoothScales
                          ? std::max(base, cfg.predictionMs * 0.001f * 0.6f)
                          : base;

                {
                    const float curMag = std::sqrt(leadVelX_ * leadVelX_ +
                                                   leadVelY_ * leadVelY_);
                    const float newMag = std::sqrt(velX_ * velX_ + velY_ * velY_);
                    const float dx2 = velX_ - leadVelX_;
                    const float dy2 = velY_ - leadVelY_;
                    const float change = std::sqrt(dx2 * dx2 + dy2 * dy2);
                    const float scale2 = std::max(40.0f,
                                                  std::max(curMag, newMag));

                    // How far the measurement has moved, relative to how
                    // fast the target is going. A reversal is a change of
                    // twice the speed, so this reaches one well before that.
                    const float rel = std::min(1.0f, change / scale2);
                    // Squared, so ordinary jitter barely shortens the window
                    // while a genuine turn collapses it.
                    tau *= (1.0f - 0.94f * rel * rel);
                    tau = std::max(0.008f, tau);
                    // Reported so the delay can be seen rather than
                    // inferred: this is how long the lead takes to follow a
                    // change of direction.
                    leadLagMs_ = tau * 1000.0f;
                }
                const float odt3 = std::max(0.004f,
                                            (float)((obsNs - prevNs_) / 1e9));
                const float a3 = 1.0f - expf(-odt3 / tau);
                // A reversal is snapped to, not eased toward.
                //
                // Easing is right for noise and wrong for a turn. While the
                // filter crosses zero on its way to the new value, the lead
                // it produces still points the old way -- so for those
                // frames the aim is actively pushed away from where the
                // target now is, which is worse than having no lead at all.
                // It reads exactly as the aim hanging back where the target
                // used to be.
                //
                // A sign change with real magnitude on both sides is not
                // something noise produces, so it is taken at face value and
                // the filter restarts from the new measurement.
                auto turned = [](float oldV, float newV) {
                    return (oldV > 0.0f) != (newV > 0.0f) &&
                           std::fabs(oldV) > 30.0f && std::fabs(newV) > 30.0f;
                };

                if (turned(leadVelX_, velX_)) { leadVelX_ = velX_; turnedNow_ = true; }
                else leadVelX_ += a3 * (velX_ - leadVelX_);

                if (turned(leadVelY_, velY_)) { leadVelY_ = velY_; turnedNow_ = true; }
                else leadVelY_ += a3 * (velY_ - leadVelY_);
            }

            // How much of the lead to actually apply, from how steady the
            // velocity has been.
            //
            // A lead multiplies velocity by a horizon, so it multiplies the
            // error in that velocity too. At forty milliseconds a wobbling
            // estimate is barely visible; at a second it throws the aim back
            // and forth across the target, which is why a long lead has
            // previously only been usable with the gain turned right down.
            // Turning the gain down is the wrong lever, though: it weakens
            // the lead when the estimate is good as well as when it is bad.
            //
            // Comparing the spread of the velocity against its mean says
            // which of those is happening. A target genuinely travelling has
            // a mean far larger than its spread and keeps its full lead; one
            // whose velocity is mostly noise has the two comparable, and its
            // lead is cut in proportion.
            {
                // The spread is allowed to forget a turn quickly.
                //
                // A reversal is a huge, entirely legitimate excursion, and a
                // slow variance estimate reads it as the velocity having
                // become unreliable -- so trust collapses and the lead is
                // cut for a second or more, exactly when it was finally
                // pointing the right way. Letting the estimate decay lets it
                // recover within a few frames of the new course settling.
                const float aX = 0.18f, aY = 0.18f;
                velMeanX_ += aX * (leadVelX_ - velMeanX_);
                velMeanY_ += aY * (leadVelY_ - velMeanY_);
                const float dX = leadVelX_ - velMeanX_;
                const float dY = leadVelY_ - velMeanY_;
                velVarX_ += aX * (dX * dX - velVarX_);
                velVarY_ += aY * (dY * dY - velVarY_);

                // A turn resets the spread rather than being counted into it.
                //
                // A reversal is a huge and entirely legitimate excursion, so
                // feeding it to a variance estimate reads it as the velocity
                // having become unreliable -- trust collapses and the lead is
                // cut for the next second, which is precisely the second in
                // which it had finally become correct. Restarting the
                // statistics at the new course is the honest thing: nothing
                // measured before the turn describes what is happening after
                // it.
                if (turnedNow_) {
                    velMeanX_ = leadVelX_;
                    velMeanY_ = leadVelY_;
                    velVarX_ = velVarY_ = 0.0f;
                    turnedNow_ = false;
                }

                const float meanMag = std::sqrt(velMeanX_ * velMeanX_ +
                                                velMeanY_ * velMeanY_);
                const float sd = std::sqrt(std::max(0.0f, velVarX_ + velVarY_));
                // Guarded so a stationary target, where both are near zero,
                // does not produce a meaningless ratio.
                leadTrust_ = (meanMag + sd > 1.0f)
                           ? std::clamp(meanMag / (meanMag + sd), 0.0f, 1.0f)
                           : 1.0f;
            }

            // The horizon and the steadying window go together. Predicting a
            // second ahead while reacting to every tenth of a second of
            // velocity is incoherent: a target that changes direction that
            // fast makes the prediction wrong whatever it is smoothed by.
            const float lead = cfg.predictionMs * 0.001f * cfg.predictionGain;
            const float trust = cfg.leadTrustOn ? leadTrust_.load() : 1.0f;
            float leadX = leadVelX_ * lead * trust;
            float leadY = leadVelY_ * lead * trust;

            // A lead must not point against the freshest measurement.
            //
            // The whole purpose of a lead is to aim where the target is
            // going. Whenever the filtered velocity disagrees in sign with
            // the latest one, the filter is behind the target rather than
            // ahead of it, and the offset it produces is pointing at where
            // the target came from. There is no version of that which helps,
            // so it is dropped rather than scaled: no lead is a worse aim
            // than a correct lead and a much better one than a backwards
            // lead.
            //
            // This is the guard that catches every remaining case the snap
            // above misses -- a turn too gentle to trip the sign test, an
            // estimator with its own inertia, a stale trust figure.
            if (std::fabs(rawVelX_) > 25.0f &&
                (leadX > 0.0f) != (rawVelX_ > 0.0f)) leadX = 0.0f;
            if (std::fabs(rawVelY_) > 25.0f &&
                (leadY > 0.0f) != (rawVelY_ > 0.0f)) leadY = 0.0f;

            // Sideways only, when asked.
            //
            // Applied before the bounds below rather than after, so nothing
            // downstream has to reason about a vertical component that is
            // not there.
            if (cfg.leadHorizontalOnly) leadY = 0.0f;

            // The lead is bounded by the target it is leading.
            //
            // A lead is added after the aim point has been clamped into the
            // box, deliberately, so that a genuinely fast target can be led
            // past its own edge. The cost of that freedom is that a vertical
            // velocity which is really detector wobble can carry the aim
            // clear above the box on a target moving only sideways -- which
            // is what was happening.
            //
            // Bounding each axis by the box's own extent in that axis keeps
            // the useful case and removes the failure: leading a fast target
            // to its leading edge still works, and no amount of noise can
            // take the aim somewhere the target has never been. The bound
            // opens up as measured speed rises, so a genuinely fast crossing
            // is not clipped.
            {
                const float openX = 0.5f + std::min(1.5f,
                    std::fabs(leadVelX_) / std::max(120.0f, cfg.maxSpeedPx * 0.25f));
                const float openY = 0.5f + std::min(1.5f,
                    std::fabs(leadVelY_) / std::max(120.0f, cfg.maxSpeedPx * 0.25f));
                // Taken from the reported box directly: the derived width
                // and height are computed further down, and this must not
                // depend on the order those happen to appear in.
                const float boxW = std::max(1.0f, bx2 - bx1);
                const float boxH = std::max(1.0f, by2 - by1);
                const float capX = boxW * openX;
                const float capY = boxH * openY;
                leadX = std::clamp(leadX, -capX, capX);
                leadY = std::clamp(leadY, -capY, capY);
            }

            // The minor axis is distrusted in proportion to how minor it is.
            //
            // Real movement is nearly always dominated by one direction. A
            // target crossing sideways has no meaningful vertical velocity,
            // so a vertical lead is measuring detector wobble -- and since a
            // lead is a position offset, that wobble becomes visible bobbing
            // on an otherwise clean horizontal track. Scaling by the ratio
            // between the axes leaves genuine diagonal movement alone, which
            // a fixed threshold would not.
            {
                const float ax = std::fabs(leadVelX_);
                const float ay = std::fabs(leadVelY_);
                const float cut = std::clamp(cfg.minorAxisCut * 0.01f, 0.0f, 1.0f);
                const float big = std::max(ax, ay);
                if (big > 1.0f && cut > 0.0f) {
                    if (ax < ay) {
                        const float ratio = ax / big;      // 0 pure vertical
                        leadX *= 1.0f - cut * (1.0f - ratio);
                    } else {
                        const float ratio = ay / big;      // 0 pure horizontal
                        leadY *= 1.0f - cut * (1.0f - ratio);
                    }
                }
            }
            if (cfg.predictionMethod == PredAccel) {
                leadX += 0.5f * accX_ * lead * lead;
                leadY += 0.5f * accY_ * lead * lead;
            }

            // Bounded by what the target could plausibly have covered in the
            // time being predicted, not by the size of its box. Sizing this
            // to the box is what made prediction appear to do nothing: a
            // target crossing at 500 px/s needs 20 px of lead over 40 ms,
            // and a 17 px wide box clamped that away entirely.
            if (cfg.safetyOn) {
                const float maxLead = std::max(24.0f, cfg.maxSpeedPx * lead);
                const float mag = std::sqrt(leadX * leadX + leadY * leadY);
                if (mag > maxLead) {
                    leadX *= maxLead / mag;
                    leadY *= maxLead / mag;
                }
            }

            aimX += leadX;
            aimY += leadY;
            lastLeadX_ = leadX;
            lastLeadY_ = leadY;
        } else {
            lastLeadX_ = lastLeadY_ = 0.0f;
        }

        // The scale is always required: it is what turns a distance in
        // pixels into a number of output units. Using the lag compensation
        // value here meant that switching compensation off divided by nearly
        // zero and multiplied every move by twenty.
        const float sc = std::max(0.02f, effectiveScale_.load());

        float flightX = 0.0f, flightY = 0.0f;
        if (cfg.lagCompOn) {
            // Movement sent after this observation was taken is on its way
            // and is not reflected in it, so it comes off the plan.
            for (const Sent& m : sent_)
                if (m.ns >= obsNs) { flightX += m.dx; flightY += m.dy; }
        }

        remainX_ = (aimX - ox) / sc - flightX;
        remainY_ = (aimY - oy) / sc - flightY;

        // A plan is bounded by the distance it is meant to cover.
        //
        // The per-tick step clamp bounds one tick, not the plan, so a scale
        // that has collapsed still pays out the maximum step every tick
        // until the plan is exhausted -- forty-five pixels at five hundred
        // hertz is the aim crossing the screen. Bounding the plan itself, in
        // units that assume the most output any plausible scale would need,
        // makes a wrong scale slow rather than catastrophic.
        if (cfg.safetyOn) {
            const float errPx = std::sqrt((aimX - ox) * (aimX - ox) +
                                          (aimY - oy) * (aimY - oy));
            // The lowest scale worth believing. Below this the estimate is
            // broken, not merely small.
            constexpr float kMinPlausibleScale = 0.05f;
            const float capUnits = errPx / kMinPlausibleScale;
            const float mag = std::sqrt(remainX_ * remainX_ + remainY_ * remainY_);
            if (mag > capUnits && mag > 1.0f) {
                const float f = capUnits / mag;
                remainX_ *= f;
                remainY_ *= f;
            }
        }
        havePlan_ = true;
    }

    // --- extrapolate to now ------------------------------------------------
    // The aim point and the whole move were worked out when the observation
    // arrived. Nothing between detections re-decides where to go; the ticks
    // only pay out what was already planned.



    // The hand's own movement, read every tick.
    //
    // Two features depend on it now -- the assist and the intent priority --
    // and it has to be drained regardless, because the accumulator it comes
    // from would otherwise grow between the moments something happened to
    // want it and report a burst rather than a speed.
    {
        // Measured over a fixed window, not per tick.
        //
        // A mouse reports at its own rate, which is usually slower than this
        // loop runs. Dividing each drain by the tick interval turned one
        // count into a spike of several hundred pixels a second, and every
        // drain that found nothing pulled the estimate back toward zero --
        // so at a 125 Hz mouse and a 1000 Hz loop, seven decays landed
        // between each real sample and the hand read far slower than it was
        // moving. Accumulating over twenty milliseconds and dividing once
        // makes the figure independent of both rates.
        float ux = 0.0f, uy = 0.0f;
        rawin::TakeMouseDelta(ux, uy);
        handAccX_ += ux;
        handAccY_ += uy;

        const int64_t nowH = now_ns();
        if (handWindowNs_ == 0) handWindowNs_ = nowH;
        const int64_t span = nowH - handWindowNs_;
        if (span >= 20'000'000LL) {
            const float secs = (float)(span / 1e9);
            const float vx = handAccX_ / secs;
            const float vy = handAccY_ / secs;
            userVelX_ = userVelX_.load() * 0.5f + vx * 0.5f;
            userVelY_ = userVelY_.load() * 0.5f + vy * 0.5f;
            handAccX_ = handAccY_ = 0.0f;
            handWindowNs_ = nowH;
        }
    }

    // --- dispense the plan --------------------------------------------------
    const float sc2 = std::max(0.02f, effectiveScale_.load());
    const float errPx = std::sqrt(remainX_ * remainX_ + remainY_ * remainY_) * sc2;

    // Letting go throws the plan away.
    //
    // Dispensing was gated on `engaged`, which stops output at the moment
    // the key comes up -- but the plan itself survived. Two consequences,
    // and the second is the one that was reported: movement already handed
    // to the sink continues to arrive for as long as it takes to be
    // consumed, and the next press resumes paying out a plan computed for
    // where the target was before the release.
    //
    // A plan is only meaningful while its owner is asking for it, so
    // releasing the key discards it outright.
    if (!engaged && wasEngaged_) {
        remainX_ = remainY_ = 0.0f;
        carryX_ = carryY_ = 0.0f;
        havePlan_ = false;
        sent_.clear();
        // A new engagement starts its curve from the beginning rather than
        // inheriting the middle of the last one.
        pathState_.clear();
        // Anything queued in the sink is dropped too, so the aim stops when
        // the key does rather than a few packets later.
        {
            std::lock_guard<std::mutex> lk(sinkMutex_);
            if (sink_) sink_->flush();
        }
    }
    wasEngaged_ = engaged;

    float dx = 0, dy = 0;
    if (engaged && havePlan_) {
        if (errPx <= (float)cfg.deadzonePx) {
            // Close enough. Drop the rest of the plan rather than nibbling at
            // it, which is what makes it sit still instead of jittering.
            remainX_ = remainY_ = 0.0f;
            carryX_ = carryY_ = 0.0f;
        } else {
            // Exponential approach over the remaining move, expressed per
            // second so the feel does not change with the tick rate.
            float sens = tuneOn_.load() ? tuneGain_.load() : cfg.sensitivity;

            // Eased off on smaller targets, when asked.
            //
            // Applied to the approach rate rather than to the plan, which
            // matters: the plan is still the full distance to the aim point,
            // so a distant target is reached in the end -- just less
            // abruptly. Scaling the plan instead would leave the aim short.
            //
            // Only ever reduces. Growing the rate on a large close target
            // would make the twitchiest case twitchier, which is the
            // opposite of the problem this solves.
            if (cfg.boxScaleOn && !tuneOn_.load()) {
                // Measured from the box's area, not its height.
                //
                // Both shrink with distance, but area uses the whole
                // detection: height alone is thrown off by a target that is
                // crouched, prone, or clipped at the edge of the region,
                // where the width is still telling the truth. Area moves as
                // the square of distance, so the square root of it is taken
                // -- that puts the figure back on the same scale as a height
                // in pixels, which is what the setting is expressed in and
                // what someone can actually judge by eye.
                // Taken from the snapshot rather than recomputed here: the
                // decision about which box represents this target was made
                // when it was chosen, where every other detection was still
                // visible. By this point only the chosen one is.
                float size = scaleSize;
                if (size <= 1.0f) {
                    const float boxW = std::max(1.0f, bx2 - bx1);
                    const float boxH = std::max(1.0f, by2 - by1);
                    size = std::sqrt(boxW * boxH);
                }
                const float ratio = std::clamp(
                    size / std::max(8.0f, cfg.boxScaleRefPx), 0.0f, 1.0f);
                // The 0-to-1 setting becomes the exponent the curve needs.
                //
                // Zero means no drop-off at all -- anything to the power of
                // nothing is one -- and the top of the range is steep enough
                // that a distant target crawls. Mapping it here rather than
                // storing an exponent keeps the number in the config the
                // same as the number on the slider.
                const float k = std::clamp(cfg.boxScaleStrength, 0.0f, 1.0f) * 3.0f;
                const float mult = std::pow(ratio, k);

                // The floor is a sensitivity, not a fraction, so it is
                // applied after the multiply rather than to it. That is what
                // makes "stay above 0.20" mean exactly that, whatever the
                // sensitivity above happens to be.
                // Captured before the value is changed, so the ratio below
                // compares against what the user set rather than against the
                // result of this very calculation.
                const float before = sens;
                const float floorSens = std::clamp(cfg.boxScaleFloor, 0.0f, before);
                sens = std::max(floorSens, before * mult);

                // Reported as what actually happened, not what the curve
                // asked for: the floor may have overridden it.
                boxScaleMult_ = (before > 0.0001f) ? (sens / before) : 1.0f;
                boxScaleSize_ = size;
            } else {
                boxScaleMult_ = 1.0f;
            }

            // Scaled by whether the hand agrees with the correction.
            //
            // Raw mouse movement is read separately from the output we
            // ourselves send, so this reflects the user's hand rather than
            // our own contribution. Moving toward the target while being
            // pulled toward it overshoots; moving away while being pulled
            // back is the aim fighting the hand.
            if (cfg.userAssistOn && !tuneOn_.load()) {
                const float wantMag = std::sqrt(remainX_ * remainX_ +
                                                remainY_ * remainY_);
                const float uvx = userVelX_.load(), uvy = userVelY_.load();
                const float handMag = std::sqrt(uvx * uvx + uvy * uvy);
                if (wantMag > 0.5f && handMag > 1.0f) {
                    // Agreement as a cosine, so a hand moving across the
                    // correction rather than along it changes nothing.
                    const float dot = (remainX_ * uvx + remainY_ * uvy) /
                                      (wantMag * handMag);
                    // Blended in by hand speed, so a resting hand leaves the
                    // gain exactly where the sensitivity slider put it.
                    const float amt = std::min(1.0f,
                        // Floored only against division by zero, not at a
                        // value that would quietly ignore the setting.
                        handMag / std::max(5.0f, cfg.assistSpeedRef));
                    const float target = (dot >= 0.0f)
                        ? cfg.assistWithPct * 0.01f
                        : cfg.assistAgainstPct * 0.01f;
                    const float mult = 1.0f + (target - 1.0f) *
                                              std::fabs(dot) * amt;
                    sens *= std::clamp(mult, 0.05f, 4.0f);
                }
            }

            // The chosen path shapes the payout and may step sideways.
            //
            // It only scales the rate and adds a perpendicular component; it
            // cannot set a position. So everything below -- the step clamp,
            // the speed ceiling, the sub-pixel carry -- still applies exactly
            // as before. A path is a route, not an override, which is what
            // keeps fifteen of them from each needing their own safety rules.
            const float errNowPx = std::sqrt(remainX_ * remainX_ +
                                             remainY_ * remainY_) * sc2;
            // A curve is restarted when the distance grows past it, not only
            // when the state is empty.
            //
            // Progress only advances, which is right within one movement and
            // wrong across several: tracking a moving target is a continuous
            // series of short plans, and after the first one finished the
            // progress stayed pinned at the end of its curve forever. Every
            // path then ran permanently at its final rate -- around half
            // speed for most of them -- and never recovered.
            //
            // Restarting when the remaining distance exceeds what the curve
            // was drawn for gives each new plan its own curve.
            if (errNowPx > 1.0f &&
                (pathState_.startDist <= 1.0f ||
                 errNowPx > pathState_.startDist * 1.25f ||
                 pathState_.progress > 0.995f))
                pathState_.begin(errNowPx,
                                 (uint32_t)(now_ns() & 0xFFFFFFFF));

            const PathStep pstep =
                ShapePath(cfg.movePath, pathState_, errNowPx,
                          std::max(1.0f, boxScaleSize_.load()),
                          dt, cfg.movePathAmount, cfg.movePathRamp);

            // The direction of travel, taken before the plan is reduced.
            //
            // Computing it afterwards divides the shortened remainder by the
            // length it had beforehand, which is not a unit vector -- it is
            // short by exactly the fraction just paid out. The sideways step
            // would then be scaled by how far along the move it happened, in
            // a way nothing intended.
            const float remainMag = std::sqrt(remainX_ * remainX_ +
                                              remainY_ * remainY_);
            const float dirX = (remainMag > 0.001f) ? remainX_ / remainMag : 0.0f;
            const float dirY = (remainMag > 0.001f) ? remainY_ / remainMag : 0.0f;

            const float k = sens * 30.0f * pstep.rate;
            const float step = 1.0f - expf(-k * dt);

            float sx = remainX_ * step;
            float sy = remainY_ * step;
            remainX_ -= sx;
            remainY_ -= sy;

            // The sideways component, perpendicular to travel. Added after
            // the plan has been reduced, so it displaces the route without
            // changing how much of the plan is considered paid.
            if (pstep.lateral != 0.0f && errNowPx > 2.0f) {
                // Perpendicular to travel: rotate the direction a quarter
                // turn and step along it.
                sx += -dirY * pstep.lateral;
                sy +=  dirX * pstep.lateral;
            }

            // Recorded before the step is quantised, so the trace shows the
            // path the controller intended rather than the integer steps the
            // mouse received.
            {
                traceX_ += sx * sc2;
                traceY_ += sy * sc2;
                const float sp = std::sqrt(sx * sx + sy * sy) * sc2 /
                                 std::max(0.0005f, dt);
                trace_[traceHead_] = { traceX_, traceY_, sp };
                traceHead_ = (traceHead_ + 1) % State::kPathMax;
                if (traceCount_ < State::kPathMax) ++traceCount_;
            }

            dx = sx + carryX_;
            dy = sy + carryY_;

            const float ix = truncf(dx), iy = truncf(dy);
            carryX_ = dx - ix;
            carryY_ = dy - iy;
            dx = ix; dy = iy;

            if (cfg.safetyOn) {
                const float stepMag = std::sqrt(dx * dx + dy * dy) * sc2;
                if (stepMag > cfg.maxStepPx) {
                    const float kk = cfg.maxStepPx / stepMag;
                    const float keptX = truncf(dx * kk);
                    const float keptY = truncf(dy * kk);

                    // What the clamp holds back goes into the plan, not into
                    // nothing.
                    //
                    // This used to drop the difference and clear the carry
                    // as well, so a move large enough to be clamped -- which
                    // is every long move, by definition -- arrived short by
                    // however much was trimmed, every tick, and never made
                    // up the distance. The speed ceiling immediately below
                    // already returns its own remainder; the two now agree.
                    //
                    // The limit still does its job: this bounds how fast the
                    // aim travels, not how far it is allowed to go.
                    remainX_ += (dx - keptX);
                    remainY_ += (dy - keptY);
                    dx = keptX;
                    dy = keptY;
                }
            }

            // A ceiling on how far the view may actually travel, measured
            // in pixels rather than in output units.
            //
            // Everything upstream of here is denominated in units, and the
            // conversion to pixels is an estimate that has now been observed
            // to collapse in both directions -- six times too small once,
            // ten times too large another time. Whichever way it fails, the
            // result is the aim crossing the screen. This bound is the one
            // that does not depend on it: it converts back through the same
            // estimate and refuses to exceed a real speed, so a broken
            // estimate makes the aim wrong rather than dangerous.
            if (cfg.safetyOn && (dx != 0.0f || dy != 0.0f)) {
                const float scNow = std::max(0.02f, effectiveScale_.load());
                const float px = std::sqrt(dx * dx + dy * dy) * scNow;
                // The aim's own ceiling, not the target velocity ceiling.
                const float budgetPx = std::max(200.0f, cfg.maxAimSpeedPx) * dt;
                if (px > budgetPx) {
                    const float f = budgetPx / px;
                    // The original is kept rather than reconstructed by
                    // dividing the scaled value back out: that reconstruction
                    // is exact only in arithmetic, and it also handed back
                    // fractions that truncation had already removed, so the
                    // plan grew slightly on every clamped tick.
                    const float wantX = dx, wantY = dy;
                    dx = truncf(wantX * f);
                    dy = truncf(wantY * f);
                    // Left in the plan rather than discarded, so a genuine
                    // long move still completes -- it simply takes the time
                    // that travelling that far honestly takes.
                    remainX_ += (wantX - dx);
                    remainY_ += (wantY - dy);
                }
            }

            if (dx != 0.0f || dy != 0.0f) {
                std::lock_guard<std::mutex> lk(sinkMutex_);
                if (sink_) {
                    sink_->send(dx, dy, count);
                    sent_.push_back({now_ns(), dx, dy});
                }
            }
        }
    } else {
        carryX_ = carryY_ = 0.0f;
    }

    // Anything older than a second is not in flight, it is lost.
    {
        const int64_t cutoff = now_ns() - 1'000'000'000LL;
        while (!sent_.empty() && sent_.front().ns < cutoff) sent_.pop_front();
    }

    // --- the action condition ------------------------------------------
    //
    // Evaluated from the distance still to travel, in pixels, which is
    // already the difference between where the aim points and the aim point
    // inside the box -- so the offsets the user set are honoured without
    // being re-applied here.
    {
        const int64_t nowA = now_ns();
        const float errPx = std::sqrt((remainX_ * sc2) * (remainX_ * sc2) +
                                      (remainY_ * sc2) * (remainY_ * sc2));

        bool cond = false;
        const bool confOk = (obsScore_ >= cfg.actionMinConf);
        // Its own key when one is set, otherwise the activation key.
        bool keyOk = true;
        if (cfg.actionNeedsKey) {
            if (cfg.actionKey > 0)
                keyOk = (GetAsyncKeyState(cfg.actionKey) & 0x8000) != 0;
            else
                keyOk = engaged;
        }

        if (confOk && keyOk) {
            switch (cfg.actionTrigger) {
            case TrigInsideBox:
                // The aim is inside the reported box, which is a different
                // question from being near the aim point: a large box can be
                // entered while still far from its centre.
                cond = (ox >= bx1 && ox <= bx2 && oy >= by1 && oy <= by2);
                break;
            case TrigInsideBoxScaled: {
                // The same test against a box scaled about the aim point
                // rather than its own centre, so shrinking it tightens
                // toward where the user actually wants to hit rather than
                // toward the middle of the detection.
                const float f = std::max(0.05f, cfg.actionBoxPct * 0.01f);
                const float hw = (bx2 - bx1) * 0.5f * f;
                const float hh = (by2 - by1) * 0.5f * f;
                const float ax = ox + (remainX_ * sc2);
                const float ay = oy + (remainY_ * sc2);
                cond = (ox >= ax - hw && ox <= ax + hw &&
                        oy >= ay - hh && oy <= ay + hh);
                break;
            }
            case TrigAcquired:
                cond = !actWasValid_;
                break;
            case TrigLost:
                cond = false;   // handled on the early-out path above
                break;
            case TrigSettled:
                // Within range and no longer closing: the aim has arrived
                // rather than merely passed through on its way somewhere.
                cond = (errPx <= (float)cfg.actionRadiusPx) &&
                       (std::sqrt(velX_ * velX_ + velY_ * velY_) < 120.0f);
                break;
            case TrigWithinPixels:
            default:
                cond = (errPx <= (float)cfg.actionRadiusPx);
                break;
            }
        }

        runAction(cfg, cond, nowA);
        actWasValid_ = true;
        out.actionFires = actFires_.load();
        out.actionArmed = cfg.actionOn && confOk && keyOk;
        out.actionDistPx = errPx;
    }

    out.hasTarget = true;
    out.errX = remainX_ * sc2;   out.errY = remainY_ * sc2;
    out.errStamp = now_ns();
    // Taken from the snapshot read under obsMutex_ at the top of the tick,
    // not from the tracker fields -- those are written by the capture thread
    // and reading them here is a race for no benefit.
    out.targetX = rawX;  out.targetY = rawY;
    out.lastDx  = dx;  out.lastDy  = dy;
    out.velX    = velX_;      out.velY  = velY_;
    out.screenVelX = screenVelX_; out.screenVelY = screenVelY_;
    out.leadX   = lastLeadX_; out.leadY = lastLeadY_;
    out.leadTrust = leadTrust_.load();
    out.leadLagMs = leadLagMs_.load();
    out.engaged = engaged;
    out.key1Down = key1Down_.load();
    out.key2Down = key2Down_.load();
    out.boxScaleMult = boxScaleMult_.load();
    out.boxScaleSize = boxScaleSize_.load();
    out.outsideActionFov = outsideFov_.load();
    {
        // Oldest first, so the consumer can draw it as a polyline without
        // knowing where the ring wraps.
        out.pathCount = traceCount_;
        for (int i = 0; i < traceCount_; ++i) {
            const int idx = (traceHead_ - traceCount_ + i + State::kPathMax * 2)
                          % State::kPathMax;
            out.pathPts[i] = { trace_[idx].x, trace_[idx].y, trace_[idx].speed };
        }
    }

    std::lock_guard<std::mutex> lk(m_);
    st_ = out;
}

void Controller::releaseActionButton() {
    // Unconditional, and safe to call when nothing is held.
    //
    // Every path that could abandon a press -- the condition going away, the
    // key being let go, control being switched off, the sink being swapped,
    // shutdown -- funnels through here. A held button that never comes up is
    // the one fault the user cannot fix from inside the program, so the
    // release is made cheap and called liberally rather than tracked
    // carefully in each of those places.
    if (!actHeld_) return;
    const int b = actHeldButton_;
    actHeld_ = false;
    actHeldButton_ = -1;
    actStep_ = 0;
    actNextStepNs_ = 0;
    std::lock_guard<std::mutex> lk(sinkMutex_);
    if (sink_ && b >= 0) sink_->button(b, false);
}

void Controller::runAction(const Config& cfg, bool conditionNow, int64_t now) {
    if (!cfg.actionOn) {
        releaseActionButton();
        actCondition_ = false;
        return;
    }

    // Rising and falling edges, which is what every mode is expressed in
    // terms of.
    const bool rose = conditionNow && !actCondition_;
    const bool fell = !conditionNow && actCondition_;
    if (rose) actSinceNs_ = now;
    actCondition_ = conditionNow;

    const int64_t holdNs = (int64_t)cfg.actionHoldMs * 1'000'000LL;
    const int64_t coolNs = (int64_t)cfg.actionCooldownMs * 1'000'000LL;
    const int64_t gapNs  = (int64_t)cfg.actionGapMs * 1'000'000LL;
    const bool matured = conditionNow && (now - actSinceNs_ >= holdNs);
    const bool cooled  = (actLastFire_ == 0) || (now - actLastFire_ >= coolNs);

    const int btn = std::clamp(cfg.actionButton, 0, 2);

    auto press = [&](bool down) {
        std::lock_guard<std::mutex> lk(sinkMutex_);
        if (sink_) sink_->button(btn, down);
    };

    switch (cfg.actionKind) {
    case ActionHold:
        // Down while the condition holds, up when it stops. The hold delay
        // still applies, so a target crossing the threshold for a single
        // frame does not produce a flicker of a press.
        if (matured && !actHeld_) {
            actHeld_ = true;
            actHeldButton_ = btn;
            actLastFire_ = now;
            actFires_ = actFires_.load() + 1;
            press(true);
        } else if (!conditionNow && actHeld_) {
            releaseActionButton();
        }
        break;

    case ActionToggleTap:
        // One press and release per entry, alternating nothing: the toggle is
        // in the game, not here. Kept distinct from Click because it fires
        // only on the edge and never repeats while the condition holds.
        if (rose && cooled) {
            actToggleState_ = !actToggleState_;
            actLastFire_ = now;
            actFires_ = actFires_.load() + 1;
            actStep_ = 1;
            actNextStepNs_ = now + gapNs;
            actHeld_ = true;
            actHeldButton_ = btn;
            press(true);
        }
        break;

    case ActionDoubleClick:
    case ActionClick:
    default:
        // A sequence run on a clock rather than in one go.
        //
        // Pressing and releasing within the same tick produces an event a
        // game may not see at all: many read input once a frame, and a press
        // that has already been released by then never happened. Spacing the
        // steps by a settable gap makes the click as visible as a real one.
        if (actStep_ == 0) {
            if (matured && cooled) {
                actLastFire_ = now;
                actFires_ = actFires_.load() + 1;
                actStep_ = 1;
                actNextStepNs_ = now + gapNs;
                actHeld_ = true;
                actHeldButton_ = btn;
                press(true);
            }
        } else if (now >= actNextStepNs_) {
            const int total = (cfg.actionKind == ActionDoubleClick) ? 4 : 2;
            if (actStep_ % 2 == 1) {
                actHeld_ = false;
                actHeldButton_ = -1;
                press(false);
            } else {
                actHeld_ = true;
                actHeldButton_ = btn;
                press(true);
            }
            ++actStep_;
            actNextStepNs_ = now + gapNs;
            if (actStep_ >= total) {
                actStep_ = 0;
                actNextStepNs_ = 0;
            }
        }
        break;
    }

    (void)fell;
}

std::vector<std::string> Controller::takeBoardLines() {
    std::lock_guard<std::mutex> lk(lineMutex_);
    std::vector<std::string> out;
    out.swap(boardLines_);
    return out;
}

bool Controller::takeDisabledWarning() {
    bool expected = true;
    return blockedByDisabled_.compare_exchange_strong(expected, false);
}

Controller::State Controller::state() const {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

} // namespace lc
