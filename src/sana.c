/* SPDX-License-Identifier: MPL-2.0 */
/*
 * sana.c - the SANA-II device interface of smsc95xx.device.
 *
 * The device is created by smsc95xx.class at start-up (MakeLibrary + AddDevice)
 * and lives as long as the class. A unit is one network interface. Requests
 * reach the unit through BeginIO, which only hands them to the binding's task
 * (PutMsg on the unit's command port); the task executes every command, so
 * the state of the unit is touched by one task. Pending requests (reads,
 * orphans, events, queued writes) sit on lists that AbortIO may also edit,
 * always under Forbid().
 *
 * Buffer management: each OpenDevice may pass S2_CopyToBuff / S2_CopyFromBuff
 * functions (A0 = to, A1 = from, D0 = bytes, result in D0). Without them the
 * data are plain memory. The DMA and packet-filter hooks are ignored: they
 * are optional optimisations.
 */

#include <stddef.h>
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/errors.h>
#include <exec/io.h>
#include <exec/interrupts.h>
#include <utility/tagitem.h>
#include <devices/newstyle.h>
#include <devices/sana2specialstats.h>
#include <proto/exec.h>
#include <proto/utility.h>

#include "smsc95xx_regs.h"
#include "smsc95xx.h"

#define STR(s)  #s
#define XSTR(s) STR(s)

extern struct Library *UtilityBase;

#define DEV_ID_STRING "smsc95xx.device " XSTR(CLASS_VERSION) "." \
                      XSTR(CLASS_REVISION) " (" CLASS_DATE ")"

/* Commands this device understands (for NSCMD_DEVICEQUERY). */
static const UWORD supported_cmds[] = {
    NSCMD_DEVICEQUERY, CMD_READ, CMD_WRITE, CMD_FLUSH,
    S2_DEVICEQUERY, S2_GETSTATIONADDRESS, S2_CONFIGINTERFACE,
    S2_ADDMULTICASTADDRESS, S2_DELMULTICASTADDRESS, S2_MULTICAST,
    S2_BROADCAST, S2_TRACKTYPE, S2_UNTRACKTYPE, S2_GETTYPESTATS,
    S2_GETSPECIALSTATS, S2_GETGLOBALSTATS, S2_ONEVENT, S2_READORPHAN,
    S2_ONLINE, S2_OFFLINE, S2_ADDMULTICASTADDRESSES, S2_DELMULTICASTADDRESSES,
    0
};

#define EVENTS_SUPPORTED (S2EVENT_ERROR | S2EVENT_TX | S2EVENT_RX | \
                          S2EVENT_ONLINE | S2EVENT_OFFLINE | S2EVENT_BUFF | \
                          S2EVENT_HARDWARE | S2EVENT_SOFTWARE)

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

static BOOL addr_is_multicast(const UBYTE *a)
{
    return (a[0] & 1) ? TRUE : FALSE;
}

static BOOL addr_is_broadcast(const UBYTE *a)
{
    int i;

    for (i = 0; i < ETH_ALEN; i++)
        if (a[i] != 0xFF)
            return FALSE;
    return TRUE;
}

static BOOL addr_equal(const UBYTE *a, const UBYTE *b)
{
    int i;

    for (i = 0; i < ETH_ALEN; i++)
        if (a[i] != b[i])
            return FALSE;
    return TRUE;
}

static void addr_copy(UBYTE *dst, const UBYTE *src)
{
    int i;

    for (i = 0; i < ETH_ALEN; i++)
        dst[i] = src[i];
}

/* exec has no new_list(); it lives in amiga.lib, which a -nostdlib build lacks. */
static void new_list(struct List *l)
{
    l->lh_Head = (struct Node *)&l->lh_Tail;
    l->lh_Tail = NULL;
    l->lh_TailPred = (struct Node *)l;
}

/* Complete a request that was handed to the task. */
static void req_done(struct IOSana2Req *r, BYTE err, ULONG wire)
{
    r->ios2_Req.io_Error = err;
    r->ios2_WireError = wire;
    ReplyMsg(&r->ios2_Req.io_Message);
}

static void req_ok(struct IOSana2Req *r)
{
    req_done(r, 0, 0);
}

/* The Ethernet CRC-32 (MSB-first register, LSB-first data bits), whose top
 * six bits index the chip's 64-bit multicast hash table. */
static ULONG eth_crc(const UBYTE *p, ULONG n)
{
    ULONG crc = 0xFFFFFFFFUL;
    ULONG i;
    int j;

    for (i = 0; i < n; i++) {
        UBYTE c = p[i];

        for (j = 0; j < 8; j++) {
            ULONG carry = ((crc >> 31) ^ (ULONG)(c & 1)) & 1;

            crc <<= 1;
            c >>= 1;
            if (carry)
                crc ^= 0x04C11DB7UL;
        }
    }
    return crc;
}

/* Call a buffer-management function: A0 = to, A1 = from, D0 = bytes. */
static BOOL call_copy(APTR fn, APTR to, const void *from, ULONG n)
{
    register APTR a0 __asm("a0") = to;
    register const void *a1 __asm("a1") = from;
    register ULONG d0 __asm("d0") = n;

    __asm volatile ("jsr (%[fn])"
        : "+r"(d0), "+r"(a0), "+r"(a1)
        : [fn] "a"(fn)
        : "d1", "memory", "cc");
    return d0 != 0;
}

