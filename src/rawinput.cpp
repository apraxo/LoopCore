// rawinput.cpp
#include "rawinput.h"

#include "inputtag.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace lc::rawin {

namespace {

// A local clock rather than common.h's.
//
// This file deliberately depends on nothing but Windows and the standard
// library -- it is the one piece that has to keep working when the rest of
// the program is being reconfigured around it. Pulling in the whole of
// common.h for a single steady_clock read would trade that for nothing.
int64_t nowNs() {
    using namespace std::chrono;
    return duration_cast<nanoseconds>(
               steady_clock::now().time_since_epoch()).count();
}

std::mutex g_m;
std::map<HANDLE, std::string> g_names;   // handle -> device path
std::atomic<uint32_t>  g_boardBtns{0};
std::atomic<int64_t>   g_boardLastNs{0};
std::atomic<int64_t>   g_otherLastNs{0};
std::atomic<uint32_t>  g_otherBtns{0};
std::atomic<bool>      g_boardSeen{false};
std::atomic<int>       g_moveX{0};
std::atomic<int>       g_moveY{0};
std::atomic<bool>      g_learning{false};
std::string            g_preferred;      // path fragment, empty = heuristic
std::string            g_lastDevice;
std::map<std::string, bool> g_active;    // path -> has sent a button

constexpr uint32_t BTN_L  = 1u << 0;
constexpr uint32_t BTN_R  = 1u << 1;
constexpr uint32_t BTN_M  = 1u << 2;
constexpr uint32_t BTN_X1 = 1u << 3;
constexpr uint32_t BTN_X2 = 1u << 4;

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::toupper(c); });
    return s;
}

// With pass-through the board re-emits the mouse as its own HID device, so
// the vendor id in the device path is the board's, not the mouse's. That is
// precisely what makes this filter work.
bool looksLikeBoard(const std::string& deviceName) {
    static const char* kVendors[] = {
        "VID_2341",  // Arduino
        "VID_2A03",  // Arduino (arduino.org era)
        "VID_1B4F",  // SparkFun
        "VID_239A",  // Adafruit
        "VID_16C0",  // PJRC / Teensy
    };
    const std::string n = upper(deviceName);
    for (const char* v : kVendors)
        if (n.find(v) != std::string::npos) return true;
    return false;
}

std::string deviceName(HANDLE dev) {
    {
        std::lock_guard<std::mutex> lk(g_m);
        auto it = g_names.find(dev);
        if (it != g_names.end()) return it->second;
    }
    UINT size = 0;
    GetRawInputDeviceInfoA(dev, RIDI_DEVICENAME, nullptr, &size);
    std::string name;
    if (size > 0 && size < 4096) {
        std::vector<char> buf(size + 1, 0);
        if (GetRawInputDeviceInfoA(dev, RIDI_DEVICENAME, buf.data(), &size) > 0)
            name = buf.data();
    }
    std::lock_guard<std::mutex> lk(g_m);
    g_names[dev] = name;
    return name;
}

// A pinned device wins outright. The vendor-id heuristic is only a fallback,
// because prebuilt firmware frequently does not advertise a vendor this can
// recognise -- which is exactly how filtering ended up rejecting everything.
bool isBoardDevice(const std::string& name) {
    std::string pref;
    {
        std::lock_guard<std::mutex> lk(g_m);
        pref = g_preferred;
    }
    if (!pref.empty()) return upper(name).find(upper(pref)) != std::string::npos;
    return looksLikeBoard(name);
}

void applyFlags(std::atomic<uint32_t>& target, USHORT flags) {
    uint32_t v = target.load();
    if (flags & RI_MOUSE_LEFT_BUTTON_DOWN)   v |= BTN_L;
    if (flags & RI_MOUSE_LEFT_BUTTON_UP)     v &= ~BTN_L;
    if (flags & RI_MOUSE_RIGHT_BUTTON_DOWN)  v |= BTN_R;
    if (flags & RI_MOUSE_RIGHT_BUTTON_UP)    v &= ~BTN_R;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_DOWN) v |= BTN_M;
    if (flags & RI_MOUSE_MIDDLE_BUTTON_UP)   v &= ~BTN_M;
    if (flags & RI_MOUSE_BUTTON_4_DOWN)      v |= BTN_X1;
    if (flags & RI_MOUSE_BUTTON_4_UP)        v &= ~BTN_X1;
    if (flags & RI_MOUSE_BUTTON_5_DOWN)      v |= BTN_X2;
    if (flags & RI_MOUSE_BUTTON_5_UP)        v &= ~BTN_X2;
    target = v;
}

uint32_t maskFor(int vk) {
    switch (vk) {
    case VK_LBUTTON:  return BTN_L;
    case VK_RBUTTON:  return BTN_R;
    case VK_MBUTTON:  return BTN_M;
    case VK_XBUTTON1: return BTN_X1;
    case VK_XBUTTON2: return BTN_X2;
    default:          return 0;
    }
}

} // namespace

bool Init(HWND hwnd) {
    RAWINPUTDEVICE rid{};
    rid.usUsagePage = 0x01;      // generic desktop
    rid.usUsage     = 0x02;      // mouse
    // INPUTSINK so events arrive even when the panel is not focused, which
    // is the normal case while this is in use.
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = hwnd;
    return RegisterRawInputDevices(&rid, 1, sizeof(rid)) == TRUE;
}

