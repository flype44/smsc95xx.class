/* SPDX-License-Identifier: MPL-2.0 */
/*
 * smsc95xx.h - shared types for smsc95xx.class and smsc95xx.device.
 */

#ifndef SMSC95XX_H
#define SMSC95XX_H

#include <exec/types.h>
#include <exec/libraries.h>
#include <exec/devices.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <devices/timer.h>
#include <devices/sana2.h>

#define SMSC_DEVICE_NAME "smsc95xx.device"

/*
 * Bulk-in transfers kept in flight: ONE. dwc2usb.device has two bulk channels,
 * and a bulk IN with nothing to say holds its channel (the core retries NAKs
 * itself) until the driver takes it back after a 100 ms slice. More than one
 * pending IN would leave nothing free for our own transmit, or for another
 * device behind the same hub (a USB stick): every packet would wait for a slice.
 */
#define LAN95_RX_QUEUE   1
#define LAN95_BUF_SIZE   2048    /* an RX buffer in single-frame mode */
#define LAN95_RX_BUF_SIZE 18944  /* multi-frame RX buffer: 16 KiB + 5 * 512 */
#define LAN95_BURST_CAP_HS 37    /* LAN95_RX_BUF_SIZE / 512, high-speed packets */

/*
 * Transmit: the frames waiting when the pipe is free leave together, in ONE
 * bulk-out transfer (the chip always accepts several frames per transfer:
 * LAN950x data sheet DS00001875D, 5.3.1.2). Still only one transfer in flight:
 * dwc2usb does not serialise the transfers of one endpoint, so two of them
 * could leave in the wrong order, or on both bulk channels at once.
 * LAN95_TX_BATCH_MAX 1 gives back one frame per transfer (version 0.10).
 */
#define LAN95_TX_BUF_SIZE  16384
#define LAN95_TX_BATCH_MAX 16

#define ETH_ALEN         6
#define ETH_HDR_LEN      14
#define ETH_MTU          1500
#define ETH_RAW_MTU      (ETH_HDR_LEN + ETH_MTU)

struct Smsc95xxDev;
struct Smsc95xxUnit;

/* The class library base. */
struct Smsc95xxClass {
    struct Library      lib;
    BPTR                seglist;
    struct Smsc95xxDev *dev;        /* the SANA-II device this class provides */
};

/* ------------------------------------------------------------------ */
/* SANA-II device (sana.c)                                             */
/* ------------------------------------------------------------------ */

/* The device base. Created by the class at start-up, so that the device
 * exists as long as the class does; units come and go with USB bindings. */
struct Smsc95xxDev {
    struct Library      lib;
    BPTR                seglist;
    struct List         units;      /* struct Smsc95xxUnit, by unit number */
};

/* One opener (one OpenDevice): its buffer-management functions and its
 * pending CMD_READ requests. Kept in ios2_BufferManagement of its requests. */
struct Opener {
    struct Node         node;
    APTR                copy_to;    /* S2_CopyToBuff function or NULL     */
    APTR                copy_from;  /* S2_CopyFromBuff function or NULL   */
    BOOL                promisc;
    struct List         reads;      /* pending CMD_READ requests          */
};

struct McRange {
    struct Node         node;
    ULONG               use;
    UBYTE               lo[ETH_ALEN];
    UBYTE               hi[ETH_ALEN];
};

struct TypeStat {
    struct Node         node;
    ULONG               type;
    struct Sana2PacketTypeStats st;
};

/*
 * A unit is one network interface. It outlives its USB binding, so that
 * restarting Poseidon does not pull the device from under a running TCP/IP
 * stack: while 'attached' is FALSE every request fails at once.
 *
 * All commands are executed by the binding's task; BeginIO only hands the
 * request over, and AbortIO takes pending requests off the queues under
 * Forbid().
 */
