// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2012-2015, 2017, 2021, The Linux Foundation. All rights reserved.
 */
#include <linux/bitmap.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/irq.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/spmi.h>

/* PMIC Arbiter configuration registers */
#define PMIC_ARB_VERSION		0x0000
#define PMIC_ARB_VERSION_V2_MIN		0x20010000
#define PMIC_ARB_VERSION_V3_MIN		0x30000000
#define PMIC_ARB_VERSION_V5_MIN		0x50000000
#define PMIC_ARB_VERSION_V7_MIN		0x70000000
#define PMIC_ARB_INT_EN			0x0004

#define PMIC_ARB_FEATURES		0x0004
#define PMIC_ARB_FEATURES_PERIPH_MASK	GENMASK(10, 0)

#define PMIC_ARB_FEATURES1		0x0008

/* PMIC Arbiter channel registers offsets */
#define PMIC_ARB_CMD			0x00
#define PMIC_ARB_CONFIG			0x04
#define PMIC_ARB_STATUS			0x08
#define PMIC_ARB_WDATA0			0x10
#define PMIC_ARB_WDATA1			0x14
#define PMIC_ARB_RDATA0			0x18
#define PMIC_ARB_RDATA1			0x1C

/* Mapping Table */
#define SPMI_MAPPING_TABLE_REG(N)	(0x0B00 + (4 * (N)))
#define SPMI_MAPPING_BIT_INDEX(X)	(((X) >> 18) & 0xF)
#define SPMI_MAPPING_BIT_IS_0_FLAG(X)	(((X) >> 17) & 0x1)
#define SPMI_MAPPING_BIT_IS_0_RESULT(X)	(((X) >> 9) & 0xFF)
#define SPMI_MAPPING_BIT_IS_1_FLAG(X)	(((X) >> 8) & 0x1)
#define SPMI_MAPPING_BIT_IS_1_RESULT(X)	(((X) >> 0) & 0xFF)

#define SPMI_MAPPING_TABLE_TREE_DEPTH	16	/* Maximum of 16-bits */
#define PMIC_ARB_MAX_PPID		BIT(12) /* PPID is 12bit */
#define PMIC_ARB_APID_VALID		BIT(15)
#define PMIC_ARB_CHAN_IS_IRQ_OWNER(reg)	((reg) & BIT(24))
#define INVALID_EE				0xFF

/* Ownership Table */
#define SPMI_OWNERSHIP_PERIPH2OWNER(X)	((X) & 0x7)

/* Channel Status fields */
enum pmic_arb_chnl_status {
	PMIC_ARB_STATUS_DONE	= BIT(0),
	PMIC_ARB_STATUS_FAILURE	= BIT(1),
	PMIC_ARB_STATUS_DENIED	= BIT(2),
	PMIC_ARB_STATUS_DROPPED	= BIT(3),
};

/* Command register fields */
#define PMIC_ARB_CMD_MAX_BYTE_COUNT	8

/* Command Opcodes */
enum pmic_arb_cmd_op_code {
	PMIC_ARB_OP_EXT_WRITEL = 0,
	PMIC_ARB_OP_EXT_READL = 1,
	PMIC_ARB_OP_EXT_WRITE = 2,
	PMIC_ARB_OP_RESET = 3,
	PMIC_ARB_OP_SLEEP = 4,
	PMIC_ARB_OP_SHUTDOWN = 5,
	PMIC_ARB_OP_WAKEUP = 6,
	PMIC_ARB_OP_AUTHENTICATE = 7,
	PMIC_ARB_OP_MSTR_READ = 8,
	PMIC_ARB_OP_MSTR_WRITE = 9,
	PMIC_ARB_OP_EXT_READ = 13,
	PMIC_ARB_OP_WRITE = 14,
	PMIC_ARB_OP_READ = 15,
	PMIC_ARB_OP_ZERO_WRITE = 16,
};

/*
 * PMIC arbiter version 5 uses different register offsets for read/write vs
 * observer channels.
 */
enum pmic_arb_channel {
	PMIC_ARB_CHANNEL_RW,
	PMIC_ARB_CHANNEL_OBS,
};

#define PMIC_ARB_MAX_BUSES		2

/* Maximum number of support PMIC peripherals */
#define PMIC_ARB_MAX_PERIPHS		512
#define PMIC_ARB_MAX_PERIPHS_V7		1024
#define PMIC_ARB_TIMEOUT_US		1000
#define PMIC_ARB_MAX_TRANS_BYTES	(8)

#define PMIC_ARB_APID_MASK		0xFF
#define PMIC_ARB_PPID_MASK		0xFFF

/* interrupt enable bit */
#define SPMI_PIC_ACC_ENABLE_BIT		BIT(0)

#define spec_to_hwirq(slave_id, periph_id, irq_id, apid) \
	((((slave_id) & 0xF)   << 28) | \
	(((periph_id) & 0xFF)  << 20) | \
	(((irq_id)    & 0x7)   << 16) | \
	(((apid)      & 0x3FF) << 0))

#define hwirq_to_sid(hwirq)  (((hwirq) >> 28) & 0xF)
#define hwirq_to_per(hwirq)  (((hwirq) >> 20) & 0xFF)
#define hwirq_to_irq(hwirq)  (((hwirq) >> 16) & 0x7)
#define hwirq_to_apid(hwirq) (((hwirq) >> 0)  & 0x3FF)

struct pmic_arb_ver_ops;

struct apid_data {
	u16		ppid;
	u8		write_ee;
	u8		irq_ee;
};

struct spmi_pmic_arb;

/**
 * struct spmi_pmic_arb_bus - SPMI PMIC Arbiter Bus object
 *
 * @pmic_arb:		the SPMI PMIC Arbiter the bus belongs to.
 * @domain:		irq domain object for PMIC IRQ domain
 * @intr:		address of the SPMI interrupt control registers.
 * @cnfg:		address of the PMIC Arbiter configuration registers.
 * @spmic:		spmi controller registered for this bus
 * @lock:		lock to synchronize accesses.
 * @base_apid:		on v7: minimum APID associated with the particular SPMI
 *			bus instance
 * @apid_count:		on v5 and v7: number of APIDs associated with the
 *			particular SPMI bus instance
 * @mapping_table:	in-memory copy of PPID -> APID mapping table.
 * @mapping_table_valid:bitmap containing valid-only periphs
 * @ppid_to_apid:	in-memory copy of PPID -> APID mapping table.
 * @last_apid:		Highest value APID in use
 * @apid_data:		Table of data for all APIDs
 * @min_apid:		minimum APID (used for bounding IRQ search)
 * @max_apid:		maximum APID
 * @irq:		PMIC ARB interrupt.
 * @id:			unique ID of the bus
 */
struct spmi_pmic_arb_bus {
	struct spmi_pmic_arb	*pmic_arb;
	struct irq_domain	*domain;
	void __iomem		*intr;
	void __iomem		*cnfg;
	struct spmi_controller	*spmic;
	raw_spinlock_t		lock;
	u16			base_apid;
	int			apid_count;
	u32			*mapping_table;
	DECLARE_BITMAP(mapping_table_valid, PMIC_ARB_MAX_PERIPHS);
	u16			*ppid_to_apid;
	u16			last_apid;
	struct apid_data	*apid_data;
	u16			min_apid;
	u16			max_apid;
	int			irq;
	u8			id;
};

/**
 * struct spmi_pmic_arb - SPMI PMIC Arbiter object
 *
 * @rd_base:		on v1 "core", on v2 "observer" register base off DT.
 * @wr_base:		on v1 "core", on v2 "chnls"    register base off DT.
 * @core:		core register base for v2 and above only (see above)
 * @core_size:		core register base size
 * @channel:		execution environment channel to use for accesses.
 * @ee:			the current Execution Environment
 * @ver_ops:		version dependent operations.
 * @max_periphs:	Number of elements in apid_data[]
 * @buses:		per arbiter buses instances
 * @buses_available:	number of buses registered
 */
struct spmi_pmic_arb {
	void __iomem		*rd_base;
	void __iomem		*wr_base;
	void __iomem		*core;
	resource_size_t		core_size;
	u8			channel;
	u8			ee;
	const struct pmic_arb_ver_ops *ver_ops;
	int			max_periphs;
	struct spmi_pmic_arb_bus *buses[PMIC_ARB_MAX_BUSES];
	int			buses_available;
};

