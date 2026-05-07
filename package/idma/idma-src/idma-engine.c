// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2022 Axel Vanoni

#include "asm/mmio.h"
#include "linux/dmaengine.h"
#include "linux/irqreturn.h"
#include "linux/list.h"
#include "linux/lockdep.h"
#include "linux/spinlock.h"
#include "linux/spinlock_types.h"
#include "linux/virtio_blk.h"
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/dmapool.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/of_dma.h>
#include <linux/of.h>
#include <linux/wait.h>

#include "dmaengine.h"
#include "virt-dma.h"

#define IIS_IDMA_MAX_LENGTH_PER_TRANSFER 0xffffffffull
#define IIS_IDMA_AXI_ID 0xff
#define IIS_IDMA_AXI_FIXED 0x0
#define IIS_IDMA_AXI_INCR 0x1
#define IIS_IDMA_AXI_WRAP 0x2
#define IIS_IDMA_SUPPORTED_ADDRESS_WIDTH (BIT(DMA_SLAVE_BUSWIDTH_1_BYTE) |  \
                                          BIT(DMA_SLAVE_BUSWIDTH_2_BYTES) | \
                                          BIT(DMA_SLAVE_BUSWIDTH_4_BYTES) | \
                                          BIT(DMA_SLAVE_BUSWIDTH_8_BYTES))

struct iis_idma_hw_desc
{
        u32 length;
        u32 flags;
        dma_addr_t next;
        dma_addr_t src;
        dma_addr_t dst;
};

struct iis_idma_channel
{
        struct virt_dma_chan vc;
        struct iis_idma_device *parent;
        struct dma_pool *pool;
        struct iis_idma_channel *next;
        bool is_terminated;
        wait_queue_head_t wait_sync;
};

struct iis_idma_device
{
        struct dma_device dma_dev;
        void __iomem *reg_base;
        struct device *dev;
        u32 n_channels;
        u32 hw_cache_slots;
        // Lock for `current_channel`. If taking in conjunction
        // with a virtual channel lock, first take this one,
        // then the one on the virtual channel to prevent
        // deadlocks inside this module
        spinlock_t current_channel_lock;
        struct iis_idma_channel *current_channel;
        struct iis_idma_channel channels[];
};

struct iis_idma_sw_desc
{
        struct virt_dma_desc vd;
        struct iis_idma_sw_desc *next;
        struct dma_pool *hw_desc_allocator;
        dma_addr_t first;
        int n_hw_descs;
        struct iis_idma_hw_desc *hw_descs[];
};

static inline struct iis_idma_channel *to_idma_chan(struct dma_chan *chan)
{
        return container_of(to_virt_chan(chan), struct iis_idma_channel, vc);
}

static inline struct iis_idma_sw_desc *to_idma_desc(struct virt_dma_desc *desc)
{
        return container_of(desc, struct iis_idma_sw_desc, vd);
}

static inline bool is_desc_completed(struct virt_dma_desc *vd)
{
        struct iis_idma_sw_desc *desc = to_idma_desc(vd);
        return desc->hw_descs[desc->n_hw_descs - 1]->flags == (u32)~0;
}

static inline bool is_last_desc(struct virt_dma_desc *vd)
{
        struct iis_idma_sw_desc *desc = to_idma_desc(vd);
        return desc->hw_descs[desc->n_hw_descs - 1]->next == (dma_addr_t)~0ull;
}