struct Smsc95xxUnit {
    struct Unit         unit;       /* MUST be first; unit_MsgPort = command port */
    struct Node         node;       /* in Smsc95xxDev.units */
    struct Smsc95xxDev *dev;
    ULONG               number;
    struct Smsc95xxBinding *binding;
    BOOL                attached;   /* the task is running and listening  */
    BOOL                exclusive;  /* opened with SANA2OPF_MINE          */

    UBYTE               mac[ETH_ALEN];      /* current station address    */
    UBYTE               rom_mac[ETH_ALEN];  /* address of the board       */
    BOOL                configured;
    BOOL                online;
    ULONG               promisc_count;
    BOOL                rx_mode_dirty;

    struct List         openers;
    struct List         writes;     /* CMD_WRITE & co. waiting for the pipe */
    struct List         tx_wire;    /* requests in the transfer on the wire
                                     * (binding task only, not abortable) */
    struct List         events;     /* S2_ONEVENT                         */
    struct List         orphans;    /* S2_READORPHAN                      */
    struct List         types;      /* tracked packet types               */
    struct List         mcasts;     /* multicast ranges                   */

    struct Sana2DeviceStats stats;
    ULONG               bad_multicast;
};

/* ------------------------------------------------------------------ */
/* One binding: one LAN95xx USB device claimed by the class            */
/* ------------------------------------------------------------------ */

/*
 * Owned by the binding method that allocated it; the task only reads and
 * clears b->task and the ready fields under Forbid().
 */
struct Smsc95xxBinding {
    struct Smsc95xxClass *cls;
    APTR            device;     /* struct PsdDevice * (Poseidon-owned) */
    struct Smsc95xxUnit *unit;  /* the network interface of this device */

    struct Task    *task;       /* NULL when the task is not running  */
    struct Task    *ready_task; /* who waits for task start/stop      */
    BYTE            ready_sig;

    struct Library *ps;         /* poseidon.library, opened by task   */
    struct MsgPort *port;       /* reply port of our pipes            */
    APTR            ep0;        /* control pipe (struct PsdPipe *)    */
    UBYTE          *iobuf;      /* 16-byte DMA-able scratch buffer    */

    ULONG           chip_id;
    ULONG           chip_rev;
    UBYTE           mac[6];

    UBYTE           phy;        /* MII address of the internal PHY    */
    ULONG           mac_cr;     /* shadow of MAC_CR                   */
    BOOL            link_up;
    UWORD           speed;      /* 10 or 100 (Mbit/s)                 */
    BOOL            full_duplex;

    /* data path (data.c) */
    APTR            iface;      /* struct PsdInterface *              */
    APTR            ep_in;      /* bulk-in endpoint                   */
    APTR            ep_out;     /* bulk-out endpoint                  */
    APTR            rx_pipe[LAN95_RX_QUEUE];
    UBYTE          *rx_buf[LAN95_RX_QUEUE];
    BOOL            rx_armed[LAN95_RX_QUEUE];
    APTR            tx_pipe;
    UBYTE          *tx_buf;     /* LAN95_TX_BUF_SIZE bytes            */
    ULONG           tx_fill;    /* where the next frame's header goes */
    ULONG           tx_end;     /* end of the last frame packed       */
    UWORD           tx_count;   /* frames packed, not yet sent        */
    UWORD           tx_wire_count; /* frames in the transfer in flight */
    UBYTE          *buffers;    /* one allocation holding all of them */
    BOOL            running;
    BOOL            tx_busy;
    BOOL            multi;      /* several frames per bulk-in transfer */
    BOOL            no_bir;     /* test: do not set "bulk-in empty response" */
    ULONG           bulk_in_dly; /* BULK_IN_DLY register value        */
    ULONG           rx_len;     /* length of each bulk-in request     */
    ULONG           rx_empty;   /* bulk-in transfers that returned nothing */

    ULONG           rx_frames, rx_bytes, rx_errors, rx_err_run;
    ULONG           tx_frames, tx_xfers, tx_errors;