/**
 * struct pmic_arb_ver_ops - version dependent functionality.
 *
 * @ver_str:		version string.
 * @get_core_resources:	initializes the core, observer and channels
 * @init_apid:		finds the apid base and count
 * @ppid_to_apid:	finds the apid for a given ppid.
 * @non_data_cmd:	on v1 issues an spmi non-data command.
 *			on v2 no HW support, returns -EOPNOTSUPP.
 * @offset:		on v1 offset of per-ee channel.
 *			on v2 offset of per-ee and per-ppid channel.
 * @fmt_cmd:		formats a GENI/SPMI command.
 * @owner_acc_status:	on v1 address of PMIC_ARB_SPMI_PIC_OWNERm_ACC_STATUSn
 *			on v2 address of SPMI_PIC_OWNERm_ACC_STATUSn.
 * @acc_enable:		on v1 address of PMIC_ARB_SPMI_PIC_ACC_ENABLEn
 *			on v2 address of SPMI_PIC_ACC_ENABLEn.
 * @irq_status:		on v1 address of PMIC_ARB_SPMI_PIC_IRQ_STATUSn
 *			on v2 address of SPMI_PIC_IRQ_STATUSn.
 * @irq_clear:		on v1 address of PMIC_ARB_SPMI_PIC_IRQ_CLEARn
 *			on v2 address of SPMI_PIC_IRQ_CLEARn.
 * @apid_map_offset:	offset of PMIC_ARB_REG_CHNLn
 * @apid_owner:		on v2 and later address of SPMI_PERIPHn_2OWNER_TABLE_REG
 */
struct pmic_arb_ver_ops {
	const char *ver_str;
	int (*get_core_resources)(struct platform_device *pdev, void __iomem *core);
	int (*init_apid)(struct spmi_pmic_arb_bus *bus, int index);
	int (*ppid_to_apid)(struct spmi_pmic_arb_bus *bus, u16 ppid);
	/* spmi commands (read_cmd, write_cmd, cmd) functionality */
	int (*offset)(struct spmi_pmic_arb_bus *bus, u8 sid, u16 addr,
		      enum pmic_arb_channel ch_type);
	u32 (*fmt_cmd)(u8 opc, u8 sid, u16 addr, u8 bc);
	int (*non_data_cmd)(struct spmi_controller *ctrl, u8 opc, u8 sid);
	/* Interrupts controller functionality (offset of PIC registers) */
	void __iomem *(*owner_acc_status)(struct spmi_pmic_arb_bus *bus, u8 m,
					  u16 n);
	void __iomem *(*acc_enable)(struct spmi_pmic_arb_bus *bus, u16 n);
	void __iomem *(*irq_status)(struct spmi_pmic_arb_bus *bus, u16 n);
	void __iomem *(*irq_clear)(struct spmi_pmic_arb_bus *bus, u16 n);
	u32 (*apid_map_offset)(u16 n);
	void __iomem *(*apid_owner)(struct spmi_pmic_arb_bus *bus, u16 n);
};

static inline void pmic_arb_base_write(struct spmi_pmic_arb *pmic_arb,
				       u32 offset, u32 val)
{
	writel_relaxed(val, pmic_arb->wr_base + offset);
}

static inline void pmic_arb_set_rd_cmd(struct spmi_pmic_arb *pmic_arb,
				       u32 offset, u32 val)
{
	writel_relaxed(val, pmic_arb->rd_base + offset);
}

/**
 * pmic_arb_read_data: reads pmic-arb's register and copy 1..4 bytes to buf
 * @pmic_arb:	the SPMI PMIC arbiter
 * @bc:		byte count -1. range: 0..3
 * @reg:	register's address
 * @buf:	output parameter, length must be bc + 1
 */
static void
pmic_arb_read_data(struct spmi_pmic_arb *pmic_arb, u8 *buf, u32 reg, u8 bc)
{
	u32 data = __raw_readl(pmic_arb->rd_base + reg);

	memcpy(buf, &data, (bc & 3) + 1);
}

/**
 * pmic_arb_write_data: write 1..4 bytes from buf to pmic-arb's register
 * @pmic_arb:	the SPMI PMIC arbiter
 * @bc:		byte-count -1. range: 0..3.
 * @reg:	register's address.
 * @buf:	buffer to write. length must be bc + 1.
 */
static void pmic_arb_write_data(struct spmi_pmic_arb *pmic_arb, const u8 *buf,
				u32 reg, u8 bc)
{
	u32 data = 0;

	memcpy(&data, buf, (bc & 3) + 1);
	__raw_writel(data, pmic_arb->wr_base + reg);
}

static int pmic_arb_wait_for_done(struct spmi_controller *ctrl,
				  void __iomem *base, u8 sid, u16 addr,
				  enum pmic_arb_channel ch_type)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u32 status = 0;
	u32 timeout = PMIC_ARB_TIMEOUT_US;
	u32 offset;
	int rc;

	rc = pmic_arb->ver_ops->offset(bus, sid, addr, ch_type);
	if (rc < 0)
		return rc;

	offset = rc;
	offset += PMIC_ARB_STATUS;

	while (timeout--) {
		status = readl_relaxed(base + offset);

		if (status & PMIC_ARB_STATUS_DONE) {
			if (status & PMIC_ARB_STATUS_DENIED) {
				dev_err(&ctrl->dev, "%s: %#x %#x: transaction denied (%#x)\n",
					__func__, sid, addr, status);
				return -EPERM;
			}

			if (status & PMIC_ARB_STATUS_FAILURE) {
				dev_err(&ctrl->dev, "%s: %#x %#x: transaction failed (%#x) reg: 0x%x\n",
					__func__, sid, addr, status, offset);
				return -EIO;
			}

			if (status & PMIC_ARB_STATUS_DROPPED) {
				dev_err(&ctrl->dev, "%s: %#x %#x: transaction dropped (%#x)\n",
					__func__, sid, addr, status);
				return -EIO;
			}

			return 0;
		}
		udelay(1);
	}

	dev_err(&ctrl->dev, "%s: %#x %#x %#x: timeout, status %#x\n",
		__func__, bus->id, sid, addr, status);
	return -ETIMEDOUT;
}

static int
pmic_arb_non_data_cmd_v1(struct spmi_controller *ctrl, u8 opc, u8 sid)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	unsigned long flags;
	u32 cmd;
	int rc;
	u32 offset;

	rc = pmic_arb->ver_ops->offset(bus, sid, 0, PMIC_ARB_CHANNEL_RW);
	if (rc < 0)
		return rc;

	offset = rc;
	cmd = ((opc | 0x40) << 27) | ((sid & 0xf) << 20);

	raw_spin_lock_irqsave(&bus->lock, flags);
	pmic_arb_base_write(pmic_arb, offset + PMIC_ARB_CMD, cmd);
	rc = pmic_arb_wait_for_done(ctrl, pmic_arb->wr_base, sid, 0,
				    PMIC_ARB_CHANNEL_RW);
	raw_spin_unlock_irqrestore(&bus->lock, flags);

	return rc;
}

static int
pmic_arb_non_data_cmd_v2(struct spmi_controller *ctrl, u8 opc, u8 sid)
{
	return -EOPNOTSUPP;
}

/* Non-data command */
static int pmic_arb_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid)
{
	struct spmi_pmic_arb *pmic_arb = spmi_controller_get_drvdata(ctrl);

	dev_dbg(&ctrl->dev, "cmd op:0x%x sid:%d\n", opc, sid);

	/* Check for valid non-data command */
	if (opc < SPMI_CMD_RESET || opc > SPMI_CMD_WAKEUP)
		return -EINVAL;

	return pmic_arb->ver_ops->non_data_cmd(ctrl, opc, sid);
}

static int pmic_arb_fmt_read_cmd(struct spmi_pmic_arb_bus *bus, u8 opc, u8 sid,
				 u16 addr, size_t len, u32 *cmd, u32 *offset)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u8 bc = len - 1;
	int rc;

	rc = pmic_arb->ver_ops->offset(bus, sid, addr,
				       PMIC_ARB_CHANNEL_OBS);
	if (rc < 0)
		return rc;

	*offset = rc;
	if (bc >= PMIC_ARB_MAX_TRANS_BYTES) {
		dev_err(&bus->spmic->dev, "pmic-arb supports 1..%d bytes per trans, but:%zu requested\n",
			PMIC_ARB_MAX_TRANS_BYTES, len);
		return  -EINVAL;
	}

	/* Check the opcode */
	if (opc >= 0x60 && opc <= 0x7F)
		opc = PMIC_ARB_OP_READ;
	else if (opc >= 0x20 && opc <= 0x2F)
		opc = PMIC_ARB_OP_EXT_READ;
	else if (opc >= 0x38 && opc <= 0x3F)
		opc = PMIC_ARB_OP_EXT_READL;
	else
		return -EINVAL;

	*cmd = pmic_arb->ver_ops->fmt_cmd(opc, sid, addr, bc);

	return 0;
}

