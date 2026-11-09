/* SPDX-License-Identifier: MPL-2.0 */
/*
 * lan95.c - LAN95xx register access, MII, chip initialisation and link state.
 *
 * Everything goes through the control pipe with vendor requests 0xA0 (write
 * register) and 0xA1 (read register). Written from the register facts in
 * smsc95xx_regs.h; the order of the initialisation steps follows the reset
 * sequence documented by the BSD drivers.
 *
 * Milestone 3: the MAC transmitter and receiver are left OFF. They are only
 * enabled once the bulk data path exists.
 */

#include <exec/types.h>

#include "psd_api.h"
#include "smsc95xx_regs.h"
#include "smsc95xx.h"

#define WAIT_TRIES_SHORT        100     /* x 1 ms */
#define WAIT_TRIES_LONG         1000    /* x 1 ms */

/* ------------------------------------------------------------------ */
/* Registers                                                           */
/* ------------------------------------------------------------------ */

BOOL lan95_read_reg(struct Smsc95xxBinding *b, UWORD reg, ULONG *val)
{
    psdPipeSetup(b->ps, b->ep0, PSD_RT_IN_VENDOR_DEV, LAN95_REQ_READ_REG,
                 0, reg);
    if (psdDoPipe(b->ps, b->ep0, b->iobuf, 4))
        return FALSE;

    /* Registers are little-endian on the wire. */
    *val = (ULONG)b->iobuf[0] | ((ULONG)b->iobuf[1] << 8) |
           ((ULONG)b->iobuf[2] << 16) | ((ULONG)b->iobuf[3] << 24);
    return TRUE;
}

BOOL lan95_write_reg(struct Smsc95xxBinding *b, UWORD reg, ULONG val)
{
    b->iobuf[0] = (UBYTE)val;
    b->iobuf[1] = (UBYTE)(val >> 8);
    b->iobuf[2] = (UBYTE)(val >> 16);
    b->iobuf[3] = (UBYTE)(val >> 24);

    psdPipeSetup(b->ps, b->ep0, PSD_RT_OUT_VENDOR_DEV, LAN95_REQ_WRITE_REG,
                 0, reg);
    return psdDoPipe(b->ps, b->ep0, b->iobuf, 4) == 0;
}

/* Wait until none of 'bits' is set in 'reg'. */
static BOOL lan95_wait_clear(struct Smsc95xxBinding *b, UWORD reg, ULONG bits,
                             ULONG tries)
{
    ULONG val;

    while (tries--) {
        if (!lan95_read_reg(b, reg, &val))
            return FALSE;
        if (!(val & bits))
            return TRUE;
        psdDelayMS(b->ps, 1);
    }
    return FALSE;
}

static BOOL lan95_modify(struct Smsc95xxBinding *b, UWORD reg, ULONG set,
                         ULONG clear)
{
    ULONG val;

    if (!lan95_read_reg(b, reg, &val))
        return FALSE;
    return lan95_write_reg(b, reg, (val & ~clear) | set);
}

/* ------------------------------------------------------------------ */
/* MII (PHY access through the MAC)                                    */
/* ------------------------------------------------------------------ */

BOOL lan95_mii_read(struct Smsc95xxBinding *b, UBYTE reg, UWORD *val)
{
    ULONG data;

    if (!lan95_wait_clear(b, LAN95_MII_ADDR, LAN95_MII_BUSY, WAIT_TRIES_SHORT))
        return FALSE;
    if (!lan95_write_reg(b, LAN95_MII_ADDR,
                         ((ULONG)b->phy << LAN95_MII_PHY_SHIFT) |
                         ((ULONG)reg << LAN95_MII_REG_SHIFT) | LAN95_MII_READ))
        return FALSE;
    if (!lan95_wait_clear(b, LAN95_MII_ADDR, LAN95_MII_BUSY, WAIT_TRIES_SHORT))
        return FALSE;
    if (!lan95_read_reg(b, LAN95_MII_DATA, &data))
        return FALSE;
    *val = (UWORD)data;
    return TRUE;
}

BOOL lan95_mii_write(struct Smsc95xxBinding *b, UBYTE reg, UWORD val)
{
    if (!lan95_wait_clear(b, LAN95_MII_ADDR, LAN95_MII_BUSY, WAIT_TRIES_SHORT))
        return FALSE;
    if (!lan95_write_reg(b, LAN95_MII_DATA, val))
        return FALSE;
    if (!lan95_write_reg(b, LAN95_MII_ADDR,
                         ((ULONG)b->phy << LAN95_MII_PHY_SHIFT) |
                         ((ULONG)reg << LAN95_MII_REG_SHIFT) | LAN95_MII_WRITE))
        return FALSE;
    return lan95_wait_clear(b, LAN95_MII_ADDR, LAN95_MII_BUSY, WAIT_TRIES_SHORT);
}

/* ------------------------------------------------------------------ */
/* MAC address                                                         */
/* ------------------------------------------------------------------ */