    /* timer and link/stat reporting (smsc95xx.class.c) */
    struct MsgPort     *timer_port;
    struct timerequest *timer_req;
    BOOL            timer_pending;
    BOOL            link_logged;
    BOOL            last_up, last_fdx;
    UWORD           last_speed;
    ULONG           ticks;
    ULONG           link_rx_seen; /* rx_frames at the last link check   */
    ULONG           stat_rx, stat_tx, stat_err;
    ULONG           poll_max_ms, poll_slow_logged;
    ULONG           selftest_ip;
    BOOL            selftest_wanted, selftest_sent;
};

/* smsc95xx.class.c */
void smsc_log(struct Library *ps, UWORD level, STRPTR fmt, ULONG *args);

/* lan95.c */
BOOL lan95_read_reg(struct Smsc95xxBinding *b, UWORD reg, ULONG *val);
BOOL lan95_write_reg(struct Smsc95xxBinding *b, UWORD reg, ULONG val);
BOOL lan95_mii_read(struct Smsc95xxBinding *b, UBYTE reg, UWORD *val);
BOOL lan95_mii_write(struct Smsc95xxBinding *b, UBYTE reg, UWORD val);
BOOL lan95_set_mac(struct Smsc95xxBinding *b, const UBYTE *mac);
BOOL lan95_get_mac(struct Smsc95xxBinding *b, UBYTE *mac);
int  lan95_chip_init(struct Smsc95xxBinding *b);
BOOL lan95_phy_start(struct Smsc95xxBinding *b);
BOOL lan95_link_update(struct Smsc95xxBinding *b);
BOOL lan95_mac_enable(struct Smsc95xxBinding *b);
void lan95_mac_disable(struct Smsc95xxBinding *b);
BOOL lan95_rx_mode(struct Smsc95xxBinding *b, BOOL promisc, BOOL allmulti,
                   const ULONG hash[2]);

/* data.c */
BOOL data_open(struct Smsc95xxBinding *b);
void data_start(struct Smsc95xxBinding *b);
void data_close(struct Smsc95xxBinding *b);
UBYTE *data_tx_slot(struct Smsc95xxBinding *b);
BOOL data_tx_add(struct Smsc95xxBinding *b, ULONG len);
BOOL data_tx_flush(struct Smsc95xxBinding *b);
BOOL data_send(struct Smsc95xxBinding *b, const UBYTE *frame, ULONG len);
BOOL data_send_arp_probe(struct Smsc95xxBinding *b, ULONG target_ip);
BOOL data_pipe_done(struct Smsc95xxBinding *b, APTR pipe);

/* platform.c */
#define PLAT_MAC_NONE           0
#define PLAT_MAC_FROM_DT        1
#define PLAT_MAC_FROM_MAILBOX   2
BOOL plat_mac_valid(const UBYTE *mac);
int  plat_get_mac(UBYTE *mac);

/* sana.c */
struct Smsc95xxDev *sana_dev_create(void);
BOOL sana_dev_remove(struct Smsc95xxDev *dev);
struct Smsc95xxUnit *sana_unit_get(struct Smsc95xxDev *dev,
                                   struct Smsc95xxBinding *b);
BOOL sana_unit_attach(struct Smsc95xxUnit *u, struct Smsc95xxBinding *b);
void sana_unit_detach(struct Smsc95xxUnit *u);
ULONG sana_unit_sigmask(struct Smsc95xxUnit *u);
void sana_handle_msgs(struct Smsc95xxUnit *u);
void sana_housekeeping(struct Smsc95xxUnit *u);
void sana_rx_frame(struct Smsc95xxUnit *u, const UBYTE *frame, ULONG len);
void sana_rx_bad(struct Smsc95xxUnit *u);
void sana_tx_kick(struct Smsc95xxUnit *u);
void sana_tx_done(struct Smsc95xxUnit *u, LONG err);

#endif /* SMSC95XX_H */
