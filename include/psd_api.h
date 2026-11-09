/* SPDX-License-Identifier: MPL-2.0 */
/*
 * psd_api.h - the part of the Poseidon ABI that smsc95xx.class uses.
 *
 * Poseidon ships no m68k SDK, so this header carries just what is needed:
 * tag values, usb request-type bits and register-based call stubs. Library
 * vector offsets follow the order of poseidon.library's function list (the
 * first user function sits at -30); the base is passed explicitly so no
 * global library base is needed. Each stub names its registers exactly as the
 * public function list does.
 *
 * Verified against poseidon.library 4.5 (14.10.14) by disassembly, see
 * docs/ABI-poseidon-4.5.md. Later Poseidon versions may differ.
 */

#ifndef PSD_API_H
#define PSD_API_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/tasks.h>
#include <exec/ports.h>
#include <utility/tagitem.h>

/* ---- tags --------------------------------------------------------- */

#define PGA_DEVICE      0x04            /* psdGetAttrsA / psdSetAttrsA type */
#define PGA_PIPE        0x09

/* Endpoint search tags (psdFindEndpointA) and transfer types. */
#define EA_Dummy        (TAG_USER + 1138)
#define EA_IsIn         (EA_Dummy + 0x01)
#define EA_TransferType (EA_Dummy + 0x11)
#define USEAF_BULK      2

/* Pipe attributes (psdSetAttrsA with PGA_PIPE). */
#define PPA_Dummy            (TAG_USER + 1234)
#define PPA_NoZeroPktTerm    (PPA_Dummy + 0x07)  /* FALSE: end a transfer that is a
                                                    multiple of the max packet size
                                                    with a zero-length packet */
#define PPA_NakTimeout       (PPA_Dummy + 0x08)
#define PPA_NakTimeoutTime   (PPA_Dummy + 0x09)  /* ms */
#define PPA_AllowRuntPackets (PPA_Dummy + 0x0a)

#define DA_Dummy        (TAG_USER + 42)
#define DA_VendorID     (DA_Dummy + 0x19)
#define DA_ProductID    (DA_Dummy + 0x1a)
#define DA_ProductName  (DA_Dummy + 0x1c)
#define DA_IDString     (DA_Dummy + 0x24)

/* ---- psdAddErrorMsg levels (same values as dos RETURN_*) ---------- */

#define PSD_MSG_INFO    0
#define PSD_MSG_WARN    5
#define PSD_MSG_ERROR   10
#define PSD_MSG_FAIL    20

/* ---- USB request type, bmRequestType ------------------------------- */

#define PSD_RT_OUT_VENDOR_DEV   0x40    /* host -> device, vendor, device */
#define PSD_RT_IN_VENDOR_DEV    0xC0    /* device -> host, vendor, device */

/* ------------------------------------------------------------------ */
/* Call stubs                                                          */
/* ------------------------------------------------------------------ */

#define PSD_LVO_DELAYMS         66
#define PSD_LVO_GETATTRSA       132
#define PSD_LVO_SETATTRSA       138
#define PSD_LVO_ALLOCPIPE       144
#define PSD_LVO_FREEPIPE        150
#define PSD_LVO_PIPESETUP       156
#define PSD_LVO_DOPIPE          162
#define PSD_LVO_SENDPIPE        168
#define PSD_LVO_ABORTPIPE       174
#define PSD_LVO_WAITPIPE        180
#define PSD_LVO_GETPIPEACTUAL   186
#define PSD_LVO_GETPIPEERROR    192
#define PSD_LVO_SPAWNSUBTASK    234
#define PSD_LVO_ADDERRORMSGA    240
#define PSD_LVO_FINDINTERFACEA  396
#define PSD_LVO_FINDENDPOINTA   402
#define PSD_LVO_BORROWLOCKSWAIT 582   /* AROS has -576: 4.5 has one more function before it */

static inline void psdDelayMS(struct Library *b, ULONG ms)
{
    register struct Library *a6 __asm("a6") = b;
    register ULONG d0 __asm("d0") = ms;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0) : "r"(a6), [lvo] "i"(PSD_LVO_DELAYMS)
        : "d1", "a0", "a1", "memory", "cc");
}

static inline LONG psdGetAttrsA(struct Library *b, ULONG type, APTR obj,
                                struct TagItem *tags)
{
    register struct Library *a6 __asm("a6") = b;
    register ULONG d0 __asm("d0") = type;
    register APTR a0 __asm("a0") = obj;
    register struct TagItem *a1 __asm("a1") = tags;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0), "+r"(a0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_GETATTRSA)
        : "d1", "memory", "cc");
    return (LONG)d0;
}

static inline APTR psdAllocPipe(struct Library *b, APTR dev,
                                struct MsgPort *mp, APTR ep)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = dev;
    register struct MsgPort *a1 __asm("a1") = mp;
    register APTR a2 __asm("a2") = ep;
    register APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a6), [lvo] "i"(PSD_LVO_ALLOCPIPE)
        : "d1", "memory", "cc");
    return d0;
}

static inline void psdFreePipe(struct Library *b, APTR pipe)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(a1) : "r"(a6), [lvo] "i"(PSD_LVO_FREEPIPE)
        : "d0", "d1", "a0", "memory", "cc");
}