// idma_device.current_channel_lock must be held when calling this function,
// but no virtual channel locks
// assumes that the channel is running
static bool try_get_first_uncached(struct iis_idma_channel *chan, struct iis_idma_sw_desc **out_sw, size_t *out_hw_index)
{
        struct iis_idma_device *idma_dev = chan->parent;
        struct iis_idma_channel *chan_cursor = idma_dev->current_channel;
        struct iis_idma_sw_desc *sw_desc_cursor;
        struct virt_dma_desc *vdesc;
        // We need to take the one after all the cache slots
        u32 index_of_first_uncached = idma_dev->hw_cache_slots + 1;
        unsigned long flags;
        size_t i;

        rmb();
        while (index_of_first_uncached)
        {
                spin_lock_irqsave(&chan_cursor->vc.lock, flags);
                list_for_each_entry(vdesc, &chan_cursor->vc.desc_issued, node)
                {
                        if (is_desc_completed(vdesc))
                                continue;
                        sw_desc_cursor = to_idma_desc(vdesc);
                        for (i = sw_desc_cursor->n_hw_descs - 1;
                             i >= 0 && index_of_first_uncached > 0;
                             --i)
                        {
                                if (sw_desc_cursor->hw_descs[i]->flags == (u32)~0)
                                        break;
                                --index_of_first_uncached;
                        }
                        if (index_of_first_uncached == 0)
                                break;
                }
                spin_unlock_irqrestore(&chan_cursor->vc.lock, flags);
                if (chan_cursor == chan)
                        break;
        }

        if (index_of_first_uncached)
                return false;

        if (chan_cursor != chan)
        {
                // the entire channel is uncached
                spin_lock_irqsave(&chan->vc.lock, flags);
                sw_desc_cursor = to_idma_desc(vchan_next_desc(&chan->vc));
                *out_sw = sw_desc_cursor;
                *out_hw_index = 0;
                spin_unlock_irqrestore(&chan->vc.lock, flags);
        }
        else
        {
                *out_sw = sw_desc_cursor;
                *out_hw_index = i;
        }

        return true;
}

// idma_device.current_channel_lock must be held when calling this function
static struct iis_idma_channel *get_channels_tail(struct iis_idma_device *dev)
{
        struct iis_idma_channel *cur;
        cur = dev->current_channel;
        while (cur && cur->next)
        {
                cur = cur->next;
        }
        return cur;
}

// idma_device.current_channel_lock must be held when calling this function
static bool is_channel_running(struct iis_idma_channel *chan, struct iis_idma_device *dev)
{
        int i;
        for (i = 0; i < dev->n_channels; ++i)
        {
                if (dev->channels[i].next == chan)
                {
                        return true;
                }
        }
        return dev->current_channel == chan;
}

// assumes that the channel is not already running
// caller needs to hold current_channel_lock and the lock on the channel vc
static void iis_idma_maybe_launch_channel(struct iis_idma_channel *channel, struct iis_idma_device *idma_dev)
{
        struct virt_dma_desc *vd;
        struct iis_idma_channel *tail;

        vd = vchan_next_desc(&channel->vc);
        if (!vd)
                return;

        tail = get_channels_tail(idma_dev);
        if (tail)
                tail->next = channel;
        else
                idma_dev->current_channel = channel;

        // make sure that descriptors are flushed to memory
        wmb();
        writeq(to_idma_desc(vd)->first, idma_dev->reg_base);
}

