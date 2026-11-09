/* SPDX-License-Identifier: MPL-2.0 */
/*
 * smsc95xx_regs.h - hardware facts for the SMSC/Microchip LAN95xx USB
 * Ethernet controllers (LAN9500/9512/9514).
 *
 * Register offsets, bit positions and frame header layouts are facts about the
 * hardware. They were cross-checked against the Microchip LAN951x datasheet
 * and the BSD-licensed OpenBSD if_smscreg.h; names and organisation are this
 * project's own.
 */

#ifndef SMSC95XX_REGS_H
#define SMSC95XX_REGS_H

/* ------------------------------------------------------------------ */
/* USB identification                                                  */
/* ------------------------------------------------------------------ */

#define LAN95_VID_SMSC          0x0424
#define LAN95_PID_LAN9500       0x9500
#define LAN95_PID_LAN9512       0xEC00  /* LAN9512/9514: Raspberry Pi 3B */

/* ------------------------------------------------------------------ */
/* Vendor control requests (bmRequestType: vendor, device; wValue = 0) */
/* ------------------------------------------------------------------ */

#define LAN95_REQ_WRITE_REG     0xA0    /* host -> device, 4 bytes LE */
#define LAN95_REQ_READ_REG      0xA1    /* device -> host, 4 bytes LE */

/* ------------------------------------------------------------------ */
/* System control registers (wIndex of the vendor request)             */
/* ------------------------------------------------------------------ */

#define LAN95_ID_REV            0x000
#define LAN95_INT_STS           0x008
#define LAN95_RX_CFG            0x00C
#define LAN95_TX_CFG            0x010
#define LAN95_HW_CFG            0x014
#define LAN95_PM_CTRL           0x020
#define LAN95_LED_GPIO_CFG      0x024
#define LAN95_GPIO_CFG          0x028
#define LAN95_AFC_CFG           0x02C
#define LAN95_E2P_CMD           0x030
#define LAN95_E2P_DATA          0x034
#define LAN95_BURST_CAP         0x038
#define LAN95_INT_EP_CTL        0x068
#define LAN95_BULK_IN_DLY       0x06C

/* ------------------------------------------------------------------ */
/* MAC control and status registers                                    */
/* ------------------------------------------------------------------ */

#define LAN95_MAC_CR            0x100
#define LAN95_ADDRH             0x104
#define LAN95_ADDRL             0x108
#define LAN95_HASHH             0x10C
#define LAN95_HASHL             0x110
#define LAN95_MII_ADDR          0x114
#define LAN95_MII_DATA          0x118
#define LAN95_FLOW              0x11C
#define LAN95_COE_CR            0x130

/* ------------------------------------------------------------------ */
/* ID_REV                                                              */
/* ------------------------------------------------------------------ */

#define LAN95_ID_REV_CHIP_MASK  0xFFFF0000UL
#define LAN95_ID_REV_CHIP_SHIFT 16
#define LAN95_ID_REV_REV_MASK   0x0000FFFFUL

/* ------------------------------------------------------------------ */
/* HW_CFG                                                              */
/* ------------------------------------------------------------------ */

#define LAN95_HW_CFG_BIR        (1UL << 12) /* bulk-in empty response */
#define LAN95_HW_CFG_RXDOFF     (3UL << 9)  /* RX data offset, 2 bits */
#define LAN95_HW_CFG_MEF        (1UL << 5)  /* multiple ethernet frames/pkt */
#define LAN95_HW_CFG_LRST       (1UL << 3)  /* lite reset, self-clearing */
#define LAN95_HW_CFG_BCE        (1UL << 1)  /* burst cap enable */

/* ------------------------------------------------------------------ */
/* PM_CTRL                                                             */
/* ------------------------------------------------------------------ */

#define LAN95_PM_CTRL_PHY_RST   (1UL << 4)  /* PHY reset, self-clearing */

/* ------------------------------------------------------------------ */
/* RX_CFG / TX_CFG                                                     */
/* ------------------------------------------------------------------ */

#define LAN95_RX_FIFO_FLUSH     (1UL << 0)

#define LAN95_TX_CFG_ON         (1UL << 2)
#define LAN95_TX_CFG_STOP       (1UL << 1)
#define LAN95_TX_CFG_FIFO_FLUSH (1UL << 0)

/* ------------------------------------------------------------------ */
/* E2P_CMD (EEPROM access, used to read the burned-in MAC address)     */
/* ------------------------------------------------------------------ */

#define LAN95_E2P_CMD_BUSY      (1UL << 31)
#define LAN95_E2P_CMD_READ      (0UL << 28)
#define LAN95_E2P_CMD_TIMEOUT   (1UL << 10)
#define LAN95_E2P_CMD_ADDR_MASK 0x000001FFUL
#define LAN95_E2P_MAC_ADDR      0x01        /* MAC address offset in EEPROM */

/* ------------------------------------------------------------------ */
/* MAC_CR                                                              */
/* ------------------------------------------------------------------ */