void Handle(LPARAM lParam) {
    UINT size = 0;
    GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &size,
                    sizeof(RAWINPUTHEADER));
    if (size == 0 || size > 1024) return;

    BYTE buf[1024];
    if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, buf, &size,
                        sizeof(RAWINPUTHEADER)) != size)
        return;

    const RAWINPUT* ri = (const RAWINPUT*)buf;
    if (ri->header.dwType != RIM_TYPEMOUSE) return;

    // Our own injected events, rejected before anything else happens.
    //
    // Raw input is registered as an input sink, so every mouse event on the
    // machine is delivered here -- including the ones loopcore just sent.
    // That is a feedback loop: injecting movement generates work for the very
    // thread that has to keep up with it, and when it cannot, the messages
    // queue. The queue keeps delivering after the user lets go, which is why
    // the stutter outlasted the key by several seconds rather than stopping
    // with it.
    //
    // The tag is checked first and the event dropped whole. Deciding this
    // after parsing, as the movement branch below used to, still paid for the
    // parse on every one.
    if (GetMessageExtraInfo() == (LPARAM)kLoopcoreInputTag) return;

    // Movement first: it arrives on its own, without any button flags.
    if (ri->data.mouse.usFlags == MOUSE_MOVE_RELATIVE &&
        (ri->data.mouse.lLastX || ri->data.mouse.lLastY)) {
        // Anything else injected, by us or by another program, is still
        // excluded from the hand estimate: a synthetic event is not the
        // user's hand whoever sent it.
        const bool injected =
            (ri->header.hDevice == nullptr) ||
            (GetMessageExtraInfo() != 0);
        if (!injected) {
            g_moveX += ri->data.mouse.lLastX;
            g_moveY += ri->data.mouse.lLastY;
        }
    }

    const USHORT flags = ri->data.mouse.usButtonFlags;
    if (flags == 0) return;      // nothing further to record

    const std::string name = deviceName(ri->header.hDevice);
    {
        std::lock_guard<std::mutex> lk(g_m);
        g_lastDevice = name;
        g_active[name] = true;
    }

    // Learning: the first button press after arming pins that device.
    bool expected = true;
    if (g_learning.compare_exchange_strong(expected, false)) {
        std::lock_guard<std::mutex> lk(g_m);
        g_preferred = name;
    }

    const bool board = isBoardDevice(name);
    if (board) {
        g_boardSeen = true;
        g_boardLastNs = nowNs();
    } else {
        g_otherLastNs = nowNs();
    }
    applyFlags(board ? g_boardBtns : g_otherBtns, flags);
}

bool BoardButtonDown(int vk) {
    const uint32_t m = maskFor(vk);
    return m && (g_boardBtns.load() & m) != 0;
}

bool AnyButtonDown(int vk) {
    const uint32_t m = maskFor(vk);
    if (!m) return false;
    return ((g_boardBtns.load() | g_otherBtns.load()) & m) != 0;
}

bool BoardSeen() { return g_boardSeen.load(); }

bool BoardResponsive() {
    const int64_t t = g_boardLastNs.load();
    return t != 0 && (nowNs() - t) < 30'000'000'000LL;   // half a minute
}

bool OtherDeviceActive() {
    const int64_t t = g_otherLastNs.load();
    return t != 0 && (nowNs() - t) < 5'000'000'000LL;
}

void TakeMouseDelta(float& dx, float& dy) {
    dx = (float)g_moveX.exchange(0);
    dy = (float)g_moveY.exchange(0);
}

std::vector<DeviceInfo> Devices() {
    std::vector<DeviceInfo> out;

    UINT count = 0;
    if (GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0)
        return out;
    if (count == 0) return out;

    std::vector<RAWINPUTDEVICELIST> list(count);
    if (GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST))
        == (UINT)-1)
        return out;

    for (UINT i = 0; i < count; ++i) {
        if (list[i].dwType != RIM_TYPEMOUSE) continue;
        DeviceInfo d;
        d.id = deviceName(list[i].hDevice);
        if (d.id.empty()) continue;

        // Paths are long and mostly boilerplate; the vendor and product ids
        // are the part a person can recognise.
        const std::string u = upper(d.id);
        const size_t vid = u.find("VID_");
        d.label = (vid != std::string::npos) ? u.substr(vid, 17) : d.id;

        d.isBoard = isBoardDevice(d.id);
        {
            std::lock_guard<std::mutex> lk(g_m);
            d.active = g_active.count(d.id) > 0;
        }
        out.push_back(std::move(d));
    }
    return out;
}

void SetPreferred(const std::string& idFragment) {
    std::lock_guard<std::mutex> lk(g_m);
    g_preferred = idFragment;
    g_boardSeen = false;   // the new choice has not proved itself yet
    g_boardLastNs = 0;
}

std::string Preferred() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_preferred;
}

void LearnNextDevice() { g_learning = true; }
bool Learning()        { return g_learning.load(); }

std::string LastButtonDevice() {
    std::lock_guard<std::mutex> lk(g_m);
    return g_lastDevice;
}

} // namespace lc::rawin