static irqreturn_t iis_idma_interrupt(int irq, void *dev_id)
{
        struct iis_idma_device *idma_dev = dev_id;
        struct iis_idma_channel *just_completed_chan;
        struct virt_dma_desc *vd;
        unsigned long iis_channel_flags;
        unsigned long vchan_flags;
        bool switch_to_next_channel;

        spin_lock_irqsave(&idma_dev->current_channel_lock, iis_channel_flags);
        just_completed_chan = idma_dev->current_channel;

        WARN(just_completed_chan == NULL, "Current channel can't be null in irq context.");
        // don't touch anything if the current channel is null
        if (unlikely(just_completed_chan == NULL))
                goto err_unlock_iis_channel;

        // make sure all descriptor changes are propagated to caches
        rmb();

        spin_lock_irqsave(&just_completed_chan->vc.lock, vchan_flags);

        vd = vchan_next_desc(&just_completed_chan->vc);
        if (!vd)
                // we have a terminated channel
                goto err_unlock_vchan;

        WARN(!is_desc_completed(vd), "Driver and hardware are out-of-sync.");
        if (unlikely(!is_desc_completed(vd)))
                goto err_unlock_vchan;

        switch_to_next_channel = is_last_desc(vd);

        list_del(&vd->node);
        if (just_completed_chan->is_terminated)
        {
                vchan_vdesc_fini(vd);
        }
        else
        {
                vchan_cookie_complete(vd);
        }

        if (switch_to_next_channel)
        {
                // update to point to the next channel
                if (just_completed_chan->next)
                {
                        idma_dev->current_channel = just_completed_chan->next;
                }
                else
                {
                        idma_dev->current_channel = NULL;
                }
                just_completed_chan->next = NULL;
        }

        if (switch_to_next_channel)
        {
                // try to relaunch, if we have new things
                iis_idma_maybe_launch_channel(just_completed_chan, idma_dev);
        }
        spin_unlock_irqrestore(&just_completed_chan->vc.lock, vchan_flags);
        spin_unlock_irqrestore(&idma_dev->current_channel_lock, iis_channel_flags);

        wake_up(&just_completed_chan->wait_sync);

        return IRQ_HANDLED;

err_unlock_vchan:
        spin_unlock_irqrestore(&just_completed_chan->vc.lock, vchan_flags);
err_unlock_iis_channel:
        spin_unlock_irqrestore(&idma_dev->current_channel_lock, iis_channel_flags);
        return IRQ_HANDLED;
}

static void iis_idma_desc_free(struct virt_dma_desc *vd)
{
        // free sw descriptor
        struct iis_idma_sw_desc *sw_desc = container_of(vd, struct iis_idma_sw_desc, vd);
        int i;

        for (i = sw_desc->n_hw_descs - 1; i >= 0; --i)
        {
                dma_pool_free(sw_desc->hw_desc_allocator, &sw_desc->hw_descs[i], i == 0 ? sw_desc->first : sw_desc->hw_descs[i - 1]->next);
        }
        sw_desc->n_hw_descs = 0;
        kfree(sw_desc);
}

static struct iis_idma_sw_desc *iis_idma_desc_alloc_and_chain(struct iis_idma_channel *chan, int n_hw_descs)
{
        struct iis_idma_sw_desc *sw_desc;
        int i;
        dma_addr_t dma_addr;

        sw_desc = kzalloc(struct_size(sw_desc, hw_descs, n_hw_descs), GFP_NOWAIT);
        if (sw_desc == NULL)
                return NULL;

        sw_desc->hw_desc_allocator = chan->pool;
        for (i = 0; i < n_hw_descs; ++i)
        {
                sw_desc->hw_descs[i] = dma_pool_alloc(sw_desc->hw_desc_allocator, GFP_NOWAIT, &dma_addr);
                if (sw_desc->hw_descs[i] == NULL)
                        goto err;
                sw_desc->hw_descs[i]->next = ~0;
                if (i == 0)
                {
                        sw_desc->first = dma_addr;
                }
                else
                {
                        sw_desc->hw_descs[i - 1]->next = dma_addr;
                }
                sw_desc->n_hw_descs++;
        }

        INIT_LIST_HEAD(&sw_desc->vd.node);

        return sw_desc;
err:
        // we failed, so free descriptor again
        iis_idma_desc_free(&sw_desc->vd);
        return NULL;
}

static int iis_idma_alloc_chan_resources(struct dma_chan *chan)
{
        struct iis_idma_channel *idma_chan = to_idma_chan(chan);

        // setup the descriptor memory pool
        if (idma_chan->parent == NULL)
                return 0;
        idma_chan->is_terminated = false;
        idma_chan->pool = dmam_pool_create("iis-dma-channel-pool", idma_chan->parent->dev,
                                           sizeof(struct iis_idma_hw_desc), __alignof__(struct iis_idma_hw_desc), 0);
        if (idma_chan->pool == NULL)
                return -ENOMEM;

        return 0;
}