#define LAN95_MAC_CR_RCVOWN     (1UL << 23) /* disable receive own (half dup) */
#define LAN95_MAC_CR_LOOPBK     (1UL << 21)
#define LAN95_MAC_CR_FDPX       (1UL << 20)
#define LAN95_MAC_CR_MCPAS      (1UL << 19) /* pass all multicast */
#define LAN95_MAC_CR_PRMS       (1UL << 18) /* promiscuous */
#define LAN95_MAC_CR_INVFILT    (1UL << 17)
#define LAN95_MAC_CR_PASSBAD    (1UL << 16)
#define LAN95_MAC_CR_HFILT      (1UL << 13) /* hash filter (multicast) */
#define LAN95_MAC_CR_BCAST      (1UL << 11) /* disable broadcast */
#define LAN95_MAC_CR_TXEN       (1UL << 3)
#define LAN95_MAC_CR_RXEN       (1UL << 2)

/* ------------------------------------------------------------------ */
/* MII_ADDR (PHY access through the MAC)                               */
/* ------------------------------------------------------------------ */

#define LAN95_MII_BUSY          (1UL << 0)
#define LAN95_MII_WRITE         (1UL << 1)
#define LAN95_MII_READ          (0UL << 1)
#define LAN95_MII_REG_SHIFT     6           /* PHY register index, bits 10:6 */
#define LAN95_MII_PHY_SHIFT     11          /* PHY address, bits 15:11 */

/* ------------------------------------------------------------------ */
/* LED_GPIO_CFG, AFC_CFG, FLOW                                         */
/* ------------------------------------------------------------------ */

#define LAN95_LED_GPIO_SPD_LED  (1UL << 24)
#define LAN95_LED_GPIO_LNK_LED  (1UL << 20)
#define LAN95_LED_GPIO_FDX_LED  (1UL << 16)

/* Default automatic flow control thresholds (high-speed USB). */
#define LAN95_AFC_CFG_DEFAULT   0x00F830A1UL
#define LAN95_AFC_CFG_BACKPRESSURE 0xFUL    /* half duplex: back pressure */

/* BULK_IN_DLY counts units of 16.667 ns (60 per microsecond): how long the
 * chip waits for more frames before it ends a bulk-in burst with a short
 * packet. 0x2000 = 136.5 us (the chip's reset value is 0x800 = 34.1 us). */
#define LAN95_BULK_IN_DLY_DEFAULT 0x00002000UL
#define LAN95_BULK_IN_DLY_PER_US  60UL
#define LAN95_BULK_IN_DLY_MAX     0x0000FFFFUL

/* ------------------------------------------------------------------ */
/* Internal PHY (MII address 1) - generic IEEE 802.3 registers         */
/* ------------------------------------------------------------------ */

#define LAN95_PHY_ADDR          1
#define MII_BMCR                0
#define MII_BMSR                1
#define MII_PHYSID1             2
#define MII_PHYSID2             3
#define MII_ADVERTISE           4
#define MII_LPA                 5

#define MII_BMCR_ANENABLE       0x1000
#define MII_BMCR_ANRESTART      0x0200
#define MII_BMSR_LSTATUS        0x0004
#define MII_BMSR_ANEGCOMPLETE   0x0020

#define MII_ADV_10HALF          0x0020
#define MII_ADV_10FULL          0x0040
#define MII_ADV_100HALF         0x0080
#define MII_ADV_100FULL         0x0100
#define MII_ADV_CSMA            0x0001  /* IEEE 802.3 selector field */
#define MII_ADV_ALL             (MII_ADV_10HALF | MII_ADV_10FULL |                                  MII_ADV_100HALF | MII_ADV_100FULL | MII_ADV_CSMA)

/* ------------------------------------------------------------------ */
/* COE_CR (hardware checksum offload - left disabled)                  */
/* ------------------------------------------------------------------ */

#define LAN95_COE_CR_TX_EN      (1UL << 16)
#define LAN95_COE_CR_RX_MODE    (1UL << 1)
#define LAN95_COE_CR_RX_EN      (1UL << 0)

/* ------------------------------------------------------------------ */
/* Internal PHY registers (via MII)                                    */
/* ------------------------------------------------------------------ */

#define LAN95_PHY_INT_SRC       29
#define LAN95_PHY_INT_MASK      30

#define LAN95_PHY_INT_ANEG_DONE (1U << 6)
#define LAN95_PHY_INT_LINK_DOWN (1U << 4)

/* ------------------------------------------------------------------ */
/* Frame headers                                                       */
/* ------------------------------------------------------------------ */

/*
 * TX: every Ethernet frame is preceded by two little-endian ULONGs.
 *   TX_A: bit 13 first segment, bit 12 last segment, bits 10:0 buffer size
 *   TX_B: bits 10:0 packet length
 */
#define LAN95_TX_HDR_SIZE       8
#define LAN95_TX_A_FIRST_SEG    (1UL << 13)
#define LAN95_TX_A_LAST_SEG     (1UL << 12)
#define LAN95_TX_LEN_MASK       0x000007FFUL

/*
 * RX: every received frame is preceded by one little-endian ULONG status
 * word; with MEF set, several frames may follow each other in one bulk-in
 * transfer, each padded to a 4-byte boundary.
 */
#define LAN95_RX_HDR_SIZE       4
#define LAN95_RX_LEN(s)         (((s) >> 16) & 0x3FFFUL)
#define LAN95_RX_MAX_LEN        1536    /* sanity limit of a frame, FCS included */
#define LAN95_RX_FILTER_FAIL    (1UL << 30)
#define LAN95_RX_ERROR          (1UL << 15)

#endif /* SMSC95XX_REGS_H */