static BOOL op_copy_to(struct Opener *o, APTR dst, const void *src, ULONG n)
{
    if (o && o->copy_to)
        return call_copy(o->copy_to, dst, src, n);
    CopyMem((APTR)src, dst, n);
    return TRUE;
}

static BOOL op_copy_from(struct Opener *o, APTR dst, const void *src, ULONG n)
{
    if (o && o->copy_from)
        return call_copy(o->copy_from, dst, src, n);
    CopyMem((APTR)src, dst, n);
    return TRUE;
}

static BOOL list_remove(struct List *l, struct Node *want)
{
    struct Node *n;

    for (n = l->lh_Head; n->ln_Succ; n = n->ln_Succ) {
        if (n == want) {
            Remove(n);
            return TRUE;
        }
    }
    return FALSE;
}

static struct TypeStat *find_type(struct Smsc95xxUnit *u, ULONG type)
{
    struct Node *n;

    for (n = u->types.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        if (((struct TypeStat *)n)->type == type)
            return (struct TypeStat *)n;
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* Receive mode (promiscuous, multicast filter)                        */
/* ------------------------------------------------------------------ */

static void apply_rx_mode(struct Smsc95xxUnit *u)
{
    struct Smsc95xxBinding *b = u->binding;
    ULONG hash[2];
    BOOL allmulti = FALSE;
    struct Node *n;

    hash[0] = 0;
    hash[1] = 0;
    if (!b)
        return;

    for (n = u->mcasts.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct McRange *m = (struct McRange *)n;

        if (addr_equal(m->lo, m->hi)) {
            ULONG h = (eth_crc(m->lo, ETH_ALEN) >> 26) & 0x3F;

            hash[h >> 5] |= 1UL << (h & 31);
        } else {
            allmulti = TRUE;            /* a range cannot be hashed */
        }
    }
    (void)lan95_rx_mode(b, u->promisc_count > 0, allmulti, hash);
    u->rx_mode_dirty = FALSE;
}

/* Ask the unit's task to re-apply the receive mode (callable from any task). */
static void rx_mode_changed(struct Smsc95xxUnit *u)
{
    Forbid();
    if (u->attached && u->binding && u->binding->task) {
        u->rx_mode_dirty = TRUE;
        Signal(u->binding->task, sana_unit_sigmask(u));
    }
    Permit();
}

/* ------------------------------------------------------------------ */
/* Events                                                              */
/* ------------------------------------------------------------------ */

static void unit_event(struct Smsc95xxUnit *u, ULONG mask)
{
    struct Node *n, *next;

    Forbid();
    for (n = u->events.lh_Head; n->ln_Succ; n = next) {
        struct IOSana2Req *r = (struct IOSana2Req *)n;

        next = n->ln_Succ;
        if (r->ios2_WireError & mask) {
            Remove(n);
            r->ios2_WireError &= mask;
            Permit();
            req_done(r, 0, r->ios2_WireError);
            Forbid();
            next = u->events.lh_Head;   /* the list may have changed */
        }
    }
    Permit();
}

/* ------------------------------------------------------------------ */
/* Receive                                                             */
/* ------------------------------------------------------------------ */

static void fill_addresses(struct IOSana2Req *r, const UBYTE *frame)
{
    int i;

    for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) {
        r->ios2_DstAddr[i] = 0;
        r->ios2_SrcAddr[i] = 0;
    }
    addr_copy(r->ios2_DstAddr, frame);
    addr_copy(r->ios2_SrcAddr, frame + ETH_ALEN);
}

/* Hand a frame to a read request and complete it. */
static void deliver(struct Smsc95xxUnit *u, struct Opener *o,
                    struct IOSana2Req *r, const UBYTE *frame, ULONG len,
                    ULONG type, struct TypeStat *ts)
{
    BOOL raw = (r->ios2_Req.io_Flags & SANA2IOF_RAW) ? TRUE : FALSE;
    const UBYTE *data = raw ? frame : frame + ETH_HDR_LEN;
    ULONG n = raw ? len : len - ETH_HDR_LEN;

    r->ios2_Req.io_Flags &= ~(SANA2IOF_BCAST | SANA2IOF_MCAST);
    if (addr_is_broadcast(frame))
        r->ios2_Req.io_Flags |= SANA2IOF_BCAST;
    else if (addr_is_multicast(frame))
        r->ios2_Req.io_Flags |= SANA2IOF_MCAST;

    r->ios2_PacketType = type;
    fill_addresses(r, frame);
    r->ios2_DataLength = n;

    if (!op_copy_to(o, r->ios2_Data, data, n)) {
        u->stats.BadData++;
        if (ts)
            ts->st.PacketsDropped++;
        req_done(r, S2ERR_NO_RESOURCES, S2WERR_BUFF_ERROR);
        unit_event(u, S2EVENT_BUFF | S2EVENT_RX | S2EVENT_ERROR);
        return;
    }

    u->stats.PacketsReceived++;
    if (ts) {
        ts->st.PacketsReceived++;
        ts->st.BytesReceived += n;
    }
    req_ok(r);
}

/* Find (and take off its list) a read request for 'type': one that asked
 * for exactly this type first, then a raw one. */
static struct IOSana2Req *take_reader(struct Smsc95xxUnit *u, ULONG type,
                                      struct Opener **owner)
{
    struct Node *on, *rn;
    struct IOSana2Req *raw = NULL;
    struct Opener *raw_owner = NULL;