static int pmic_arb_read_cmd_unlocked(struct spmi_controller *ctrl, u32 cmd,
				      u32 offset, u8 sid, u16 addr, u8 *buf,
				      size_t len)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u8 bc = len - 1;
	int rc;

	pmic_arb_set_rd_cmd(pmic_arb, offset + PMIC_ARB_CMD, cmd);
	rc = pmic_arb_wait_for_done(ctrl, pmic_arb->rd_base, sid, addr,
				    PMIC_ARB_CHANNEL_OBS);
	if (rc)
		return rc;

	pmic_arb_read_data(pmic_arb, buf, offset + PMIC_ARB_RDATA0,
		     min_t(u8, bc, 3));

	if (bc > 3)
		pmic_arb_read_data(pmic_arb, buf + 4, offset + PMIC_ARB_RDATA1,
					bc - 4);
	return 0;
}

static int pmic_arb_read_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			     u16 addr, u8 *buf, size_t len)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	unsigned long flags;
	u32 cmd, offset;
	int rc;

	rc = pmic_arb_fmt_read_cmd(bus, opc, sid, addr, len, &cmd,
				   &offset);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&bus->lock, flags);
	rc = pmic_arb_read_cmd_unlocked(ctrl, cmd, offset, sid, addr, buf, len);
	raw_spin_unlock_irqrestore(&bus->lock, flags);

	return rc;
}

static int pmic_arb_fmt_write_cmd(struct spmi_pmic_arb_bus *bus, u8 opc,
				  u8 sid, u16 addr, size_t len, u32 *cmd,
				  u32 *offset)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u8 bc = len - 1;
	int rc;

	rc = pmic_arb->ver_ops->offset(bus, sid, addr,
					PMIC_ARB_CHANNEL_RW);
	if (rc < 0)
		return rc;

	*offset = rc;
	if (bc >= PMIC_ARB_MAX_TRANS_BYTES) {
		dev_err(&bus->spmic->dev, "pmic-arb supports 1..%d bytes per trans, but:%zu requested\n",
			PMIC_ARB_MAX_TRANS_BYTES, len);
		return  -EINVAL;
	}

	/* Check the opcode */
	if (opc >= 0x40 && opc <= 0x5F)
		opc = PMIC_ARB_OP_WRITE;
	else if (opc <= 0x0F)
		opc = PMIC_ARB_OP_EXT_WRITE;
	else if (opc >= 0x30 && opc <= 0x37)
		opc = PMIC_ARB_OP_EXT_WRITEL;
	else if (opc >= 0x80)
		opc = PMIC_ARB_OP_ZERO_WRITE;
	else
		return -EINVAL;

	*cmd = pmic_arb->ver_ops->fmt_cmd(opc, sid, addr, bc);

	return 0;
}

static int pmic_arb_write_cmd_unlocked(struct spmi_controller *ctrl, u32 cmd,
				      u32 offset, u8 sid, u16 addr,
				      const u8 *buf, size_t len)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u8 bc = len - 1;

	/* Write data to FIFOs */
	pmic_arb_write_data(pmic_arb, buf, offset + PMIC_ARB_WDATA0,
				min_t(u8, bc, 3));
	if (bc > 3)
		pmic_arb_write_data(pmic_arb, buf + 4, offset + PMIC_ARB_WDATA1,
					bc - 4);

	/* Start the transaction */
	pmic_arb_base_write(pmic_arb, offset + PMIC_ARB_CMD, cmd);
	return pmic_arb_wait_for_done(ctrl, pmic_arb->wr_base, sid, addr,
				      PMIC_ARB_CHANNEL_RW);
}

static int pmic_arb_write_cmd(struct spmi_controller *ctrl, u8 opc, u8 sid,
			      u16 addr, const u8 *buf, size_t len)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	unsigned long flags;
	u32 cmd, offset;
	int rc;

	rc = pmic_arb_fmt_write_cmd(bus, opc, sid, addr, len, &cmd,
				    &offset);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&bus->lock, flags);
	rc = pmic_arb_write_cmd_unlocked(ctrl, cmd, offset, sid, addr, buf,
					 len);
	raw_spin_unlock_irqrestore(&bus->lock, flags);

	return rc;
}

static int pmic_arb_masked_write(struct spmi_controller *ctrl, u8 sid, u16 addr,
				 const u8 *buf, const u8 *mask, size_t len)
{
	struct spmi_pmic_arb_bus *bus = spmi_controller_get_drvdata(ctrl);
	u32 read_cmd, read_offset, write_cmd, write_offset;
	u8 temp[PMIC_ARB_MAX_TRANS_BYTES];
	unsigned long flags;
	int rc, i;

	rc = pmic_arb_fmt_read_cmd(bus, SPMI_CMD_EXT_READL, sid, addr, len,
				   &read_cmd, &read_offset);
	if (rc)
		return rc;

	rc = pmic_arb_fmt_write_cmd(bus, SPMI_CMD_EXT_WRITEL, sid, addr,
				    len, &write_cmd, &write_offset);
	if (rc)
		return rc;

	raw_spin_lock_irqsave(&bus->lock, flags);
	rc = pmic_arb_read_cmd_unlocked(ctrl, read_cmd, read_offset, sid, addr,
					temp, len);
	if (rc)
		goto done;

	for (i = 0; i < len; i++)
		temp[i] = (temp[i] & ~mask[i]) | (buf[i] & mask[i]);

	rc = pmic_arb_write_cmd_unlocked(ctrl, write_cmd, write_offset, sid,
					 addr, temp, len);
done:
	raw_spin_unlock_irqrestore(&bus->lock, flags);

	return rc;
}

enum qpnpint_regs {
	QPNPINT_REG_RT_STS		= 0x10,
	QPNPINT_REG_SET_TYPE		= 0x11,
	QPNPINT_REG_POLARITY_HIGH	= 0x12,
	QPNPINT_REG_POLARITY_LOW	= 0x13,
	QPNPINT_REG_LATCHED_CLR		= 0x14,
	QPNPINT_REG_EN_SET		= 0x15,
	QPNPINT_REG_EN_CLR		= 0x16,
	QPNPINT_REG_LATCHED_STS		= 0x18,
};

struct spmi_pmic_arb_qpnpint_type {
	u8 type; /* 1 -> edge */
	u8 polarity_high;
	u8 polarity_low;
} __packed;

/* Simplified accessor functions for irqchip callbacks */
static void qpnpint_spmi_write(struct irq_data *d, u8 reg, void *buf,
			       size_t len)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);
	u8 sid = hwirq_to_sid(d->hwirq);
	u8 per = hwirq_to_per(d->hwirq);

	if (pmic_arb_write_cmd(bus->spmic, SPMI_CMD_EXT_WRITEL, sid,
			       (per << 8) + reg, buf, len))
		dev_err_ratelimited(&bus->spmic->dev, "failed irqchip transaction on %x\n",
				    d->irq);
}

static void qpnpint_spmi_read(struct irq_data *d, u8 reg, void *buf, size_t len)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);
	u8 sid = hwirq_to_sid(d->hwirq);
	u8 per = hwirq_to_per(d->hwirq);

	if (pmic_arb_read_cmd(bus->spmic, SPMI_CMD_EXT_READL, sid,
			      (per << 8) + reg, buf, len))
		dev_err_ratelimited(&bus->spmic->dev, "failed irqchip transaction on %x\n",
				    d->irq);
}

static int qpnpint_spmi_masked_write(struct irq_data *d, u8 reg,
				     const void *buf, const void *mask,
				     size_t len)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);
	u8 sid = hwirq_to_sid(d->hwirq);
	u8 per = hwirq_to_per(d->hwirq);
	int rc;

	rc = pmic_arb_masked_write(bus->spmic, sid, (per << 8) + reg, buf,
				   mask, len);
	if (rc)
		dev_err_ratelimited(&bus->spmic->dev, "failed irqchip transaction on %x rc=%d\n",
				    d->irq, rc);
	return rc;
}

static void cleanup_irq(struct spmi_pmic_arb_bus *bus, u16 apid, int id)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u16 ppid = bus->apid_data[apid].ppid;
	u8 sid = ppid >> 8;
	u8 per = ppid & 0xFF;
	u8 irq_mask = BIT(id);

	dev_err_ratelimited(&bus->spmic->dev, "%s apid=%d sid=0x%x per=0x%x irq=%d\n",
			    __func__, apid, sid, per, id);
	writel_relaxed(irq_mask, pmic_arb->ver_ops->irq_clear(bus, apid));
}

static int periph_interrupt(struct spmi_pmic_arb_bus *bus, u16 apid)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	unsigned int irq;
	u32 status, id;
	int handled = 0;
	u8 sid = (bus->apid_data[apid].ppid >> 8) & 0xF;
	u8 per = bus->apid_data[apid].ppid & 0xFF;

	status = readl_relaxed(pmic_arb->ver_ops->irq_status(bus, apid));
	while (status) {
		id = ffs(status) - 1;
		status &= ~BIT(id);
		irq = irq_find_mapping(bus->domain,
				       spec_to_hwirq(sid, per, id, apid));
		if (irq == 0) {
			cleanup_irq(bus, apid, id);
			continue;
		}
		generic_handle_irq(irq);
		handled++;
	}

	return handled;
}

