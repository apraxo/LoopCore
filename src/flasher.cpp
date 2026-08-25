// flasher.cpp
#include "flasher.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <sstream>
#include <vector>

#include "store.h"
#include "serialports.h"

namespace lc {

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::toupper(c); });
    return s;
}

bool fileExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool dirExists(const std::string& p) {
    const DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

std::string expand(const char* var, const char* tail) {
    char buf[MAX_PATH]{};
    if (!GetEnvironmentVariableA(var, buf, MAX_PATH)) return {};
    return std::string(buf) + tail;
}

// Runs a command, streaming output into the log and the status line.
bool runCommand(const std::string& cmd, Log& log, Flasher* fl,
                const char* phase, float frac,
                void (Flasher::*setter)(const std::string&, float, const std::string&),
                std::string* tailOut)
{
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return false;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');

    if (!CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        log.error("Could not start: " + cmd);
        return false;
    }
    CloseHandle(wr);

    std::string line;
    char buf[4096];
    DWORD n = 0;
    while (ReadFile(rd, buf, sizeof(buf) - 1, &n, nullptr) && n > 0) {
        buf[n] = '\0';
        for (DWORD i = 0; i < n; ++i) {
            if (buf[i] == '\n' || buf[i] == '\r') {
                if (!line.empty()) {
                    log.info(line);
                    if (tailOut) *tailOut = line;
                    if (fl && setter) (fl->*setter)(phase, frac, line);
                    line.clear();
                }
            } else {
                line.push_back(buf[i]);
            }
        }
    }
    if (!line.empty()) { log.info(line); if (tailOut) *tailOut = line; }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(rd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code == 0;
}

} // namespace

// ------------------------------------------------------------------ boards

const char* BoardName(int board) {
    switch (board) {
    case BoardMicro:      return "Arduino Micro";
    case BoardProMicro:   return "Pro Micro (32u4)";
    case BoardTeensyNote:  return "Teensy (see note)";
    default:              return "Arduino Leonardo";
    }
}

const char* BoardFqbn(int board) {
    switch (board) {
    case BoardMicro:     return "arduino:avr:micro";
    // A Pro Micro is a Micro electrically; the SparkFun core only differs in
    // its bootloader timing, and the Arduino core uploads to it fine.
    case BoardProMicro:  return "arduino:avr:micro";
    case BoardTeensyNote: return "";
    default:             return "arduino:avr:leonardo";
    }
}

int BoardFromFriendlyName(const std::string& friendlyName) {
    const std::string f = upper(friendlyName);
    if (f.find("LEONARDO") != std::string::npos) return BoardLeonardo;
    if (f.find("PRO MICRO") != std::string::npos) return BoardProMicro;
    if (f.find("MICRO") != std::string::npos)    return BoardMicro;
    if (f.find("TEENSY") != std::string::npos)   return BoardTeensyNote;
    return -1;
}

// --------------------------------------------------------------- discovery

namespace {

// One level of wildcard, since winget and Scoop both bury the binary under a
// versioned folder name that cannot be predicted.
std::string firstMatch(const std::string& parent, const std::string& leaf) {
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((parent + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string found;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if (fd.cFileName[0] == '.') continue;
        const std::string cand = parent + "\\" + fd.cFileName + "\\" + leaf;
        if (fileExists(cand)) { found = cand; break; }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

} // namespace

std::string FindArduinoCli() {
    // Already copied in: that copy wins, so a deliberate one is never
    // overridden by whatever else happens to be installed.
    const std::string local = store::binDir() + "\\tools\\arduino-cli.exe";
    if (fileExists(local)) return local;

    std::vector<std::string> candidates;

    char found[MAX_PATH]{};
    if (SearchPathA(nullptr, "arduino-cli.exe", nullptr, MAX_PATH, found, nullptr))
        candidates.push_back(found);

    // winget puts a shim here and the real binary under Packages.
    candidates.push_back(expand("LOCALAPPDATA",
        "\\Microsoft\\WinGet\\Links\\arduino-cli.exe"));
    const std::string wingetPkgs = expand("LOCALAPPDATA",
        "\\Microsoft\\WinGet\\Packages");
    if (!wingetPkgs.empty()) {
        const std::string m = firstMatch(wingetPkgs, "arduino-cli.exe");
        if (!m.empty()) candidates.push_back(m);
    }

    // The Arduino IDE ships its own copy, which is a perfectly good one.
    candidates.push_back(expand("LOCALAPPDATA",
        "\\Programs\\Arduino IDE\\resources\\app\\lib\\backend\\resources\\arduino-cli.exe"));
    candidates.push_back(expand("ProgramFiles",
        "\\Arduino IDE\\resources\\app\\lib\\backend\\resources\\arduino-cli.exe"));

    // Scoop, Chocolatey, and the plain unzip-it-somewhere cases.
    candidates.push_back(expand("USERPROFILE", "\\scoop\\shims\\arduino-cli.exe"));
    candidates.push_back("C:\\ProgramData\\chocolatey\\bin\\arduino-cli.exe");
    candidates.push_back(expand("LOCALAPPDATA", "\\Arduino15\\arduino-cli.exe"));
    candidates.push_back(expand("LOCALAPPDATA",
        "\\Programs\\arduino-cli\\arduino-cli.exe"));
    candidates.push_back(expand("ProgramFiles", "\\arduino-cli\\arduino-cli.exe"));
    candidates.push_back(expand("USERPROFILE", "\\arduino-cli\\arduino-cli.exe"));
    candidates.push_back(expand("USERPROFILE", "\\Downloads\\arduino-cli.exe"));
    candidates.push_back("C:\\arduino-cli\\arduino-cli.exe");

    for (const auto& c : candidates)
        if (!c.empty() && fileExists(c)) return c;

    return {};
}

bool AdoptArduinoCli(Log& log) {
    const std::string dst = store::binDir() + "\\tools\\arduino-cli.exe";
    if (fileExists(dst)) return true;

    const std::string src = FindArduinoCli();
    if (src.empty() || src == dst) return false;

    CreateDirectoryA((store::binDir() + "\\tools").c_str(), nullptr);
    if (!CopyFileA(src.c_str(), dst.c_str(), FALSE)) {
        // Not fatal: it will just be found again next time. Using it in
        // place is fine, the copy is only to make lookup instant.
        log.warn("Found arduino-cli at " + src +
                 " but could not copy it into bin\\tools. It will be used "
                 "where it is.");
        return false;
    }
    log.info("Copied arduino-cli from " + src + " into bin\\tools.");
    return true;
}

std::string FindAvrdude(const std::string& hexPath) {
    // Beside the hex first: bundles ship avrdude and its .conf together, and
    // a mismatched conf is worse than none.
    if (!hexPath.empty()) {
        const size_t slash = hexPath.find_last_of("\\/");
        if (slash != std::string::npos) {
            const std::string dir = hexPath.substr(0, slash);
            if (fileExists(dir + "\\avrdude.exe")) return dir + "\\avrdude.exe";
        }
    }
    const std::string local = store::binDir() + "\\tools\\avrdude.exe";
    if (fileExists(local)) return local;

    char found[MAX_PATH]{};
    if (SearchPathA(nullptr, "avrdude.exe", nullptr, MAX_PATH, found, nullptr))
        return found;

    // arduino-cli installs one as part of the AVR core.
    const std::string pkg = expand("LOCALAPPDATA",
        "\\Arduino15\\packages\\arduino\\tools\\avrdude");
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((pkg + "\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if (fd.cFileName[0] == '.') continue;
            const std::string cand = pkg + "\\" + fd.cFileName + "\\bin\\avrdude.exe";
            if (fileExists(cand)) { FindClose(h); return cand; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    return {};
}

std::string FindSketchDir() {
    const char* name = "loopcore_mouse";
    const std::string ino = std::string(name) + ".ino";
    const std::string roots[] = {
        store::appDir() + "\\firmware\\" + name,
        store::appDir() + "\\..\\firmware\\" + name,
        store::appDir() + "\\..\\..\\firmware\\" + name,
    };
    for (const auto& c : roots)
        if (dirExists(c) && fileExists(c + "\\" + ino)) return c;
    return {};
}

std::string FindBundledHex(bool withShield) {
    const std::string leaf = withShield ? "YesHostShield.hex" : "NoHostShield.hex";
    const std::string candidates[] = {
        store::appDir() + "\\firmware\\hex\\" + leaf,
        store::appDir() + "\\..\\firmware\\hex\\" + leaf,
        store::appDir() + "\\..\\..\\firmware\\hex\\" + leaf,
    };
    for (const auto& c : candidates)
        if (fileExists(c)) return c;
    return {};
}

// ---------------------------------------------------------------- flasher

Flasher::~Flasher() {
    if (worker_.joinable()) worker_.join();
}

void Flasher::setPhase(const std::string& phase, float frac,
                       const std::string& detail) {
    std::lock_guard<std::mutex> lk(m_);
    st_.phase = phase;
    st_.fraction = frac;
    if (!detail.empty()) st_.detail = detail;
}

Flasher::Status Flasher::status() const {
    std::lock_guard<std::mutex> lk(m_);
    return st_;
}

bool Flasher::takeCompletion() {
    bool expected = true;
    return completion_.compare_exchange_strong(expected, false);
}

bool Flasher::start(int port, int board, bool passthrough, int layout,
                    bool dump, Log& log) {
    if (state_.load() == FlashState::Running) return false;
    if (worker_.joinable()) worker_.join();
    state_ = FlashState::Running;
    setPhase("Starting", 0.0f, "");
    worker_ = std::thread(&Flasher::run, this, port, board, passthrough,
                          layout, dump, &log);
    return true;
}

bool Flasher::startHex(int port, const std::string& hexPath, Log& log) {
    if (state_.load() == FlashState::Running) return false;
    if (worker_.joinable()) worker_.join();
    state_ = FlashState::Running;
    setPhase("Starting", 0.0f, "");
    worker_ = std::thread(&Flasher::runHex, this, port, hexPath, &log);
    return true;
}

void Flasher::runHex(int port, std::string hexPath, Log* logp) {
    Log& log = *logp;

    if (!fileExists(hexPath)) {
        log.error("No file at " + hexPath);
        setPhase("Failed", 0.0f, "hex not found");
        state_ = FlashState::Failed;
        return;
    }

    const std::string avrdude = FindAvrdude(hexPath);
    if (avrdude.empty()) {
        log.error("avrdude was not found. Put avrdude.exe and avrdude.conf "
                  "beside the .hex file, or in bin\\tools.");
        setPhase("Failed", 0.0f, "avrdude missing");
        state_ = FlashState::Failed;
        return;
    }
    log.info("Using " + avrdude);

    // A 32u4 runs the sketch, not the bootloader. Opening its port at 1200
    // baud and closing again is the documented signal to reset into the
    // bootloader, which then appears on a *different* COM port for a few
    // seconds. avrdude has to be pointed at that one, not the original.
    setPhase("Resetting board", 0.2f, "1200 baud touch");
    const auto before = EnumerateSerialPorts();

    {
        const std::string name = "\\\\.\\COM" + std::to_string(port);

        // Retried, because releasing a port is not instantaneous.
        //
        // The controller closes its handle before this runs, but Windows
        // does not always make the port available again on the same
        // instruction -- and a single failed open here means the board never
        // enters its bootloader, so the upload that follows fails for a
        // reason that has nothing to do with the upload.
        HANDLE h = INVALID_HANDLE_VALUE;
        DWORD lastErr = 0;
        for (int attempt = 0; attempt < 10; ++attempt) {
            h = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                            nullptr);
            if (h != INVALID_HANDLE_VALUE) break;
            lastErr = GetLastError();
            Sleep(120);
        }

        if (h != INVALID_HANDLE_VALUE) {
            DCB dcb{};
            dcb.DCBlength = sizeof(dcb);
            if (GetCommState(h, &dcb)) {
                dcb.BaudRate = 1200;
                dcb.fDtrControl = DTR_CONTROL_ENABLE;   // the reset needs DTR
                SetCommState(h, &dcb);
            }
            EscapeCommFunction(h, SETDTR);
            Sleep(60);
            EscapeCommFunction(h, CLRDTR);
            CloseHandle(h);
        } else {
            // The actual reason, because the two common ones need opposite
            // responses and a generic message tells the user neither.
            const char* why =
                (lastErr == ERROR_ACCESS_DENIED)
                    ? " Something still has the port open -- a serial monitor, "
                      "or another copy of loopcore."
                : (lastErr == ERROR_FILE_NOT_FOUND)
                    ? " That port does not exist. The board may have been "
                      "unplugged, or it enumerates under a different number "
                      "now."
                    : "";
            log.warn("Could not open COM" + std::to_string(port) +
                     " for the reset touch after several tries (error " +
                     std::to_string(lastErr) + ")." + why +
                     " Trying the port as-is, which usually fails.");
        }
    }

    // Wait for the bootloader port, and keep waiting long enough for a
    // person to reach the board.
    //
    // The 32u4 bootloader appears under a COM number that was not there a
    // moment ago and stays for about eight seconds. The touch above asks the
    // sketch to reset itself, which works only if the sketch is running and
    // listening -- and after a bad flash, or with the port held by something
    // else, it is neither.
    //
    // Pressing the physical reset button always works, and produces exactly
    // the same event. So rather than giving up after a few seconds and
    // telling the user to time a second attempt by hand, this watches for
    // half a minute and fires the moment the port appears. That removes the
    // guesswork entirely: press reset whenever you like and it is caught
    // within a tenth of a second, which is far better timing than anyone
    // can manage against a fixed wait.
    setPhase("Waiting for bootloader", 0.4f, "touch sent");

    auto findFresh = [&](const std::vector<SerialPortInfo>& now) -> int {
        // A number that was not present before is the bootloader.
        for (const auto& n : now) {
            bool seen = false;
            for (const auto& b : before)
                if (b.number == n.number) { seen = true; break; }
            if (!seen) return n.number;
        }
        // Failing that, a port that names itself as a bootloader. Some
        // boards keep the same number across the reset, so "new" never
        // happens and only the name distinguishes the two states.
        for (const auto& n : now) {
            std::string nm = n.friendlyName;
            for (auto& c : nm) c = (char)tolower((unsigned char)c);
            if (nm.find("bootloader") != std::string::npos) return n.number;
        }
        return 0;
    };

    int target = 0;
    const int64_t deadline = now_ns() + 30'000'000'000LL;
    bool asked = false;
    while (now_ns() < deadline) {
        Sleep(100);
        const int fresh = findFresh(EnumerateSerialPorts());
        if (fresh) { target = fresh; break; }

        // After the automatic path has had its chance, say plainly what to
        // do rather than continuing to look busy.
        if (!asked && now_ns() > deadline - 26'000'000'000LL) {
            asked = true;
            setPhase("Press the reset button on the board", 0.45f,
                     "flashing starts the instant it appears");
            log.info("No bootloader yet. Press the reset button on the board "
                     "-- the upload starts on its own the moment it appears, "
                     "so there is nothing to time.");
        }
    }

    if (target) {
        log.info("Bootloader appeared on COM" + std::to_string(target) + ".");
    } else {
        target = port;
        log.warn("No bootloader appeared in thirty seconds. Trying COM" +
                 std::to_string(port) + " as it is, which usually fails. "
                 "Check that the board is plugged in and that its reset "
                 "button works.");
    }

    setPhase("Writing flash", 0.7f, hexPath);
    std::ostringstream cmd;
    cmd << "\"" << avrdude << "\"";

    const size_t slash = avrdude.find_last_of("\\/");
    const std::string conf = (slash == std::string::npos)
                           ? std::string("avrdude.conf")
                           : avrdude.substr(0, slash) + "\\avrdude.conf";
    if (fileExists(conf)) cmd << " -C\"" << conf << "\"";

    cmd << " -patmega32u4 -cavr109 -PCOM" << target
        << " -b57600 -D -Uflash:w:\"" << hexPath << "\":i";

    if (!runCommand(cmd.str(), log, this, "Writing flash", 0.7f,
                    &Flasher::setPhase, nullptr)) {
        log.error("avrdude failed. If it says the port could not be opened, "
                  "the bootloader window closed first: double-tap reset and "
                  "press Flash within a second or two.");
        setPhase("Failed", 0.0f, "avrdude failed");
        state_ = FlashState::Failed;
        return;
    }

    setPhase("Done", 1.0f, "hex flashed");
    log.info("Flashed " + hexPath + ".");
    completion_ = true;
    state_ = FlashState::Done;
}

void Flasher::run(int port, int board, bool passthrough, int layout,
                  bool dump, Log* logp) {
    Log& log = *logp;

    if (board == BoardTeensyNote) {
        log.error("Teensy boards use their own loader, which this cannot "
                  "drive. Flash loopcore_mouse.ino with the Teensyduino IDE "
                  "instead; the sketch itself works unchanged.");
        setPhase("Failed", 0.0f, "unsupported board");
        state_ = FlashState::Failed;
        return;
    }

    const std::string cli = FindArduinoCli();
    if (cli.empty()) {
        log.error("arduino-cli was not found. Install it, or drop "
                  "arduino-cli.exe into bin\\tools next to loopcore.");
        setPhase("Failed", 0.0f, "arduino-cli missing");
        state_ = FlashState::Failed;
        return;
    }
    log.info("Using " + cli);

    const std::string sketch = FindSketchDir();
    if (sketch.empty()) {
        log.error("Could not find the firmware folder beside loopcore.exe. "
                  "Re-extract the zip so the folder layout is intact.");
        setPhase("Failed", 0.0f, "sketch missing");
        state_ = FlashState::Failed;
        return;
    }

    const std::string fqbn = BoardFqbn(board);
    const std::string comPort = "COM" + std::to_string(port);

    // --- 1. the AVR core -------------------------------------------------
    // Installing when already present is a fast no-op, so there is no point
    // asking first.
    setPhase("Installing board support", 0.12f, "arduino:avr");
    log.info("Ensuring the arduino:avr core is installed. First run needs "
             "network access and takes a minute or two.");
    if (!runCommand("\"" + cli + "\" core install arduino:avr", log, this,
                    "Installing board support", 0.12f, &Flasher::setPhase, nullptr)) {
        log.error("Could not install the arduino:avr core. Check the network "
                  "connection, then try again.");
        setPhase("Failed", 0.0f, "core install failed");
        state_ = FlashState::Failed;
        return;
    }

    // --- 2. the Mouse library --------------------------------------------
    // Mouse.h is not part of the AVR core. It ships as a separate library
    // and has to be installed on its own, which is not obvious from the
    // compiler error it produces when missing.
    setPhase("Installing Mouse library", 0.3f, "");
    if (!runCommand("\"" + cli + "\" lib install Mouse", log, this,
                    "Installing Mouse library", 0.3f, &Flasher::setPhase, nullptr)) {
        log.error("Could not install the Mouse library. Check the network "
                  "connection, then try again.");
        setPhase("Failed", 0.0f, "library install failed");
        state_ = FlashState::Failed;
        return;
    }

    // --- 3. the host shield library, only when asked for -----------------
    if (passthrough) {
        setPhase("Installing host shield library", 0.36f, "");
        if (!runCommand("\"" + cli + "\" lib install \"USB Host Shield Library 2.0\"",
                        log, this, "Installing host shield library", 0.36f,
                        &Flasher::setPhase, nullptr)) {
            log.error("Could not install the USB Host Shield Library 2.0.");
            setPhase("Failed", 0.0f, "library install failed");
            state_ = FlashState::Failed;
            return;
        }

        // The stock Mouse library has three buttons and no wheel, so a
        // scroll wheel and buttons four and five cannot pass through it
        // however well the host side reads them.
        setPhase("Installing HID-Project", 0.44f, "");
        if (!runCommand("\"" + cli + "\" lib install HID-Project", log, this,
                        "Installing HID-Project", 0.44f,
                        &Flasher::setPhase, nullptr)) {
            log.error("Could not install HID-Project, which supplies the "
                      "five-button mouse descriptor with a wheel.");
            setPhase("Failed", 0.0f, "library install failed");
            state_ = FlashState::Failed;
            return;
        }
    }

    // --- 4. compile ------------------------------------------------------
    // The passthrough switch is passed as a define rather than edited into
    // the sketch, so the toggle in the app is the single source of truth.
    std::string defs;
    if (passthrough) {
        defs = "-DENABLE_PASSTHROUGH=1";
        if (layout > 0) defs += " -DMOUSE_LAYOUT=" + std::to_string(layout);
        if (dump)       defs += " -DDUMP_REPORTS=1";
    }
    std::string extra;
    if (!defs.empty())
        extra = " --build-property \"compiler.cpp.extra_flags=" + defs + "\"";

    setPhase("Compiling", 0.5f, passthrough ? "with host shield passthrough"
                                            : "HID only");
    if (!runCommand("\"" + cli + "\" compile --fqbn " + fqbn + extra +
                    " \"" + sketch + "\"",
                    log, this, "Compiling", 0.5f, &Flasher::setPhase, nullptr)) {
        // Point at the actual compiler output rather than guessing: the
        // guess was wrong often enough to be worse than no guess.
        log.error("Compilation failed. The lines above this are the compiler's "
                  "own output, and the first error in them is the real one. "
                  "Copy the log from the Debug tab if it is not obvious.");
        setPhase("Failed", 0.0f, "compile failed");
        state_ = FlashState::Failed;
        return;
    }

    // --- 5. upload -------------------------------------------------------
    // A 32u4 exposes its bootloader on a *different* COM port for a few
    // seconds after a 1200-baud touch. arduino-cli performs the touch and
    // follows the port change itself, which is the main reason to shell out
    // to it rather than drive avrdude directly.
    setPhase("Uploading", 0.8f, comPort);
    log.info("Uploading to " + comPort + ". The board will disappear and come "
             "back on a different port for a moment; that is the bootloader.");
    std::string tail;
    if (!runCommand("\"" + cli + "\" upload -p " + comPort + " --fqbn " + fqbn +
                    " \"" + sketch + "\"",
                    log, this, "Uploading", 0.8f, &Flasher::setPhase, &tail)) {
        log.error("Upload failed. Common causes: the port is held by another "
                  "program, the wrong board is selected, or the board needs a "
                  "manual reset. Double-tap reset and try again within a "
                  "couple of seconds.");
        setPhase("Failed", 0.0f, "upload failed");
        state_ = FlashState::Failed;
        return;
    }

    setPhase("Done", 1.0f, "firmware flashed");
    log.info(passthrough
             ? "Firmware flashed with host shield support. The shield's "
               "downstream port is powered again and a mouse plugged into it "
               "will pass through."
             : "Firmware flashed. The board is now listening for loopcore "
               "packets. Note that a USB Host Shield is left unpowered in "
               "this mode.");
    completion_ = true;
    state_ = FlashState::Done;
}

} // namespace lc
