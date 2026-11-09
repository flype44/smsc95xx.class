/* SPDX-License-Identifier: MPL-2.0 */
/*
 * data.c - bulk data path of the LAN95xx: endpoints, pipes, receive, transmit.
 *
 * Framing used (one Ethernet frame per bulk transfer, no burst mode):
 *
 *   RX bulk-in : [ RX status, 4 bytes LE ][ frame, FCS included ]
 *   TX bulk-out: [ TX cmd A, 4 bytes LE ][ TX cmd B, 4 bytes LE ][ frame ]
 *
 * The functions here run in the binding's task only; they are not re-entrant.
 * Milestone 4: received frames are only counted and a few are logged; there
 * is no SANA-II delivery yet.
 */

#include <exec/types.h>
#include <exec/memory.h>
#include <proto/exec.h>

#include "psd_api.h"
#include "smsc95xx_regs.h"
#include "smsc95xx.h"

#define PIPE_NAK_TIMEOUT_MS     5000
#define RX_ERR_LIMIT            20      /* consecutive errors before giving up */

static ULONG le32_get(const UBYTE *p)
{
    return (ULONG)p[0] | ((ULONG)p[1] << 8) | ((ULONG)p[2] << 16) |
           ((ULONG)p[3] << 24);
}

static void le32_put(UBYTE *p, ULONG v)
{
    p[0] = (UBYTE)v;
    p[1] = (UBYTE)(v >> 8);
    p[2] = (UBYTE)(v >> 16);
    p[3] = (UBYTE)(v >> 24);
}

/* ------------------------------------------------------------------ */
/* Setup and teardown                                                  */
/* ------------------------------------------------------------------ */

BOOL data_open(struct Smsc95xxBinding *b)
{
    struct Library *ps = b->ps;
    struct TagItem find[3], rxattr[4], txattr[4], none[1];
    ULONG args[2];
    int i;

    none[0].ti_Tag = TAG_END;
    none[0].ti_Data = 0;
    if (!(b->iface = psdFindInterfaceA(ps, b->device, NULL, none))) {
        smsc_log(ps, PSD_MSG_FAIL, "No USB interface found.", args);
        return FALSE;
    }

    find[0].ti_Tag = EA_IsIn;           find[0].ti_Data = TRUE;
    find[1].ti_Tag = EA_TransferType;   find[1].ti_Data = USEAF_BULK;
    find[2].ti_Tag = TAG_END;           find[2].ti_Data = 0;
    b->ep_in = psdFindEndpointA(ps, b->iface, NULL, find);
    find[0].ti_Data = FALSE;
    b->ep_out = psdFindEndpointA(ps, b->iface, NULL, find);
    if (!b->ep_in || !b->ep_out) {
        smsc_log(ps, PSD_MSG_FAIL, "Bulk IN or OUT endpoint missing.", args);
        return FALSE;
    }

    b->buffers = AllocVec((ULONG)LAN95_RX_QUEUE * LAN95_RX_BUF_SIZE +
                          LAN95_BUF_SIZE, MEMF_PUBLIC | MEMF_CLEAR);
    if (!b->buffers) {
        smsc_log(ps, PSD_MSG_FAIL, "Out of memory (data buffers).", args);
        return FALSE;
    }
    for (i = 0; i < LAN95_RX_QUEUE; i++)
        b->rx_buf[i] = b->buffers + (ULONG)i * LAN95_RX_BUF_SIZE;
    b->tx_buf = b->buffers + (ULONG)LAN95_RX_QUEUE * LAN95_RX_BUF_SIZE;
    b->rx_len = b->multi ? LAN95_RX_BUF_SIZE : LAN95_BUF_SIZE;

    /* Receive pipes wait for ever and accept short packets. */
    rxattr[0].ti_Tag = PPA_NakTimeout;          rxattr[0].ti_Data = FALSE;
    rxattr[1].ti_Tag = PPA_NakTimeoutTime;      rxattr[1].ti_Data = PIPE_NAK_TIMEOUT_MS;
    rxattr[2].ti_Tag = PPA_AllowRuntPackets;    rxattr[2].ti_Data = TRUE;
    rxattr[3].ti_Tag = TAG_END;                 rxattr[3].ti_Data = 0;

    /* The transmit pipe ends a transfer that is an exact multiple of the max
     * packet size with a zero-length packet (the chip needs it). */
    txattr[0].ti_Tag = PPA_NoZeroPktTerm;       txattr[0].ti_Data = FALSE;
    txattr[1].ti_Tag = PPA_NakTimeout;          txattr[1].ti_Data = TRUE;
    txattr[2].ti_Tag = PPA_NakTimeoutTime;      txattr[2].ti_Data = PIPE_NAK_TIMEOUT_MS;
    txattr[3].ti_Tag = TAG_END;                 txattr[3].ti_Data = 0;

    for (i = 0; i < LAN95_RX_QUEUE; i++) {
        if (!(b->rx_pipe[i] = psdAllocPipe(ps, b->device, b->port, b->ep_in))) {
            smsc_log(ps, PSD_MSG_FAIL, "Cannot allocate a receive pipe.", args);
            return FALSE;
        }
        psdSetAttrsA(ps, PGA_PIPE, b->rx_pipe[i], rxattr);
    }
    if (!(b->tx_pipe = psdAllocPipe(ps, b->device, b->port, b->ep_out))) {
        smsc_log(ps, PSD_MSG_FAIL, "Cannot allocate the transmit pipe.", args);
        return FALSE;
    }
    psdSetAttrsA(ps, PGA_PIPE, b->tx_pipe, txattr);
    return TRUE;
}