    for (on = u->openers.lh_Head; on->ln_Succ; on = on->ln_Succ) {
        struct Opener *o = (struct Opener *)on;

        for (rn = o->reads.lh_Head; rn->ln_Succ; rn = rn->ln_Succ) {
            struct IOSana2Req *r = (struct IOSana2Req *)rn;

            if (r->ios2_Req.io_Flags & SANA2IOF_RAW) {
                if (!raw) {
                    raw = r;
                    raw_owner = o;
                }
            } else if (r->ios2_PacketType == type) {
                Remove(rn);
                *owner = o;
                return r;
            }
        }
    }
    if (raw) {
        Remove(&raw->ios2_Req.io_Message.mn_Node);
        *owner = raw_owner;
    }
    return raw;
}

void sana_rx_bad(struct Smsc95xxUnit *u)
{
    u->stats.BadData++;
}

void sana_rx_frame(struct Smsc95xxUnit *u, const UBYTE *frame, ULONG len)
{
    struct IOSana2Req *r;
    struct Opener *o = NULL;
    struct TypeStat *ts;
    ULONG type;

    if (!u->online || len < ETH_HDR_LEN)
        return;

    type = ((ULONG)frame[12] << 8) | frame[13];
    ts = find_type(u, type);

    Forbid();
    r = take_reader(u, type, &o);
    Permit();

    if (r) {
        deliver(u, o, r, frame, len, type, ts);
        return;
    }

    /* Nobody wants it by type: a tracked type counts it as dropped; the
     * others go to a reader of orphans if there is one. */
    if (ts) {
        ts->st.PacketsDropped++;
        return;
    }
    Forbid();
    r = (struct IOSana2Req *)RemHead(&u->orphans);
    Permit();
    if (r)
        deliver(u, (struct Opener *)r->ios2_BufferManagement, r, frame, len,
                type, NULL);
    else
        u->stats.UnknownTypesReceived++;
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

/* Build the wire frame of a write request in the TX buffer. Returns its
 * length, or 0 and an error in *err / *wire. */
static ULONG build_frame(struct Smsc95xxUnit *u, struct IOSana2Req *r,
                         UBYTE *out, BYTE *err, ULONG *wire)
{
    struct Opener *o = (struct Opener *)r->ios2_BufferManagement;
    ULONG n = r->ios2_DataLength;

    if (r->ios2_Req.io_Flags & SANA2IOF_RAW) {
        if (n < ETH_HDR_LEN || n > ETH_RAW_MTU) {
            *err = (n < ETH_HDR_LEN) ? S2ERR_BAD_ARGUMENT : S2ERR_MTU_EXCEEDED;
            *wire = S2WERR_GENERIC_ERROR;
            return 0;
        }
        if (!op_copy_from(o, out, r->ios2_Data, n)) {
            *err = S2ERR_NO_RESOURCES;
            *wire = S2WERR_BUFF_ERROR;
            return 0;
        }
        return n;
    }

    if (n > ETH_MTU) {
        *err = S2ERR_MTU_EXCEEDED;
        *wire = S2WERR_GENERIC_ERROR;
        return 0;
    }
    if (r->ios2_Req.io_Command == S2_BROADCAST ||
        (r->ios2_Req.io_Flags & SANA2IOF_BCAST)) {
        int i;

        for (i = 0; i < ETH_ALEN; i++)
            out[i] = 0xFF;
    } else {
        addr_copy(out, r->ios2_DstAddr);
        if (r->ios2_Req.io_Command == S2_MULTICAST &&
            !addr_is_multicast(out)) {
            *err = S2ERR_BAD_ADDRESS;
            *wire = S2WERR_BAD_MULTICAST;
            return 0;
        }
    }
    addr_copy(out + ETH_ALEN, u->mac);
    out[12] = (UBYTE)(r->ios2_PacketType >> 8);
    out[13] = (UBYTE)r->ios2_PacketType;
    if (!op_copy_from(o, out + ETH_HDR_LEN, r->ios2_Data, n)) {
        *err = S2ERR_NO_RESOURCES;
        *wire = S2WERR_BUFF_ERROR;
        return 0;
    }
    return ETH_HDR_LEN + n;
}

void sana_tx_kick(struct Smsc95xxUnit *u)
{
    struct Smsc95xxBinding *b = u->binding;

    while (b && u->attached && !u->tx_cur) {
        struct IOSana2Req *r;
        UBYTE *frame;
        ULONG len, wire = 0;
        BYTE err = 0;

        Forbid();
        r = (struct IOSana2Req *)RemHead(&u->writes);
        Permit();
        if (!r)
            return;

        if (!(frame = data_tx_frame(b))) {
            Forbid();
            AddHead(&u->writes, &r->ios2_Req.io_Message.mn_Node);
            Permit();
            return;                     /* the pipe is busy: try again later */
        }
        len = build_frame(u, r, frame, &err, &wire);
        if (!len) {
            req_done(r, err, wire);
            unit_event(u, S2EVENT_BUFF | S2EVENT_TX | S2EVENT_ERROR);
            continue;
        }
        if (!data_tx_commit(b, len)) {
            req_done(r, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
            unit_event(u, S2EVENT_TX | S2EVENT_ERROR);
            continue;
        }
        u->tx_cur = r;
    }
}

void sana_tx_done(struct Smsc95xxUnit *u, LONG err)
{
    struct IOSana2Req *r = u->tx_cur;
    struct TypeStat *ts;

    if (!r)
        return;                         /* not one of ours (self-test) */
    u->tx_cur = NULL;

    if (err) {
        req_done(r, S2ERR_TX_FAILURE, S2WERR_GENERIC_ERROR);
        unit_event(u, S2EVENT_TX | S2EVENT_ERROR);
    } else {
        u->stats.PacketsSent++;
        ts = find_type(u, r->ios2_PacketType);
        if (ts && !(r->ios2_Req.io_Flags & SANA2IOF_RAW)) {
            ts->st.PacketsSent++;
            ts->st.BytesSent += r->ios2_DataLength;
        }
        req_ok(r);
    }
    sana_tx_kick(u);
}

/* ------------------------------------------------------------------ */
/* Commands                                                            */
/* ------------------------------------------------------------------ */

static void cmd_devicequery(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    struct Sana2DeviceQuery *dq = (struct Sana2DeviceQuery *)r->ios2_StatData;
    struct Sana2DeviceQuery tmp;
    ULONG avail, n, bps = 100000000UL;

    if (!dq) {
        req_done(r, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }
    if (u->binding && u->binding->link_up)
        bps = (ULONG)u->binding->speed * 1000000UL;

    avail = dq->SizeAvailable;
    tmp.SizeAvailable = sizeof(tmp);
    tmp.SizeSupplied = sizeof(tmp);
    tmp.DevQueryFormat = 0;
    tmp.DeviceLevel = 0;
    tmp.AddrFieldSize = ETH_ALEN * 8;
    tmp.MTU = ETH_MTU;
    tmp.BPS = bps;
    tmp.HardwareType = S2WireType_Ethernet;
    tmp.RawMTU = ETH_RAW_MTU;

    n = avail < sizeof(tmp) ? avail : sizeof(tmp);
    CopyMem(&tmp, dq, n);
    dq->SizeSupplied = n;
    dq->SizeAvailable = sizeof(tmp);
    req_ok(r);
}

static void cmd_getstationaddress(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    int i;

    for (i = 0; i < SANA2_MAX_ADDR_BYTES; i++) {
        r->ios2_SrcAddr[i] = 0;
        r->ios2_DstAddr[i] = 0;
    }
    addr_copy(r->ios2_SrcAddr, u->mac);
    addr_copy(r->ios2_DstAddr, u->rom_mac);
    req_ok(r);
}

static void go_online(struct Smsc95xxUnit *u)
{
    if (!u->online) {
        u->online = TRUE;
        unit_event(u, S2EVENT_ONLINE);
    }
}

/* Abort every pending read / orphan request and the queued writes. The lists
 * can also be edited by AbortIO and CloseDevice, so all of it is done under
 * Forbid(); ReplyMsg() only signals and is safe there. */
static void abort_queued(struct Smsc95xxUnit *u, BYTE err, ULONG wire)
{
    struct Node *on;
    struct IOSana2Req *r;

    Forbid();
    for (on = u->openers.lh_Head; on->ln_Succ; on = on->ln_Succ) {
        struct Opener *o = (struct Opener *)on;

        while ((r = (struct IOSana2Req *)RemHead(&o->reads)))
            req_done(r, err, wire);
    }
    while ((r = (struct IOSana2Req *)RemHead(&u->orphans)))
        req_done(r, err, wire);
    while ((r = (struct IOSana2Req *)RemHead(&u->writes)))
        req_done(r, err, wire);
    Permit();
}

static void cmd_configinterface(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    if (u->configured) {
        req_done(r, S2ERR_BAD_STATE, S2WERR_IS_CONFIGURED);
        return;
    }
    if (!addr_equal(r->ios2_SrcAddr, u->mac)) {
        if (addr_is_multicast(r->ios2_SrcAddr)) {
            req_done(r, S2ERR_BAD_ADDRESS, S2WERR_SRC_ADDRESS);
            return;
        }
        if (u->binding && !lan95_set_mac(u->binding, r->ios2_SrcAddr)) {
            req_done(r, S2ERR_SOFTWARE, S2WERR_GENERIC_ERROR);
            return;
        }
        addr_copy(u->mac, r->ios2_SrcAddr);
    }
    u->configured = TRUE;
    go_online(u);
    req_ok(r);
}

static void cmd_multicast(struct Smsc95xxUnit *u, struct IOSana2Req *r,
                          BOOL add, BOOL range)
{
    const UBYTE *lo = r->ios2_SrcAddr;
    const UBYTE *hi = range ? r->ios2_DstAddr : r->ios2_SrcAddr;
    struct Node *n;
    struct McRange *m = NULL;

    if (!addr_is_multicast(lo) || !addr_is_multicast(hi)) {
        req_done(r, S2ERR_BAD_ADDRESS, S2WERR_BAD_MULTICAST);
        return;
    }
    for (n = u->mcasts.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct McRange *c = (struct McRange *)n;

        if (addr_equal(c->lo, lo) && addr_equal(c->hi, hi)) {
            m = c;
            break;
        }
    }
    if (add) {
        if (m) {
            m->use++;
        } else {
            m = AllocVec(sizeof(*m), MEMF_PUBLIC | MEMF_CLEAR);
            if (!m) {
                req_done(r, S2ERR_NO_RESOURCES, S2WERR_MULTICAST_FULL);
                return;
            }
            addr_copy(m->lo, lo);
            addr_copy(m->hi, hi);
            m->use = 1;
            AddTail(&u->mcasts, &m->node);
        }
    } else {
        if (!m) {
            req_done(r, S2ERR_BAD_STATE, S2WERR_BAD_MULTICAST);
            return;
        }
        if (--m->use == 0) {
            Remove(&m->node);
            FreeVec(m);
        }
    }
    apply_rx_mode(u);
    req_ok(r);
}

static void cmd_tracktype(struct Smsc95xxUnit *u, struct IOSana2Req *r, BOOL track)
{
    struct TypeStat *ts = find_type(u, r->ios2_PacketType);

    if (track) {
        if (ts) {
            req_done(r, S2ERR_BAD_STATE, S2WERR_ALREADY_TRACKED);
            return;
        }
        ts = AllocVec(sizeof(*ts), MEMF_PUBLIC | MEMF_CLEAR);
        if (!ts) {
            req_done(r, S2ERR_NO_RESOURCES, S2WERR_GENERIC_ERROR);
            return;
        }
        ts->type = r->ios2_PacketType;
        AddTail(&u->types, &ts->node);
    } else {
        if (!ts) {
            req_done(r, S2ERR_BAD_STATE, S2WERR_NOT_TRACKED);
            return;
        }
        Remove(&ts->node);
        FreeVec(ts);
    }
    req_ok(r);
}

static void cmd_gettypestats(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    struct TypeStat *ts = find_type(u, r->ios2_PacketType);

    if (!ts) {
        req_done(r, S2ERR_BAD_STATE, S2WERR_NOT_TRACKED);
        return;
    }
    if (!r->ios2_StatData) {
        req_done(r, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }
    CopyMem(&ts->st, r->ios2_StatData, sizeof(ts->st));
    req_ok(r);
}

static void cmd_getspecialstats(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    struct Sana2SpecialStatHeader *h =
        (struct Sana2SpecialStatHeader *)r->ios2_StatData;
    struct Sana2SpecialStatRecord *rec;
    ULONG i, count;

    if (!h) {
        req_done(r, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        return;
    }
    rec = (struct Sana2SpecialStatRecord *)(h + 1);
    count = 0;
    for (i = 0; i < 3 && count < h->RecordCountMax; i++) {
        switch (i) {
        case 0:
            rec[count].Type = S2SS_ETHERNET_BADMULTICAST;
            rec[count].Count = u->bad_multicast;
            rec[count].String = (STRPTR)"Bad multicasts";
            break;
        case 1:
            rec[count].Type = S2SS_ETHERNET_RETRIES;
            rec[count].Count = 0;
            rec[count].String = (STRPTR)"Retries";
            break;
        default:
            rec[count].Type = S2SS_ETHERNET_FIFO_UNDERRUNS;
            rec[count].Count = 0;
            rec[count].String = (STRPTR)"FIFO underruns";
            break;
        }
        count++;
    }
    h->RecordCountSupplied = count;
    req_ok(r);
}

static void cmd_onevent(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    ULONG want = r->ios2_WireError;

    if (want & ~EVENTS_SUPPORTED) {
        req_done(r, S2ERR_NOT_SUPPORTED, S2WERR_BAD_EVENT);
        return;
    }
    /* A state that already holds completes the request at once. */
    if ((want & S2EVENT_ONLINE) && u->online) {
        req_done(r, 0, S2EVENT_ONLINE);
        return;
    }
    if ((want & S2EVENT_OFFLINE) && !u->online) {
        req_done(r, 0, S2EVENT_OFFLINE);
        return;
    }
    Forbid();
    AddTail(&u->events, &r->ios2_Req.io_Message.mn_Node);
    Permit();
}

static void cmd_nsquery(struct IOSana2Req *r)
{
    struct IOStdReq *sr = (struct IOStdReq *)r;
    struct NSDeviceQueryResult *q = (struct NSDeviceQueryResult *)sr->io_Data;

    if (!q || sr->io_Length < sizeof(*q)) {
        r->ios2_Req.io_Error = IOERR_BADLENGTH;
        ReplyMsg(&r->ios2_Req.io_Message);
        return;
    }
    q->nsdqr_DevQueryFormat = 0;
    q->nsdqr_SizeAvailable = sizeof(*q);
    q->nsdqr_DeviceType = NSDEVTYPE_SANA2;
    q->nsdqr_DeviceSubType = 0;
    q->nsdqr_SupportedCommands = (APTR)supported_cmds;
    sr->io_Actual = sizeof(*q);
    r->ios2_Req.io_Error = 0;
    ReplyMsg(&r->ios2_Req.io_Message);
}

/* Execute one request, in the task. */
static void handle_cmd(struct Smsc95xxUnit *u, struct IOSana2Req *r)
{
    struct Opener *o = (struct Opener *)r->ios2_BufferManagement;

    r->ios2_Req.io_Error = 0;
    r->ios2_WireError = (r->ios2_Req.io_Command == S2_ONEVENT) ?
                        r->ios2_WireError : 0;

    switch (r->ios2_Req.io_Command) {
    case NSCMD_DEVICEQUERY:
        cmd_nsquery(r);
        break;

    case CMD_READ:
        if (!u->online) {
            req_done(r, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        } else if (!o) {
            req_done(r, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        } else {
            Forbid();
            AddTail(&o->reads, &r->ios2_Req.io_Message.mn_Node);
            Permit();
        }
        break;

    case CMD_WRITE:
    case S2_BROADCAST:
    case S2_MULTICAST:
        if (!u->online) {
            req_done(r, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        } else {
            Forbid();
            AddTail(&u->writes, &r->ios2_Req.io_Message.mn_Node);
            Permit();
            sana_tx_kick(u);
        }
        break;

    case CMD_FLUSH:
        abort_queued(u, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
        req_ok(r);
        break;

    case S2_DEVICEQUERY:
        cmd_devicequery(u, r);
        break;
    case S2_GETSTATIONADDRESS:
        cmd_getstationaddress(u, r);
        break;
    case S2_CONFIGINTERFACE:
        cmd_configinterface(u, r);
        break;
    case S2_ADDMULTICASTADDRESS:
        cmd_multicast(u, r, TRUE, FALSE);
        break;
    case S2_DELMULTICASTADDRESS:
        cmd_multicast(u, r, FALSE, FALSE);
        break;
    case S2_ADDMULTICASTADDRESSES:
        cmd_multicast(u, r, TRUE, TRUE);
        break;
    case S2_DELMULTICASTADDRESSES:
        cmd_multicast(u, r, FALSE, TRUE);
        break;
    case S2_TRACKTYPE:
        cmd_tracktype(u, r, TRUE);
        break;
    case S2_UNTRACKTYPE:
        cmd_tracktype(u, r, FALSE);
        break;
    case S2_GETTYPESTATS:
        cmd_gettypestats(u, r);
        break;
    case S2_GETSPECIALSTATS:
        cmd_getspecialstats(u, r);
        break;
    case S2_GETGLOBALSTATS:
        if (!r->ios2_StatData) {
            req_done(r, S2ERR_BAD_ARGUMENT, S2WERR_NULL_POINTER);
        } else {
            CopyMem(&u->stats, r->ios2_StatData, sizeof(u->stats));
            req_ok(r);
        }
        break;
    case S2_ONEVENT:
        cmd_onevent(u, r);
        break;
    case S2_READORPHAN:
        if (!u->online) {
            req_done(r, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
        } else {
            Forbid();
            AddTail(&u->orphans, &r->ios2_Req.io_Message.mn_Node);
            Permit();
        }
        break;
    case S2_ONLINE:
        go_online(u);
        req_ok(r);
        break;
    case S2_OFFLINE:
        if (u->online) {
            u->online = FALSE;
            abort_queued(u, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
            unit_event(u, S2EVENT_OFFLINE);
        }
        req_ok(r);
        break;

    default:
        r->ios2_Req.io_Error = IOERR_NOCMD;
        r->ios2_WireError = S2WERR_GENERIC_ERROR;
        ReplyMsg(&r->ios2_Req.io_Message);
        break;
    }
}

void sana_handle_msgs(struct Smsc95xxUnit *u)
{
    struct Message *m;

    while ((m = GetMsg(&u->unit.unit_MsgPort)))
        handle_cmd(u, (struct IOSana2Req *)m);
}

void sana_housekeeping(struct Smsc95xxUnit *u)
{
    if (u->rx_mode_dirty)
        apply_rx_mode(u);
}

ULONG sana_unit_sigmask(struct Smsc95xxUnit *u)
{
    return 1UL << u->unit.unit_MsgPort.mp_SigBit;
}

/* ------------------------------------------------------------------ */
/* Units                                                               */
/* ------------------------------------------------------------------ */

struct Smsc95xxUnit *sana_unit_get(struct Smsc95xxDev *dev,
                                   struct Smsc95xxBinding *b)
{
    struct Smsc95xxUnit *u = NULL;
    struct Node *n;
    ULONG number = 0;

    (void)b;
    Forbid();
    for (n = dev->units.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct Smsc95xxUnit *c = (struct Smsc95xxUnit *)
            ((UBYTE *)n - offsetof(struct Smsc95xxUnit, node));

        if (!c->attached && !c->binding)
            u = c;                      /* reuse an orphan: stable unit number */
        number = c->number + 1;
    }
    Permit();
    if (u)
        return u;

    u = AllocVec(sizeof(*u), MEMF_PUBLIC | MEMF_CLEAR);
    if (!u)
        return NULL;
    u->dev = dev;
    u->number = number;
    new_list(&u->unit.unit_MsgPort.mp_MsgList);
    u->unit.unit_MsgPort.mp_Node.ln_Type = NT_MSGPORT;
    u->unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    new_list(&u->openers);
    new_list(&u->writes);
    new_list(&u->events);
    new_list(&u->orphans);
    new_list(&u->types);
    new_list(&u->mcasts);
    Forbid();
    AddTail(&dev->units, &u->node);
    Permit();
    return u;
}

/* Called in the binding's task, once the data path runs. */
BOOL sana_unit_attach(struct Smsc95xxUnit *u, struct Smsc95xxBinding *b)
{
    LONG sig = AllocSignal(-1);

    if (sig < 0)
        return FALSE;

    addr_copy(u->rom_mac, b->mac);
    if (!u->configured)
        addr_copy(u->mac, u->rom_mac);
    else if (!addr_equal(u->mac, b->mac))
        (void)lan95_set_mac(b, u->mac);    /* keep the address a stack was given */

    u->unit.unit_MsgPort.mp_SigBit = (UBYTE)sig;
    u->unit.unit_MsgPort.mp_SigTask = FindTask(NULL);
    u->unit.unit_MsgPort.mp_Flags = PA_SIGNAL;

    Forbid();
    u->binding = b;
    u->attached = TRUE;
    Permit();

    apply_rx_mode(u);
    return TRUE;
}

/* Called in the binding's task before its pipes go away: no more requests
 * are accepted and every pending one fails. */
void sana_unit_detach(struct Smsc95xxUnit *u)
{
    struct Message *m;

    Forbid();
    u->attached = FALSE;
    u->binding = NULL;
    u->unit.unit_MsgPort.mp_Flags = PA_IGNORE;
    Permit();

    while ((m = GetMsg(&u->unit.unit_MsgPort)))
        req_done((struct IOSana2Req *)m, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);

    if (u->tx_cur) {
        struct IOSana2Req *r = u->tx_cur;

        u->tx_cur = NULL;
        req_done(r, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    }
    abort_queued(u, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    for (;;) {
        struct IOSana2Req *r;

        Forbid();
        r = (struct IOSana2Req *)RemHead(&u->events);
        Permit();
        if (!r)
            break;
        req_done(r, S2ERR_OUTOFSERVICE, S2WERR_UNIT_OFFLINE);
    }
    u->online = FALSE;
    FreeSignal((LONG)u->unit.unit_MsgPort.mp_SigBit);
    u->unit.unit_MsgPort.mp_SigTask = NULL;
}

/* ------------------------------------------------------------------ */
/* Device vectors                                                      */
/* ------------------------------------------------------------------ */

static struct Smsc95xxUnit *find_unit(struct Smsc95xxDev *dev, ULONG number)
{
    struct Node *n;

    for (n = dev->units.lh_Head; n->ln_Succ; n = n->ln_Succ) {
        struct Smsc95xxUnit *u = (struct Smsc95xxUnit *)
            ((UBYTE *)n - offsetof(struct Smsc95xxUnit, node));

        if (u->number == number)
            return u;
    }
    return NULL;
}

static struct Smsc95xxDev *dev_open(
    struct IOSana2Req *req asm("a1"), ULONG unitno asm("d0"),
    ULONG flags asm("d1"), struct Smsc95xxDev *base asm("a6"));
static struct Smsc95xxDev *dev_open(
    struct IOSana2Req *req asm("a1"), ULONG unitno asm("d0"),
    ULONG flags asm("d1"), struct Smsc95xxDev *base asm("a6"))
{
    struct Smsc95xxUnit *u;
    struct Opener *o;
    struct TagItem *tags;

    /* Some programs leave mn_Length at zero; refuse only a wrong, non-zero one. */
    if (req->ios2_Req.io_Message.mn_Length &&
        req->ios2_Req.io_Message.mn_Length < sizeof(struct IOSana2Req)) {
        req->ios2_Req.io_Error = IOERR_BADLENGTH;
        goto fail;
    }

    Forbid();
    u = find_unit(base, unitno);
    if (!u) {
        Permit();
        req->ios2_Req.io_Error = IOERR_OPENFAIL;
        goto fail;
    }
    if (u->exclusive || ((flags & SANA2OPF_MINE) && u->unit.unit_OpenCnt)) {
        Permit();
        req->ios2_Req.io_Error = IOERR_UNITBUSY;
        goto fail;
    }
    Permit();

    o = AllocVec(sizeof(*o), MEMF_PUBLIC | MEMF_CLEAR);
    if (!o) {
        req->ios2_Req.io_Error = IOERR_OPENFAIL;
        goto fail;
    }
    new_list(&o->reads);
    tags = (struct TagItem *)req->ios2_BufferManagement;
    if (tags) {
        o->copy_to = (APTR)GetTagData(S2_CopyToBuff, 0, tags);
        o->copy_from = (APTR)GetTagData(S2_CopyFromBuff, 0, tags);
    }
    o->promisc = (flags & SANA2OPF_PROM) ? TRUE : FALSE;

    Forbid();
    if (flags & SANA2OPF_MINE)
        u->exclusive = TRUE;
    if (o->promisc)
        u->promisc_count++;
    AddTail(&u->openers, &o->node);
    u->unit.unit_OpenCnt++;
    base->lib.lib_OpenCnt++;
    base->lib.lib_Flags &= ~LIBF_DELEXP;
    Permit();

    if (o->promisc)
        rx_mode_changed(u);

    req->ios2_BufferManagement = o;
    req->ios2_Req.io_Unit = &u->unit;
    req->ios2_Req.io_Device = (struct Device *)base;
    req->ios2_Req.io_Message.mn_Node.ln_Type = NT_REPLYMSG;
    req->ios2_Req.io_Error = 0;
    return base;

fail:
    req->ios2_Req.io_Unit = (struct Unit *)-1;
    req->ios2_Req.io_Device = (struct Device *)-1;
    return NULL;
}

static BPTR dev_close(struct IOSana2Req *req asm("a1"),
                      struct Smsc95xxDev *base asm("a6"));
static BPTR dev_close(struct IOSana2Req *req asm("a1"),
                      struct Smsc95xxDev *base asm("a6"))
{
    struct Smsc95xxUnit *u = (struct Smsc95xxUnit *)req->ios2_Req.io_Unit;
    struct Opener *o = (struct Opener *)req->ios2_BufferManagement;

    if (u && u != (struct Smsc95xxUnit *)-1) {
        if (o) {
            struct IOSana2Req *r;
            BOOL promisc = o->promisc;

            for (;;) {
                Forbid();
                r = (struct IOSana2Req *)RemHead(&o->reads);
                Permit();
                if (!r)
                    break;
                req_done(r, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
            }
            Forbid();
            Remove(&o->node);
            if (promisc && u->promisc_count)
                u->promisc_count--;
            Permit();
            FreeVec(o);
            if (promisc)
                rx_mode_changed(u);
        }
        Forbid();
        if (u->unit.unit_OpenCnt)
            u->unit.unit_OpenCnt--;
        if (!u->unit.unit_OpenCnt)
            u->exclusive = FALSE;
        Permit();
    }

    req->ios2_Req.io_Unit = (struct Unit *)-1;
    req->ios2_Req.io_Device = (struct Device *)-1;
    req->ios2_BufferManagement = NULL;

    Forbid();
    if (base->lib.lib_OpenCnt)
        base->lib.lib_OpenCnt--;
    Permit();
    return 0;
}

/* The device belongs to the class: it goes when the class goes. */
static BPTR dev_expunge(struct Smsc95xxDev *base asm("a6"));
static BPTR dev_expunge(struct Smsc95xxDev *base asm("a6"))
{
    base->lib.lib_Flags |= LIBF_DELEXP;
    return 0;
}

static ULONG dev_reserved(void)
{
    return 0;
}

static void dev_beginio(struct IOSana2Req *req asm("a1"),
                        struct Smsc95xxDev *base asm("a6"));
static void dev_beginio(struct IOSana2Req *req asm("a1"),
                        struct Smsc95xxDev *base asm("a6"))
{
    struct Smsc95xxUnit *u = (struct Smsc95xxUnit *)req->ios2_Req.io_Unit;

    (void)base;
    req->ios2_Req.io_Error = 0;

    Forbid();
    if (u->attached) {
        req->ios2_Req.io_Flags &= ~IOF_QUICK;
        PutMsg(&u->unit.unit_MsgPort, &req->ios2_Req.io_Message);
        Permit();
        return;
    }
    Permit();

    /* The USB device is not there: fail at once. A quick request is simply
     * returned, any other is replied. */
    req->ios2_Req.io_Error = S2ERR_OUTOFSERVICE;
    req->ios2_WireError = S2WERR_UNIT_OFFLINE;
    if (!(req->ios2_Req.io_Flags & IOF_QUICK))
        ReplyMsg(&req->ios2_Req.io_Message);
}

static ULONG dev_abortio(struct IOSana2Req *req asm("a1"),
                         struct Smsc95xxDev *base asm("a6"));
static ULONG dev_abortio(struct IOSana2Req *req asm("a1"),
                         struct Smsc95xxDev *base asm("a6"))
{
    struct Smsc95xxUnit *u = (struct Smsc95xxUnit *)req->ios2_Req.io_Unit;
    struct Node *want = &req->ios2_Req.io_Message.mn_Node;
    struct Node *on;
    BOOL found = FALSE;

    (void)base;
    Forbid();
    if (want->ln_Type == NT_MESSAGE) {
        found = list_remove(&u->unit.unit_MsgPort.mp_MsgList, want) ||
                list_remove(&u->writes, want) ||
                list_remove(&u->orphans, want) ||
                list_remove(&u->events, want);
        for (on = u->openers.lh_Head; !found && on->ln_Succ; on = on->ln_Succ)
            found = list_remove(&((struct Opener *)on)->reads, want);
    }
    Permit();

    if (found)
        req_done(req, IOERR_ABORTED, S2WERR_GENERIC_ERROR);
    return 0;
}

static const APTR dev_funcs[] = {
    (APTR)dev_open,
    (APTR)dev_close,
    (APTR)dev_expunge,
    (APTR)dev_reserved,
    (APTR)dev_beginio,
    (APTR)dev_abortio,
    (APTR)-1
};

extern const char sana_dev_name[];
const char sana_dev_name[] = SMSC_DEVICE_NAME;
static const char dev_id_string[] = DEV_ID_STRING;

struct Smsc95xxDev *sana_dev_create(void)
{
    struct Smsc95xxDev *d;

    d = (struct Smsc95xxDev *)MakeLibrary((APTR)dev_funcs, NULL, NULL,
                                          sizeof(*d), 0);
    if (!d)
        return NULL;

    d->lib.lib_Node.ln_Type = NT_DEVICE;
    d->lib.lib_Node.ln_Name = (char *)sana_dev_name;
    d->lib.lib_Flags = LIBF_SUMUSED | LIBF_CHANGED;
    d->lib.lib_Version = CLASS_VERSION;
    d->lib.lib_Revision = CLASS_REVISION;
    d->lib.lib_IdString = (APTR)dev_id_string;
    new_list(&d->units);
    d->lib.lib_OpenCnt = 1;             /* the class's own reference */

    Forbid();
    AddDevice((struct Device *)d);
    Permit();
    return d;
}

/* Remove the device and every unit. Fails while anybody has it open. */
BOOL sana_dev_remove(struct Smsc95xxDev *d)
{
    struct Node *n;

    Forbid();
    if (d->lib.lib_OpenCnt > 1) {
        Permit();
        return FALSE;
    }
    RemDevice((struct Device *)d);
    Permit();

    while ((n = RemHead(&d->units))) {
        struct Smsc95xxUnit *u = (struct Smsc95xxUnit *)
            ((UBYTE *)n - offsetof(struct Smsc95xxUnit, node));
        struct Node *m;

        while ((m = RemHead(&u->types)))
            FreeVec(m);
        while ((m = RemHead(&u->mcasts)))
            FreeVec(m);
        FreeVec(u);
    }
    FreeMem((UBYTE *)d - d->lib.lib_NegSize,
            d->lib.lib_NegSize + d->lib.lib_PosSize);
    return TRUE;
}
