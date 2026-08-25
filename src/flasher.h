// flasher.h -- compiles and uploads the firmware, from inside the app.
//
// This shells out to arduino-cli rather than reimplementing anything. AVR
// uploading means STK500 over a bootloader that only exists for a few
// seconds after a 1200-baud touch, and getting that wrong bricks nothing but
// wastes a lot of time. arduino-cli already does it correctly.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "common.h"

namespace lc {

enum BoardType {
    BoardLeonardo = 0,
    BoardMicro,
    BoardProMicro,
    BoardTeensyNote,     // not supported here; shown so it can be explained
    BoardCount
};

const char* BoardName(int board);
const char* BoardFqbn(int board);

// Guesses the board from a port's friendly name, or -1 if it cannot tell.
int BoardFromFriendlyName(const std::string& friendlyName);

// Looks on PATH, in the usual install locations, and in bin\tools.
// Empty when arduino-cli is not installed.
std::string FindArduinoCli();

// Copies a found arduino-cli into bin\tools so later lookups are instant.
// Returns true when one is available there afterwards.
bool AdoptArduinoCli(Log& log);

// avrdude, for flashing a prebuilt .hex. Checked next to the hex first, then
// bin\tools, then whatever arduino-cli installed.
std::string FindAvrdude(const std::string& hexPath);

// Where the sketch lives, beside the exe. Empty if it is missing.
std::string FindSketchDir();

// The bundled prebuilt firmware, beside the exe. Empty if missing.
// `withShield` picks YesHostShield over NoHostShield.
std::string FindBundledHex(bool withShield);

enum class FlashState { Idle, Running, Done, Failed };

class Flasher {
public:
    ~Flasher();

    // `port` is the COM number. `passthrough` builds the sketch with USB
    // Host Shield support, which also brings up VBUS on the shield's
    // downstream port. Returns false if a flash is already running.
    // `layout` picks the mouse report layout (0 = guess), `dump` makes the
    // board print its raw HID reports back over serial.
    bool start(int port, int board, bool passthrough, int layout, bool dump,
               Log& log);

    // Flashes an already-compiled .hex with avrdude. Nothing is built, so a
    // firmware someone else produced can be used as-is.
    bool startHex(int port, const std::string& hexPath, Log& log);

    FlashState state() const { return state_.load(); }
    bool       busy()  const { return state_.load() == FlashState::Running; }

    struct Status {
        std::string phase;
        std::string detail;
        float       fraction = 0.0f;
    };
    Status status() const;

    // True once after a successful flash, then cleared.
    bool takeCompletion();

private:
    void run(int port, int board, bool passthrough, int layout,
             bool dump, Log* log);
    void runHex(int port, std::string hexPath, Log* log);
    void setPhase(const std::string& phase, float frac, const std::string& detail = {});

    std::thread             worker_;
    std::atomic<FlashState> state_{FlashState::Idle};
    std::atomic<bool>       completion_{false};

    mutable std::mutex m_;
    Status st_;
};

} // namespace lc
