/*
 * Integrated netback in NetFPGA nic driver
 * - Most codes are brought from Linux drivers/net/xen-netback/
 * - Make sure original netback needs to be unloaded before loading this module.
 * - Differences:
 *   1) vif is no more associated with netdev.
 *   2) packet handling to/from netfront doesn't rely on skb.
 *      so, xenvif_rx_action doesn't rely on skb.
 * - TODO:
 *   see several FIXME, #if 0, pr_debug...
 */
#include <xen/xenbus.h>
#include <xen/events.h>			/* evtchn */
#include <xen/interface/io/netif.h>	/* ring and gnttab */
#include <linux/netdevice.h>		/* IFNAMSIZ */
#include <asm/xen/page.h>		/* virt_to_mfn */
#include <linux/kthread.h>		/* xenvif_kthread */
#include "xen_nfback.h"

/* host adapter initialized in xen_nfback_init */
static struct nf10_adapter *host_adapter;

#define XEN_NETIF_TX_RING_SIZE __CONST_RING_SIZE(xen_netif_tx, PAGE_SIZE)
#define XEN_NETIF_RX_RING_SIZE __CONST_RING_SIZE(xen_netif_rx, PAGE_SIZE)

/* MAX_GRANT_COPY_OPS used to be (MAX_SKB_FRAGS * XEN_NETIF_RX_RING_SIZE),
 * since 64KB can be copied by using a single RX ring entry by means of gso.
 * But, nfback doesn't mind gso and uses its own buffer implementation 
 * regardless of skb or gso */
/* FIXME: 1! */
#define MAX_GRANT_COPY_OPS (1 * XEN_NETIF_RX_RING_SIZE)

struct xenvif {
	/* Unique identifier for this interface. */
	domid_t          domid;
	unsigned int     handle;

	/* vif of netback is origianally virtual network device
	 * but, for nfback no need to register a network device */
	char		name[IFNAMSIZ];
	struct device	*parent_dev;

	/* Use kthread for guest RX */
	struct task_struct *task;
	wait_queue_head_t wq;

	/* When feature-split-event-channels = 0, tx_irq = rx_irq. */
	unsigned int tx_irq;
	/* Only used when feature-split-event-channels = 1 */
	char tx_irq_name[IFNAMSIZ+4]; /* DEVNAME-tx */
	struct xen_netif_tx_back_ring tx;

	/* When feature-split-event-channels = 0, tx_irq = rx_irq. */
	unsigned int rx_irq;
	/* Only used when feature-split-event-channels = 1 */
	char rx_irq_name[IFNAMSIZ+4]; /* DEVNAME-rx */
	struct xen_netif_rx_back_ring rx;
	struct lbuf_head rx_buf_head;

	RING_IDX rx_last_slots;

	/* This array is allocated seperately as it is large */ 
	struct gnttab_copy *grant_copy_op;

	struct {
		int id;
		int size;
	} meta[XEN_NETIF_RX_RING_SIZE];
};
struct xenvif *FIXME_vif;	/* FIXME after classification support of nf */

struct backend_info {
	struct xenbus_device *dev;
	struct xenvif *vif;

	/* This is the state that will be reflected in xenstore when any
	 * active hotplug script completes.
	 */
	enum xenbus_state state;

	enum xenbus_state frontend_state;
	struct xenbus_watch hotplug_status_watch;
	u8 have_hotplug_status_watch:1;
};

void xenvif_kick_thread(struct xenvif *vif)
{
	wake_up(&vif->wq);
}   

static irqreturn_t xenvif_tx_interrupt(int irq, void *dev_id)
{       
#if 0
	struct xenvif *vif = dev_id;

	if (RING_HAS_UNCONSUMED_REQUESTS(&vif->tx))
		napi_schedule(&vif->napi);
#endif
	return IRQ_HANDLED;
}

static irqreturn_t xenvif_rx_interrupt(int irq, void *dev_id)
{       
	struct xenvif *vif = dev_id;

	pr_debug("%s\n", __func__);
	xenvif_kick_thread(vif);
	return IRQ_HANDLED;
}