static void pmic_arb_chained_irq(struct irq_desc *desc)
{
	struct spmi_pmic_arb_bus *bus = irq_desc_get_handler_data(desc);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	const struct pmic_arb_ver_ops *ver_ops = pmic_arb->ver_ops;
	struct irq_chip *chip = irq_desc_get_chip(desc);
	int first = bus->min_apid;
	int last = bus->max_apid;
	/*
	 * acc_offset will be non-zero for the secondary SPMI bus instance on
	 * v7 controllers.
	 */
	int acc_offset = bus->base_apid >> 5;
	u8 ee = pmic_arb->ee;
	u32 status, enable, handled = 0;
	int i, id, apid;
	/* status based dispatch */
	bool acc_valid = false;
	u32 irq_status = 0;

	chained_irq_enter(chip, desc);

	for (i = first >> 5; i <= last >> 5; ++i) {
		status = readl_relaxed(ver_ops->owner_acc_status(bus, ee, i - acc_offset));
		if (status)
			acc_valid = true;

		while (status) {
			id = ffs(status) - 1;
			status &= ~BIT(id);
			apid = id + i * 32;
			if (apid < first || apid > last) {
				WARN_ONCE(true, "spurious spmi irq received for apid=%d\n",
					apid);
				continue;
			}
			enable = readl_relaxed(
					ver_ops->acc_enable(bus, apid));
			if (enable & SPMI_PIC_ACC_ENABLE_BIT)
				if (periph_interrupt(bus, apid) != 0)
					handled++;
		}
	}

	/* ACC_STATUS is empty but IRQ fired check IRQ_STATUS */
	if (!acc_valid) {
		for (i = first; i <= last; i++) {
			/* skip if APPS is not irq owner */
			if (bus->apid_data[i].irq_ee != pmic_arb->ee)
				continue;

			irq_status = readl_relaxed(
					     ver_ops->irq_status(bus, i));
			if (irq_status) {
				enable = readl_relaxed(
					     ver_ops->acc_enable(bus, i));
				if (enable & SPMI_PIC_ACC_ENABLE_BIT) {
					dev_dbg(&bus->spmic->dev,
						"Dispatching IRQ for apid=%d status=%x\n",
						i, irq_status);
					if (periph_interrupt(bus, i) != 0)
						handled++;
				}
			}
		}
	}

	if (handled == 0)
		handle_bad_irq(desc);

	chained_irq_exit(chip, desc);
}

static void qpnpint_irq_ack(struct irq_data *d)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u8 irq = hwirq_to_irq(d->hwirq);
	u16 apid = hwirq_to_apid(d->hwirq);
	u8 data;

	writel_relaxed(BIT(irq), pmic_arb->ver_ops->irq_clear(bus, apid));

	data = BIT(irq);
	qpnpint_spmi_write(d, QPNPINT_REG_LATCHED_CLR, &data, 1);
}

static void qpnpint_irq_mask(struct irq_data *d)
{
	u8 irq = hwirq_to_irq(d->hwirq);
	u8 data = BIT(irq);

	qpnpint_spmi_write(d, QPNPINT_REG_EN_CLR, &data, 1);
}

static void qpnpint_irq_unmask(struct irq_data *d)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	const struct pmic_arb_ver_ops *ver_ops = pmic_arb->ver_ops;
	u8 irq = hwirq_to_irq(d->hwirq);
	u16 apid = hwirq_to_apid(d->hwirq);
	u8 buf[2];

	writel_relaxed(SPMI_PIC_ACC_ENABLE_BIT,
			ver_ops->acc_enable(bus, apid));

	qpnpint_spmi_read(d, QPNPINT_REG_EN_SET, &buf[0], 1);
	if (!(buf[0] & BIT(irq))) {
		/*
		 * Since the interrupt is currently disabled, write to both the
		 * LATCHED_CLR and EN_SET registers so that a spurious interrupt
		 * cannot be triggered when the interrupt is enabled
		 */
		buf[0] = BIT(irq);
		buf[1] = BIT(irq);
		qpnpint_spmi_write(d, QPNPINT_REG_LATCHED_CLR, &buf, 2);
	}
}

static int qpnpint_irq_set_type(struct irq_data *d, unsigned int flow_type)
{
	struct spmi_pmic_arb_qpnpint_type type = {0};
	struct spmi_pmic_arb_qpnpint_type mask;
	irq_flow_handler_t flow_handler;
	u8 irq_bit = BIT(hwirq_to_irq(d->hwirq));
	int rc;

	if (flow_type & (IRQF_TRIGGER_RISING | IRQF_TRIGGER_FALLING)) {
		type.type = irq_bit;
		if (flow_type & IRQF_TRIGGER_RISING)
			type.polarity_high = irq_bit;
		if (flow_type & IRQF_TRIGGER_FALLING)
			type.polarity_low = irq_bit;

		flow_handler = handle_edge_irq;
	} else {
		if ((flow_type & (IRQF_TRIGGER_HIGH)) &&
		    (flow_type & (IRQF_TRIGGER_LOW)))
			return -EINVAL;

		if (flow_type & IRQF_TRIGGER_HIGH)
			type.polarity_high = irq_bit;
		else
			type.polarity_low = irq_bit;

		flow_handler = handle_level_irq;
	}

	mask.type = irq_bit;
	mask.polarity_high = irq_bit;
	mask.polarity_low = irq_bit;

	rc = qpnpint_spmi_masked_write(d, QPNPINT_REG_SET_TYPE, &type, &mask,
				       sizeof(type));
	irq_set_handler_locked(d, flow_handler);

	return rc;
}

static int qpnpint_irq_set_wake(struct irq_data *d, unsigned int on)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);

	return irq_set_irq_wake(bus->irq, on);
}

static int qpnpint_get_irqchip_state(struct irq_data *d,
				     enum irqchip_irq_state which,
				     bool *state)
{
	u8 irq = hwirq_to_irq(d->hwirq);
	u8 status = 0;

	if (which != IRQCHIP_STATE_LINE_LEVEL)
		return -EINVAL;

	qpnpint_spmi_read(d, QPNPINT_REG_RT_STS, &status, 1);
	*state = !!(status & BIT(irq));

	return 0;
}

static int qpnpint_irq_domain_activate(struct irq_domain *domain,
				       struct irq_data *d, bool reserve)
{
	struct spmi_pmic_arb_bus *bus = irq_data_get_irq_chip_data(d);
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u16 periph = hwirq_to_per(d->hwirq);
	u16 apid = hwirq_to_apid(d->hwirq);
	u16 sid = hwirq_to_sid(d->hwirq);
	u16 irq = hwirq_to_irq(d->hwirq);
	u8 buf;

	if (bus->apid_data[apid].irq_ee != pmic_arb->ee) {
		dev_err(&bus->spmic->dev, "failed to xlate sid = %#x, periph = %#x, irq = %u: ee=%u but owner=%u\n",
			sid, periph, irq, pmic_arb->ee,
			bus->apid_data[apid].irq_ee);
		return -ENODEV;
	}

	buf = BIT(irq);
	qpnpint_spmi_write(d, QPNPINT_REG_EN_CLR, &buf, 1);
	qpnpint_spmi_write(d, QPNPINT_REG_LATCHED_CLR, &buf, 1);

	return 0;
}

static struct irq_chip pmic_arb_irqchip = {
	.name		= "pmic_arb",
	.irq_ack	= qpnpint_irq_ack,
	.irq_mask	= qpnpint_irq_mask,
	.irq_unmask	= qpnpint_irq_unmask,
	.irq_set_type	= qpnpint_irq_set_type,
	.irq_set_wake	= qpnpint_irq_set_wake,
	.irq_get_irqchip_state	= qpnpint_get_irqchip_state,
	.flags		= IRQCHIP_MASK_ON_SUSPEND,
};

