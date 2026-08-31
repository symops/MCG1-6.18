// SPDX-License-Identifier: GPL-2.0
/*
 * PFE control-message channel -- ported from the 3.2.26 vendor tree's
 * pfe_ctrl/pfe_ctrl.c (kmodules/mspd-c2k/pfe/ in symops/MCG1-3.2.26).
 * See pfe_ctrl.h for what was deliberately left out of this port and
 * why (the FCI control plane, and the linker-shadow-section address
 * lookup trick).
 */

#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/stddef.h>

#include "pfe_mod.h"
#include "pfe_ctrl.h"
#include "pfe_hw_lib.h"

#define TIMEOUT_MS	1000

/*
 * Busy-poll for the first couple of jiffies (the PE side normally
 * replies within a handful of microseconds), then fall back to
 * cooperative rescheduling for up to TIMEOUT_MS before giving up.
 * Unchanged from the vendor driver's own relax().
 */
static inline int relax(unsigned long end)
{
	if (time_after(jiffies, end)) {
		if (time_after(jiffies, end + msecs_to_jiffies(TIMEOUT_MS)))
			return -1;

		if (need_resched())
			schedule();
	}

	return 0;
}

/**
 * pe_sync_stop() - stop packet processing on a set of PEs
 * @ctrl: control context
 * @pe_mask: mask of PE ids to stop
 *
 * The caller must hold ctrl->mutex.
 */
int pe_sync_stop(struct pfe_ctrl *ctrl, int pe_mask)
{
	int pe_stopped = 0;
	unsigned long end = jiffies + 2;
	int i;

	for (i = 0; i < MAX_PE; i++)
		if (pe_mask & (1 << i))
			pe_dmem_write(i, cpu_to_be32(0x1),
				      ctrl->sync_mailbox_baseaddr[i] +
				      offsetof(struct pe_sync_mailbox, stop), 4);

	while (pe_stopped != pe_mask) {
		for (i = 0; i < MAX_PE; i++)
			if ((pe_mask & (1 << i)) && !(pe_stopped & (1 << i))) {
				u32 stopped = pe_dmem_read(i,
						ctrl->sync_mailbox_baseaddr[i] +
						offsetof(struct pe_sync_mailbox, stopped), 4);

				if (stopped & cpu_to_be32(0x1))
					pe_stopped |= (1 << i);
			}

		if (relax(end) < 0)
			goto err;
	}

	return 0;

err:
	pr_err("%s: timeout, pe_mask=%#x pe_stopped=%#x\n", __func__, pe_mask, pe_stopped);

	for (i = 0; i < MAX_PE; i++)
		if (pe_mask & (1 << i))
			pe_dmem_write(i, 0,
				      ctrl->sync_mailbox_baseaddr[i] +
				      offsetof(struct pe_sync_mailbox, stop), 4);

	return -EIO;
}

/**
 * pe_start() - resume packet processing on a set of PEs
 * @ctrl: control context
 * @pe_mask: mask of PE ids to start
 *
 * The caller must hold ctrl->mutex.
 */
void pe_start(struct pfe_ctrl *ctrl, int pe_mask)
{
	int i;

	for (i = 0; i < MAX_PE; i++)
		if (pe_mask & (1 << i))
			pe_dmem_write(i, 0,
				      ctrl->sync_mailbox_baseaddr[i] +
				      offsetof(struct pe_sync_mailbox, stop), 4);
}

/**
 * pe_request() - send a control request to a given PE, asking it to
 * copy data to/from its own internal memory from/to DDR.
 * @ctrl: control context
 * @id: PE id
 * @cmd_type: request sub-command
 * @dst: physical destination address of data
 * @src: physical source address of data
 * @len: data length
 *
 * The caller must hold ctrl->mutex.
 */
int pe_request(struct pfe_ctrl *ctrl, int id, unsigned short cmd_type,
		unsigned long dst, unsigned long src, int len)
{
	struct pe_msg_mailbox mbox = {
		.dst = cpu_to_be32(dst),
		.src = cpu_to_be32(src),
		.len = cpu_to_be32(len),
		.request = cpu_to_be32(((u32)cmd_type << 16) | 0x1),
	};
	unsigned long mbox_addr = ctrl->msg_mailbox_baseaddr[id];
	unsigned long end = jiffies + 2;
	u32 rc;

	/* This works because .request is written last. */
	pe_dmem_memcpy_to32(id, mbox_addr, &mbox, sizeof(mbox));

	while ((rc = pe_dmem_read(id, mbox_addr + offsetof(struct pe_msg_mailbox, request), 4))
			& cpu_to_be32(0xffff)) {
		if (relax(end) < 0)
			goto err;
	}

	return be32_to_cpu(rc) >> 16;

err:
	pr_err("%s: timeout, rc=%#x\n", __func__, be32_to_cpu(rc));
	pe_dmem_write(id, 0, mbox_addr + offsetof(struct pe_msg_mailbox, request), 4);
	return -EIO;
}

/**
 * tmu_pe_request() - send a control request to a TMU PE
 * @ctrl: control context
 * @id: TMU PE id
 * @tmu_cmd_bitmask: bitmask of commands sent to the TMU
 *
 * The caller must hold ctrl->mutex.
 */
int tmu_pe_request(struct pfe_ctrl *ctrl, int id, unsigned int tmu_cmd_bitmask)
{
	if (id < TMU0_ID || id > TMU_MAX_ID)
		return -EIO;

	return pe_request(ctrl, id, 0, tmu_cmd_bitmask, 0, 0);
}

int pfe_ctrl_init(struct pfe *pfe)
{
	struct pfe_ctrl *ctrl = &pfe->ctrl;

	mutex_init(&ctrl->mutex);

	return 0;
}

void pfe_ctrl_exit(struct pfe *pfe)
{
}
