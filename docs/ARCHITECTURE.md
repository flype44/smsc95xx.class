# Architecture

`smsc95xx.class` is one binary that is at the same time

- a **Poseidon class** (an Exec library with the Poseidon class vectors), and
- the home of a **SANA-II device**, `smsc95xx.device`, created when the class is loaded.

Everything below runs on top of Poseidon, which sits on a DWC2 host controller driver.

```
 Roadshow (or another SANA-II stack)
        |  OpenDevice("smsc95xx.device", 0), CMD_READ / CMD_WRITE / S2_*
 smsc95xx.device   (src/sana.c)        SANA-II commands, queues, statistics
        |
 binding task      (smsc95xx.class.c)  one task per bound LAN95xx
        |  bulk IN / OUT, control (vendor requests), pipes
 Poseidon 4.x      (psd_api.h)
        |
 dwc2usb.device    (Lemaru, separate)  DWC2 host controller driver
        |
 LAN9514 on the Raspberry Pi 3B
```

## Files

| File | Role |
|---|---|
| `src/smsc95xx.class.c` | ROMTag and library shell, Poseidon class vectors, bindings, the binding task and its event loop (pipes, SANA-II requests, 500 ms tick), timer, link and statistics reports, the opt-in self-test |
| `src/lan95.c` | Register access by vendor control requests, MII/PHY, chip initialisation, link polling, receive filter |
| `src/data.c` | Bulk endpoints and pipes, receive framing (several frames per transfer), transmit framing |
| `src/sana.c` | The SANA-II device: device vectors, units, openers, commands, receive delivery, transmit queue, multicast, events, abort |
| `src/platform.c` | Where the board MAC comes from: device tree, then the VideoCore mailbox |
| `include/psd_api.h` | The part of the Poseidon ABI that is used: tags, request types, register-based call stubs |
| `include/dt_api.h` | The same for `devicetree.resource` |
| `include/smsc95xx_regs.h` | Hardware facts: registers, bits, frame headers |
| `include/smsc95xx.h` | Shared types |
| `tools/hunkvec.py` | Reads the library vector table of an Amiga hunk file (used to check the Poseidon ABI) |

## Class and binding

Poseidon 4.x calls a class through 13 vectors (see
[`ABI-poseidon-4.5.md`](ABI-poseidon-4.5.md)): the four standard ones, six binding
functions (`-30` to `-60`), then `usbGetAttrsA`, `usbSetAttrsA` and `usbDoMethodA`. Binding of
a device is done through the legacy vectors (`-42` attempt, `-60` force, `-48` release).
`smsc95xx.class` binds whole devices only (`0424:EC00` and `0424:9500`).

A binding starts one task. The task opens Poseidon, allocates the control pipe, reads the chip,
initialises it, opens the bulk pipes, enables the MAC, attaches the SANA-II unit and then runs
its event loop. Everything that touches the chip happens in that task: no locking is needed
for the hardware state.

Before its event loop the task sets its own priority to 0, the priority of the TCP/IP stack
(`ENV:smsc95xx.taskpri` overrides it). At the same priority a write posted by the stack does
not pre-empt the stack, so writes pile up and leave in one transfer; at Poseidon's sub-task
priority (5) the task took every write as it came and sent the frames one by one.

## Chip bring-up (`lan95.c`)

1. Read `ID_REV`.
2. Take the station address from the platform (the Pi 3 has no EEPROM).
3. Lite reset, PHY reset, set the MAC address, receive format (multi-frame or single-frame),
   bulk-in delay, LEDs, flow control, no checksum offload.
4. Start auto-negotiation. The link is polled with **one** MII read in the steady state (each
   MII access costs several control transfers, during which the task serves no traffic): not at
   all while frames arrive (they prove the link is up), once a second when the link is up and
   idle, every 500 ms while it is down. Speed and duplex are resolved only when the link comes
   up or has bounced.

## Data path (`data.c`)

- **Receive:** one bulk-in transfer is kept pending. With the chip's multi-frame mode, a transfer
  carries several frames, each as `[status word][frame + FCS][padding to 4]`; the parser walks
  the transfer and drops frames that carry an error flag, a bad length, or that lose
  synchronisation. The FCS is removed before delivery.
- **Transmit:** the writes waiting when the pipe is free leave together in one bulk-out transfer
  (up to `LAN95_TX_BATCH_MAX` frames in a 16 KiB buffer), each as `[cmd A][cmd B][frame]`, the next
  one on a 4-byte boundary; the transfer ends with the last frame, unpadded. The chip always accepts
  several frames per transfer (LAN950x data sheet DS00001875D, 5.3.1.2 and 5.4.2). Frames are built
  straight in the transmit buffer by `sana.c`; their requests are replied when the transfer completes.
  Only **one** transfer is in flight: dwc2usb does not serialise the transfers of one endpoint, so
  two could leave out of order or on both bulk channels at once.

### Why only one bulk-in is pending

The DWC2 driver has two bulk channels. A bulk IN with nothing to say keeps its channel (the core
retries NAKs by itself) until the driver takes it back after a 100 ms slice. With four pending
IN transfers, the two channels were permanently busy: every packet waited for a slice (pings of
60 to 1400 ms with 28 % loss), and a USB stick behind the same hub needed minutes to be listed.
With one, a channel stays free for transmit and for other devices.

## SANA-II (`sana.c`)

- The device base is created by `MakeLibrary` + `AddDevice` when the class is initialised, and
  removed with the class. Its open count includes one reference held by the class.
- A **unit** is one network interface. It is created for a binding and survives it: restarting
  Poseidon rebinds the same unit, so a TCP/IP stack that kept the device open does not hold a
  dangling pointer. While the binding task is not running (`attached` is false), `BeginIO` fails
  every request at once.
- `BeginIO` only hands the request to the binding task (`PutMsg` on the unit's command port).
  The task executes every command, so the unit state is touched by one task. Pending requests
  (reads, orphan reads, events, queued writes) are on lists that `AbortIO` and `CloseDevice` also
  edit, always under `Forbid()`.
- Each `OpenDevice` creates an **opener** with its buffer-management functions
  (`S2_CopyToBuff`, `S2_CopyFromBuff`; plain memory copies when absent) and its read queue; it is
  stored in `ios2_BufferManagement` of the requests.
- Received frames are matched to read requests by packet type (a raw read takes anything left),
  then to orphan reads; tracked types count dropped frames, others count unknown types.
- The multicast list is applied to the chip as a 64-bit hash (the Ethernet CRC-32, top six
  bits); a multicast range, or promiscuous mode, switches to "all multicast".

## Device lifetime and Poseidon

Class and device are removed together, and only when nobody has the device open. The class keeps
no pointer to Poseidon-owned memory beyond the binding's own lifetime. A binding is released by
signalling its task (`CTRL_C`) and waiting for it to finish.

## Decisions worth remembering

- The Poseidon 4.5 ABI is **not** the AROS layout. Offsets above `-240` must not be taken from
  AROS' `poseidon.conf` (`psdBorrowLocksWait` is at `-582`, not `-576`). See the ABI note.
- A class that exposes a device must keep `-30` to `-60` real (binding) functions; stubs that
  return 0 make Poseidon fail every binding silently.
- The unit's request port belongs to the binding task: its signal is allocated by the task and
  released by it on detach.
- `NewList` is not in `exec.library`; a `-nostdlib` build has to supply it.
