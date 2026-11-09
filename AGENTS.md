# smsc95xx.class Agent Notes

## What this is

A Poseidon USB class that drives the SMSC/Microchip LAN95xx USB Ethernet controllers (the
LAN9514 of the Raspberry Pi 3B) and provides the SANA-II device `smsc95xx.device`, for
AmigaOS 3 on PiStorm / Emu68. Read `README.md` first, then `docs/ARCHITECTURE.md` and
`docs/ABI-poseidon-4.5.md`.

The USB host controller driver (DWC2) is **not** part of this repository: it is
`dwc2usb.device` by Lemaru, released as binaries at <https://github.com/LemaruX/dwc2usb.device>.
Its sources are not published; never copy or reproduce them.

## Build

bebbo's `m68k-amigaos-gcc` (tested with 6.5). `devices/sana2.h` is not in every toolchain:
pass the directory of the SANA-II / Roadshow part of the NDK.

```sh
make PREFIX=<toolchain> SANA_INC=<NDK>/SANA+RoadshowTCP-IP/include
make PREFIX=<toolchain> fpucheck
```

Output: `build/smsc95xx.class`. The build is `-nostdlib -m68040 -msoft-float`. With this NDK
`STRPTR` is `unsigned char *`, hence `-Wno-pointer-sign`. No FPU instruction is allowed
(`make fpucheck` must pass). A `-nostdlib` build has no `NewList()`, `memcpy()` or libgcc:
supply what is needed and keep integer division on constants.

## Code conventions

- Language: C (gnu11), Amiga idioms: register-argument library functions are declared with
  `asm("a0")`-style parameters; library calls into Poseidon and `devicetree.resource` go through
  the stubs in `include/psd_api.h` and `include/dt_api.h` (base passed explicitly).
- `src/smsc95xx.class.c` must stay the first source in the link: `_start()` has to be at text
  offset 0 (running the file from a shell must fail cleanly, not execute the ROMTag).
- English for code, comments, identifiers and docs. Every source file starts with
  `SPDX-License-Identifier: MPL-2.0`.
- Warnings are errors. Keep the build clean.

## Rules for changes

- The hardware facts (registers, init sequence, frame formats) come from the Microchip LAN951x
  datasheet and the BSD-licensed OpenBSD/NetBSD `if_smsc` drivers. **Do not transcribe Linux
  `smsc95xx.c`** (GPL-2.0) and do not take the Poseidon vector table from AROS above `-240`:
  verify against a real Poseidon binary (`tools/hunkvec.py`, `docs/ABI-poseidon-4.5.md`).
- Keep **one** bulk-in transfer pending (see `docs/ARCHITECTURE.md`): more starve the two bulk
  channels of the DWC2 driver.
- Keep **one** bulk-out transfer in flight, carrying every frame queued meanwhile: the DWC2
  driver does not keep the transfers of one endpoint in order.
- The binding task runs at the TCP/IP stack's priority (0) on purpose: above it, every write
  pre-empts the stack and frames leave one by one (half the upload rate). Measure before
  changing it (throughput, and the task's CPU share and task switches).
- Anything that cannot be tested on the real machine is to be said so in the notes and the commit
  message.
- Do not commit binaries (`build/` is ignored) or secrets.
