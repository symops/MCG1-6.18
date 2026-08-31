/* SPDX-License-Identifier: GPL-2.0 */
/*
 * PFE control-message channel -- the mailbox protocol used to talk to
 * the CLASS/TMU/UTIL PE firmware while it's running (start/stop a PE,
 * and issue synchronous "PE requests" that ask a PE to copy data
 * to/from its own DMEM from/to DDR). Ported from the 3.2.26 vendor
 * tree's pfe_ctrl/pfe_ctrl.c (kmodules/mspd-c2k/pfe/ in
 * symops/MCG1-3.2.26), scoped down to just this self-contained
 * primitive layer.
 *
 * NOT ported: the vendor pfe_ctrl.c also hosts the "FCI" control
 * plane (__pfe_ctrl_cmd_handler() in pfe/pfe/c2000/__pfe_ctrl.c,
 * dispatching into module_ipv4.c/module_ipv6.c/module_bridge.c/
 * module_vlan.c/module_pppoe.c/module_ipsec.c/module_tunnel.c/
 * module_wifi.c/module_rtp_relay.c/module_mc4.c/module_mc6.c/
 * module_qm.c/module_socket.c/module_hidrv.c/module_tx.c/module_Rx.c/
 * module_stat.c/icc.c) -- a full IP routing/bridging/VLAN/PPPoE/
 * IPsec/tunnel/WiFi-offload/RTP-relay/multicast/QoS control plane
 * fronting the vendor's userspace "FCI" tool. None of that is needed
 * to get GEM0-2 passing plain Ethernet traffic (Stage P7-P9), so it --
 * and everything that only exists to support it
 * (comcerto_fpp_send_command()/pfe_ctrl_set_eth_state()/
 * pfe_ctrl_set_lro()/comcerto_fpp_register_event_cb(), the periodic
 * ctrl->timer_thread and its TIMER_ENTRY list, the dma_pool/
 * dma_pool_512/null_ct conntrack-entry allocators, and the route-
 * table/IPsec-LMEM address fields) -- is dropped here, matching the
 * same pattern already used to drop pfe_vwd.c/pfe_pci.c/pfe_diags.c/
 * pfe_sysfs.c/pfe_mspsync.c/pfe_unit_test.c earlier in this port.
 *
 * Also NOT ported: the vendor's own mechanism for finding
 * sync_mailbox/msg_mailbox's PE-side address. It builds host-side
 * "shadow" copies of these structs into specially-named linker
 * sections (the CLASS_DMEM_SH2()-family macros in pfe_ctrl_hal.h)
 * inside its own pfe_ctrl.ko, then computes each PE address from the
 * relative offset between that shadow section and this driver's own
 * copy of it -- a trick that requires the host driver to be built as
 * a loadable module with its own linker script fragment, which
 * doesn't fit this built-in (=y) driver. Since the firmware ELFs are
 * unstripped, this port instead looks the exact "sync_mbox"/
 * "msg_mbox" symbols up directly in each firmware's .symtab
 * (pfe_firmware.c:get_elf_symbol_addr()) and uses their st_value
 * as-is -- confirmed those symbols exist, with sizes matching
 * struct pe_sync_mailbox/pe_msg_mailbox below, in all three firmware
 * blobs in this tree.
 */
#ifndef _PFE_CTRL_H_
#define _PFE_CTRL_H_

#include <linux/mutex.h>
#include <linux/types.h>

#include "pfe_hw_lib.h"

struct pfe;

/*
 * Layout ported as-is from the vendor tree's pfe/pfe/c2000/pfe/pfe.h.
 * This is PE-side DMEM, accessed only via pe_dmem_read()/
 * pe_dmem_write()/pe_dmem_memcpy_to32() -- never dereferenced as a
 * host pointer, only used via offsetof() to compute field addresses.
 */
struct pe_sync_mailbox {
	u32 stop;
	u32 stopped;
};

struct pe_msg_mailbox {
	u32 dst;
	u32 src;
	u32 len;
	u32 request;
};

struct pfe_ctrl {
	struct mutex mutex;

	/*
	 * PE-side (DMEM) addresses of each PE's mailbox structs, looked
	 * up by symbol name from the firmware ELF at load time -- see
	 * pfe_firmware.c:pfe_ctrl_set_mailbox_addrs().
	 */
	unsigned long sync_mailbox_baseaddr[MAX_PE];
	unsigned long msg_mailbox_baseaddr[MAX_PE];
};

int pfe_ctrl_init(struct pfe *pfe);
void pfe_ctrl_exit(struct pfe *pfe);

int pe_sync_stop(struct pfe_ctrl *ctrl, int pe_mask);
void pe_start(struct pfe_ctrl *ctrl, int pe_mask);
int pe_request(struct pfe_ctrl *ctrl, int id, unsigned short cmd_type,
		unsigned long dst, unsigned long src, int len);
int tmu_pe_request(struct pfe_ctrl *ctrl, int id, unsigned int tmu_cmd_bitmask);

#endif /* _PFE_CTRL_H_ */
