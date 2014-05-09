#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/interrupt.h>

#include "nf10.h"

char nf10_driver_name[] = "nf10";
u64 nf10_test_dev_addr = 0x000f530dd165;

enum {
	DMA_LARGE_BUFFER = 1,
};
static int dma_version = DMA_LARGE_BUFFER;
module_param(dma_version, int, 0);
MODULE_PARM_DESC(dma_version, "nf10 DMA version (1: large buffer)");

#define DEFAULT_MSG_ENABLE (NETIF_MSG_DRV|NETIF_MSG_PROBE|NETIF_MSG_LINK|NETIF_MSG_IFDOWN|NETIF_MSG_IFUP)
static int debug = -1;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level");

static int nf10_open(struct net_device *netdev)
{
	struct nf10_adapter *adapter = netdev_priv(netdev);

	netif_start_queue(netdev);
	/* TODO */

	netif_info(adapter, ifup, netdev, "open\n");
	return 0;
}

static int nf10_close(struct net_device *netdev)
{
	struct nf10_adapter *adapter = netdev_priv(netdev);

	netif_stop_queue(netdev);
	/* TODO */

	netif_info(adapter, ifdown, netdev, "close\n");
	return 0;
}

static netdev_tx_t nf10_start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	/* TODO */
	return NETDEV_TX_BUSY;
}

static const struct net_device_ops nf10_netdev_ops = {
	.ndo_open		= nf10_open,
	.ndo_stop		= nf10_close,
	.ndo_start_xmit		= nf10_start_xmit
};

extern irqreturn_t mdio_access_interrupt_handler(int irq, void *dev_id);
extern int configure_ael2005_phy_chips(struct nf10_adapter *adapter);

irqreturn_t nf10_interrupt_handler(int irq, void *data)
{
	struct pci_dev *pdev = data;
	struct nf10_adapter *adapter = pci_get_drvdata(pdev);

	/* TODO: IRQ disable */
	
	napi_schedule(&adapter->napi);

	return IRQ_HANDLED;
}

static int nf10_init_phy(struct pci_dev *pdev)
{
	/* AEL2005 MDIO configuration */
	int err = 0;
	struct nf10_adapter *adapter = pci_get_drvdata(pdev);
	if ((err = request_irq(pdev->irq, mdio_access_interrupt_handler,
					0, nf10_driver_name, pdev)))
		return err;
	err = configure_ael2005_phy_chips(adapter);
	free_irq(pdev->irq, pdev);

	return err;
}

/* DMA engine-dependent functions */
static int nf10_init_buffers(struct pci_dev *pdev)
{
	if (dma_version == DMA_LARGE_BUFFER)
		return nf10_lbuf_init(pdev);

	return -EINVAL;
}

static int nf10_free_buffers(struct pci_dev *pdev)
{
	if (dma_version == DMA_LARGE_BUFFER) {
		nf10_lbuf_free(pdev);
		return 0;
	}

	return -EINVAL;
}

int nf10_clean_tx_irq(struct nf10_adapter *adapter)
{
	/* TODO */
	return 1;
}
void nf10_process_rx_irq(struct nf10_adapter *adapter, int *work_done, int budget)
{
	printk("IRQ delivered\n");
}

int nf10_poll(struct napi_struct *napi, int budget)
{       
	struct nf10_adapter *adapter = 
		container_of(napi, struct nf10_adapter, napi);
	int tx_clean_complete, work_done = 0;

	tx_clean_complete = nf10_clean_tx_irq(adapter);

	nf10_process_rx_irq(adapter, &work_done, budget);

	if (!tx_clean_complete)
		work_done = budget;

	if (work_done < budget) {
		napi_complete(napi);

		/* TODO: enable IRQ */
	}

	return work_done;
}