static int qpnpint_irq_domain_translate(struct irq_domain *d,
					struct irq_fwspec *fwspec,
					unsigned long *out_hwirq,
					unsigned int *out_type)
{
	struct spmi_pmic_arb_bus *bus = d->host_data;
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u32 *intspec = fwspec->param;
	u16 apid, ppid;
	int rc;

	dev_dbg(&bus->spmic->dev, "intspec[0] 0x%1x intspec[1] 0x%02x intspec[2] 0x%02x\n",
		intspec[0], intspec[1], intspec[2]);

	if (irq_domain_get_of_node(d) != bus->spmic->dev.of_node)
		return -EINVAL;
	if (fwspec->param_count != 4)
		return -EINVAL;
	if (intspec[0] > 0xF || intspec[1] > 0xFF || intspec[2] > 0x7)
		return -EINVAL;

	ppid = intspec[0] << 8 | intspec[1];
	rc = pmic_arb->ver_ops->ppid_to_apid(bus, ppid);
	if (rc < 0) {
		dev_err(&bus->spmic->dev, "failed to xlate sid = %#x, periph = %#x, irq = %u rc = %d\n",
			intspec[0], intspec[1], intspec[2], rc);
		return rc;
	}

	apid = rc;
	/* Keep track of {max,min}_apid for bounding search during interrupt */
	if (apid > bus->max_apid)
		bus->max_apid = apid;
	if (apid < bus->min_apid)
		bus->min_apid = apid;

	*out_hwirq = spec_to_hwirq(intspec[0], intspec[1], intspec[2], apid);
	*out_type  = intspec[3] & IRQ_TYPE_SENSE_MASK;

	dev_dbg(&bus->spmic->dev, "out_hwirq = %lu\n", *out_hwirq);

	return 0;
}

static struct lock_class_key qpnpint_irq_lock_class, qpnpint_irq_request_class;

static void qpnpint_irq_domain_map(struct spmi_pmic_arb_bus *bus,
				   struct irq_domain *domain, unsigned int virq,
				   irq_hw_number_t hwirq, unsigned int type)
{
	irq_flow_handler_t handler;

	dev_dbg(&bus->spmic->dev, "virq = %u, hwirq = %lu, type = %u\n",
		virq, hwirq, type);

	if (type & IRQ_TYPE_EDGE_BOTH)
		handler = handle_edge_irq;
	else
		handler = handle_level_irq;


	irq_set_lockdep_class(virq, &qpnpint_irq_lock_class,
			      &qpnpint_irq_request_class);
	irq_domain_set_info(domain, virq, hwirq, &pmic_arb_irqchip, bus,
			    handler, NULL, NULL);
}

static int qpnpint_irq_domain_alloc(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs,
				    void *data)
{
	struct spmi_pmic_arb_bus *bus = domain->host_data;
	struct irq_fwspec *fwspec = data;
	irq_hw_number_t hwirq;
	unsigned int type;
	int ret, i;

	ret = qpnpint_irq_domain_translate(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	for (i = 0; i < nr_irqs; i++)
		qpnpint_irq_domain_map(bus, domain, virq + i, hwirq + i,
				       type);

	return 0;
}

static int pmic_arb_init_apid_min_max(struct spmi_pmic_arb_bus *bus)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;

	/*
	 * Initialize max_apid/min_apid to the opposite bounds, during
	 * the irq domain translation, we are sure to update these
	 */
	bus->max_apid = 0;
	bus->min_apid = pmic_arb->max_periphs - 1;

	return 0;
}

static int pmic_arb_get_core_resources_v1(struct platform_device *pdev,
					  void __iomem *core)
{
	struct spmi_pmic_arb *pmic_arb = platform_get_drvdata(pdev);

	pmic_arb->wr_base = core;
	pmic_arb->rd_base = core;

	pmic_arb->max_periphs = PMIC_ARB_MAX_PERIPHS;

	return 0;
}

static int pmic_arb_init_apid_v1(struct spmi_pmic_arb_bus *bus, int index)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u32 *mapping_table;

	if (index) {
		dev_err(&bus->spmic->dev, "Unsupported buses count %d detected\n",
			index);
		return -EINVAL;
	}

	mapping_table = devm_kcalloc(&bus->spmic->dev, pmic_arb->max_periphs,
				     sizeof(*mapping_table), GFP_KERNEL);
	if (!mapping_table)
		return -ENOMEM;

	bus->mapping_table = mapping_table;

	return pmic_arb_init_apid_min_max(bus);
}

static int pmic_arb_ppid_to_apid_v1(struct spmi_pmic_arb_bus *bus, u16 ppid)
{
	u32 *mapping_table = bus->mapping_table;
	int index = 0, i;
	u16 apid_valid;
	u16 apid;
	u32 data;

	apid_valid = bus->ppid_to_apid[ppid];
	if (apid_valid & PMIC_ARB_APID_VALID) {
		apid = apid_valid & ~PMIC_ARB_APID_VALID;
		return apid;
	}

	for (i = 0; i < SPMI_MAPPING_TABLE_TREE_DEPTH; ++i) {
		if (!test_and_set_bit(index, bus->mapping_table_valid))
			mapping_table[index] = readl_relaxed(bus->cnfg +
						SPMI_MAPPING_TABLE_REG(index));

		data = mapping_table[index];

		if (ppid & BIT(SPMI_MAPPING_BIT_INDEX(data))) {
			if (SPMI_MAPPING_BIT_IS_1_FLAG(data)) {
				index = SPMI_MAPPING_BIT_IS_1_RESULT(data);
			} else {
				apid = SPMI_MAPPING_BIT_IS_1_RESULT(data);
				bus->ppid_to_apid[ppid]
					= apid | PMIC_ARB_APID_VALID;
				bus->apid_data[apid].ppid = ppid;
				return apid;
			}
		} else {
			if (SPMI_MAPPING_BIT_IS_0_FLAG(data)) {
				index = SPMI_MAPPING_BIT_IS_0_RESULT(data);
			} else {
				apid = SPMI_MAPPING_BIT_IS_0_RESULT(data);
				bus->ppid_to_apid[ppid]
					= apid | PMIC_ARB_APID_VALID;
				bus->apid_data[apid].ppid = ppid;
				return apid;
			}
		}
	}

	return -ENODEV;
}

/* v1 offset per ee */
static int pmic_arb_offset_v1(struct spmi_pmic_arb_bus *bus, u8 sid, u16 addr,
			      enum pmic_arb_channel ch_type)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return 0x800 + 0x80 * pmic_arb->channel;
}

static u16 pmic_arb_find_apid(struct spmi_pmic_arb_bus *bus, u16 ppid)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	struct apid_data *apidd = &bus->apid_data[bus->last_apid];
	u32 regval, offset;
	u16 id, apid;

	for (apid = bus->last_apid; ; apid++, apidd++) {
		offset = pmic_arb->ver_ops->apid_map_offset(apid);
		if (offset >= pmic_arb->core_size)
			break;

		regval = readl_relaxed(pmic_arb->ver_ops->apid_owner(bus,
								     apid));
		apidd->irq_ee = SPMI_OWNERSHIP_PERIPH2OWNER(regval);
		apidd->write_ee = apidd->irq_ee;

		regval = readl_relaxed(pmic_arb->core + offset);
		if (!regval)
			continue;

		id = (regval >> 8) & PMIC_ARB_PPID_MASK;
		bus->ppid_to_apid[id] = apid | PMIC_ARB_APID_VALID;
		apidd->ppid = id;
		if (id == ppid) {
			apid |= PMIC_ARB_APID_VALID;
			break;
		}
	}
	bus->last_apid = apid & ~PMIC_ARB_APID_VALID;

	return apid;
}

static int pmic_arb_get_obsrvr_chnls_v2(struct platform_device *pdev)
{
	struct spmi_pmic_arb *pmic_arb = platform_get_drvdata(pdev);

	pmic_arb->rd_base = devm_platform_ioremap_resource_byname(pdev, "obsrvr");
	if (IS_ERR(pmic_arb->rd_base))
		return PTR_ERR(pmic_arb->rd_base);

	pmic_arb->wr_base = devm_platform_ioremap_resource_byname(pdev, "chnls");
	if (IS_ERR(pmic_arb->wr_base))
		return PTR_ERR(pmic_arb->wr_base);

	return 0;
}

static int pmic_arb_get_core_resources_v2(struct platform_device *pdev,
					  void __iomem *core)
{
	struct spmi_pmic_arb *pmic_arb = platform_get_drvdata(pdev);

	pmic_arb->core = core;

	pmic_arb->max_periphs = PMIC_ARB_MAX_PERIPHS;

	return pmic_arb_get_obsrvr_chnls_v2(pdev);
}

static int pmic_arb_ppid_to_apid_v2(struct spmi_pmic_arb_bus *bus, u16 ppid)
{
	u16 apid_valid;

	apid_valid = bus->ppid_to_apid[ppid];
	if (!(apid_valid & PMIC_ARB_APID_VALID))
		apid_valid = pmic_arb_find_apid(bus, ppid);
	if (!(apid_valid & PMIC_ARB_APID_VALID))
		return -ENODEV;

	return apid_valid & ~PMIC_ARB_APID_VALID;
}