static inline void psdPipeSetup(struct Library *b, APTR pipe, UWORD rt,
                                UWORD rq, UWORD val, UWORD idx)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    register ULONG d0 __asm("d0") = rt;
    register ULONG d1 __asm("d1") = rq;
    register ULONG d2 __asm("d2") = val;
    register ULONG d3 __asm("d3") = idx;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(a1), "+r"(d0), "+r"(d1), "+r"(d2), "+r"(d3)
        : "r"(a6), [lvo] "i"(PSD_LVO_PIPESETUP)
        : "a0", "memory", "cc");
}

static inline LONG psdDoPipe(struct Library *b, APTR pipe, APTR data,
                             ULONG len)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    register APTR a0 __asm("a0") = data;
    register ULONG d0 __asm("d0") = len;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0), "+r"(a0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_DOPIPE)
        : "d1", "memory", "cc");
    return (LONG)d0;
}

static inline LONG psdSetAttrsA(struct Library *b, ULONG type, APTR obj,
                                struct TagItem *tags)
{
    register struct Library *a6 __asm("a6") = b;
    register ULONG d0 __asm("d0") = type;
    register APTR a0 __asm("a0") = obj;
    register struct TagItem *a1 __asm("a1") = tags;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0), "+r"(a0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_SETATTRSA)
        : "d1", "memory", "cc");
    return (LONG)d0;
}

static inline void psdSendPipe(struct Library *b, APTR pipe, APTR data,
                               ULONG len)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    register APTR a0 __asm("a0") = data;
    register ULONG d0 __asm("d0") = len;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0), "+r"(a0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_SENDPIPE)
        : "d1", "memory", "cc");
}

static inline void psdAbortPipe(struct Library *b, APTR pipe)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(a1) : "r"(a6), [lvo] "i"(PSD_LVO_ABORTPIPE)
        : "d0", "d1", "a0", "memory", "cc");
}

static inline LONG psdWaitPipe(struct Library *b, APTR pipe)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    register LONG d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_WAITPIPE)
        : "d1", "a0", "memory", "cc");
    return d0;
}

static inline ULONG psdGetPipeActual(struct Library *b, APTR pipe)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    register ULONG d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_GETPIPEACTUAL)
        : "d1", "a0", "memory", "cc");
    return d0;
}

static inline LONG psdGetPipeError(struct Library *b, APTR pipe)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a1 __asm("a1") = pipe;
    register LONG d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_GETPIPEERROR)
        : "d1", "a0", "memory", "cc");
    return d0;
}

static inline APTR psdFindInterfaceA(struct Library *b, APTR dev, APTR prev,
                                     struct TagItem *tags)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = dev;
    register APTR a2 __asm("a2") = prev;
    register struct TagItem *a1 __asm("a1") = tags;
    register APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a6), [lvo] "i"(PSD_LVO_FINDINTERFACEA)
        : "d1", "memory", "cc");
    return d0;
}

static inline APTR psdFindEndpointA(struct Library *b, APTR iface, APTR prev,
                                    struct TagItem *tags)
{
    register struct Library *a6 __asm("a6") = b;
    register APTR a0 __asm("a0") = iface;
    register APTR a2 __asm("a2") = prev;
    register struct TagItem *a1 __asm("a1") = tags;
    register APTR d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a6), [lvo] "i"(PSD_LVO_FINDENDPOINTA)
        : "d1", "memory", "cc");
    return d0;
}

static inline struct Task *psdSpawnSubTask(struct Library *b, STRPTR name,
                                           APTR initpc, APTR userdata)
{
    register struct Library *a6 __asm("a6") = b;
    register STRPTR a0 __asm("a0") = name;
    register APTR a1 __asm("a1") = initpc;
    register APTR a2 __asm("a2") = userdata;
    register struct Task *d0 __asm("d0");
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "=r"(d0), "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a6), [lvo] "i"(PSD_LVO_SPAWNSUBTASK)
        : "d1", "memory", "cc");
    return d0;
}

static inline APTR psdAddErrorMsgA(struct Library *b, UWORD level,
                                   STRPTR origin, STRPTR fmt, APTR fmtdata)
{
    register struct Library *a6 __asm("a6") = b;
    register ULONG d0 __asm("d0") = level;
    register STRPTR a0 __asm("a0") = origin;
    register STRPTR a1 __asm("a1") = fmt;
    register APTR a2 __asm("a2") = fmtdata;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0), "+r"(a0), "+r"(a1), "+r"(a2)
        : "r"(a6), [lvo] "i"(PSD_LVO_ADDERRORMSGA)
        : "d1", "memory", "cc");
    return (APTR)d0;
}

static inline ULONG psdBorrowLocksWait(struct Library *b, struct Task *task,
                                       ULONG sigs)
{
    register struct Library *a6 __asm("a6") = b;
    register struct Task *a1 __asm("a1") = task;
    register ULONG d0 __asm("d0") = sigs;
    __asm volatile ("jsr -%c[lvo](%%a6)"
        : "+r"(d0), "+r"(a1)
        : "r"(a6), [lvo] "i"(PSD_LVO_BORROWLOCKSWAIT)
        : "d1", "a0", "memory", "cc");
    return d0;
}

#endif /* PSD_API_H */
