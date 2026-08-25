// rawinput.h -- which device did that click come from.
//
// GetAsyncKeyState reports the state of "the mouse", merged across every
// mouse attached. When a board is re-emitting a pass-through mouse as its
// own HID device, that merge makes the two indistinguishable -- so a second
// mouse on the desk can trigger the activation key just as well.
//
// Raw Input keeps the device handle on every event, which is the only way to
// tell them apart.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace lc::rawin {

// Registers for mouse input, including while unfocused. Safe to call once.
bool Init(HWND hwnd);

// Called from WM_INPUT.
void Handle(LPARAM lParam);

// Button state from the device chosen as the board, and from everything
// else.
bool BoardButtonDown(int vk);
bool AnyButtonDown(int vk);

// True once the chosen device has actually sent something, so the UI can say
// whether filtering will work rather than silently ignoring every click.
bool BoardSeen();

// True when the chosen device has produced a button press recently.
//
// Distinct from BoardSeen, which stays true forever once a device has ever
// matched. Filtering on a device that has since stopped answering is how
// activation ends up permanently dead after the board is reflashed and
// enumerates under a different path.
bool BoardResponsive();
// A device other than the chosen one is currently pressing something.
bool OtherDeviceActive();

struct DeviceInfo {
    std::string id;        // the device path, which is stable across runs
    std::string label;     // a shortened, readable form
    bool        isBoard = false;
    bool        active  = false;   // has sent a button since launch
};

// Every mouse currently attached.
std::vector<DeviceInfo> Devices();

// Pin filtering to one device. A fragment of the path is enough; matching a
// vendor id by guesswork fails on boards that do not advertise one, which is
// most prebuilt firmware.
void SetPreferred(const std::string& idFragment);
std::string Preferred();

// Arms a one-shot capture: the next device to send a button press becomes
// the preferred one. Far more reliable than guessing from a name.
void LearnNextDevice();
bool Learning();

// The device that most recently sent a button, for showing what was picked.
std::string LastButtonDevice();

// Physical mouse movement since the last call, and clears it.
//
// Deliberately excludes movement this program injected: SendInput sets a
// marker on its own events, and a board's HID output arrives from the board
// rather than from the user's mouse, so the device filter covers that case.
// Without both, the assist would read its own output as the user's hand and
// feed back on itself.
void TakeMouseDelta(float& dx, float& dy);

} // namespace lc::rawin
