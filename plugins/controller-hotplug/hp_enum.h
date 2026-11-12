// Controller fix: the worker's pure logic (no Windows device calls, unit-tested).
//
//  - Schedule: when to list the controllers again after a device notification or an XInput change. One list is
//    not enough: a pad's HID device can show up before DirectInput lists it, and an Xbox pad's game controller
//    device only appears once the pad is switched on. Lists at 0.5, 2, 5 and 10 s after the last change.
//  - describe / diff: log lines for each controller (name, kind, VID/PID, XInput) and what changed.
//  - diagnose: why the game may not see a pad (an Xbox pad on USB without a game controller device = switched
//    off; an XInput pad that DirectInput does not list).
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

#include "hp_tracker.h"

namespace hp {

class Schedule {
public:
    static constexpr int kSteps = 4;
    static constexpr uint32_t kAfterMs[kSteps] = {500, 2000, 5000, 10000};

    void trigger(uint32_t nowMs);          // a device change: (re)start the series
    bool take(uint32_t nowMs);             // true when a list is due now (consumes every step that is due)
    bool pending() const { return next_ < kSteps; }
    uint32_t waitMs(uint32_t nowMs) const; // ms until the next step (0 = due), 0xFFFFFFFF = none

private:
    uint32_t start_ = 0;
    int next_ = kSteps;
};

// DirectInput device type (DI8DEVTYPE_*) as a word: "gamepad", "joystick", "wheel", "flight stick", ...
const char* typeName(uint32_t devType);
// VID/PID from a DirectInput product GUID of a HID device ({PIDVID-0000-0000-0000-504944564944}); false if not HID
bool productIds(const Guid& product, uint16_t& vid, uint16_t& pid);
// "Controller (Xbox One For Windows) (gamepad, VID 045E PID 02FF, XInput)"
std::string describe(const Attached& a);
// short form of an instance GUID for the log: first 8 hex digits
std::string shortGuid(const Guid& g);

struct Diff {
    std::vector<Attached> added, gone;
};
Diff diff(const std::vector<Attached>& before, const std::vector<Attached>& after);

struct Environment {
    int xboxUsb = 0;             // Xbox controllers present as USB/Bluetooth devices (setup class XboxComposite)
    uint32_t xinputMask = 0;     // XInput user slots that report a pad (bit 0..3)
    bool xinputKnown = false;    // XInput could be asked
    std::vector<Attached> lists; // what DirectInput lists as game controllers
};
enum class Hint {
    None,
    XboxOff,          // an Xbox pad is plugged in but has no game controller device: switched off or asleep
    XInputNotListed,  // XInput reports more pads than DirectInput lists XInput devices
};
Hint diagnose(const Environment& e);
const char* hintText(Hint h); // log line

// IG_ in a HID device path: the XInput HID device of an Xbox-class pad
bool isXInputPath(const wchar_t* path);
// "VID_045E PID_02FF" from a device path, empty if none
std::string idsFromPath(const wchar_t* path);

} // namespace hp