static irqreturn_t xenvif_interrupt(int irq, void *dev_id)
{
	xenvif_tx_interrupt(irq, dev_id);
	xenvif_rx_interrupt(irq, dev_id);

	return IRQ_HANDLED;
}

struct xenvif *xenvif_alloc(struct device *parent, domid_t domid,
		unsigned int handle)
{
	struct xenvif *vif;

	if ((vif = kzalloc(sizeof(struct xenvif), GFP_KERNEL)) == NULL)
		return NULL;

	snprintf(vif->name, IFNAMSIZ - 1, "vif%u.%u", domid, handle);
	vif->grant_copy_op = vmalloc(sizeof(struct gnttab_copy) *
			MAX_GRANT_COPY_OPS);	/* FIXME */
	if (vif->grant_copy_op == NULL) {
		pr_warn("Could not allocate grant copy space for %s\n",
				vif->name);
		kfree(vif);
		return ERR_PTR(-ENOMEM);
	}

	vif->domid  = domid;
	vif->handle = handle;
	vif->parent_dev = parent;

	lbuf_head_init(&vif->rx_buf_head);

	pr_debug("Successfully created xenvif\n");
#if 0
	__module_get(THIS_MODULE);
#endif

	return vif;
}

static inline void backend_switch_state(struct backend_info *be,
		enum xenbus_state state)
{
	struct xenbus_device *dev = be->dev;

	pr_debug("%s -> %s\n", dev->nodename, xenbus_strstate(state));
	be->state = state;

	/* If we are waiting for a hotplug script then defer the
	 * actual xenbus state change.
	 */
	if (!be->have_hotplug_status_watch)
		xenbus_switch_state(dev, state);
}

static inline struct xenbus_device *xenvif_to_xenbus_device(struct xenvif *vif)
{                        
	return to_xenbus_device(vif->parent_dev);
}

void xenvif_unmap_frontend_rings(struct xenvif *vif)
{
	if (vif->tx.sring)
		xenbus_unmap_ring_vfree(xenvif_to_xenbus_device(vif),
				vif->tx.sring);
	if (vif->rx.sring)
		xenbus_unmap_ring_vfree(xenvif_to_xenbus_device(vif),
				vif->rx.sring);
}

int xenvif_map_frontend_rings(struct xenvif *vif,
		grant_ref_t tx_ring_ref,
		grant_ref_t rx_ring_ref)
{       
	void *addr;
	struct xen_netif_tx_sring *txs;
	struct xen_netif_rx_sring *rxs;

	int err = -ENOMEM;

	err = xenbus_map_ring_valloc(xenvif_to_xenbus_device(vif),
			tx_ring_ref, &addr);
	if (err)
		goto err;

	txs = (struct xen_netif_tx_sring *)addr;
	BACK_RING_INIT(&vif->tx, txs, PAGE_SIZE);

	err = xenbus_map_ring_valloc(xenvif_to_xenbus_device(vif),
			rx_ring_ref, &addr);
	if (err)         
		goto err;

	rxs = (struct xen_netif_rx_sring *)addr;
	BACK_RING_INIT(&vif->rx, rxs, PAGE_SIZE);

	return 0;

err:    
	xenvif_unmap_frontend_rings(vif);
	return err;
}

