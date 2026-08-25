// inputtag.h -- the marker loopcore stamps on input it injects.
//
// Its own header, deliberately.
//
// Two parts need this value and neither should depend on the other: the
// controller stamps it onto every synthetic event, and raw input checks for
// it so those events can be thrown away before any work is done on them.
// Putting it in control.h would drag the whole controller into rawinput.cpp,
// which is deliberately standalone -- it depends on nothing but Windows and
// the standard library, so that it keeps working while everything around it
// is being reconfigured.
//
// Why the tag is needed at all: raw input is registered as an input sink, so
// every mouse event on the machine arrives here, including the ones loopcore
// just sent. Without a way to recognise them, injecting movement generates
// work for the very thread that has to keep up with it.
#pragma once

#include <windows.h>

namespace lc {

// Arbitrary; it only has to be a value no other program is likely to use.
constexpr ULONG_PTR kLoopcoreInputTag = 0x4C4F4F50;   // 'LOOP'

} // namespace lc
