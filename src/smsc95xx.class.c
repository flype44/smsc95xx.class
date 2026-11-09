/* SPDX-License-Identifier: MPL-2.0 */
/*
 * smsc95xx.class.c - Poseidon class for SMSC/Microchip LAN95xx USB Ethernet.
 *
 * Milestone 2 ("probe"): bind to the device, read ID_REV and the MAC address
 * registers over the control pipe, log them in Poseidon's message list, then
 * wait to be released. No network traffic and no SANA-II yet.
 *
 * This file MUST be linked first: _start() has to land at text offset 0 so
 * that running the file from the CLI returns an error instead of executing
 * the ROMTag.
 */

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/libraries.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/utility.h>
#include <dos/var.h>
#include <proto/dos.h>

#include "psd_api.h"
#include "smsc95xx_regs.h"
#include "smsc95xx.h"

#define STR(s)  #s
#define XSTR(s) STR(s)

#ifndef CLASS_VERSION
#define CLASS_VERSION 0
#endif
#ifndef CLASS_REVISION
#define CLASS_REVISION 1
#endif
#ifndef CLASS_DATE
#define CLASS_DATE "00.00.0000"
#endif

#define CLASS_NAME  "smsc95xx.class"
#define CLASS_PRI   0

#define OPENLIB_POSEIDON_VER    4

struct ExecBase    *SysBase;
struct Library     *UtilityBase;
struct DosLibrary  *DOSBase;

/*
 * A library with a ROMTag must begin with moveq #-1,d0 so that a user who
 * runs the file gets an error rather than a crash. Keep everything else
 * textually below this.
 */
int __attribute__((no_reorder)) _start(void)
{
    return -1;
}

extern const char class_name[];
extern const char class_id_string[];
extern const ULONG init_table[4];

asm("romtag:                                \n"
    "       dc.w    "XSTR(RTC_MATCHWORD)"   \n"
    "       dc.l    romtag                  \n"
    "       dc.l    endcode                 \n"
    "       dc.b    "XSTR(RTF_AUTOINIT)"    \n"
    "       dc.b    "XSTR(CLASS_VERSION)"   \n"
    "       dc.b    "XSTR(NT_LIBRARY)"      \n"
    "       dc.b    "XSTR(CLASS_PRI)"       \n"
    "       dc.l    _class_name             \n"
    "       dc.l    _class_id_string        \n"
    "       dc.l    _init_table             \n"
    "endcode:                               \n");

const char class_name[]      = CLASS_NAME;
const char class_id_string[] = "$VER: " CLASS_NAME " " XSTR(CLASS_VERSION) "."
                               XSTR(CLASS_REVISION) " (" CLASS_DATE ")";

/* ------------------------------------------------------------------ */
/* Devices this class binds to                                         */
/* ------------------------------------------------------------------ */

struct BindEntry {
    UWORD vid;
    UWORD pid;
};

static const struct BindEntry bind_table[] = {
    { LAN95_VID_SMSC, LAN95_PID_LAN9512 },   /* LAN9512/9514: Raspberry Pi 3B */
    { LAN95_VID_SMSC, LAN95_PID_LAN9500 },
    { 0, 0 }
};

