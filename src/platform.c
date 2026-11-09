/* SPDX-License-Identifier: MPL-2.0 */
/*
 * platform.c - where the Ethernet MAC address of the board comes from.
 *
 * The LAN9514 on a Raspberry Pi 3B has no EEPROM, so its MAC registers read
 * 'ff:ff:ff:ff:ff:ff' until software programs them. The address belongs to
 * the board and is held by the VideoCore firmware. Two ways to get it, tried
 * in this order:
 *
 *   1. devicetree.resource: "local-mac-address" of the node that the
 *      /aliases/ethernet0 alias points to (the firmware patches it in).
 *   2. The VideoCore mailbox, property tag 0x00010003 "get board MAC address",
 *      addressed directly (Emu68 1.0.x has no mailbox.resource). The mailbox
 *      address is found in the device tree, as Emu68Info does.
 *
 * The raw mailbox path is not serialised against other users of the mailbox;
 * it is one short exchange at binding time, done under Forbid().
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>

#include "psd_api.h"
#include "dt_api.h"
#include "smsc95xx.h"

#define MBOX_TAG_BOARD_MAC      0x00010003UL
#define MBOX_CHANNEL_PROP       8UL
#define MBOX_READ               0x00
#define MBOX_STATUS             0x18
#define MBOX_WRITE              0x20
#define MBOX_TX_FULL            (1UL << 31)
#define MBOX_RX_EMPTY           (1UL << 30)
#define MBOX_SPIN_LIMIT         2000000UL

static ULONG le32(ULONG v)
{
    return __builtin_bswap32(v);
}

BOOL plat_mac_valid(const UBYTE *mac)
{
    UBYTE or_all = 0, and_all = 0xFF;
    int i;

    for (i = 0; i < 6; i++) {
        or_all |= mac[i];
        and_all &= mac[i];
    }
    if (or_all == 0 || and_all == 0xFF)
        return FALSE;           /* all zeroes or all ones */
    if (mac[0] & 1)
        return FALSE;           /* multicast bit: not a station address */
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* 1. devicetree.resource                                              */
/* ------------------------------------------------------------------ */

static BOOL mac_from_devicetree(APTR dt, UBYTE *mac)
{
    APTR key, prop;
    CONST_STRPTR path;
    BOOL ok = FALSE;

    if (!(key = DT_OpenKey(dt, "/aliases")))
        return FALSE;
    prop = DT_FindProperty(dt, key, "ethernet0");
    path = prop ? (CONST_STRPTR)DT_GetPropValue(dt, prop) : NULL;
    DT_CloseKey(dt, key);
    if (!path)
        return FALSE;

    if ((key = DT_OpenKey(dt, path))) {
        static const char *const names[] = { "local-mac-address", "mac-address", NULL };
        int n;

        for (n = 0; names[n] && !ok; n++) {
            prop = DT_FindProperty(dt, key, (CONST_STRPTR)names[n]);
            if (prop && DT_GetPropLen(dt, prop) >= 6) {
                const UBYTE *v = (const UBYTE *)DT_GetPropValue(dt, prop);
                int i;

                for (i = 0; i < 6; i++)
                    mac[i] = v[i];
                ok = plat_mac_valid(mac);
            }
        }
        DT_CloseKey(dt, key);
    }
    return ok;
}

/* ------------------------------------------------------------------ */
/* 2. VideoCore mailbox                                                */
/* ------------------------------------------------------------------ */

/* The cell count of a key, inherited from its parents; 'dflt' if absent. */
static ULONG dt_cells(APTR dt, APTR key, CONST_STRPTR name, ULONG dflt)
{
    APTR prop = DT_FindPropertyRecursive(dt, key, name);

    if (prop) {
        const ULONG *v = (const ULONG *)DT_GetPropValue(dt, prop);

        if (v)
            return *v;
    }
    return dflt;
}