int xenvif_kthread(void *data);
int xenvif_connect(struct xenvif *vif, unsigned long tx_ring_ref,
		unsigned long rx_ring_ref, unsigned int tx_evtchn,
		unsigned int rx_evtchn)
{
	struct task_struct *task;
	int err = -ENOMEM;

	BUG_ON(vif->tx_irq);

	err = xenvif_map_frontend_rings(vif, tx_ring_ref, rx_ring_ref);
	if (err < 0)
		goto err;

	init_waitqueue_head(&vif->wq);

	if (tx_evtchn == rx_evtchn) {
		/* feature-split-event-channels == 0 */
		err = bind_interdomain_evtchn_to_irqhandler(
				vif->domid, tx_evtchn, xenvif_interrupt, 0,
				vif->name, vif);
		if (err < 0)
			goto err_unmap;
		vif->tx_irq = vif->rx_irq = err;
		disable_irq(vif->tx_irq);
	} else {
		/* feature-split-event-channels == 1 */
		snprintf(vif->tx_irq_name, sizeof(vif->tx_irq_name),
				"%s-tx", vif->name);
		err = bind_interdomain_evtchn_to_irqhandler(
				vif->domid, tx_evtchn, xenvif_tx_interrupt, 0,
				vif->tx_irq_name, vif);
		if (err < 0)
			goto err_unmap;
		vif->tx_irq = err;
		disable_irq(vif->tx_irq);

		snprintf(vif->rx_irq_name, sizeof(vif->rx_irq_name),
				"%s-rx", vif->name);
		err = bind_interdomain_evtchn_to_irqhandler(
				vif->domid, rx_evtchn, xenvif_rx_interrupt, 0,
				vif->rx_irq_name, vif);
		if (err < 0)
			goto err_tx_unbind;
		vif->rx_irq = err;
		disable_irq(vif->rx_irq);
	}

	task = kthread_create(xenvif_kthread, (void *)vif, "%s", vif->name);
	if (IS_ERR(task)) {
		pr_warn("Could not allocate kthread for %s\n", vif->name);
		err = PTR_ERR(task);
		goto err_rx_unbind;
	}
	vif->task = task;

	enable_irq(vif->tx_irq);
	if (vif->tx_irq != vif->rx_irq)
		enable_irq(vif->rx_irq);

	wake_up_process(vif->task);

	return 0;

err_rx_unbind:
	unbind_from_irqhandler(vif->rx_irq, vif);
	vif->rx_irq = 0;
err_tx_unbind:
	unbind_from_irqhandler(vif->tx_irq, vif);
	vif->tx_irq = 0;
err_unmap:
	xenvif_unmap_frontend_rings(vif);
err:
#if 0
	module_put(THIS_MODULE);
#endif
	return err;
}

static int connect_rings(struct backend_info *be)
{
	struct xenvif *vif = be->vif;
	struct xenbus_device *dev = be->dev;
	unsigned long tx_ring_ref, rx_ring_ref;
	unsigned int tx_evtchn, rx_evtchn, rx_copy;
	int err;

	err = xenbus_gather(XBT_NIL, dev->otherend,
			"tx-ring-ref", "%lu", &tx_ring_ref,
			"rx-ring-ref", "%lu", &rx_ring_ref, NULL);
	if (err) {
		xenbus_dev_fatal(dev, err,
				"reading %s/ring-ref",
				dev->otherend);
		return err;
	}

	/* Try split event channels first, then single event channel. */
	err = xenbus_gather(XBT_NIL, dev->otherend,
			"event-channel-tx", "%u", &tx_evtchn,
			"event-channel-rx", "%u", &rx_evtchn, NULL);
	if (err < 0) {
		err = xenbus_scanf(XBT_NIL, dev->otherend,
				"event-channel", "%u", &tx_evtchn);
		if (err < 0) {
			xenbus_dev_fatal(dev, err,
					"reading %s/event-channel(-tx/rx)",
					dev->otherend);
			return err;
		}
		rx_evtchn = tx_evtchn;
	}

	err = xenbus_scanf(XBT_NIL, dev->otherend, "request-rx-copy", "%u",
			&rx_copy);
	if (err == -ENOENT) {
		err = 0;
		rx_copy = 0;
	}
	if (err < 0) {
		xenbus_dev_fatal(dev, err, "reading %s/request-rx-copy",
				dev->otherend);
		return err;
	}
	if (!rx_copy)
		return -EOPNOTSUPP;

	/* Map the shared frame, irq etc. */
	err = xenvif_connect(vif, tx_ring_ref, rx_ring_ref,
			tx_evtchn, rx_evtchn);
	if (err) {
		xenbus_dev_fatal(dev, err,
				"mapping shared-frames %lu/%lu port tx %u rx %u",
				tx_ring_ref, rx_ring_ref,
				tx_evtchn, rx_evtchn);
		return err;
	}
	return 0;
}

static void unregister_hotplug_status_watch(struct backend_info *be)
{
	if (be->have_hotplug_status_watch) {
		unregister_xenbus_watch(&be->hotplug_status_watch);
		kfree(be->hotplug_status_watch.node);
	}
	be->have_hotplug_status_watch = 0;
}