static int pmic_arb_read_apid_map_v5(struct spmi_pmic_arb_bus *bus)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	struct apid_data *apidd;
	struct apid_data *prev_apidd;
	u16 i, apid, ppid, apid_max;
	bool valid, is_irq_ee;
	u32 regval, offset;

	/*
	 * In order to allow multiple EEs to write to a single PPID in arbiter
	 * version 5 and 7, there is more than one APID mapped to each PPID.
	 * The owner field for each of these mappings specifies the EE which is
	 * allowed to write to the APID.  The owner of the last (highest) APID
	 * which has the IRQ owner bit set for a given PPID will receive
	 * interrupts from the PPID.
	 *
	 * In arbiter version 7, the APID numbering space is divided between
	 * the primary bus (0) and secondary bus (1) such that:
	 * APID = 0 to N-1 are assigned to the primary bus
	 * APID = N to N+M-1 are assigned to the secondary bus
	 * where N = number of APIDs supported by the primary bus and
	 *       M = number of APIDs supported by the secondary bus
	 */
	apidd = &bus->apid_data[bus->base_apid];
	apid_max = bus->base_apid + bus->apid_count;
	for (i = bus->base_apid; i < apid_max; i++, apidd++) {
		offset = pmic_arb->ver_ops->apid_map_offset(i);
		if (offset >= pmic_arb->core_size)
			break;

		regval = readl_relaxed(pmic_arb->core + offset);
		if (!regval)
			continue;
		ppid = (regval >> 8) & PMIC_ARB_PPID_MASK;
		is_irq_ee = PMIC_ARB_CHAN_IS_IRQ_OWNER(regval);

		regval = readl_relaxed(pmic_arb->ver_ops->apid_owner(bus, i));
		apidd->write_ee = SPMI_OWNERSHIP_PERIPH2OWNER(regval);

		apidd->irq_ee = is_irq_ee ? apidd->write_ee : INVALID_EE;

		valid = bus->ppid_to_apid[ppid] & PMIC_ARB_APID_VALID;
		apid = bus->ppid_to_apid[ppid] & ~PMIC_ARB_APID_VALID;
		prev_apidd = &bus->apid_data[apid];

		if (!valid || apidd->write_ee == pmic_arb->ee) {
			/* First PPID mapping or one for this EE */
			bus->ppid_to_apid[ppid] = i | PMIC_ARB_APID_VALID;
		} else if (valid && is_irq_ee &&
			   prev_apidd->write_ee == pmic_arb->ee) {
			/*
			 * Duplicate PPID mapping after the one for this EE;
			 * override the irq owner
			 */
			prev_apidd->irq_ee = apidd->irq_ee;
		}

		apidd->ppid = ppid;
		bus->last_apid = i;
	}

	/* Dump the mapping table for debug purposes. */
	dev_dbg(&bus->spmic->dev, "PPID APID Write-EE IRQ-EE\n");
	for (ppid = 0; ppid < PMIC_ARB_MAX_PPID; ppid++) {
		apid = bus->ppid_to_apid[ppid];
		if (apid & PMIC_ARB_APID_VALID) {
			apid &= ~PMIC_ARB_APID_VALID;
			apidd = &bus->apid_data[apid];
			dev_dbg(&bus->spmic->dev, "%#03X %3u %2u %2u\n",
				ppid, apid, apidd->write_ee, apidd->irq_ee);
		}
	}

	return 0;
}

static int pmic_arb_ppid_to_apid_v5(struct spmi_pmic_arb_bus *bus, u16 ppid)
{
	if (!(bus->ppid_to_apid[ppid] & PMIC_ARB_APID_VALID))
		return -ENODEV;

	return bus->ppid_to_apid[ppid] & ~PMIC_ARB_APID_VALID;
}

/* v2 offset per ppid and per ee */
static int pmic_arb_offset_v2(struct spmi_pmic_arb_bus *bus, u8 sid, u16 addr,
			      enum pmic_arb_channel ch_type)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u16 apid;
	u16 ppid;
	int rc;

	ppid = sid << 8 | ((addr >> 8) & 0xFF);
	rc = pmic_arb_ppid_to_apid_v2(bus, ppid);
	if (rc < 0)
		return rc;

	apid = rc;
	return 0x1000 * pmic_arb->ee + 0x8000 * apid;
}

static int pmic_arb_init_apid_v5(struct spmi_pmic_arb_bus *bus, int index)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	int ret;

	if (index) {
		dev_err(&bus->spmic->dev, "Unsupported buses count %d detected\n",
			index);
		return -EINVAL;
	}

	bus->base_apid = 0;
	bus->apid_count = readl_relaxed(pmic_arb->core + PMIC_ARB_FEATURES) &
					PMIC_ARB_FEATURES_PERIPH_MASK;

	if (bus->base_apid + bus->apid_count > pmic_arb->max_periphs) {
		dev_err(&bus->spmic->dev, "Unsupported APID count %d detected\n",
			bus->base_apid + bus->apid_count);
		return -EINVAL;
	}

	ret = pmic_arb_init_apid_min_max(bus);
	if (ret)
		return ret;

	ret = pmic_arb_read_apid_map_v5(bus);
	if (ret) {
		dev_err(&bus->spmic->dev, "could not read APID->PPID mapping table, rc= %d\n",
			ret);
		return ret;
	}

	return 0;
}

/*
 * v5 offset per ee and per apid for observer channels and per apid for
 * read/write channels.
 */
static int pmic_arb_offset_v5(struct spmi_pmic_arb_bus *bus, u8 sid, u16 addr,
			      enum pmic_arb_channel ch_type)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u16 apid;
	int rc;
	u32 offset = 0;
	u16 ppid = (sid << 8) | (addr >> 8);

	rc = pmic_arb_ppid_to_apid_v5(bus, ppid);
	if (rc < 0)
		return rc;

	apid = rc;
	switch (ch_type) {
	case PMIC_ARB_CHANNEL_OBS:
		offset = 0x10000 * pmic_arb->ee + 0x80 * apid;
		break;
	case PMIC_ARB_CHANNEL_RW:
		if (bus->apid_data[apid].write_ee != pmic_arb->ee) {
			dev_err(&bus->spmic->dev, "disallowed SPMI write to sid=%u, addr=0x%04X\n",
				sid, addr);
			return -EPERM;
		}
		offset = 0x10000 * apid;
		break;
	}

	return offset;
}

static int pmic_arb_get_core_resources_v7(struct platform_device *pdev,
					  void __iomem *core)
{
	struct spmi_pmic_arb *pmic_arb = platform_get_drvdata(pdev);

	pmic_arb->core = core;

	pmic_arb->max_periphs = PMIC_ARB_MAX_PERIPHS_V7;

	return pmic_arb_get_obsrvr_chnls_v2(pdev);
}

/*
 * Only v7 supports 2 buses. Each bus will get a different apid count, read
 * from different registers.
 */
static int pmic_arb_init_apid_v7(struct spmi_pmic_arb_bus *bus, int index)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	int ret;

	if (index == 0) {
		bus->base_apid = 0;
		bus->apid_count = readl_relaxed(pmic_arb->core + PMIC_ARB_FEATURES) &
						   PMIC_ARB_FEATURES_PERIPH_MASK;
	} else if (index == 1) {
		bus->base_apid = readl_relaxed(pmic_arb->core + PMIC_ARB_FEATURES) &
						  PMIC_ARB_FEATURES_PERIPH_MASK;
		bus->apid_count = readl_relaxed(pmic_arb->core + PMIC_ARB_FEATURES1) &
						   PMIC_ARB_FEATURES_PERIPH_MASK;
	} else {
		dev_err(&bus->spmic->dev, "Unsupported buses count %d detected\n",
			bus->id);
		return -EINVAL;
	}

	if (bus->base_apid + bus->apid_count > pmic_arb->max_periphs) {
		dev_err(&bus->spmic->dev, "Unsupported APID count %d detected\n",
			bus->base_apid + bus->apid_count);
		return -EINVAL;
	}

	ret = pmic_arb_init_apid_min_max(bus);
	if (ret)
		return ret;

	ret = pmic_arb_read_apid_map_v5(bus);
	if (ret) {
		dev_err(&bus->spmic->dev, "could not read APID->PPID mapping table, rc= %d\n",
			ret);
		return ret;
	}

	return 0;
}

/*
 * v7 offset per ee and per apid for observer channels and per apid for
 * read/write channels.
 */
