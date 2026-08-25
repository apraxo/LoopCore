// serialports.cpp
#include "serialports.h"

#include <windows.h>
#include <setupapi.h>
#include <devguid.h>

#include <algorithm>
#include <cctype>

namespace lc {

namespace {

std::string narrow(const wchar_t* w) {
    if (!w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)::toupper(c); });
    return s;
}

// Friendly names look like "Arduino Leonardo (COM5)".
int portFromFriendly(const std::string& friendly) {
    const size_t open = friendly.rfind("(COM");
    if (open == std::string::npos) return 0;
    return atoi(friendly.c_str() + open + 4);
}

// USB vendor IDs that ship boards people wire up this way, plus the bridge
// chips that sit in front of the cheaper clones.
bool looksLikeBoard(const std::string& friendlyUpper,
                    const std::string& hardwareUpper) {
    static const char* kVendors[] = {
        "VID_2341",   // Arduino
        "VID_2A03",   // Arduino (arduino.org era)
        "VID_1B4F",   // SparkFun
        "VID_239A",   // Adafruit
        "VID_16C0",   // PJRC / Teensy
        "VID_1A86",   // CH340 / CH9102 clones
        "VID_0403",   // FTDI
        "VID_10C4",   // Silicon Labs CP210x
    };
    for (const char* v : kVendors)
        if (hardwareUpper.find(v) != std::string::npos) return true;

    static const char* kNames[] = {
        "ARDUINO", "LEONARDO", "PRO MICRO", "TEENSY",
        "CH340", "CH9102", "USB-SERIAL", "USB SERIAL DEVICE",
    };
    for (const char* n : kNames)
        if (friendlyUpper.find(n) != std::string::npos) return true;

    return false;
}

} // namespace

std::vector<SerialPortInfo> EnumerateSerialPorts() {
    std::vector<SerialPortInfo> out;

    HDEVINFO hdi = SetupDiGetClassDevsW(&GUID_DEVCLASS_PORTS, nullptr, nullptr,
                                        DIGCF_PRESENT);
    if (hdi == INVALID_HANDLE_VALUE) return out;

    SP_DEVINFO_DATA dev{};
    dev.cbSize = sizeof(SP_DEVINFO_DATA);

    for (DWORD i = 0; SetupDiEnumDeviceInfo(hdi, i, &dev); ++i) {
        wchar_t buf[512]{};
        SerialPortInfo info;

        if (SetupDiGetDeviceRegistryPropertyW(hdi, &dev, SPDRP_FRIENDLYNAME,
                                              nullptr, (PBYTE)buf, sizeof(buf),
                                              nullptr))
            info.friendlyName = narrow(buf);

        buf[0] = 0;
        if (SetupDiGetDeviceRegistryPropertyW(hdi, &dev, SPDRP_HARDWAREID,
                                              nullptr, (PBYTE)buf, sizeof(buf),
                                              nullptr))
            info.hardwareId = narrow(buf);

        info.number = portFromFriendly(info.friendlyName);
        if (info.number <= 0) continue;   // LPT and other non-COM entries

        info.name = "COM" + std::to_string(info.number);
        info.likelyBoard = looksLikeBoard(upper(info.friendlyName),
                                          upper(info.hardwareId));
        out.push_back(std::move(info));
    }
    SetupDiDestroyDeviceInfoList(hdi);

    std::sort(out.begin(), out.end(),
              [](const SerialPortInfo& a, const SerialPortInfo& b) {
                  return a.number < b.number;
              });
    return out;
}

int AutoPickPort(const std::vector<SerialPortInfo>& ports) {
    // Prefer something that names itself an Arduino outright.
    for (const auto& p : ports) {
        const std::string f = upper(p.friendlyName);
        if (f.find("ARDUINO") != std::string::npos ||
            f.find("LEONARDO") != std::string::npos ||
            f.find("TEENSY") != std::string::npos)
            return p.number;
    }
    // Otherwise the first device that looks like a board at all.
    for (const auto& p : ports)
        if (p.likelyBoard) return p.number;

    // One port and nothing to distinguish it: it is probably the one.
    if (ports.size() == 1) return ports[0].number;
    return 0;
}

} // namespace lc