static void hotplug_status_changed(struct xenbus_watch *watch,
		const char **vec,
		unsigned int vec_size)
{
	struct backend_info *be = container_of(watch,
			struct backend_info,
			hotplug_status_watch);
	char *str;
	unsigned int len;

	str = xenbus_read(XBT_NIL, be->dev->nodename, "hotplug-status", &len);
	if (IS_ERR(str))
		return;
	if (len == sizeof("connected")-1 && !memcmp(str, "connected", len)) {
		/* Complete any pending state change */
		xenbus_switch_state(be->dev, be->state);

		/* Not interested in this watch anymore. */
		unregister_hotplug_status_watch(be);
	}
	kfree(str);
}

static void connect(struct backend_info *be)
{
	int err;
	struct xenbus_device *dev = be->dev;

	pr_debug("connect!!\n");

	err = connect_rings(be);
	if (err)
		return;
	pr_debug("connect_rings succeeded!\n");

	unregister_hotplug_status_watch(be);
	err = xenbus_watch_pathfmt(dev, &be->hotplug_status_watch,
			hotplug_status_changed,
			"%s/%s", dev->nodename, "hotplug-status");
	if (!err)
		be->have_hotplug_status_watch = 1;

}

static void backend_create_xenvif(struct backend_info *be)
{
	int err;
	long handle;
	struct xenbus_device *dev = be->dev;

	if (be->vif != NULL)
		return;

	err = xenbus_scanf(XBT_NIL, dev->nodename, "handle", "%li", &handle);
	if (err != 1) {
		xenbus_dev_fatal(dev, err, "reading handle");
		return;
	}

	be->vif = xenvif_alloc(&dev->dev, dev->otherend_id, handle);
	if (IS_ERR(be->vif)) {
		err = PTR_ERR(be->vif);
		be->vif = NULL;
		xenbus_dev_fatal(dev, err, "creating interface");
		return;
	}

	/* TODO: should register vif to nf core */
	FIXME_vif = be->vif;

	kobject_uevent(&dev->dev.kobj, KOBJ_ONLINE);
}

void xenvif_disconnect(struct xenvif *vif)
{
	pr_debug("disconnect!!\n");

	disable_irq(vif->tx_irq);
	if (vif->tx_irq != vif->rx_irq)
		disable_irq(vif->rx_irq);

	if (vif->task) {
		kthread_stop(vif->task);
		vif->task = NULL;
	}

	if (vif->tx_irq) {
		if (vif->tx_irq == vif->rx_irq)
			unbind_from_irqhandler(vif->tx_irq, vif);
		else {
			unbind_from_irqhandler(vif->tx_irq, vif);
			unbind_from_irqhandler(vif->rx_irq, vif);
		}
		vif->tx_irq = 0;
	}

	xenvif_unmap_frontend_rings(vif);
}

static void backend_disconnect(struct backend_info *be)
{
	if (be->vif)
		xenvif_disconnect(be->vif);
}

static void backend_connect(struct backend_info *be)
{
	if (be->vif)
		connect(be);
}

void xenvif_free(struct xenvif *vif)
{
	vfree(vif->grant_copy_op);
	kfree(vif);
#if 0
	module_put(THIS_MODULE);
#endif
}

/* Handle backend state transitions:
 *
 * The backend state starts in InitWait and the following transitions are
 * allowed.
 *
 * InitWait -> Connected
 *
 *    ^    \         |
 *    |     \        |
 *    |      \       |
 *    |       \      |
 *    |        \     |
 *    |         \    |
 *    |          V   V
 *
 *  Closed  <-> Closing
 *
 * The state argument specifies the eventual state of the backend and the
 * function transitions to that state via the shortest path.
 */