static void iis_idma_free_chan_resources(struct dma_chan *chan)
{
        struct iis_idma_channel *idma_chan = to_idma_chan(chan);

        vchan_free_chan_resources(&idma_chan->vc);
        dmam_pool_destroy(idma_chan->pool);
        idma_chan->pool = NULL;
}

static inline u32 iis_idma_flags(enum dma_transaction_type tx_type, bool do_interrupt)
{
        // from the hw source:
        // Flags for this request. Currently, the following are defined:
        // bit  0         set to trigger an irq on completion, unset to not be notified
        // bits 2:1       burst type for source, fixed: 00, incr: 01, wrap: 10
        // bits 4:3       burst type for destination, fixed: 00, incr: 01, wrap: 10
        //                for a description of these modes, check AXI-Pulp documentation
        // bit  5         set to decouple reads and writes in the backend
        // bit  6         set to serialize requests. Not setting might violate AXI spec
        // bit  7         set to deburst (each burst is split into own transfer)
        //                for a more thorough description, refer to the iDMA backend documentation
        // bits 11:8      Bitfield for AXI cache attributes for the source
        // bits 15:12     Bitfield for AXI cache attributes for the destination
        //                bits of the bitfield (refer to AXI-Pulp for a description):
        //                bit 0: cache bufferable
        //                bit 1: cache modifiable
        //                bit 2: cache read alloc
        //                bit 3: cache write alloc
        // bits 23:16     AXI ID used for the transfer
        // bits 31:24     unused/reserved

        switch (tx_type)
        {
        case DMA_MEMCPY:
                return IIS_IDMA_AXI_ID << 16 |
                       0 << 12 |
                       0 << 8 |
                       0 << 7 |
                       1 << 6 |
                       0 << 5 |
                       IIS_IDMA_AXI_INCR << 3 |
                       IIS_IDMA_AXI_INCR << 1 |
                       do_interrupt << 0;
        default:
                // ERROR
                break;
        }
        return 0;
}

static dma_cookie_t
iis_idma_tx_submit(struct dma_async_tx_descriptor *desc)
{
        struct iis_idma_sw_desc *sw_desc = to_idma_desc(container_of(desc, struct virt_dma_desc, tx));
        struct iis_idma_sw_desc *prev_desc;
        struct iis_idma_channel *chan = to_idma_chan(desc->chan);
        struct virt_dma_desc *vd;
        unsigned long flags;

        // chain the last hw descriptor of the last submitted transfer
        // to the first hw descriptor of this new one
        spin_lock_irqsave(&chan->vc.lock, flags);
        if (!list_empty(&chan->vc.desc_submitted))
        {
                vd = list_last_entry(&chan->vc.desc_submitted, struct virt_dma_desc, node);
                prev_desc = to_idma_desc(vd);
                prev_desc->hw_descs[prev_desc->n_hw_descs - 1]->next = sw_desc->first;
        }
        spin_unlock_irqrestore(&chan->vc.lock, flags);
        return vchan_tx_submit(desc);
}

static inline struct dma_async_tx_descriptor *
iis_idma_tx_prep(struct iis_idma_channel *idma_channel, struct iis_idma_sw_desc *sw_desc, unsigned long flags)
{
        struct dma_async_tx_descriptor *as_desc;
        as_desc = vchan_tx_prep(&idma_channel->vc, &sw_desc->vd, flags);
        // we hook into as_desc->tx_submit to chain as we are submitting
        as_desc->tx_submit = iis_idma_tx_submit;
        return as_desc;
}