/* CPU-physical address of the mailbox registers, or 0. */
static ULONG mailbox_base(APTR dt)
{
    APTR key, prop;
    CONST_STRPTR path;
    ULONG vc_addr = 0, base = 0;
    ULONG cells;
    const ULONG *reg;

    if (!(key = DT_OpenKey(dt, "/aliases")))
        return 0;
    prop = DT_FindProperty(dt, key, "mailbox");
    path = prop ? (CONST_STRPTR)DT_GetPropValue(dt, prop) : NULL;
    DT_CloseKey(dt, key);
    if (!path)
        return 0;

    if (!(key = DT_OpenKey(dt, path)))
        return 0;
    cells = dt_cells(dt, key, "#address-cells", 1);
    prop = DT_FindProperty(dt, key, "reg");
    reg = prop ? (const ULONG *)DT_GetPropValue(dt, prop) : NULL;
    if (reg && cells >= 1)
        vc_addr = reg[cells - 1];
    DT_CloseKey(dt, key);
    if (!vc_addr)
        return 0;

    /* Translate the VideoCore bus address with /soc "ranges". */
    if ((key = DT_OpenKey(dt, "/soc"))) {
        ULONG soc_cells = dt_cells(dt, key, "#address-cells", 1);
        ULONG cpu_cells = 1;
        APTR root = DT_OpenKey(dt, "/");

        if (root) {
            prop = DT_FindProperty(dt, root, "#address-cells");
            if (prop) {
                const ULONG *v = (const ULONG *)DT_GetPropValue(dt, prop);

                if (v)
                    cpu_cells = *v;
            }
            DT_CloseKey(dt, root);
        }
        prop = DT_FindProperty(dt, key, "ranges");
        reg = prop ? (const ULONG *)DT_GetPropValue(dt, prop) : NULL;
        if (reg && soc_cells >= 1 && cpu_cells >= 1) {
            ULONG phys_vc = reg[soc_cells - 1];
            ULONG phys_cpu = reg[soc_cells + cpu_cells - 1];

            base = vc_addr - phys_vc + phys_cpu;
        }
        DT_CloseKey(dt, key);
    }
    return base;
}

static BOOL mac_from_mailbox(APTR dt, UBYTE *mac)
{
    ULONG base = mailbox_base(dt);
    volatile ULONG *rd, *st, *wr;
    APTR raw;
    UBYTE *msg;
    ULONG *m, len = 32, spin, reply;
    BOOL ok = FALSE;
    int i;

    if (!base)
        return FALSE;
    rd = (volatile ULONG *)(base + MBOX_READ);
    st = (volatile ULONG *)(base + MBOX_STATUS);
    wr = (volatile ULONG *)(base + MBOX_WRITE);

    /* The message must be 16-byte aligned; its low 4 address bits carry the
     * channel. */
    if (!(raw = AllocMem(len + 16, MEMF_PUBLIC | MEMF_CLEAR)))
        return FALSE;
    msg = (UBYTE *)(((ULONG)raw + 15) & ~15UL);
    m = (ULONG *)msg;

    m[0] = le32(len);                   /* total size */
    m[1] = 0;                           /* process request */
    m[2] = le32(MBOX_TAG_BOARD_MAC);    /* tag */
    m[3] = le32(8);                     /* value buffer size */
    m[4] = 0;                           /* request */
    m[5] = 0;                           /* response: MAC bytes 0..3 */
    m[6] = 0;                           /* response: MAC bytes 4..5 */
    m[7] = 0;                           /* end tag */

    CachePreDMA(msg, &len, 0);
    Forbid();

    for (spin = 0; (le32(*st) & MBOX_TX_FULL) && spin < MBOX_SPIN_LIMIT; spin++)
        ;
    if (spin < MBOX_SPIN_LIMIT) {
        *wr = le32(((ULONG)msg & ~0xFUL) | MBOX_CHANNEL_PROP);

        for (;;) {
            for (spin = 0; (le32(*st) & MBOX_RX_EMPTY) && spin < MBOX_SPIN_LIMIT; spin++)
                ;
            if (spin >= MBOX_SPIN_LIMIT)
                break;
            reply = le32(*rd);
            if ((reply & 0xF) == MBOX_CHANNEL_PROP) {
                ok = TRUE;
                break;
            }
        }
    }
    Permit();
    CachePostDMA(msg, &len, 0);

    if (ok) {
        /* The response words hold the MAC in memory order, as Emu68Info
         * prints it (first byte = most significant byte of the first word). */
        for (i = 0; i < 6; i++)
            mac[i] = msg[20 + i];
        ok = plat_mac_valid(mac);
    }
    FreeMem(raw, len + 16);
    return ok;
}

/* ------------------------------------------------------------------ */
/* Entry point                                                         */
/* ------------------------------------------------------------------ */

/* Returns PLAT_MAC_FROM_DT / PLAT_MAC_FROM_MAILBOX, or PLAT_MAC_NONE. */
int plat_get_mac(UBYTE *mac)
{
    APTR dt = OpenResource("devicetree.resource");

    if (!dt)
        return PLAT_MAC_NONE;
    if (mac_from_devicetree(dt, mac))
        return PLAT_MAC_FROM_DT;
    if (mac_from_mailbox(dt, mac))
        return PLAT_MAC_FROM_MAILBOX;
    return PLAT_MAC_NONE;
}