static void set_backend_state(struct backend_info *be,
			      enum xenbus_state state)
{
	while (be->state != state) {
		switch (be->state) {
		case XenbusStateClosed:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateConnected:
				pr_info("%s: prepare for reconnect\n",
					be->dev->nodename);
				backend_switch_state(be, XenbusStateInitWait);
				break;
			case XenbusStateClosing:
				backend_switch_state(be, XenbusStateClosing);
				break;
			default:
				BUG();
			}
			break;
		case XenbusStateInitWait:
			switch (state) {
			case XenbusStateConnected:
				backend_connect(be);
				backend_switch_state(be, XenbusStateConnected);
				break;
			case XenbusStateClosing:
			case XenbusStateClosed:
				backend_switch_state(be, XenbusStateClosing);
				break;
			default:
				BUG();
			}
			break;
		case XenbusStateConnected:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateClosing:
			case XenbusStateClosed:
				backend_disconnect(be);
				backend_switch_state(be, XenbusStateClosing);
				break;
			default:
				BUG();
			}
			break;
		case XenbusStateClosing:
			switch (state) {
			case XenbusStateInitWait:
			case XenbusStateConnected:
			case XenbusStateClosed:
				backend_switch_state(be, XenbusStateClosed);
				break;
			default:
				BUG();
			}
			break;
		default:
			BUG();
		}
	}
}

/**
 * Callback received when the frontend's state changes.
 */
static void frontend_changed(struct xenbus_device *dev,
		enum xenbus_state frontend_state)
{
	struct backend_info *be = dev_get_drvdata(&dev->dev);

	pr_debug("%s -> %s\n", dev->otherend, xenbus_strstate(frontend_state));

	be->frontend_state = frontend_state;

	switch (frontend_state) {
		case XenbusStateInitialising:
			set_backend_state(be, XenbusStateInitWait);
			break;

		case XenbusStateInitialised:
			break;

		case XenbusStateConnected:
			set_backend_state(be, XenbusStateConnected);
			break;

		case XenbusStateClosing:
			set_backend_state(be, XenbusStateClosing);
			break;

		case XenbusStateClosed:
			set_backend_state(be, XenbusStateClosed);
			if (xenbus_dev_is_online(dev))
				break;
			/* fall through if not online */
		case XenbusStateUnknown:
			set_backend_state(be, XenbusStateClosed);
			device_unregister(&dev->dev);
			break;

		default:
			xenbus_dev_fatal(dev, -EINVAL, "saw state %d at frontend",
					frontend_state);
			break;
	}
}

/* xen support is not mandatory for nf native driver */
static bool registered;

static int nfback_remove(struct xenbus_device *dev)
{
	struct backend_info *be = dev_get_drvdata(&dev->dev);

	set_backend_state(be, XenbusStateClosed);

	unregister_hotplug_status_watch(be);
	if (be->vif) {
		kobject_uevent(&dev->dev.kobj, KOBJ_OFFLINE);
		xenbus_rm(XBT_NIL, dev->nodename, "hotplug-status");
		xenvif_free(be->vif);
		be->vif = NULL;
	}
	kfree(be);
	dev_set_drvdata(&dev->dev, NULL);
	return 0;
}

static int nfback_probe(struct xenbus_device *dev,
		const struct xenbus_device_id *id)
{
	const char *message;
	struct xenbus_transaction xbt;
	int err;
	struct backend_info *be = kzalloc(sizeof(struct backend_info),
			GFP_KERNEL);
	if (!be) {
		xenbus_dev_fatal(dev, -ENOMEM,
				"allocating backend structure");
		return -ENOMEM;
	}

	pr_debug("probe!!\n");

	be->dev = dev;
	dev_set_drvdata(&dev->dev, be);

	do {
		err = xenbus_transaction_start(&xbt);
		if (err) {
			xenbus_dev_fatal(dev, err, "starting transaction");
			goto fail;
		}
		err = xenbus_printf(xbt, dev->nodename,
				"feature-rx-copy", "%d", 1);
		if (err) {
			message = "writing feature-rx-copy";
			goto abort_transaction;
		}
		err = xenbus_transaction_end(xbt, 0);
	} while (err == -EAGAIN);

	if (err) {
		xenbus_dev_fatal(dev, err, "completing transaction");
		goto fail;
	}

	err = xenbus_switch_state(dev, XenbusStateInitWait);
	if (err)
		goto fail;

	be->state = XenbusStateInitWait;

	/* This kicks hotplug scripts, so do it immediately. */
	backend_create_xenvif(be);

	return 0;

abort_transaction:
	xenbus_transaction_end(xbt, 1);
	xenbus_dev_fatal(dev, err, "%s", message);
fail:
	pr_debug("failed\n");
	nfback_remove(dev);
	return err;
}

