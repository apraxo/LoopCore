Prebuilt firmware
=================

YesHostShield.hex   for a board with a USB Host Shield, with a mouse plugged
                    into the shield and passed through
NoHostShield.hex    for a board that only injects movement, with the mouse
                    plugged straight into the PC

Both speak the ASCII protocol: "x,y,click\n" at 115200. Set Protocol to
"ASCII  x,y,click" on the Settings tab when using either of them.

These are flashed with avrdude, not compiled. loopcore performs the
1200-baud touch that puts a 32u4 into its bootloader, waits for the
bootloader's own COM port to appear, and writes to that.

The bundled sketch in ../loopcore_mouse is the alternative: it is built from
source and speaks either protocol -- it accepts a framed binary packet or a
text line, so one build works whichever setting loopcore is on.

../loopcore_pad is the controller build. It reads a gamepad from the shield
and reports sticks and buttons back over the same serial link, while movement
still leaves the board as mouse motion.

There is no prebuilt hex for it, and no controller equivalent of the two hex
files above, for a reason worth stating: presenting as an Xbox controller
requires impersonating a genuine Xbox vendor and product id, because that is
what Windows binds its XInput driver to. Doing so means replacing the
bootloader and the board's identity -- and the serial device loopcore talks to
disappears in the process. Reading the pad and sending mouse movement is the
arrangement that can work on a 32u4 alongside loopcore.

loopcore_pad needs the USB Host Shield 2.0 library installed in the Arduino
IDE. It is built from source through arduino-cli like the mouse sketch.