/* Start receiving: every receive pipe gets a buffer. */
void data_start(struct Smsc95xxBinding *b)
{
    int i;

    b->running = TRUE;
    for (i = 0; i < LAN95_RX_QUEUE; i++) {
        psdSendPipe(b->ps, b->rx_pipe[i], b->rx_buf[i], b->rx_len);
        b->rx_armed[i] = TRUE;
    }
}

/* Abort everything in flight and release the pipes and buffers. */
void data_close(struct Smsc95xxBinding *b)
{
    struct Library *ps = b->ps;
    int i;

    b->running = FALSE;
    for (i = 0; i < LAN95_RX_QUEUE; i++) {
        if (b->rx_pipe[i]) {
            if (b->rx_armed[i]) {
                psdAbortPipe(ps, b->rx_pipe[i]);
                psdWaitPipe(ps, b->rx_pipe[i]);
                b->rx_armed[i] = FALSE;
            }
            psdFreePipe(ps, b->rx_pipe[i]);
            b->rx_pipe[i] = NULL;
        }
    }
    if (b->tx_pipe) {
        if (b->tx_busy) {
            psdAbortPipe(ps, b->tx_pipe);
            psdWaitPipe(ps, b->tx_pipe);
            b->tx_busy = FALSE;
        }
        psdFreePipe(ps, b->tx_pipe);
        b->tx_pipe = NULL;
    }
    if (b->buffers) {
        FreeVec(b->buffers);
        b->buffers = NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Transmit                                                            */
/* ------------------------------------------------------------------ */

/*
 * Transmit in two steps, so the caller builds the frame straight into the
 * buffer: data_tx_frame() returns where the frame goes (or NULL while the
 * previous transfer is still on the wire), data_tx_commit() sends 'len' bytes.
 */
UBYTE *data_tx_frame(struct Smsc95xxBinding *b)
{
    if (!b->running || b->tx_busy)
        return NULL;
    return b->tx_buf + LAN95_TX_HDR_SIZE;
}

BOOL data_tx_commit(struct Smsc95xxBinding *b, ULONG len)
{
    if (!b->running || b->tx_busy || len == 0 ||
        len + LAN95_TX_HDR_SIZE > LAN95_BUF_SIZE)
        return FALSE;

    le32_put(b->tx_buf, LAN95_TX_A_FIRST_SEG | LAN95_TX_A_LAST_SEG |
                        (len & LAN95_TX_LEN_MASK));
    le32_put(b->tx_buf + 4, len & LAN95_TX_LEN_MASK);

    b->tx_busy = TRUE;
    psdSendPipe(b->ps, b->tx_pipe, b->tx_buf, len + LAN95_TX_HDR_SIZE);
    return TRUE;
}

/* Queue one frame. Returns FALSE if the transmitter is busy or the frame does
 * not fit. The frame is copied; the caller's buffer is free on return. */
BOOL data_send(struct Smsc95xxBinding *b, const UBYTE *frame, ULONG len)
{
    UBYTE *dst = data_tx_frame(b);
    ULONG i;

    if (!dst || len == 0 || len + LAN95_TX_HDR_SIZE > LAN95_BUF_SIZE)
        return FALSE;
    for (i = 0; i < len; i++)
        dst[i] = frame[i];
    return data_tx_commit(b, len);
}

/*
 * Self-test frame: an ARP probe (RFC 5227: sender protocol address 0.0.0.0)
 * for 'target_ip', broadcast. Harmless on a network; the owner of the address
 * answers with an ARP reply, which exercises both directions.
 */
BOOL data_send_arp_probe(struct Smsc95xxBinding *b, ULONG target_ip)
{
    UBYTE f[42];
    int i;

    for (i = 0; i < 42; i++)
        f[i] = 0;
    for (i = 0; i < 6; i++) {
        f[i] = 0xFF;                    /* destination: broadcast */
        f[6 + i] = b->mac[i];           /* source */
        f[22 + i] = b->mac[i];          /* ARP sender hardware address */
    }
    f[12] = 0x08; f[13] = 0x06;         /* EtherType ARP */
    f[14] = 0x00; f[15] = 0x01;         /* hardware: Ethernet */
    f[16] = 0x08; f[17] = 0x00;         /* protocol: IPv4 */
    f[18] = 6;    f[19] = 4;
    f[20] = 0x00; f[21] = 0x01;         /* operation: request */
    /* sender protocol address f[28..31] stays 0.0.0.0, target hw f[32..37] 0 */
    f[38] = (UBYTE)(target_ip >> 24);
    f[39] = (UBYTE)(target_ip >> 16);
    f[40] = (UBYTE)(target_ip >> 8);
    f[41] = (UBYTE)target_ip;
    return data_send(b, f, 42);
}

/* ------------------------------------------------------------------ */
/* Completion handling                                                 */
/* ------------------------------------------------------------------ */

static void rx_done(struct Smsc95xxBinding *b, int idx)
{
    struct Library *ps = b->ps;
    APTR pipe = b->rx_pipe[idx];
    UBYTE *buf = b->rx_buf[idx];
    LONG err = psdGetPipeError(ps, pipe);
    ULONG n = psdGetPipeActual(ps, pipe);
    ULONG args[3];

    b->rx_armed[idx] = FALSE;

    if (err) {
        b->rx_errors++;
        if (++b->rx_err_run == 1) {
            args[0] = (ULONG)err;
            smsc_log(ps, PSD_MSG_WARN, "Receive error %ld.", args);
        }
        if (b->rx_err_run >= RX_ERR_LIMIT) {
            smsc_log(ps, PSD_MSG_FAIL, "Too many receive errors, receiver stopped.", args);
            return;                     /* do not re-arm this pipe */
        }
        psdDelayMS(ps, 20);
    } else {
        ULONG pos = 0;

        b->rx_err_run = 0;
        if (n == 0)
            b->rx_empty++;              /* an empty answer to the IN token */

        /*
         * One or more frames, each: a status word (4 bytes), then the frame
         * with its FCS, then padding up to a multiple of 4 before the next
         * status word.
         */
        while (pos + LAN95_RX_HDR_SIZE <= n) {
            ULONG st = le32_get(buf + pos);
            ULONG len = LAN95_RX_LEN(st);
            ULONG padded = (len + 3) & ~3UL;

            if (len > LAN95_RX_MAX_LEN || pos + LAN95_RX_HDR_SIZE + len > n) {
                /* Out of step or cut short: nothing after this is trustworthy. */
                b->rx_errors++;
                if (b->unit)
                    sana_rx_bad(b->unit);
                break;
            }
            if ((st & LAN95_RX_ERROR) || len < 18) {
                b->rx_errors++;
                if (b->unit)
                    sana_rx_bad(b->unit);
            } else {
                /* The length includes the 4-byte FCS: drop it. */
                const UBYTE *frame = buf + pos + LAN95_RX_HDR_SIZE;
                ULONG flen = len - 4;

                b->rx_frames++;
                b->rx_bytes += flen;
                if (b->unit)
                    sana_rx_frame(b->unit, frame, flen);
            }
            pos += LAN95_RX_HDR_SIZE + padded;
        }
    }

    if (b->running) {
        psdSendPipe(ps, pipe, buf, b->rx_len);
        b->rx_armed[idx] = TRUE;
    }
}

static void tx_done(struct Smsc95xxBinding *b)
{
    LONG err = psdGetPipeError(b->ps, b->tx_pipe);
    ULONG args[2];

    b->tx_busy = FALSE;
    if (err) {
        b->tx_errors++;
        args[0] = (ULONG)err;
        smsc_log(b->ps, PSD_MSG_WARN, "Transmit error %ld.", args);
    } else {
        b->tx_frames++;
    }
    if (b->unit)
        sana_tx_done(b->unit, err);
}

/* A pipe message came back to the task's port: dispatch it. Returns FALSE if
 * the pipe is not one of the data pipes (so the caller may ignore it). */
BOOL data_pipe_done(struct Smsc95xxBinding *b, APTR pipe)
{
    int i;

    for (i = 0; i < LAN95_RX_QUEUE; i++) {
        if (pipe == b->rx_pipe[i]) {
            rx_done(b, i);
            return TRUE;
        }
    }
    if (pipe == b->tx_pipe) {
        tx_done(b);
        return TRUE;
    }
    return FALSE;
}