BOOL lan95_set_mac(struct Smsc95xxBinding *b, const UBYTE *mac)
{
    ULONG lo = (ULONG)mac[0] | ((ULONG)mac[1] << 8) |
               ((ULONG)mac[2] << 16) | ((ULONG)mac[3] << 24);
    ULONG hi = (ULONG)mac[4] | ((ULONG)mac[5] << 8);

    return lan95_write_reg(b, LAN95_ADDRL, lo) &&
           lan95_write_reg(b, LAN95_ADDRH, hi);
}

BOOL lan95_get_mac(struct Smsc95xxBinding *b, UBYTE *mac)
{
    ULONG lo, hi;

    if (!lan95_read_reg(b, LAN95_ADDRL, &lo) ||
        !lan95_read_reg(b, LAN95_ADDRH, &hi))
        return FALSE;
    mac[0] = (UBYTE)lo;
    mac[1] = (UBYTE)(lo >> 8);
    mac[2] = (UBYTE)(lo >> 16);
    mac[3] = (UBYTE)(lo >> 24);
    mac[4] = (UBYTE)hi;
    mac[5] = (UBYTE)(hi >> 8);
    return TRUE;
}

/* ------------------------------------------------------------------ */
/* Chip initialisation                                                 */
/* ------------------------------------------------------------------ */

/*
 * Returns 0 on success, or the number of the step that failed (for the log).
 */
int lan95_chip_init(struct Smsc95xxBinding *b)
{
    /* 1. Lite reset of the MAC/FIFOs; the bit clears itself. */
    if (!lan95_write_reg(b, LAN95_HW_CFG, LAN95_HW_CFG_LRST))
        return 1;
    if (!lan95_wait_clear(b, LAN95_HW_CFG, LAN95_HW_CFG_LRST, WAIT_TRIES_LONG))
        return 2;

    /* 2. Reset the internal PHY; the bit clears itself. */
    if (!lan95_write_reg(b, LAN95_PM_CTRL, LAN95_PM_CTRL_PHY_RST))
        return 3;
    if (!lan95_wait_clear(b, LAN95_PM_CTRL, LAN95_PM_CTRL_PHY_RST, WAIT_TRIES_LONG))
        return 4;
    psdDelayMS(b->ps, 40);

    /* 3. Station address. */
    if (!lan95_set_mac(b, b->mac))
        return 5;

    /* 4. Receive format. Several Ethernet frames may share one bulk-in
     *    transfer ("multiple ethernet frames", with a burst capacity of
     *    LAN95_BURST_CAP_HS high-speed packets), which saves one complete
     *    USB round trip per frame; or one frame per transfer. */
    {
        ULONG all = LAN95_HW_CFG_BIR | LAN95_HW_CFG_MEF | LAN95_HW_CFG_BCE;
        ULONG set = b->no_bir ? 0 : LAN95_HW_CFG_BIR;

        if (b->multi)
            set |= LAN95_HW_CFG_MEF | LAN95_HW_CFG_BCE;
        if (!lan95_modify(b, LAN95_HW_CFG, set, all))
            return 6;
    }
    if (!lan95_write_reg(b, LAN95_BURST_CAP, b->multi ? LAN95_BURST_CAP_HS : 0))
        return 7;
    if (!lan95_write_reg(b, LAN95_BULK_IN_DLY, LAN95_BULK_IN_DLY_DEFAULT))
        return 8;

    /* 5. Acknowledge any pending interrupt status. */
    if (!lan95_write_reg(b, LAN95_INT_STS, 0xFFFFFFFFUL))
        return 9;

    /* 6. LEDs, flow control, and no checksum offload. */
    if (!lan95_write_reg(b, LAN95_LED_GPIO_CFG,
                         LAN95_LED_GPIO_SPD_LED | LAN95_LED_GPIO_LNK_LED |
                         LAN95_LED_GPIO_FDX_LED))
        return 10;
    if (!lan95_write_reg(b, LAN95_FLOW, 0))
        return 11;
    if (!lan95_write_reg(b, LAN95_AFC_CFG, LAN95_AFC_CFG_DEFAULT))
        return 12;
    if (!lan95_write_reg(b, LAN95_COE_CR, 0))
        return 13;

    /* 7. Remember the MAC configuration; TX and RX stay disabled. */
    if (!lan95_read_reg(b, LAN95_MAC_CR, &b->mac_cr))
        return 14;
    b->mac_cr &= ~(LAN95_MAC_CR_TXEN | LAN95_MAC_CR_RXEN);
    if (!lan95_write_reg(b, LAN95_MAC_CR, b->mac_cr))
        return 15;

    return 0;
}

/* ------------------------------------------------------------------ */
/* PHY and link                                                        */
/* ------------------------------------------------------------------ */

/* Advertise 10/100 half and full duplex and start auto-negotiation. */
BOOL lan95_phy_start(struct Smsc95xxBinding *b)
{
    return lan95_mii_write(b, MII_ADVERTISE, MII_ADV_ALL) &&
           lan95_mii_write(b, MII_BMCR, MII_BMCR_ANENABLE | MII_BMCR_ANRESTART);
}