static int nf10_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct nf10_adapter *adapter;
	struct net_device *netdev;
	int err;

	if ((err = pci_enable_device(pdev)))
		return err;

	if ((err = pci_set_dma_mask(pdev, DMA_BIT_MASK(64))) ||
	    (err = pci_set_consistent_dma_mask(pdev, DMA_BIT_MASK(64)))) {
		pr_err("DMA configuration failed to set 64bit mask\n");
		goto err_dma;
	}

	if ((err = pci_request_regions(pdev, nf10_driver_name)))
		goto err_request_regions;

	pci_set_master(pdev);

	netdev = alloc_etherdev(sizeof(struct nf10_adapter));
	if (!netdev) {
		err = -ENOMEM;
		goto err_alloc_etherdev;
	}
	SET_NETDEV_DEV(netdev, &pdev->dev);

	adapter = netdev_priv(netdev);
	pci_set_drvdata(pdev, adapter);

	adapter->netdev = netdev;
	adapter->pdev = pdev;
	adapter->msg_enable = netif_msg_init(debug, DEFAULT_MSG_ENABLE);
	if ((adapter->bar0 = pci_iomap(pdev, 0, 0)) == NULL) {
		err = -EIO;
		goto err_pci_iomap_bar0;
	}
	if ((adapter->bar2 = pci_iomap(pdev, 2, 0)) == NULL) {
		err = -EIO;
		goto err_pci_iomap_bar2;
	}

	/* TODO: ethtool & features & watchdog setting */

	if ((err = pci_enable_msi(pdev))) {
		pr_err("Failed to enable MSI: err=%d\n", err);
		goto err_enable_msi;
	}

	if ((err = nf10_init_phy(pdev))) {
		pr_err("failed to initialize PHY chip\n");
		goto err_init_phy;
	}

	if ((err = request_irq(pdev->irq, nf10_interrupt_handler, 0, 
					nf10_driver_name, pdev))) {
		pr_err("failed to request irq%d\n", pdev->irq);
		goto err_request_irq;
	}

	netdev->netdev_ops = &nf10_netdev_ops;
	strcpy(netdev->name, "nf%d");
	memcpy(netdev->dev_addr, &nf10_test_dev_addr, ETH_ALEN);
	if ((err = register_netdev(netdev))) {
		pr_err("failed to register netdev\n");
		goto err_register_netdev;
	}

	if ((err = nf10_init_buffers(pdev))) {
		pr_err("failed to initialize packet buffers: err=%d\n", err);
		goto err_init_buffers;
	}

	netif_napi_add(netdev, &adapter->napi, nf10_poll, 64);
	napi_enable(&adapter->napi);

	netif_info(adapter, probe, netdev, "probe is done successfully\n");

	return 0;

err_init_buffers:
	unregister_netdev(netdev);
err_register_netdev:
	free_irq(pdev->irq, pdev);
err_request_irq:
err_init_phy:
	pci_disable_msi(pdev);
err_enable_msi:
	pci_iounmap(pdev, adapter->bar2);
err_pci_iomap_bar2:
	pci_iounmap(pdev, adapter->bar0);
err_pci_iomap_bar0:
	free_netdev(netdev);
	pci_set_drvdata(pdev, NULL);
err_alloc_etherdev:
	pci_clear_master(pdev);
	pci_release_regions(pdev);
err_request_regions:
err_dma:
	pci_disable_device(pdev);
	return err;
}

static void nf10_remove(struct pci_dev *pdev)
{
	struct nf10_adapter *adapter = pci_get_drvdata(pdev);
	struct net_device *netdev;

	if (!adapter)
		return;

	netdev = adapter->netdev;

	nf10_free_buffers(pdev);
        unregister_netdev(netdev);
	free_irq(pdev->irq, pdev);
	pci_disable_msi(pdev);
	pci_iounmap(pdev, adapter->bar2);
	pci_iounmap(pdev, adapter->bar0);
	free_netdev(netdev);
	pci_set_drvdata(pdev, NULL);
	pci_clear_master(pdev);
	pci_release_regions(pdev);
	pci_disable_device(pdev);

	netif_info(adapter, probe, netdev, "remove is done successfully\n");
}

static struct pci_device_id pci_id[] = {
	{PCI_DEVICE(NF10_VENDOR_ID, NF10_DEVICE_ID)},
	{0}
};
MODULE_DEVICE_TABLE(pci, pci_id);

pci_ers_result_t nf10_pcie_error(struct pci_dev *dev, 
				 enum pci_channel_state state)
{
	/* TODO */
	return PCI_ERS_RESULT_RECOVERED;
}

static struct pci_error_handlers pcie_err_handlers = {
	.error_detected = nf10_pcie_error
};

static struct pci_driver pci_driver = {
	.name = nf10_driver_name,
	.id_table = pci_id,
	.probe = nf10_probe,
	.remove = nf10_remove,
	.err_handler = &pcie_err_handlers
};

module_pci_driver(pci_driver);

MODULE_LICENSE("Dual BSD/GPL");
MODULE_AUTHOR("Cambridge NaaS Team");
MODULE_DESCRIPTION("Device driver for NetFPGA 10g reference NIC");