static BOOL is_supported(UWORD vid, UWORD pid)
{
    const struct BindEntry *e;

    for (e = bind_table; e->vid; e++) {
        if (e->vid == vid && e->pid == pid)
            return TRUE;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* Logging in Poseidon's message list                                  */
/* ------------------------------------------------------------------ */

/*
 * psdAddErrorMsgA takes an exec RawDoFmt argument array; on m68k every %ld
 * and %lx consumes one 32-bit slot.
 */
void smsc_log(struct Library *ps, UWORD level, STRPTR fmt, ULONG *args)
{
    psdAddErrorMsgA(ps, level, (STRPTR)class_name, fmt, args);
}

/* ------------------------------------------------------------------ */
/* The binding's task                                                  */
/* ------------------------------------------------------------------ */

/* Tell whoever is waiting in Force/ReleaseDeviceBinding that we are done
 * starting up (or shutting down). Forbid() keeps the waiter's Task valid. */
static void notify_ready(struct Smsc95xxBinding *b)
{
    Forbid();
    if (b->ready_task)
        Signal(b->ready_task, 1UL << b->ready_sig);
    Permit();
}

/* Fixed locally-administered address used only if the platform gives none. */
static const UBYTE fallback_mac[6] = { 0x02, 0x80, 0xE1, 0x00, 0x00, 0x01 };

static void log_mac(struct Smsc95xxBinding *b, STRPTR text, const UBYTE *mac)
{
    ULONG args[7];
    int i;

    args[0] = (ULONG)text;
    for (i = 0; i < 6; i++)
        args[i + 1] = mac[i];
    smsc_log(b->ps, PSD_MSG_INFO,
             "%s %02lx:%02lx:%02lx:%02lx:%02lx:%02lx.", args);
}

/* Is the global variable ENV:<name> set (to anything)? */
static BOOL env_flag(const char *name)
{
    UBYTE buf[4];

    return GetVar((CONST_STRPTR)name, buf, sizeof(buf), GVF_GLOBAL_ONLY) >= 0;
}

static BOOL bring_up_chip(struct Smsc95xxBinding *b)
{
    ULONG idrev;
    ULONG args[8];
    UBYTE reg_mac[6];
    int src, step;
    UWORD id1 = 0, id2 = 0;

    if (!lan95_read_reg(b, LAN95_ID_REV, &idrev)) {
        smsc_log(b->ps, PSD_MSG_FAIL, "Cannot read ID_REV.", args);
        return FALSE;
    }
    b->chip_id = (idrev & LAN95_ID_REV_CHIP_MASK) >> LAN95_ID_REV_CHIP_SHIFT;
    b->chip_rev = idrev & LAN95_ID_REV_REV_MASK;
    args[0] = b->chip_id;
    args[1] = b->chip_rev;
    smsc_log(b->ps, PSD_MSG_INFO, "LAN95xx chip id %04lx, revision %04lx.", args);

    /* The Pi 3 has no EEPROM: take the board's address from the platform. */
    b->phy = LAN95_PHY_ADDR;
    src = plat_get_mac(b->mac);
    if (src == PLAT_MAC_NONE) {
        CopyMem((APTR)fallback_mac, b->mac, 6);
        log_mac(b, (STRPTR)"No board MAC found, using fallback", b->mac);
    } else {
        log_mac(b, (STRPTR)(src == PLAT_MAC_FROM_DT ? "Board MAC (device tree)"
                                                    : "Board MAC (mailbox)"),
                b->mac);
    }

    /* Receive format switches, for trying things out on a running system:
     * ENV:smsc95xx.single selects one frame per transfer, ENV:smsc95xx.nobir
     * leaves the "bulk-in empty response" bit clear. */
    b->multi = !env_flag("smsc95xx.single");
    b->no_bir = env_flag("smsc95xx.nobir");
    args[0] = (ULONG)(b->multi ? "multi-frame" : "single-frame");
    args[1] = (ULONG)(b->no_bir ? "off" : "on");
    smsc_log(b->ps, PSD_MSG_INFO, "Receive mode: %s, bulk-in empty response %s.", args);

    step = lan95_chip_init(b);
    if (step) {
        args[0] = step;
        smsc_log(b->ps, PSD_MSG_FAIL, "Chip initialisation failed at step %ld.", args);
        return FALSE;
    }

    /* Read the address back to prove the write reached the chip. */
    if (lan95_get_mac(b, reg_mac))
        log_mac(b, (STRPTR)"MAC registers now", reg_mac);

    if (lan95_mii_read(b, MII_PHYSID1, &id1) &&
        lan95_mii_read(b, MII_PHYSID2, &id2)) {
        args[0] = id1;
        args[1] = id2;
        smsc_log(b->ps, PSD_MSG_INFO, "PHY id %04lx:%04lx.", args);
    } else {
        smsc_log(b->ps, PSD_MSG_WARN, "Cannot read the PHY id.", args);
    }

    if (!lan95_phy_start(b)) {
        smsc_log(b->ps, PSD_MSG_FAIL, "Cannot start auto-negotiation.", args);
        return FALSE;
    }
    smsc_log(b->ps, PSD_MSG_INFO, "Chip initialised, waiting for link.", args);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Timer, link and statistics                                          */
/* ------------------------------------------------------------------ */

#define LINK_POLL_MS        500
#define STATS_EVERY_TICKS   120         /* x LINK_POLL_MS = 60 s */
#define SELFTEST_AFTER_TICKS 4

static BOOL timer_open(struct Smsc95xxBinding *b)
{
    if (!(b->timer_port = CreateMsgPort()))
        return FALSE;
    b->timer_req = (struct timerequest *)CreateIORequest(
        b->timer_port, sizeof(struct timerequest));
    if (!b->timer_req)
        return FALSE;
    if (OpenDevice("timer.device", UNIT_VBLANK,
                   (struct IORequest *)b->timer_req, 0)) {
        DeleteIORequest((struct IORequest *)b->timer_req);
        b->timer_req = NULL;
        return FALSE;
    }
    return TRUE;
}

static void timer_start(struct Smsc95xxBinding *b, ULONG ms)
{
    b->timer_req->tr_node.io_Command = TR_ADDREQUEST;
    b->timer_req->tr_time.tv_secs = ms / 1000;
    b->timer_req->tr_time.tv_micro = (ms % 1000) * 1000;
    SendIO((struct IORequest *)b->timer_req);
    b->timer_pending = TRUE;
}

static void timer_close(struct Smsc95xxBinding *b)
{
    if (b->timer_req) {
        if (b->timer_pending) {
            AbortIO((struct IORequest *)b->timer_req);
            WaitIO((struct IORequest *)b->timer_req);
            b->timer_pending = FALSE;
        }
        CloseDevice((struct IORequest *)b->timer_req);
        DeleteIORequest((struct IORequest *)b->timer_req);
        b->timer_req = NULL;
    }
    if (b->timer_port) {
        DeleteMsgPort(b->timer_port);
        b->timer_port = NULL;
    }
}

/* timer.device GetSysTime() (LVO -66, A0 = struct timeval *). */
static void timer_now(struct Smsc95xxBinding *b, struct timeval *tv)
{
    register struct Device *a6 __asm("a6") = b->timer_req->tr_node.io_Device;
    register struct timeval *a0 __asm("a0") = tv;

    __asm volatile ("jsr -66(%%a6)"
        : "+r"(a0) : "r"(a6) : "d0", "d1", "a1", "memory", "cc");
}

#define POLL_SLOW_MS    20      /* a link poll longer than this is logged */
#define POLL_SLOW_LOGS  8       /* ... for the first few times only      */

static void link_tick(struct Smsc95xxBinding *b)
{
    ULONG args[3];
    struct timeval t0, t1;
    ULONG ms;
    BOOL ok;

    /* The task serves RX, TX and SANA-II requests only between two steps of
     * this poll, so how long it takes is what the traffic waits for. */
    timer_now(b, &t0);
    ok = lan95_link_update(b);
    timer_now(b, &t1);
    ms = (t1.tv_secs - t0.tv_secs) * 1000UL;
    ms += t1.tv_micro / 1000UL;
    ms -= t0.tv_micro / 1000UL;
    if (ms > b->poll_max_ms)
        b->poll_max_ms = ms;
    if (ms > POLL_SLOW_MS && b->poll_slow_logged < POLL_SLOW_LOGS) {
        b->poll_slow_logged++;
        args[0] = ms;
        smsc_log(b->ps, PSD_MSG_INFO, "Link poll took %ld ms.", args);
    }

    if (!ok) {
        smsc_log(b->ps, PSD_MSG_WARN, "Link status read failed.", args);
        return;
    }
    if (b->link_logged && b->link_up == b->last_up &&
        b->speed == b->last_speed && b->full_duplex == b->last_fdx)
        return;

    if (b->link_up) {
        args[0] = b->speed;
        args[1] = (ULONG)(b->full_duplex ? "full" : "half");
        smsc_log(b->ps, PSD_MSG_INFO, "Link up: %ld Mbit/s, %s duplex.", args);
    } else if (b->link_logged) {
        smsc_log(b->ps, PSD_MSG_INFO, "Link down.", args);
    }
    b->link_logged = TRUE;
    b->last_up = b->link_up;
    b->last_speed = b->speed;
    b->last_fdx = b->full_duplex;
}

static void stats_log(struct Smsc95xxBinding *b, BOOL force)
{
    ULONG args[6];

    if (!force && b->stat_rx == b->rx_frames && b->stat_tx == b->tx_frames &&
        b->stat_err == b->rx_errors + b->tx_errors)
        return;
    b->stat_rx = b->rx_frames;
    b->stat_tx = b->tx_frames;
    b->stat_err = b->rx_errors + b->tx_errors;

    args[0] = b->rx_frames;
    args[1] = b->rx_bytes;
    args[2] = b->rx_errors;
    args[3] = b->tx_frames;
    args[4] = b->tx_errors;
    args[5] = b->rx_empty;
    smsc_log(b->ps, PSD_MSG_INFO,
             "Stats: RX %ld frames (%ld bytes, %ld errors), TX %ld frames (%ld errors), %ld empty reads.",
             args);
}

/*
 * Optional self-test: if the global variable ENV:smsc95xx.selftest holds an
 * IPv4 address ("192.168.1.1"), one ARP probe for it is sent a couple of
 * seconds after the link comes up. Nothing is sent without that variable.
 */
static void selftest_read(struct Smsc95xxBinding *b)
{
    UBYTE buf[32];
    LONG n;
    ULONG ip = 0, octet = 0, parts = 0;
    int i;

    n = GetVar("smsc95xx.selftest", buf, sizeof(buf), GVF_GLOBAL_ONLY);
    if (n <= 0)
        return;
    for (i = 0; i <= n && i < (int)sizeof(buf); i++) {
        if (i < n && buf[i] >= '0' && buf[i] <= '9') {
            octet = octet * 10 + (ULONG)(buf[i] - '0');
            if (octet > 255)
                return;
        } else if (i == n || buf[i] == '.') {
            ip = (ip << 8) | octet;
            octet = 0;
            parts++;
        } else {
            return;                     /* not a dotted quad */
        }
    }
    if (parts == 4) {
        b->selftest_ip = ip;
        b->selftest_wanted = TRUE;
    }
}

static void selftest_tick(struct Smsc95xxBinding *b)
{
    ULONG args[4];

    if (!b->selftest_wanted || b->selftest_sent || !b->link_up ||
        b->ticks < SELFTEST_AFTER_TICKS)
        return;
    b->selftest_sent = TRUE;
    args[0] = (b->selftest_ip >> 24) & 0xFF;
    args[1] = (b->selftest_ip >> 16) & 0xFF;
    args[2] = (b->selftest_ip >> 8) & 0xFF;
    args[3] = b->selftest_ip & 0xFF;
    if (data_send_arp_probe(b, b->selftest_ip))
        smsc_log(b->ps, PSD_MSG_INFO,
                 "Self-test: ARP probe for %ld.%ld.%ld.%ld sent.", args);
    else
        smsc_log(b->ps, PSD_MSG_WARN, "Self-test: could not send.", args);
}

/* Event loop: finished pipes, the half-second tick, and CTRL_C to stop. */
static void run_loop(struct Smsc95xxBinding *b)
{
    ULONG psig = 1UL << b->port->mp_SigBit;
    ULONG tsig = 1UL << b->timer_port->mp_SigBit;
    ULONG usig = sana_unit_sigmask(b->unit);
    ULONG sigs;
    struct Message *msg;

    timer_start(b, LINK_POLL_MS);
    for (;;) {
        sigs = Wait(psig | tsig | usig | SIGBREAKF_CTRL_C);

        while ((msg = GetMsg(b->port)))
            (void)data_pipe_done(b, (APTR)msg);

        /* SANA-II requests, then whatever they or a finished pipe unlocked. */
        sana_handle_msgs(b->unit);
        sana_housekeeping(b->unit);
        sana_tx_kick(b->unit);

        if (sigs & tsig) {
            (void)GetMsg(b->timer_port);
            b->timer_pending = FALSE;
            b->ticks++;
            link_tick(b);
            selftest_tick(b);
            if (b->ticks % STATS_EVERY_TICKS == 0)
                stats_log(b, FALSE);
            timer_start(b, LINK_POLL_MS);
        }
        if (sigs & SIGBREAKF_CTRL_C)
            break;
    }
}

/* The data path: endpoints and pipes, timer, transmitter/receiver. */
static void run_data_path(struct Smsc95xxBinding *b)
{
    ULONG args[2];

    if (!data_open(b) || !timer_open(b)) {
        smsc_log(b->ps, PSD_MSG_FAIL, "Data path setup failed.", args);
        Wait(SIGBREAKF_CTRL_C);
        return;
    }
    if (!lan95_mac_enable(b)) {
        smsc_log(b->ps, PSD_MSG_FAIL, "Cannot enable the MAC.", args);
        Wait(SIGBREAKF_CTRL_C);
        return;
    }
    selftest_read(b);
    data_start(b);
    if (!sana_unit_attach(b->unit, b)) {
        smsc_log(b->ps, PSD_MSG_FAIL, "Cannot attach the SANA-II unit.", args);
        Wait(SIGBREAKF_CTRL_C);
        return;
    }
    args[0] = b->unit->number;
    smsc_log(b->ps, PSD_MSG_INFO,
             "Receiver and transmitter running; smsc95xx.device unit %ld.", args);
    run_loop(b);
    sana_unit_detach(b->unit);
    stats_log(b, TRUE);
}

static void smsc95xx_task(void)
{
    struct Smsc95xxBinding *b = (struct Smsc95xxBinding *)FindTask(NULL)->tc_UserData;

    ULONG args[2];

    do {
        if (!(b->ps = OpenLibrary("poseidon.library", OPENLIB_POSEIDON_VER)))
            break;

        if (!(b->iobuf = AllocVec(16, MEMF_PUBLIC | MEMF_CLEAR))) {
            smsc_log(b->ps, PSD_MSG_FAIL, "Out of memory (iobuf).", args);
            break;
        }
        if (!(b->port = CreateMsgPort())) {
            smsc_log(b->ps, PSD_MSG_FAIL, "Cannot create message port.", args);
            break;
        }
        if (!(b->ep0 = psdAllocPipe(b->ps, b->device, b->port, NULL))) {
            smsc_log(b->ps, PSD_MSG_FAIL, "Cannot allocate the control pipe.", args);
            break;
        }
        smsc_log(b->ps, PSD_MSG_INFO, "Binding task started.", args);

        b->task = FindTask(NULL);
        notify_ready(b);

        if (bring_up_chip(b))
            run_data_path(b);
        else
            Wait(SIGBREAKF_CTRL_C);   /* keep the binding for the log */
    } while (0);

    /* Stop the MAC before its pipes go away. */
    if (b->ep0 && (b->mac_cr & (LAN95_MAC_CR_TXEN | LAN95_MAC_CR_RXEN)))
        lan95_mac_disable(b);
    if (b->ps) {
        data_close(b);
        timer_close(b);
    }
    if (b->ep0)
        psdFreePipe(b->ps, b->ep0);
    if (b->port)
        DeleteMsgPort(b->port);
    if (b->iobuf)
        FreeVec(b->iobuf);
    if (b->ps)
        CloseLibrary(b->ps);
    b->ep0 = NULL;
    b->port = NULL;
    b->iobuf = NULL;
    b->ps = NULL;

    /* Clearing b->task and signalling happen under Forbid(), so a waiting
     * ReleaseDeviceBinding sees a consistent state and this task is still
     * alive while it is signalled. */
    Forbid();
    b->task = NULL;
    if (b->ready_task)
        Signal(b->ready_task, 1UL << b->ready_sig);
    /* The task ends when this function returns. */
}

/* ------------------------------------------------------------------ */
/* Binding methods                                                     */
/* ------------------------------------------------------------------ */

static struct Smsc95xxBinding *bind_force(struct Smsc95xxClass *cb, APTR pd)
{
    struct Library *ps;
    struct Smsc95xxBinding *b;
    struct Task *task;

    if (!(ps = OpenLibrary("poseidon.library", OPENLIB_POSEIDON_VER)))
        return NULL;

    if (!(b = AllocVec(sizeof(*b), MEMF_PUBLIC | MEMF_CLEAR))) {
        CloseLibrary(ps);
        return NULL;
    }
    b->cls = cb;
    b->device = pd;
    b->unit = sana_unit_get(cb->dev, b);
    if (!b->unit) {
        FreeVec(b);
        CloseLibrary(ps);
        return NULL;
    }

    b->ready_task = FindTask(NULL);
    b->ready_sig = SIGB_SINGLE;
    SetSignal(0, SIGF_SINGLE);

    task = psdSpawnSubTask(ps, (STRPTR)class_name, (APTR)smsc95xx_task, b);
    if (task) {
        /* The task may need Poseidon's locks that we hold, hence not Wait(). */
        psdBorrowLocksWait(ps, task, SIGF_SINGLE);
    }
    b->ready_task = NULL;

    if (!task || !b->task) {
        ULONG args[2];

        args[0] = (ULONG)task;
        smsc_log(ps, PSD_MSG_FAIL, "Binding failed (task %08lx).", args);
        FreeVec(b);
        CloseLibrary(ps);
        return NULL;
    }
    CloseLibrary(ps);
    return b;
}

static struct Smsc95xxBinding *bind_attempt(struct Smsc95xxClass *cb, APTR pd)
{
    struct Library *ps;
    ULONG vid = 0, pid = 0;
    struct TagItem tags[3];

    if (!(ps = OpenLibrary("poseidon.library", OPENLIB_POSEIDON_VER)))
        return NULL;

    tags[0].ti_Tag = DA_VendorID;
    tags[0].ti_Data = (ULONG)&vid;
    tags[1].ti_Tag = DA_ProductID;
    tags[1].ti_Data = (ULONG)&pid;
    tags[2].ti_Tag = TAG_END;
    tags[2].ti_Data = 0;
    psdGetAttrsA(ps, PGA_DEVICE, pd, tags);

    /* Log only SMSC devices, or every unrelated device would be logged on
     * each scan. */
    if (vid == LAN95_VID_SMSC) {
        ULONG args[2];

        args[0] = vid;
        args[1] = pid;
        smsc_log(ps, PSD_MSG_INFO, "AttemptDeviceBinding %04lx:%04lx.", args);
    }
    CloseLibrary(ps);

    if (!is_supported((UWORD)vid, (UWORD)pid))
        return NULL;
    return bind_force(cb, pd);
}

static void bind_release(struct Smsc95xxBinding *b)
{
    Forbid();
    b->ready_sig = SIGB_SINGLE;
    b->ready_task = FindTask(NULL);
    SetSignal(0, SIGF_SINGLE);
    if (b->task)
        Signal(b->task, SIGBREAKF_CTRL_C);
    Permit();

    while (b->task)
        Wait(SIGF_SINGLE);

    FreeVec(b);
}

/* ------------------------------------------------------------------ */
/* usbclass ABI vectors                                                */
/* ------------------------------------------------------------------ */

#define UGA_CLASS               0x01
#define UGA_BINDING             0x02

#define UCCA_Dummy              (TAG_USER + 4489)
#define UCCA_Priority           (UCCA_Dummy + 0x01)
#define UCCA_Description        (UCCA_Dummy + 0x02)
#define UCCA_HasClassCfgGUI     (UCCA_Dummy + 0x10)
#define UCCA_HasBindingCfgGUI   (UCCA_Dummy + 0x11)
#define UCCA_AfterDOSRestart    (UCCA_Dummy + 0x20)
#define UCCA_UsingDefaultCfg    (UCCA_Dummy + 0x30)
#define UCCA_SupportsSuspend    (UCCA_Dummy + 0x40)

#define UCBA_Dummy              (TAG_USER + 103)
#define UCBA_UsingDefaultCfg    (UCBA_Dummy + 0x30)

#define UCM_AttemptDeviceBinding 0x0004
#define UCM_ForceDeviceBinding   0x0005
#define UCM_ReleaseDeviceBinding 0x0006

static void set_tag(struct TagItem *tags, ULONG tag, ULONG value, LONG *count)
{
    struct TagItem *ti = FindTagItem(tag, tags);

    if (ti) {
        *(ULONG *)ti->ti_Data = value;
        (*count)++;
    }
}

LONG usbGetAttrsA(ULONG type asm("d0"), APTR usbstruct asm("a0"),
                  struct TagItem *tags asm("a1"),
                  struct Smsc95xxClass *cb asm("a6"));
LONG usbGetAttrsA(ULONG type asm("d0"), APTR usbstruct asm("a0"),
                  struct TagItem *tags asm("a1"),
                  struct Smsc95xxClass *cb asm("a6"))
{
    LONG count = 0;
    struct TagItem *ti;

    (void)usbstruct;
    (void)cb;

    switch (type) {
    case UGA_CLASS:
        set_tag(tags, UCCA_Priority, 0, &count);
        if ((ti = FindTagItem(UCCA_Description, tags))) {
            *(STRPTR *)ti->ti_Data =
                "Ethernet SANA driver for SMSC LAN95xx chipsets via smsc95xx.device";
            count++;
        }
        set_tag(tags, UCCA_HasClassCfgGUI, FALSE, &count);
        set_tag(tags, UCCA_HasBindingCfgGUI, FALSE, &count);
        set_tag(tags, UCCA_AfterDOSRestart, FALSE, &count);
        set_tag(tags, UCCA_UsingDefaultCfg, TRUE, &count);
        set_tag(tags, UCCA_SupportsSuspend, FALSE, &count);
        break;
    case UGA_BINDING:
        set_tag(tags, UCBA_UsingDefaultCfg, TRUE, &count);
        break;
    }
    return count;
}

LONG usbSetAttrsA(ULONG type asm("d0"), APTR usbstruct asm("a0"),
                  struct TagItem *tags asm("a1"),
                  struct Smsc95xxClass *cb asm("a6"));
LONG usbSetAttrsA(ULONG type asm("d0"), APTR usbstruct asm("a0"),
                  struct TagItem *tags asm("a1"),
                  struct Smsc95xxClass *cb asm("a6"))
{
    (void)type;
    (void)usbstruct;
    (void)tags;
    (void)cb;
    return 0;
}

LONG usbDoMethodA(ULONG method asm("d0"), ULONG *data asm("a1"),
                  struct Smsc95xxClass *cb asm("a6"));
LONG usbDoMethodA(ULONG method asm("d0"), ULONG *data asm("a1"),
                  struct Smsc95xxClass *cb asm("a6"))
{
    switch (method) {
    case UCM_AttemptDeviceBinding:
        return (LONG)bind_attempt(cb, (APTR)data[0]);
    case UCM_ForceDeviceBinding:
        return (LONG)bind_force(cb, (APTR)data[0]);
    case UCM_ReleaseDeviceBinding:
        bind_release((struct Smsc95xxBinding *)data[0]);
        return TRUE;
    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ */
/* Library shell                                                       */
/* ------------------------------------------------------------------ */

static struct Smsc95xxClass *lib_init(
    struct Smsc95xxClass *cb asm("d0"), BPTR seglist asm("a0"),
    struct ExecBase *sysbase asm("a6"))
{
    SysBase = sysbase;
    cb->seglist = seglist;

    cb->lib.lib_Node.ln_Type = NT_LIBRARY;
    cb->lib.lib_Node.ln_Name = (char *)class_name;
    cb->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    cb->lib.lib_Version = CLASS_VERSION;
    cb->lib.lib_Revision = CLASS_REVISION;
    cb->lib.lib_IdString = (APTR)class_id_string;

    UtilityBase = OpenLibrary("utility.library", 39);
    if (!UtilityBase)
        return NULL;
    DOSBase = (struct DosLibrary *)OpenLibrary("dos.library", 36);
    if (!DOSBase) {
        CloseLibrary(UtilityBase);
        return NULL;
    }
    cb->dev = sana_dev_create();
    if (!cb->dev) {
        CloseLibrary((struct Library *)DOSBase);
        CloseLibrary(UtilityBase);
        return NULL;
    }
    return cb;
}

static struct Smsc95xxClass *lib_open(struct Smsc95xxClass *cb asm("a6"))
{
    cb->lib.lib_OpenCnt++;
    cb->lib.lib_Flags &= ~LIBF_DELEXP;
    return cb;
}

static BPTR lib_expunge(struct Smsc95xxClass *cb asm("a6"));

static BPTR lib_close(struct Smsc95xxClass *cb asm("a6"))
{
    cb->lib.lib_OpenCnt--;
    if (cb->lib.lib_OpenCnt == 0 && (cb->lib.lib_Flags & LIBF_DELEXP))
        return lib_expunge(cb);
    return 0;
}

static BPTR lib_expunge(struct Smsc95xxClass *cb asm("a6"))
{
    BPTR seglist;

    if (cb->lib.lib_OpenCnt > 0) {
        cb->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    /* The device goes with the class, but not while somebody has it open. */
    if (!sana_dev_remove(cb->dev)) {
        cb->lib.lib_Flags |= LIBF_DELEXP;
        return 0;
    }

    seglist = cb->seglist;
    Remove(&cb->lib.lib_Node);
    CloseLibrary((struct Library *)DOSBase);
    CloseLibrary(UtilityBase);
    FreeMem((APTR)((ULONG)cb - cb->lib.lib_NegSize),
            cb->lib.lib_NegSize + cb->lib.lib_PosSize);
    return seglist;
}

static ULONG lib_reserved(void)
{
    return 0;
}

/*
 * Poseidon 4.x class ABI (checked against poseidon.library 4.5 and its own
 * classes, see docs/ABI-poseidon-4.5.md). After the four standard vectors:
 *   -30 usbAttemptInterfaceBinding(a0 interface)   -> binding or 0
 *   -36 usbReleaseInterfaceBinding(a0 binding)
 *   -42 usbAttemptDeviceBinding(a0 device)         -> binding or 0
 *   -48 usbReleaseDeviceBinding(a0 binding)
 *   -54 usbForceInterfaceBinding(a0 interface)     -> binding or 0
 *   -60 usbForceDeviceBinding(a0 device)           -> binding or 0
 *   -66 usbGetAttrsA  -72 usbSetAttrsA  -78 usbDoMethodA
 * Poseidon 4.5 binds devices by calling these vectors directly (the
 * usbDoMethodA binding methods are only a second route to the same code).
 * The AROS layout (entry points at -30/-36/-42) does NOT apply here.
 */
static ULONG lib_no_interface(void)
{
    return 0;                   /* this class binds whole devices only */
}

static void lib_no_release(void)
{
}

struct Smsc95xxBinding *usbAttemptDeviceBinding(
    APTR dev asm("a0"), struct Smsc95xxClass *cb asm("a6"));
struct Smsc95xxBinding *usbAttemptDeviceBinding(
    APTR dev asm("a0"), struct Smsc95xxClass *cb asm("a6"))
{
    return bind_attempt(cb, dev);
}

struct Smsc95xxBinding *usbForceDeviceBinding(
    APTR dev asm("a0"), struct Smsc95xxClass *cb asm("a6"));
struct Smsc95xxBinding *usbForceDeviceBinding(
    APTR dev asm("a0"), struct Smsc95xxClass *cb asm("a6"))
{
    return bind_force(cb, dev);
}

void usbReleaseDeviceBinding(
    struct Smsc95xxBinding *b asm("a0"), struct Smsc95xxClass *cb asm("a6"));
void usbReleaseDeviceBinding(
    struct Smsc95xxBinding *b asm("a0"), struct Smsc95xxClass *cb asm("a6"))
{
    (void)cb;
    bind_release(b);
}

static const APTR func_table[] = {
    (APTR)lib_open,
    (APTR)lib_close,
    (APTR)lib_expunge,
    (APTR)lib_reserved,
    (APTR)lib_no_interface,     /* -30 AttemptInterfaceBinding */
    (APTR)lib_no_release,       /* -36 ReleaseInterfaceBinding */
    (APTR)usbAttemptDeviceBinding, /* -42 */
    (APTR)usbReleaseDeviceBinding, /* -48 */
    (APTR)lib_no_interface,     /* -54 ForceInterfaceBinding   */
    (APTR)usbForceDeviceBinding,   /* -60 */
    (APTR)usbGetAttrsA,         /* -66 */
    (APTR)usbSetAttrsA,         /* -72 */
    (APTR)usbDoMethodA,         /* -78 */
    (APTR)-1
};

const ULONG init_table[4] = {
    sizeof(struct Smsc95xxClass),
    (ULONG)func_table,
    0,                          /* no InitStruct table: lib_init fills in */
    (ULONG)lib_init
};