/*
 * Poll the link. This runs every half second in the task that also serves
 * the network, and each MII access costs several control transfers, so the
 * steady state must be cheap: ONE read of BMSR while the link stays up.
 * The speed and duplex are resolved (and programmed into the MAC) only when
 * the link comes up or has bounced since the last poll.
 *
 * BMSR latches "link down": a first read showing no link may be stale, so it
 * is read again for the current state; a latched drop forces a new resolve.
 *
 * Fills b->link_up / b->speed / b->full_duplex. Returns FALSE if the chip
 * could not be read (USB error).
 */
BOOL lan95_link_update(struct Smsc95xxBinding *b)
{
    UWORD bmsr, lpa, adv, common;
    BOOL bounced = FALSE;

    if (!lan95_mii_read(b, MII_BMSR, &bmsr))
        return FALSE;
    if (!(bmsr & MII_BMSR_LSTATUS)) {
        bounced = TRUE;
        if (!lan95_mii_read(b, MII_BMSR, &bmsr))
            return FALSE;
    }

    if (!(bmsr & MII_BMSR_LSTATUS) || !(bmsr & MII_BMSR_ANEGCOMPLETE)) {
        b->link_up = FALSE;
        return TRUE;
    }
    if (b->link_up && !bounced)
        return TRUE;                    /* still up, nothing changed */

    if (!lan95_mii_read(b, MII_LPA, &lpa) ||
        !lan95_mii_read(b, MII_ADVERTISE, &adv))
        return FALSE;

    common = lpa & adv;
    if (common & MII_ADV_100FULL) {
        b->speed = 100; b->full_duplex = TRUE;
    } else if (common & MII_ADV_100HALF) {
        b->speed = 100; b->full_duplex = FALSE;
    } else if (common & MII_ADV_10FULL) {
        b->speed = 10;  b->full_duplex = TRUE;
    } else {
        b->speed = 10;  b->full_duplex = FALSE;
    }
    b->link_up = TRUE;

    if (b->full_duplex) {
        b->mac_cr &= ~LAN95_MAC_CR_RCVOWN;
        b->mac_cr |= LAN95_MAC_CR_FDPX;
        if (!lan95_modify(b, LAN95_AFC_CFG, 0, LAN95_AFC_CFG_BACKPRESSURE))
            return FALSE;
    } else {
        b->mac_cr &= ~LAN95_MAC_CR_FDPX;
        b->mac_cr |= LAN95_MAC_CR_RCVOWN;
        if (!lan95_modify(b, LAN95_AFC_CFG, LAN95_AFC_CFG_BACKPRESSURE, 0))
            return FALSE;
    }
    return lan95_write_reg(b, LAN95_MAC_CR, b->mac_cr);
}

/* ------------------------------------------------------------------ */
/* Transmitter and receiver                                            */
/* ------------------------------------------------------------------ */

/* Turn on the MAC transmitter, then the receiver. */
BOOL lan95_mac_enable(struct Smsc95xxBinding *b)
{
    b->mac_cr |= LAN95_MAC_CR_TXEN;
    if (!lan95_write_reg(b, LAN95_MAC_CR, b->mac_cr))
        return FALSE;
    if (!lan95_write_reg(b, LAN95_TX_CFG, LAN95_TX_CFG_ON))
        return FALSE;
    b->mac_cr |= LAN95_MAC_CR_RXEN;
    return lan95_write_reg(b, LAN95_MAC_CR, b->mac_cr);
}

/* Receiver first, then transmitter. Errors are ignored: this is teardown. */
void lan95_mac_disable(struct Smsc95xxBinding *b)
{
    b->mac_cr &= ~(LAN95_MAC_CR_RXEN | LAN95_MAC_CR_TXEN);
    (void)lan95_write_reg(b, LAN95_MAC_CR, b->mac_cr);
    (void)lan95_write_reg(b, LAN95_TX_CFG, LAN95_TX_CFG_STOP);
}

/*
 * Receive filter: promiscuous (pass everything), "all multicast" (also used
 * when a multicast range cannot be hashed), or the 64-bit multicast hash
 * table. Broadcast and the station address always pass.
 */
BOOL lan95_rx_mode(struct Smsc95xxBinding *b, BOOL promisc, BOOL allmulti,
                   const ULONG hash[2])
{
    b->mac_cr &= ~(LAN95_MAC_CR_HFILT | LAN95_MAC_CR_MCPAS | LAN95_MAC_CR_PRMS);
    if (promisc)
        b->mac_cr |= LAN95_MAC_CR_PRMS | LAN95_MAC_CR_MCPAS;
    else if (allmulti)
        b->mac_cr |= LAN95_MAC_CR_MCPAS;
    else
        b->mac_cr |= LAN95_MAC_CR_HFILT;

    return lan95_write_reg(b, LAN95_HASHH, hash[1]) &&
           lan95_write_reg(b, LAN95_HASHL, hash[0]) &&
           lan95_write_reg(b, LAN95_MAC_CR, b->mac_cr);
}
