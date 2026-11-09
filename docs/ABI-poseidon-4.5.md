# poseidon.library 4.5 (14.10.14): checked vector offsets

Source of truth: `Libs/poseidon.library` from the PoseidonV4 archive, 101
vectors (LVO -6 .. -606), read from its ROMTag and function table, then each
used vector disassembled to confirm its register usage.

| Function            | LVO  | Evidence in 4.5                                    |
|---------------------|------|----------------------------------------------------|
| psdDelayMS          | -66  | d0 = ms, uses timer.device                         |
| psdGetAttrsA        | -132 | d0 type (<= 15, jump table), a0 object, a1 taglist |
| psdAllocPipe        | -144 | a0 device, a1 msgport, a2 endpoint (optional)      |
| psdFreePipe         | -150 | a1 pipe, checks node type 5                        |
| psdPipeSetup        | -156 | a1 pipe, d0..d3 stored as setup packet at +104     |
| psdDoPipe           | -162 | a1 pipe, a0 data, d0 length                        |
| psdAbortPipe        | -174 | a1 pipe, checks node type 5                        |
| psdGetPipeError     | -192 | returns pipe error byte (+73)                      |
| psdSpawnSubTask     | -234 | a0 name, a1 initpc, a2 userdata; used by asixeth   |
| psdAddErrorMsgA     | -240 | d0 level, a0 origin, a1 fmt, a2 fmtdata            |
| psdBorrowLocksWait  | -582 | called by 4.5's own asixeth.class after SpawnSubTask |

**Difference with AROS' poseidon.conf (v5.5):** AROS places
psdBorrowLocksWait at -576; in 4.5 that slot is a different function (used by
usbaudio/usbvideo), and psdBorrowLocksWait is at -582. Vectors from -30 to
-240 agree with AROS. Do not take offsets above -240 from the AROS table.

Method: `python3 -I tools/hunkvec.py <file>` (parses hunks, follows ROMTag to the function
table), then `objdump -d` of the vector targets; call sites cross-checked in
the 4.5 classes with `jsr -N(a6)`.

## Class library ABI (Poseidon 4.x) - differs from AROS

A class is an Exec library with 13 vectors, not 7. Seen in 4.5's own
asixeth.class / hub.class, and from the calls made by poseidon.library:

| LVO | Entry                                                          |
|-----|----------------------------------------------------------------|
| -6 ..-24 | Open, Close, Expunge, Reserved                              |
| -30 | usbAttemptInterfaceBinding(a0 interface) -> binding or 0       |
| -36 | usbReleaseInterfaceBinding(a0 binding)                         |
| -42 | usbAttemptDeviceBinding(a0 device) -> binding or 0             |
| -48 | usbReleaseDeviceBinding(a0 binding)                            |
| -54 | usbForceInterfaceBinding(a0 interface) -> binding or 0         |
| -60 | usbForceDeviceBinding(a0 device) -> binding or 0               |
| -66 | usbGetAttrsA(d0 type, a0 object, a1 taglist)                   |
| -72 | usbSetAttrsA (a stub in shipped classes)                       |
| -78 | usbDoMethodA(d0 method, a1 methoddata); methods 1-6 dispatch to the binding functions above, others e.g. 0x50 (suspend) |

Poseidon 4.5 binds devices by calling -42 / -60 / -48 directly. Stubs that
return 0 there make every binding attempt fail silently ("Forced device
binding ... failed" with no message from the class). Fixed in 0.3.
Placing the entry points at -30/-36/-42 (the AROS layout) makes Poseidon jump
outside the library at scan time: Trident crashes while scanning classes.
Fixed in smsc95xx.class 0.1 (second build).