static int pmic_arb_offset_v7(struct spmi_pmic_arb_bus *bus, u8 sid, u16 addr,
			      enum pmic_arb_channel ch_type)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	u16 apid;
	int rc;
	u32 offset = 0;
	u16 ppid = (sid << 8) | (addr >> 8);

	rc = pmic_arb->ver_ops->ppid_to_apid(bus, ppid);
	if (rc < 0)
		return rc;

	apid = rc;
	switch (ch_type) {
	case PMIC_ARB_CHANNEL_OBS:
		offset = 0x8000 * pmic_arb->ee + 0x20 * apid;
		break;
	case PMIC_ARB_CHANNEL_RW:
		if (bus->apid_data[apid].write_ee != pmic_arb->ee) {
			dev_err(&bus->spmic->dev, "disallowed SPMI write to sid=%u, addr=0x%04X\n",
				sid, addr);
			return -EPERM;
		}
		offset = 0x1000 * apid;
		break;
	}

	return offset;
}

static u32 pmic_arb_fmt_cmd_v1(u8 opc, u8 sid, u16 addr, u8 bc)
{
	return (opc << 27) | ((sid & 0xf) << 20) | (addr << 4) | (bc & 0x7);
}

static u32 pmic_arb_fmt_cmd_v2(u8 opc, u8 sid, u16 addr, u8 bc)
{
	return (opc << 27) | ((addr & 0xff) << 4) | (bc & 0x7);
}

static void __iomem *
pmic_arb_owner_acc_status_v1(struct spmi_pmic_arb_bus *bus, u8 m, u16 n)
{
	return bus->intr + 0x20 * m + 0x4 * n;
}

static void __iomem *
pmic_arb_owner_acc_status_v2(struct spmi_pmic_arb_bus *bus, u8 m, u16 n)
{
	return bus->intr + 0x100000 + 0x1000 * m + 0x4 * n;
}

static void __iomem *
pmic_arb_owner_acc_status_v3(struct spmi_pmic_arb_bus *bus, u8 m, u16 n)
{
	return bus->intr + 0x200000 + 0x1000 * m + 0x4 * n;
}

static void __iomem *
pmic_arb_owner_acc_status_v5(struct spmi_pmic_arb_bus *bus, u8 m, u16 n)
{
	return bus->intr + 0x10000 * m + 0x4 * n;
}

static void __iomem *
pmic_arb_owner_acc_status_v7(struct spmi_pmic_arb_bus *bus, u8 m, u16 n)
{
	return bus->intr + 0x1000 * m + 0x4 * n;
}

static void __iomem *
pmic_arb_acc_enable_v1(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->intr + 0x200 + 0x4 * n;
}

static void __iomem *
pmic_arb_acc_enable_v2(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->intr + 0x1000 * n;
}

static void __iomem *
pmic_arb_acc_enable_v5(struct spmi_pmic_arb_bus *bus, u16 n)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return pmic_arb->wr_base + 0x100 + 0x10000 * n;
}

static void __iomem *
pmic_arb_acc_enable_v7(struct spmi_pmic_arb_bus *bus, u16 n)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return pmic_arb->wr_base + 0x100 + 0x1000 * n;
}

static void __iomem *
pmic_arb_irq_status_v1(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->intr + 0x600 + 0x4 * n;
}

static void __iomem *
pmic_arb_irq_status_v2(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->intr + 0x4 + 0x1000 * n;
}

static void __iomem *
pmic_arb_irq_status_v5(struct spmi_pmic_arb_bus *bus, u16 n)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return pmic_arb->wr_base + 0x104 + 0x10000 * n;
}

static void __iomem *
pmic_arb_irq_status_v7(struct spmi_pmic_arb_bus *bus, u16 n)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return pmic_arb->wr_base + 0x104 + 0x1000 * n;
}

static void __iomem *
pmic_arb_irq_clear_v1(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->intr + 0xA00 + 0x4 * n;
}

static void __iomem *
pmic_arb_irq_clear_v2(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->intr + 0x8 + 0x1000 * n;
}

static void __iomem *
pmic_arb_irq_clear_v5(struct spmi_pmic_arb_bus *bus, u16 n)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return pmic_arb->wr_base + 0x108 + 0x10000 * n;
}

static void __iomem *
pmic_arb_irq_clear_v7(struct spmi_pmic_arb_bus *bus, u16 n)
{
	struct spmi_pmic_arb *pmic_arb = bus->pmic_arb;
	return pmic_arb->wr_base + 0x108 + 0x1000 * n;
}

static u32 pmic_arb_apid_map_offset_v2(u16 n)
{
	return 0x800 + 0x4 * n;
}

static u32 pmic_arb_apid_map_offset_v5(u16 n)
{
	return 0x900 + 0x4 * n;
}

static u32 pmic_arb_apid_map_offset_v7(u16 n)
{
	return 0x2000 + 0x4 * n;
}

static void __iomem *
pmic_arb_apid_owner_v2(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->cnfg + 0x700 + 0x4 * n;
}

/*
 * For arbiter version 7, APID ownership table registers have independent
 * numbering space for each SPMI bus instance, so each is indexed starting from
 * 0.
 */
static void __iomem *
pmic_arb_apid_owner_v7(struct spmi_pmic_arb_bus *bus, u16 n)
{
	return bus->cnfg + 0x4 * (n - bus->base_apid);
}

static const struct pmic_arb_ver_ops pmic_arb_v1 = {
	.ver_str		= "v1",
	.get_core_resources	= pmic_arb_get_core_resources_v1,
	.init_apid		= pmic_arb_init_apid_v1,
	.ppid_to_apid		= pmic_arb_ppid_to_apid_v1,
	.non_data_cmd		= pmic_arb_non_data_cmd_v1,
	.offset			= pmic_arb_offset_v1,
	.fmt_cmd		= pmic_arb_fmt_cmd_v1,
	.owner_acc_status	= pmic_arb_owner_acc_status_v1,
	.acc_enable		= pmic_arb_acc_enable_v1,
	.irq_status		= pmic_arb_irq_status_v1,
	.irq_clear		= pmic_arb_irq_clear_v1,
	.apid_map_offset	= pmic_arb_apid_map_offset_v2,
	.apid_owner		= pmic_arb_apid_owner_v2,
};

static const struct pmic_arb_ver_ops pmic_arb_v2 = {
	.ver_str		= "v2",
	.get_core_resources	= pmic_arb_get_core_resources_v2,
	.init_apid		= pmic_arb_init_apid_v1,
	.ppid_to_apid		= pmic_arb_ppid_to_apid_v2,
	.non_data_cmd		= pmic_arb_non_data_cmd_v2,
	.offset			= pmic_arb_offset_v2,
	.fmt_cmd		= pmic_arb_fmt_cmd_v2,
	.owner_acc_status	= pmic_arb_owner_acc_status_v2,
	.acc_enable		= pmic_arb_acc_enable_v2,
	.irq_status		= pmic_arb_irq_status_v2,
	.irq_clear		= pmic_arb_irq_clear_v2,
	.apid_map_offset	= pmic_arb_apid_map_offset_v2,
	.apid_owner		= pmic_arb_apid_owner_v2,
};

static const struct pmic_arb_ver_ops pmic_arb_v3 = {
	.ver_str		= "v3",
	.get_core_resources	= pmic_arb_get_core_resources_v2,
	.init_apid		= pmic_arb_init_apid_v1,
	.ppid_to_apid		= pmic_arb_ppid_to_apid_v2,
	.non_data_cmd		= pmic_arb_non_data_cmd_v2,
	.offset			= pmic_arb_offset_v2,
	.fmt_cmd		= pmic_arb_fmt_cmd_v2,
	.owner_acc_status	= pmic_arb_owner_acc_status_v3,
	.acc_enable		= pmic_arb_acc_enable_v2,
	.irq_status		= pmic_arb_irq_status_v2,
	.irq_clear		= pmic_arb_irq_clear_v2,
	.apid_map_offset	= pmic_arb_apid_map_offset_v2,
	.apid_owner		= pmic_arb_apid_owner_v2,
};

static const struct pmic_arb_ver_ops pmic_arb_v5 = {
	.ver_str		= "v5",
	.get_core_resources	= pmic_arb_get_core_resources_v2,
	.init_apid		= pmic_arb_init_apid_v5,
	.ppid_to_apid		= pmic_arb_ppid_to_apid_v5,
	.non_data_cmd		= pmic_arb_non_data_cmd_v2,
	.offset			= pmic_arb_offset_v5,
	.fmt_cmd		= pmic_arb_fmt_cmd_v2,
	.owner_acc_status	= pmic_arb_owner_acc_status_v5,
	.acc_enable		= pmic_arb_acc_enable_v5,
	.irq_status		= pmic_arb_irq_status_v5,
	.irq_clear		= pmic_arb_irq_clear_v5,
	.apid_map_offset	= pmic_arb_apid_map_offset_v5,
	.apid_owner		= pmic_arb_apid_owner_v2,
};