static int nfback_uevent(struct xenbus_device *xdev,
		struct kobj_uevent_env *env)
{
	struct backend_info *be = dev_get_drvdata(&xdev->dev);
	char *val;

	val = xenbus_read(XBT_NIL, xdev->nodename, "script", NULL);
	if (IS_ERR(val)) {
		int err = PTR_ERR(val);
		xenbus_dev_fatal(xdev, err, "reading script");
		return err;
	} else {
		if (add_uevent_var(env, "script=%s", val)) {
			kfree(val);
			return -ENOMEM;
		}
		kfree(val);
	}

	if (!be || !be->vif)
		return 0;

	return add_uevent_var(env, "vif=%s", be->vif->name);
}

static const struct xenbus_device_id nfback_ids[] = {
	{ "vif" },
	{ "" }
};

static DEFINE_XENBUS_DRIVER(nfback, ,
	.probe = nfback_probe,
	.remove = nfback_remove,
	.uevent = nfback_uevent,
	.otherend_changed = frontend_changed,
);

int xen_nfback_init(struct nf10_adapter *adapter)
{
	host_adapter = adapter;
	return !(registered = xenbus_register_backend(&nfback_driver) == 0);
}

void xen_nfback_fini(void)
{
	if (registered)
		xenbus_unregister_driver(&nfback_driver);
}

/* followings are helper functions for data tx/rx */
bool xenvif_rx_ring_slots_available(struct xenvif *vif, int needed)
{       
	RING_IDX prod, cons;

	do {
		prod = vif->rx.sring->req_prod;
		cons = vif->rx.req_cons;

		if (prod - cons >= needed)
			return true;

		vif->rx.sring->req_event = prod + 1;

		/* Make sure event is visible before we check prod
		 * again.
		 */
		mb();
	} while (vif->rx.sring->req_prod != prod);

	return false;
}

static struct xen_netif_rx_response *make_rx_response(struct xenvif *vif,
						      u16      id,
						      s8       st,
						      u16      offset,
						      u16      size,
						      u16      flags)
{
	RING_IDX i = vif->rx.rsp_prod_pvt;
	struct xen_netif_rx_response *resp;

	resp = RING_GET_RESPONSE(&vif->rx, i);
	resp->offset     = offset;
	resp->flags      = flags;
	resp->id         = id;
	resp->status     = (s16)size;
	if (st < 0)
		resp->status = (s16)st;

	vif->rx.rsp_prod_pvt = ++i;

	return resp;
}

/* packet processing part interacting with nf dma engine (e.g., lbuf)
 * nf core should identify domid, by which vif is located */