static struct dma_async_tx_descriptor *
iis_idma_prep_memcpy(struct dma_chan *chan, dma_addr_t dst,
                     dma_addr_t src, size_t len, unsigned long flags)
{
        // setup sw + hw descriptors, chain them, etc.
        // put a function pointer to the submit function inside the descriptor

        struct iis_idma_sw_desc *sw_desc;
        struct iis_idma_channel *idma_chan;
        size_t n_hw_descs;
        size_t next_length;
        int i = 0;

        if (!chan || !len)
                return NULL;

        n_hw_descs = DIV_ROUND_UP(len, IIS_IDMA_MAX_LENGTH_PER_TRANSFER);
        idma_chan = to_idma_chan(chan);

        sw_desc = iis_idma_desc_alloc_and_chain(idma_chan, n_hw_descs);
        if (sw_desc == NULL)
                return NULL;

        for (i = 0; i < n_hw_descs; ++i)
        {
                next_length = min_t(size_t, len, IIS_IDMA_MAX_LENGTH_PER_TRANSFER);
                len -= next_length;
                sw_desc->hw_descs[i]->length = next_length;
                sw_desc->hw_descs[i]->flags = iis_idma_flags(DMA_MEMCPY, i == n_hw_descs - 1);
                sw_desc->hw_descs[i]->src = src;
                sw_desc->hw_descs[i]->dst = dst;
        }

        return vchan_tx_prep(&idma_chan->vc, &sw_desc->vd, flags);
}

static int iis_idma_terminate_all(struct dma_chan *chan)
{
        struct iis_idma_channel *idma_chan = to_idma_chan(chan);
        struct iis_idma_device *idma_dev = idma_chan->parent;
        struct iis_idma_sw_desc *uncached_sw;

        size_t uncached_hw_index;
        unsigned long iis_channel_flags;
        unsigned long vchan_flags;
        LIST_HEAD(head);

        idma_chan->is_terminated = true;

        // dequeue everything we can, but leave needed descriptors in the
        // queues for the irq handler.
        spin_lock_irqsave(&idma_dev->current_channel_lock, iis_channel_flags);
        if (is_channel_running(idma_chan, idma_dev) &&
            try_get_first_uncached(idma_chan, &uncached_sw, &uncached_hw_index))
        {
                // make sure that the transfer stops here
                uncached_sw->hw_descs[uncached_hw_index]->next = ~0;

                spin_lock_irqsave(&idma_chan->vc.lock, vchan_flags);
                // move all sw descriptors after the uncached into our local list
                if (!list_is_last(&uncached_sw->vd.node, &idma_chan->vc.desc_issued))
                {
                        list_cut_before(&head, &uncached_sw->vd.node, uncached_sw->vd.node.next);
                }
        }
        else
        {
                // Either channel is not running, so we have no issued,
                // or all descriptors might be cached in hw, so don't touch them
                spin_lock_irqsave(&idma_chan->vc.lock, vchan_flags);
        }

        // get the rest
        list_splice_tail_init(&idma_chan->vc.desc_allocated, &head);
        list_splice_tail_init(&idma_chan->vc.desc_submitted, &head);
        list_splice_tail_init(&idma_chan->vc.desc_completed, &head);
        list_splice_tail_init(&idma_chan->vc.desc_terminated, &head);

        spin_unlock_irqrestore(&idma_chan->vc.lock, vchan_flags);
        spin_unlock_irqrestore(&idma_dev->current_channel_lock, iis_channel_flags);

        vchan_dma_desc_free_list(&idma_chan->vc, &head);
        return 0;
}

static bool channel_wq_condition(struct iis_idma_channel *idma_chan)
{
        unsigned long flags;
        bool ret;
        spin_lock_irqsave(&idma_chan->parent->current_channel_lock, flags);
        ret = !is_channel_running(idma_chan, idma_chan->parent);
        spin_unlock_irqrestore(&idma_chan->parent->current_channel_lock, flags);
        return ret;
}

static void iis_idma_synchronize(struct dma_chan *chan)
{
        // if channel was not terminated, vchan_synchronize
        // something something vchan_synchronize
        // else, wait for the next channel to start,
        // with a wait queue or something
        struct iis_idma_channel *idma_chan = to_idma_chan(chan);

        wait_event(idma_chan->wait_sync, channel_wq_condition(idma_chan));
        vchan_synchronize(&idma_chan->vc);
}