static const struct pmic_arb_ver_ops pmic_arb_v7 = {
	.ver_str		= "v7",
	.get_core_resources	= pmic_arb_get_core_resources_v7,
	.init_apid		= pmic_arb_init_apid_v7,
	.ppid_to_apid		= pmic_arb_ppid_to_apid_v5,
	.non_data_cmd		= pmic_arb_non_data_cmd_v2,
	.offset			= pmic_arb_offset_v7,
	.fmt_cmd		= pmic_arb_fmt_cmd_v2,
	.owner_acc_status	= pmic_arb_owner_acc_status_v7,
	.acc_enable		= pmic_arb_acc_enable_v7,
	.irq_status		= pmic_arb_irq_status_v7,
	.irq_clear		= pmic_arb_irq_clear_v7,
	.apid_map_offset	= pmic_arb_apid_map_offset_v7,
	.apid_owner		= pmic_arb_apid_owner_v7,
};

static const struct irq_domain_ops pmic_arb_irq_domain_ops = {
	.activate = qpnpint_irq_domain_activate,
	.alloc = qpnpint_irq_domain_alloc,
	.free = irq_domain_free_irqs_common,
	.translate = qpnpint_irq_domain_translate,
};

static int spmi_pmic_arb_bus_init(struct platform_device *pdev,
				  struct device_node *node,
				  struct spmi_pmic_arb *pmic_arb)
{
	int bus_index = pmic_arb->buses_available;
	struct spmi_pmic_arb_bus *bus;
	struct device *dev = &pdev->dev;
	struct spmi_controller *ctrl;
	void __iomem *intr;
	void __iomem *cnfg;
	int index, ret;
	int irq;

	ctrl = devm_spmi_controller_alloc(dev, sizeof(*bus));
	if (IS_ERR(ctrl))
		return PTR_ERR(ctrl);

	ctrl->cmd = pmic_arb_cmd;
	ctrl->read_cmd = pmic_arb_read_cmd;
	ctrl->write_cmd = pmic_arb_write_cmd;

	bus = spmi_controller_get_drvdata(ctrl);

	pmic_arb->buses[bus_index] = bus;

	raw_spin_lock_init(&bus->lock);

	bus->ppid_to_apid = devm_kcalloc(dev, PMIC_ARB_MAX_PPID,
					 sizeof(*bus->ppid_to_apid),
					 GFP_KERNEL);
	if (!bus->ppid_to_apid)
		return -ENOMEM;

	bus->apid_data = devm_kcalloc(dev, pmic_arb->max_periphs,
				      sizeof(*bus->apid_data),
				      GFP_KERNEL);
	if (!bus->apid_data)
		return -ENOMEM;

	index = of_property_match_string(node, "reg-names", "cnfg");
	if (index < 0) {
		dev_err(dev, "cnfg reg region missing\n");
		return -EINVAL;
	}

	cnfg = devm_of_iomap(dev, node, index, NULL);
	if (IS_ERR(cnfg))
		return PTR_ERR(cnfg);

	index = of_property_match_string(node, "reg-names", "intr");
	if (index < 0) {
		dev_err(dev, "intr reg region missing\n");
		return -EINVAL;
	}

	intr = devm_of_iomap(dev, node, index, NULL);
	if (IS_ERR(intr))
		return PTR_ERR(intr);

	irq = of_irq_get_byname(node, "periph_irq");
	if (irq <= 0)
		return irq ?: -ENXIO;

	bus->pmic_arb = pmic_arb;
	bus->intr = intr;
	bus->cnfg = cnfg;
	bus->irq = irq;
	bus->spmic = ctrl;
	bus->id = bus_index;

	ret = pmic_arb->ver_ops->init_apid(bus, bus_index);
	if (ret)
		return ret;

	dev_dbg(&pdev->dev, "adding irq domain for bus %d\n", bus_index);

	bus->domain = irq_domain_add_tree(node, &pmic_arb_irq_domain_ops, bus);
	if (!bus->domain) {
		dev_err(&pdev->dev, "unable to create irq_domain\n");
		return -ENOMEM;
	}

	irq_set_chained_handler_and_data(bus->irq,
					 pmic_arb_chained_irq, bus);

	ctrl->dev.of_node = node;
	dev_set_name(&ctrl->dev, "spmi-%d", bus_index);

	ret = devm_spmi_controller_add(dev, ctrl);
	if (ret)
		return ret;

	pmic_arb->buses_available++;

	return 0;
}

static int spmi_pmic_arb_register_buses(struct spmi_pmic_arb *pmic_arb,
					struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *node = dev->of_node;
	int ret;

	/* legacy mode doesn't provide child node for the bus */
	if (of_device_is_compatible(node, "qcom,spmi-pmic-arb"))
		return spmi_pmic_arb_bus_init(pdev, node, pmic_arb);

	for_each_available_child_of_node_scoped(node, child) {
		if (of_node_name_eq(child, "spmi")) {
			ret = spmi_pmic_arb_bus_init(pdev, child, pmic_arb);
			if (ret)
				return ret;
		}
	}

	return ret;
}

static void spmi_pmic_arb_deregister_buses(struct spmi_pmic_arb *pmic_arb)
{
	int i;

	for (i = 0; i < pmic_arb->buses_available; i++) {
		struct spmi_pmic_arb_bus *bus = pmic_arb->buses[i];

		irq_set_chained_handler_and_data(bus->irq,
						 NULL, NULL);
		irq_domain_remove(bus->domain);
	}
}

static int spmi_pmic_arb_probe(struct platform_device *pdev)
{
	struct spmi_pmic_arb *pmic_arb;
	struct device *dev = &pdev->dev;
	struct resource *res;
	void __iomem *core;
	u32 channel, ee, hw_ver;
	int err;

	pmic_arb = devm_kzalloc(dev, sizeof(*pmic_arb), GFP_KERNEL);
	if (!pmic_arb)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "core");
	core = devm_ioremap(dev, res->start, resource_size(res));
	if (!core)
		return -ENOMEM;

	pmic_arb->core_size = resource_size(res);

	platform_set_drvdata(pdev, pmic_arb);

	hw_ver = readl_relaxed(core + PMIC_ARB_VERSION);

	if (hw_ver < PMIC_ARB_VERSION_V2_MIN)
		pmic_arb->ver_ops = &pmic_arb_v1;
	else if (hw_ver < PMIC_ARB_VERSION_V3_MIN)
		pmic_arb->ver_ops = &pmic_arb_v2;
	else if (hw_ver < PMIC_ARB_VERSION_V5_MIN)
		pmic_arb->ver_ops = &pmic_arb_v3;
	else if (hw_ver < PMIC_ARB_VERSION_V7_MIN)
		pmic_arb->ver_ops = &pmic_arb_v5;
	else
		pmic_arb->ver_ops = &pmic_arb_v7;

	err = pmic_arb->ver_ops->get_core_resources(pdev, core);
	if (err)
		return err;

	dev_info(dev, "PMIC arbiter version %s (0x%x)\n",
		 pmic_arb->ver_ops->ver_str, hw_ver);

	err = of_property_read_u32(pdev->dev.of_node, "qcom,channel", &channel);
	if (err) {
		dev_err(&pdev->dev, "channel unspecified.\n");
		return err;
	}

	if (channel > 5) {
		dev_err(&pdev->dev, "invalid channel (%u) specified.\n",
			channel);
		return -EINVAL;
	}

	pmic_arb->channel = channel;

	err = of_property_read_u32(pdev->dev.of_node, "qcom,ee", &ee);
	if (err) {
		dev_err(&pdev->dev, "EE unspecified.\n");
		return err;
	}

	if (ee > 5) {
		dev_err(&pdev->dev, "invalid EE (%u) specified\n", ee);
		return -EINVAL;
	}

	pmic_arb->ee = ee;

	return spmi_pmic_arb_register_buses(pmic_arb, pdev);
}

static void spmi_pmic_arb_remove(struct platform_device *pdev)
{
	struct spmi_pmic_arb *pmic_arb = platform_get_drvdata(pdev);

	spmi_pmic_arb_deregister_buses(pmic_arb);
}

static const struct of_device_id spmi_pmic_arb_match_table[] = {
	{ .compatible = "qcom,spmi-pmic-arb", },
	{ .compatible = "qcom,x1e80100-spmi-pmic-arb", },
	{},
};
MODULE_DEVICE_TABLE(of, spmi_pmic_arb_match_table);

static struct platform_driver spmi_pmic_arb_driver = {
	.probe		= spmi_pmic_arb_probe,
	.remove_new	= spmi_pmic_arb_remove,
	.driver		= {
		.name	= "spmi_pmic_arb",
		.of_match_table = spmi_pmic_arb_match_table,
	},
};
module_platform_driver(spmi_pmic_arb_driver);

MODULE_DESCRIPTION("Qualcomm MSM SPMI Controller (PMIC Arbiter) driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("platform:spmi_pmic_arb");