static int xenvif_rx_action(struct xenvif *vif)
{
	struct desc *desc;
	struct lbuf_head rxq;
	RING_IDX max_slots_needed;
	unsigned copy_prod, copy_cons;
	struct gnttab_copy *copy;
	int remaining_size;
	void *buf_addr;
	unsigned long bytes;
	struct xen_netif_rx_request *req;
	struct xen_netif_rx_response *resp;
	int status;
	int ret;
	bool need_to_notify = false;

	lbuf_head_init(&rxq);

	copy_prod = 0;
	while ((desc = lbuf_dequeue(&vif->rx_buf_head))) {
		pr_debug("%s: addr=%p size=%u off=%u\n", __func__, desc->kern_addr, desc->size, desc->offset);

		for (; desc->offset < desc->size; desc->offset += bytes) {
			buf_addr = desc->kern_addr + desc->offset;
			max_slots_needed = 1;

			if (!xenvif_rx_ring_slots_available(vif, max_slots_needed)) {
				pr_debug("RX ring is NOT available (slots=%u, addr=%p, off=%u)\n",
					 max_slots_needed, buf_addr, desc->offset);
				lbuf_queue_head(&vif->rx_buf_head, desc);
				vif->rx_last_slots = max_slots_needed;
				goto gntcopy;
			}
			remaining_size = desc->size - desc->offset;

			req = RING_GET_REQUEST(&vif->rx, vif->rx.req_cons++);
			bytes = remaining_size < PAGE_SIZE ? remaining_size : PAGE_SIZE;

			/* keep id and size for resp afterward */
			vif->meta[copy_prod].id = req->id;
			vif->meta[copy_prod].size = bytes;

			copy = vif->grant_copy_op + copy_prod;
			copy->flags = GNTCOPY_dest_gref;
			copy->len = bytes;

			copy->source.domid = DOMID_SELF;
			copy->source.u.gmfn = virt_to_mfn(buf_addr);
			copy->source.offset = 0;

			copy->dest.domid = vif->domid;
			copy->dest.offset = 0;
			copy->dest.u.ref = req->gref;
#if 0
			pr_debug("    [copy_prod=%u] id=%u d%d addr=%p mfn=%lx bytes=%lu ref=%u\n",
					copy_prod, req->id, vif->domid, buf_addr, copy->source.u.gmfn, bytes, req->gref);
#endif
			copy_prod++;
		}
		__lbuf_queue_tail(&rxq, desc);
	}
gntcopy:
	BUG_ON(copy_prod > MAX_GRANT_COPY_OPS);
	gnttab_batch_copy(vif->grant_copy_op, copy_prod);

	for (copy_cons = 0; copy_cons < copy_prod; copy_cons++) {
		copy = vif->grant_copy_op + copy_cons;
		status = copy->status == GNTST_okay ?
			XEN_NETIF_RSP_OKAY : XEN_NETIF_RSP_ERROR;

		/* FIXME: last flag arg to be appropriate like
		 * XEN_NETRXF_data_validated */
		resp = make_rx_response(vif, vif->meta[copy_cons].id,
				status, 0, vif->meta[copy_cons].size, 0);  

		RING_PUSH_RESPONSES_AND_CHECK_NOTIFY(&vif->rx, ret);

		need_to_notify |= !!ret;
#if 0
		pr_debug("    [copy_cons=%u] id=%u status=%d bytes=%lu ret=%d need_to_notify=%d\n",
			 copy_cons, vif->meta[copy_cons].id, status, vif->meta[copy_cons].size, ret, need_to_notify);
#endif
	}
	if (need_to_notify)
		notify_remote_via_irq(vif->rx_irq);
	
	/* FIXME: can be just moved out of critical path? */
	while ((desc = __lbuf_dequeue(&rxq)))
		release_lbuf(host_adapter, desc);

	pr_debug("end of xmit to front\n");

	return 0;
}

static inline int rx_work_todo(struct xenvif *vif)
{
	int empty = lbuf_queue_empty(&vif->rx_buf_head);
	int avail = xenvif_rx_ring_slots_available(vif, vif->rx_last_slots);
	pr_debug("rx_work_todo: non-empty=%d avail=%d last=%u\n",
		 !empty, avail, vif->rx_last_slots);
	return !empty && avail;
}

int xenvif_kthread(void *data)
{
	struct xenvif *vif = data;
	struct desc *desc;

	while (!kthread_should_stop()) {
		wait_event_interruptible(vif->wq,
				rx_work_todo(vif) ||
				kthread_should_stop());
		if (kthread_should_stop())
			break;

		if (!lbuf_queue_empty(&vif->rx_buf_head))
			xenvif_rx_action(vif);

		/* XXX: flow control - do we need? */

		cond_resched();
	}

	/* Bin any remaining bufs */
	while ((desc = lbuf_dequeue(&vif->rx_buf_head)) != NULL)
		release_lbuf(host_adapter, desc);

	return 0;
}

bool xenvif_connected(unsigned long domid)
{
	struct xenvif *vif = FIXME_vif;	/* FIXME: dom-to-vif */

	if (vif == NULL || vif->task == NULL)
		return false;
	return true;
}

int xenvif_start_xmit(unsigned long domid, struct desc *desc)
{
	struct xenvif *vif = FIXME_vif;	/* FIXME: dom-to-vif */

	lbuf_queue_tail(&vif->rx_buf_head, desc);
	xenvif_kick_thread(vif);

	return 0;
}