static u32 iis_idma_get_residue(struct iis_idma_channel *chan, dma_cookie_t cookie)
{
        struct virt_dma_desc *vd = NULL;
        struct iis_idma_sw_desc *sw_desc;
        u32 residue = 0;
        int i;
        unsigned long flags;
        spin_lock_irqsave(&chan->vc.lock, flags);
        vd = vchan_find_desc(&chan->vc, cookie);
        if (!vd)
                goto out;
        sw_desc = to_idma_desc(vd);

        // DMA writes into memory, so flush caches before accessing hw descriptors
        rmb();

        // traverse back to front, in order to be able to break once
        // the first complete transfer is found
        for (i = sw_desc->n_hw_descs - 1; i >= 0; --i)
        {
                if (sw_desc->hw_descs[i]->flags == (u32)~0)
                {
                        // this hw descriptor is complete, break
                        break;
                }
                residue += sw_desc->hw_descs[i]->length;
        }
out:
        spin_unlock_irqrestore(&chan->vc.lock, flags);
        return residue;
}

static enum dma_status iis_idma_tx_status(struct dma_chan *chan, dma_cookie_t cookie, struct dma_tx_state *tx_state)
{
        struct iis_idma_channel *idma_chan = to_idma_chan(chan);
        enum dma_status ret;

        ret = dma_cookie_status(chan, cookie, tx_state);
        if (tx_state && (ret != DMA_ERROR))
        {
                dma_set_residue(tx_state, iis_idma_get_residue(idma_chan, cookie));
        }
        return ret;
}

static void iis_idma_issue_pending(struct dma_chan *chan)
{
        // vchan_issue_pending, write start address to register
        // and update all information that is needed
        struct iis_idma_channel *idma_chan = to_idma_chan(chan);
        struct iis_idma_device *idma_dev = idma_chan->parent;
        unsigned long iis_channel_flags;
        unsigned long vchan_flags;
        spin_lock_irqsave(&idma_dev->current_channel_lock, iis_channel_flags);
        spin_lock_irqsave(&idma_chan->vc.lock, vchan_flags);

        if (!vchan_issue_pending(&idma_chan->vc))
                goto out;

        if (is_channel_running(idma_chan, idma_dev))
        {
                // TODO: try hotchaining
                // if hotchaining fails, the interrupt handler
                // will reschedule the channel as needed
                goto out;
        }

        iis_idma_maybe_launch_channel(idma_chan, idma_dev);
out:
        spin_unlock_irqrestore(&idma_chan->vc.lock, vchan_flags);
        spin_unlock_irqrestore(&idma_dev->current_channel_lock, iis_channel_flags);
}

static int iis_idma_chan_init(struct platform_device *pdev,
                              struct iis_idma_device *idma_dev,
                              struct iis_idma_channel *idma_chan)
{
        vchan_init(&idma_chan->vc, &idma_dev->dma_dev);
        idma_chan->vc.desc_free = iis_idma_desc_free;
        init_waitqueue_head(&idma_chan->wait_sync);

        idma_chan->is_terminated = false;
        idma_chan->next = NULL;
        idma_chan->parent = idma_dev;
        return 0;
}

