// serialports.h -- what serial devices are actually attached.
//
// A number picker for COM ports is a bad interface: the user has to go find
// the number in Device Manager, and every value they pass through on the way
// is a real port that something else may own. Enumerating gives real names
// and lets the right one be picked automatically.
#pragma once

#include <string>
#include <vector>

namespace lc {

struct SerialPortInfo {
    int         number = 0;        // the n in COMn
    std::string name;              // "COM5"
    std::string friendlyName;      // "Arduino Leonardo (COM5)"
    std::string hardwareId;        // "USB\\VID_2341&PID_8036..."
    bool        likelyBoard = false;   // looks like an Arduino-class device
};

// Present devices only, sorted by port number.
std::vector<SerialPortInfo> EnumerateSerialPorts();

// The port most likely to be the board, or 0 if nothing stands out.
int AutoPickPort(const std::vector<SerialPortInfo>& ports);

} // namespace lc
