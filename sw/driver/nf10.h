#ifndef _NF10_H
#define _NF10_H

#include <linux/netdevice.h>
#include <linux/types.h>
#include <linux/ethtool.h>
#include <linux/pci.h>
#include <linux/cdev.h>
#include "nf10_lbuf.h"
#ifdef CONFIG_XEN_NF_BACKEND
#include "xen_nfback.h"
#endif

#define NF10_VENDOR_ID	0x10ee
#define NF10_DEVICE_ID	0x4245

struct nf10_adapter {
	struct napi_struct napi;
	struct net_device *netdev;
	struct pci_dev *pdev;

	u8 __iomem *bar0;
	u8 __iomem *bar2;

	struct nf10_hw_ops *hw_ops;

	u16 msg_enable;

	/* for phy chip */
	atomic_t mdio_access_rdy;

	/* direct user access (kernel bypass) */
	struct nf10_user_ops *user_ops;
	struct cdev cdev;
	unsigned int nr_user_mmap;
	wait_queue_head_t wq_user_intr;
};

/* cmd for ctrl_irq() */
enum {
	IRQ_CTRL_ENABLE_BIT = 0,
	IRQ_CTRL_RX_BIT,
	IRQ_CTRL_END_BIT,
};
#define IRQ_CTRL_ENABLE		(1 << IRQ_CTRL_ENABLE_BIT)
#define IRQ_CTRL_RX		(1 << IRQ_CTRL_RX_BIT)
#define IRQ_CTRL_MASK		((1 << IRQ_CTRL_END_BIT) - 1)

#define IRQ_CTRL_TX_ENABLE	((IRQ_CTRL_ENABLE & ~IRQ_CTRL_RX) & IRQ_CTRL_MASK)

struct nf10_hw_ops {
	int		(*init)(struct nf10_adapter *adapter);
	void		(*free)(struct nf10_adapter *adapter);
	int		(*init_buffers)(struct nf10_adapter *adapter);
	void		(*free_buffers)(struct nf10_adapter *adapter);
	int		(*get_napi_budget)(void);
	void		(*process_rx_irq)(struct nf10_adapter *adapter, 
					  int *work_done, int budget);
	netdev_tx_t     (*start_xmit)(struct nf10_adapter *adapter,
				      struct sk_buff *skb, 
				      struct net_device *dev);
	int		(*clean_tx_irq)(struct nf10_adapter *adapter);
	unsigned long	(*ctrl_irq)(struct nf10_adapter *adapter, unsigned long cmd);
};

struct nf10_user_ops {
	u64		(*init)(struct nf10_adapter *adapter);
	unsigned long	(*get_pfn)(struct nf10_adapter *adapter, unsigned long arg);
	void		(*prepare_rx_buffer)(struct nf10_adapter *adapter,
					     unsigned long arg);
};

struct nf10_xen_ops {
	void	(*free_buffer)(struct nf10_adapter *adapter,
			       void *kern_addr, dma_addr_t dma_addr);
};

static inline void nf10_writel(struct nf10_adapter *adapter, int off, u32 val)
{
	writel(val, adapter->bar2 + off);
}

static inline void nf10_writeq(struct nf10_adapter *adapter, int off, u64 val)
{
	writeq(val, adapter->bar2 + off);
}

extern void nf10_set_ethtool_ops(struct net_device *netdev);

#endif