static int iis_idma_probe(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;
        struct device_node *of_node = dev->of_node;
        struct iis_idma_device *idma_dev;
        u32 number_of_channels, hw_cache_slots;
        int ret, i;
        int irq;

        // count the number of channels to allocate:
        // get eth,input-slots with e.g. of_property_read_u32_index
        // TODO: perhaps there is a standard property name we can use
        ret = of_property_read_u32(of_node, "eth,input-slots", &number_of_channels);
        if (ret < 0)
                return ret;

        ret = of_property_read_u32(of_node, "eth,pending-slots", &hw_cache_slots);
        if (ret < 0)
                return ret;

        // allocate a struct iis_dma_device (with enough space for all channels)
        idma_dev = devm_kzalloc(dev, struct_size(idma_dev, channels, number_of_channels), GFP_KERNEL);
        if (!idma_dev)
                return -ENOMEM;

        idma_dev->dev = &pdev->dev;
        idma_dev->hw_cache_slots = hw_cache_slots;

        // get memory base from device tree file and map it
        idma_dev->reg_base = devm_platform_get_and_ioremap_resource(pdev, 0, NULL);
        if (IS_ERR(idma_dev->reg_base))
                return PTR_ERR(idma_dev->reg_base);

        // register interrupt
        irq = platform_get_irq(pdev, 0);
        if (irq < 0)
                return irq;

        ret = devm_request_irq(dev, irq, iis_idma_interrupt, IRQF_SHARED, "iis-idma", idma_dev);
        if (ret)
                return ret;

        // get eth,pending-slots with e.g. of_property_read_u32_index
        // setup capability struct
        dma_cap_set(DMA_MEMCPY, idma_dev->dma_dev.cap_mask);
        dma_cap_set(DMA_SLAVE, idma_dev->dma_dev.cap_mask);
        INIT_LIST_HEAD(&idma_dev->dma_dev.channels);
        idma_dev->dma_dev.device_alloc_chan_resources = iis_idma_alloc_chan_resources;
        idma_dev->dma_dev.device_free_chan_resources = iis_idma_free_chan_resources;
        idma_dev->dma_dev.device_tx_status = iis_idma_tx_status;
        idma_dev->dma_dev.device_issue_pending = iis_idma_issue_pending;
        idma_dev->dma_dev.device_synchronize = iis_idma_synchronize;
        idma_dev->dma_dev.device_terminate_all = iis_idma_terminate_all;
        idma_dev->dma_dev.device_prep_dma_memcpy = iis_idma_prep_memcpy;
        idma_dev->dma_dev.dev = dev;
        idma_dev->dma_dev.residue_granularity = DMA_RESIDUE_GRANULARITY_SEGMENT;
        idma_dev->dma_dev.src_addr_widths = IIS_IDMA_SUPPORTED_ADDRESS_WIDTH;
        idma_dev->dma_dev.dst_addr_widths = IIS_IDMA_SUPPORTED_ADDRESS_WIDTH;

        // initialize any fields in the idma_dev, such as waiting lists

        // setup channels
        for (i = 0; i < number_of_channels; ++i)
        {
                ret = iis_idma_chan_init(pdev, idma_dev, &idma_dev->channels[i]);
                if (ret)
                        return ret;
        }
        idma_dev->n_channels = number_of_channels;
        // register driver
        ret = dma_async_device_register(&idma_dev->dma_dev);
        if (ret)
                return ret;

        // TODO: is this translate function the right one to use?
        ret = of_dma_controller_register(of_node, of_dma_xlate_by_chan_id, idma_dev);
        if (ret)
                goto unregister_async;

        // register driver data, to be able to access it from the remove function
        platform_set_drvdata(pdev, idma_dev);

        return 0;

unregister_async:
        dma_async_device_unregister(&idma_dev->dma_dev);
        return ret;
}

static int iis_idma_remove(struct platform_device *pdev)
{
        struct iis_idma_device *idma_dev = platform_get_drvdata(pdev);
        struct dma_chan *chan;
        int ret;

        of_dma_controller_free(pdev->dev.of_node);
        dma_async_device_unregister(&idma_dev->dma_dev);

        list_for_each_entry(chan, &idma_dev->dma_dev.channels, device_node)
        {
                ret = dmaengine_terminate_sync(chan);
                if (ret)
                        return ret;
        }

        return 0;
}

static const struct of_device_id iis_idma_match[] = {
    {.compatible = "eth,idma-engine"},
    {/* sentinel */}};
MODULE_DEVICE_TABLE(of, iis_idma_match);

static struct platform_driver iis_idma_driver = {
    .probe = iis_idma_probe,
    .remove = iis_idma_remove,
    .driver = {
        .name = "iis-idma-engine",
        .of_match_table = iis_idma_match,
    },
};
module_platform_driver(iis_idma_driver);

MODULE_DESCRIPTION("IIS iDMA DMA engine driver");
MODULE_LICENSE("GPL v2");