// SPDX-License-Identifier: GPL-2.0
//
// Copyright (C) 2025 Kevin Schaerer

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/cdev.h>
#include <linux/of_dma.h>
#include <linux/fs.h>
#include <linux/ioctl.h>
#include <linux/dma-mapping.h>

#include "idma-proxy.h"

#define MAX_IDMA_PROXY_DEVICES 8

/**
 * struct idma_port - structure representing a port for iDMA transfers
 *
 * This structure is used to represent a port for iDMA transfers. (Either source or destination)
 *
 * @addr:        The user virtual address of the port
 * @phys_addr:   The physical address of the port
 * @type:        The type of the port (IDMA_KERNEL_BUFFER, IDMA_COPY_BUFFER, IDMA_USER_DIRECT)
 * @length:      The length of the buffer referenced by the port in bytes
 * @num_pages:   The number of pages, only used for IDMA_USER_DIRECT
 * @pages:       The pages for user direct access, only used for IDMA_USER_DIRECT
 */
struct idma_port
{
        uintptr_t addr;
        dma_addr_t phys_addr;
        idma_port_type_t type;
        size_t length;
        int num_pages;
        struct page **pages;
};

/**
 * struct idma_proxy_channel - structure representing a channel for iDMA transfers
 *
 * This structure is used to represent a channel for iDMA transfers.
 *
 * @dev:          The device associated with the channel
 * @chan:         The DMA channel used for the transfer
 * @cdev:         The character device associated with the channel
 * @chr_dev:      The character device for user space access
 * @buffers:      The kernel buffers used for the transfer, one for source and one for destination
 *                Those are used if the respective port type is IDMA_KERNEL_BUFFER
 * @buffer_phys_addrs: The physical addresses of the buffers
 * @cmp:          The completion structure used to signal transfer completion
 * @cookie:       The DMA cookie for the transfer
 * @src_port:     The source port for the transfer
 * @dst_port:     The destination port for the transfer
 */
struct idma_proxy_channel
{
        struct device *dev;
        struct dma_chan *chan;
        struct cdev cdev;
        struct device *chr_dev;
        struct idma_proxy_kernel_buffer *buffers[2];
        dma_addr_t buffer_phys_addrs[2];
        struct completion cmp;
        dma_cookie_t cookie;
        struct idma_port src_port;
        struct idma_port dst_port;
};

/**
 * struct idma_proxy_device - structure representing the iDMA proxy device
 *
 * This structure is used to represent the iDMA proxy device, which manages multiple channels.
 *
 * @dev:          The device associated with the iDMA proxy
 * @n_channels:   The number of channels available in the iDMA proxy
 * @channels:     The array of channels managed by the iDMA proxy
 * @names:        The names of the channels, used for device creation
 * @device_num_base: The base device number for character devices
 * @device_class: The class for the character devices
 */
struct idma_proxy_device
{
        struct device *dev;
        int n_channels;
        struct idma_proxy_channel *channels;
        char **names;
        dev_t device_num_base;
        struct class *device_class;
};

/**
 * @brief: Callback function for DMA transfer completion
 * This function is called when a DMA transfer completes.
 *
 * @param completion: Pointer to the completion structure to signal
 */
static void idma_proxy_callback(void *completion)
{
        complete(completion);
}

/**
 * @brief: Prepares the idma_port for the specified type
 *
 * This function prepares the idma_port for the specified type by allocating memory,
 * copying data from user space, or getting user pages as needed.
 * This function can be used to prepare both source and destination ports for the transfer.
 *
 * @param port: Pointer to the idma_port to prepare
 * @return: 0 on success, negative error code on failure
 */
static inline int prepare_idma_port_for_type(struct idma_port *port)
{
        int ret;

        switch (port->type)
        {
        case IDMA_KERNEL_BUFFER:
        {
                // No action needed for kernel buffer
                break;
        }
        case IDMA_COPY_BUFFER:
        {
                void *result_virt = 0;
                result_virt = (void *)__get_free_pages(GFP_KERNEL | GFP_DMA32, order_base_2(ALIGN(port->length, PAGE_SIZE) / PAGE_SIZE));
                if (!result_virt)
                {
                        pr_err("Failed to allocate memory for copy buffer\n");
                        return -ENOMEM;
                }
                ret = copy_from_user(result_virt, (const void __user *)port->addr, port->length);
                if (ret)
                {
                        pr_err("Failed to copy data from user space to kernel buffer\n");
                        free_pages((unsigned long)result_virt, order_base_2(ALIGN(port->length, PAGE_SIZE) / PAGE_SIZE));
                        return ret;
                }
                port->phys_addr = virt_to_phys(result_virt);
                break;
        }
        case IDMA_USER_DIRECT:
        {
                port->num_pages = DIV_ROUND_UP(port->length, PAGE_SIZE);
                port->pages = kcalloc(port->num_pages, sizeof(struct page *), GFP_KERNEL);
                if (!port->pages)
                {
                        pr_err("Failed to allocate memory for user pages\n");
                        return -ENOMEM;
                }
                ret = get_user_pages_fast(port->addr, port->num_pages, 0, port->pages);
                if (ret < 0)
                {
                        pr_err("Failed to get user pages for address %lx\n", port->addr);
                        return ret;
                }
                port->phys_addr = page_to_phys(port->pages[0]);
                break;
        }
        default:
                pr_err("Unsupported port type %d\n", port->type);
                return -EINVAL;
        }

        return 0;
}

/**
 * @brief: Destructs the idma_port for the specified type
 *
 * This function destructs the idma_port for the specified type by freeing memory,
 * copying data to user space, or releasing user pages as needed.
 * This function can be used to destruct both source and destination ports after the transfer.
 *
 * @param port: Pointer to the idma_port to destruct
 * @return: 0 on success, negative error code on failure
 */
static inline int destruct_idma_port_for_type(struct idma_port *port)
{
        int i, ret;

        switch (port->type)
        {
        case IDMA_KERNEL_BUFFER:
        {
                // No action needed for kernel buffer
                break;
        }
        case IDMA_COPY_BUFFER:
        {
                void *virt_addr = phys_to_virt(port->phys_addr);
                if (!virt_addr)
                {
                        pr_err("Failed to convert physical address %llx to virtual address\n", port->phys_addr);
                        return -EFAULT;
                }
                ret = copy_to_user((void __user *)port->addr, virt_addr, port->length);
                if (ret)
                {
                        pr_err("Failed to copy data from kernel buffer to user space\n");
                }
                free_pages((unsigned long)virt_addr, order_base_2(ALIGN(port->length, PAGE_SIZE) / PAGE_SIZE));
                break;
        }
        case IDMA_USER_DIRECT:
        {
                if (!port->pages)
                {
                        pr_err("No pages allocated for user direct access\n");
                        return -EFAULT;
                }
                for (i = 0; i < port->num_pages; i++)
                {
                        if (port->pages[i])
                                put_page(port->pages[i]);
                }
                kfree(port->pages);
                port->pages = NULL;
                port->num_pages = 0;
                port->phys_addr = 0;
                port->addr = 0;

                break;
        }
        default:
                pr_err("Unsupported port type %d\n", port->type);
                return -EINVAL;
        }

        return 0;
}

/**
 * @brief: Starts the iDMA transfer using the specified channel and transfer parameters
 *
 * This function prepares the source and destination ports, sets up the DMA transfer,
 * and submits it to the DMA engine.
 *
 * @param proxy_chan: Pointer to the idma_proxy_channel to use for the transfer
 * @param transfer: Pointer to the idma_memcpy_transfer structure containing transfer parameters
 * @return: 0 on success, negative error code on failure
 */
static int start_idma_transfer(struct idma_proxy_channel *proxy_chan, struct idma_memcpy_transfer *transfer)
{
        enum dma_ctrl_flags flags = DMA_CTRL_ACK | DMA_PREP_INTERRUPT;
        struct dma_async_tx_descriptor *chan_desc;
        struct dma_device *dma_device = proxy_chan->chan->device;
        int ret;

        proxy_chan->src_port.addr = transfer->src;
        proxy_chan->dst_port.addr = transfer->dst;
        proxy_chan->src_port.type = transfer->src_type;
        proxy_chan->dst_port.type = transfer->dst_type;
        proxy_chan->src_port.length = transfer->length;
        proxy_chan->dst_port.length = transfer->length;
        proxy_chan->src_port.num_pages = 0;
        proxy_chan->dst_port.num_pages = 0;
        proxy_chan->src_port.pages = NULL;
        proxy_chan->dst_port.pages = NULL;

        // Fail-safe: By default use the buffers allocated in the channel
        proxy_chan->src_port.phys_addr = proxy_chan->buffer_phys_addrs[0];
        proxy_chan->dst_port.phys_addr = proxy_chan->buffer_phys_addrs[1];

        ret = prepare_idma_port_for_type(&proxy_chan->src_port);
        if (ret)
        {
                pr_err("Failed to prepare source port for type %d\n", transfer->src_type);
                return ret;
        }
        ret = prepare_idma_port_for_type(&proxy_chan->dst_port);
        if (ret)
        {
                pr_err("Failed to prepare destination port for type %d\n", transfer->dst_type);
                return ret;
        }
        pr_debug("Prepared source address %lx (phys: %llx) and destination address %lx (phys: %llx)\n",
                transfer->src, proxy_chan->src_port.phys_addr, transfer->dst, proxy_chan->dst_port.phys_addr);

        chan_desc = dma_device->device_prep_dma_memcpy(proxy_chan->chan,
                                                       proxy_chan->dst_port.phys_addr,
                                                       proxy_chan->src_port.phys_addr,
                                                       transfer->length, flags);
        if (!chan_desc)
        {
                pr_err("Failed to prepare DMA memcpy transfer\n");
                return -ENOMEM;
        }
        pr_debug("Prepared DMA memcpy transfer with length %zu\n", transfer->length);
        wmb();

        chan_desc->callback = idma_proxy_callback;
        chan_desc->callback_param = &proxy_chan->cmp;

        init_completion(&proxy_chan->cmp);

        proxy_chan->cookie = chan_desc->tx_submit(chan_desc);
        if (dma_submit_error(proxy_chan->cookie))
        {
                pr_err("Failed to submit DMA memcpy transfer\n");
                return proxy_chan->cookie;
        }
        pr_debug("Submitted DMA memcpy transfer with cookie %d\n", proxy_chan->cookie);
        dma_async_issue_pending(proxy_chan->chan);
        return 0;
}

/**
 * @brief: Waits for the iDMA transfer to complete and cleans up resources
 *
 * This function waits for the DMA transfer to complete, checks the status,
 * and destructs the source and destination ports.
 *
 * @param proxy_chan: Pointer to the idma_proxy_channel used for the transfer
 * @param transfer: Pointer to the idma_memcpy_transfer structure containing transfer parameters
 * @return: 0 on success, negative error code on failure
 */
static int wait_for_idma_transfer(struct idma_proxy_channel *proxy_chan, struct idma_memcpy_transfer *transfer)
{
        enum dma_status status;
        int ret;

        if (!wait_for_completion_timeout(&proxy_chan->cmp, msecs_to_jiffies(5000)))
        {
                pr_err("Timeout waiting for DMA transfer to complete\n");
                return -ETIMEDOUT;
        }

        status = dma_async_is_tx_complete(proxy_chan->chan, proxy_chan->cookie, NULL, NULL);

        if (status != DMA_COMPLETE)
        {
                pr_err("DMA transfer failed with status %d\n", status);
                return -EIO;
        }
        pr_debug("DMA transfer completed successfully with cookie %d\n", proxy_chan->cookie);

        // Free the buffers if they were allocated
        ret = destruct_idma_port_for_type(&proxy_chan->src_port);
        if (ret)
        {
                pr_err("Failed to destruct source port for type %d\n", transfer->src_type);
                return ret;
        }
        ret = destruct_idma_port_for_type(&proxy_chan->dst_port);
        if (ret)
        {
                pr_err("Failed to destruct destination port for type %d\n", transfer->dst_type);
                return ret;
        }
        pr_debug("Destructed source and destination ports successfully\n");
        // Reset the completion for the next transfer
        init_completion(&proxy_chan->cmp);
        proxy_chan->cookie = 0;
        return 0;
}

/**
 * @brief: Opens the iDMA proxy device
 *
 * This function is called when the iDMA proxy device is opened.
 * It sets the private data for the file to the idma_proxy_channel.
 *
 * @param inode: Pointer to the inode of the device
 * @param filp: Pointer to the file structure for the device
 * @return: 0 on success, negative error code on failure
 */
static int idma_proxy_open(struct inode *inode, struct file *filp)
{
        struct idma_proxy_channel *proxy_chan = container_of(inode->i_cdev, struct idma_proxy_channel, cdev);
        filp->private_data = proxy_chan;
        pr_debug("Opened iDMA proxy device\n");
        return 0;
}

/**
 * @brief: Releases the iDMA proxy device
 *
 * This function is called when the iDMA proxy device is released.
 * It resets the private data for the file to NULL.
 *
 * @param inode: Pointer to the inode of the device
 * @param filp: Pointer to the file structure for the device
 * @return: 0 on success, negative error code on failure
 */
static int idma_proxy_release(struct inode *inode, struct file *filp)
{
        // struct idma_proxy_channel *proxy_chan = filp->private_data;
        pr_debug("Released iDMA proxy device\n");
        filp->private_data = NULL;

        // TODO: Free resources if necessary
        return 0;
}

/**
 * @brief: Memory mapping function for the iDMA proxy device
 *
 * This function maps the kernel buffers for source and destination to user space.
 * It checks if the requested size matches the expected size and returns an error if not.
 *
 * @param filp: Pointer to the file structure for the device
 * @param vma: Pointer to the vm_area_struct representing the memory area to map
 * @return: 0 on success, negative error code on failure
 */
static int idma_proxy_mmap(struct file *filp, struct vm_area_struct *vma)
{
        struct idma_proxy_channel *proxy_chan = filp->private_data;
        unsigned long size = vma->vm_end - vma->vm_start;

        if (size != KERNEL_BUFFER_SIZE << 1)
        {
                pr_err("Requested size %lu does not match kernel buffer sizes (src & dst) %d\n", size, KERNEL_BUFFER_SIZE << 1);
                return -EINVAL;
        }

        return dma_mmap_coherent(proxy_chan->dev, vma,
                                 proxy_chan->buffers[0], proxy_chan->buffer_phys_addrs[0],
                                 KERNEL_BUFFER_SIZE << 1);
}

/**
 * @brief: IOCTL handler for the iDMA proxy device
 *
 * This function handles IOCTL commands for the iDMA proxy device.
 *
 * @param filp: Pointer to the file structure for the device
 * @param cmd: The IOCTL command
 * @param arg: The argument for the IOCTL command
 * @return: 0 on success, negative error code on failure
 */
static long idma_proxy_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
        struct idma_proxy_channel *proxy_chan = filp->private_data;
        int ret;

        switch (cmd)
        {
        case IOCTL_ISSUE_MEMCPY_TRANSFER:
        {
                idma_memcpy_transfer_t transfer;
                if (copy_from_user(&transfer, (const void __user *)arg, sizeof(transfer)))
                {
                        pr_err("Error when copying transfer request from user space\n");
                        return -EFAULT;
                }

                if (transfer.length <= 0)
                {
                        pr_err("Transfer length must be greater than 0\n");
                        return -EINVAL;
                }

                ret = start_idma_transfer(proxy_chan, &transfer);
                if (ret)
                {
                        pr_err("Failed to start iDMA transfer: %d\n", ret);
                        return ret;
                }
                ret = wait_for_idma_transfer(proxy_chan, &transfer);
                if (ret)
                {
                        pr_err("Failed to wait for iDMA transfer: %d\n", ret);
                        return ret;
                }
                pr_debug("iDMA transfer completed successfully\n");
                break;
        }
        default:
        {
                pr_warn("Unhandled IOCTL command\n");
                break;
        }
        }
        return 0;
}

static struct file_operations idma_proxy_fops = {
    .owner = THIS_MODULE,
    .open = idma_proxy_open,
    .release = idma_proxy_release,
    .unlocked_ioctl = idma_proxy_ioctl,
    .mmap = idma_proxy_mmap,
};

/**
 * @brief: Initializes a channel for the iDMA proxy device
 *
 * This function initializes a channel for the iDMA proxy device by requesting a DMA channel,
 * allocating kernel buffers, and creating a character device for user space access.
 *
 * @param idma_proxy_dev: Pointer to the idma_proxy_device structure representing the iDMA proxy device
 * @param i: The index of the channel to initialize
 *
 * @return: 0 on success, negative error code on failure
 */
static int idma_proxy_init_channel(struct idma_proxy_device *idma_proxy_dev, int i)
{
        struct idma_proxy_channel *proxy_chan = &idma_proxy_dev->channels[i];
        char *name = idma_proxy_dev->names[i];
        void *buffer_ptr;
        int ret;

        proxy_chan->dev = idma_proxy_dev->dev;
        proxy_chan->chan = dma_request_chan(proxy_chan->dev, name);
        if (IS_ERR(proxy_chan->chan))
        {
                pr_err("Failed to request DMA channel %d: %ld\n", i, PTR_ERR(proxy_chan->chan));
                return PTR_ERR(proxy_chan->chan);
        }

        cdev_init(&proxy_chan->cdev, &idma_proxy_fops);
        proxy_chan->cdev.owner = THIS_MODULE;
        ret = cdev_add(&proxy_chan->cdev, idma_proxy_dev->device_num_base + i, 1);
        if (ret)
        {
                pr_err("Failed to add cdev for channel %d\n", i);
                return ret;
        }

        proxy_chan->chr_dev = device_create(idma_proxy_dev->device_class, NULL, idma_proxy_dev->device_num_base + i, NULL, name);
        if (IS_ERR(proxy_chan->chr_dev))
        {
                pr_err("Failed to create device for idma\n");
                cdev_del(&proxy_chan->cdev);
                return PTR_ERR(proxy_chan->chr_dev);
        }

        buffer_ptr = (struct idma_proxy_kernel_buffer *)
            dmam_alloc_coherent(proxy_chan->dev,
                                sizeof(struct idma_proxy_kernel_buffer) * 2,
                                &proxy_chan->buffer_phys_addrs[0], GFP_KERNEL);

        if (!buffer_ptr)
        {
                pr_err("Failed to allocate memory for channel %d buffers\n", i);
                device_destroy(idma_proxy_dev->device_class, idma_proxy_dev->device_num_base + i);
                cdev_del(&proxy_chan->cdev);
                return -ENOMEM;
        }

        proxy_chan->buffers[0] = (struct idma_proxy_kernel_buffer *)buffer_ptr;
        // proxy_chan->buffer_phys_addrs[0] = virt_to_phys(proxy_chan->buffers[0]);
        proxy_chan->buffers[1] = proxy_chan->buffers[0] + 1;
        proxy_chan->buffer_phys_addrs[1] = proxy_chan->buffer_phys_addrs[0] + sizeof(struct idma_proxy_kernel_buffer);

        pr_info("Allocated src_buffer for channel %d at %lx with phys addr %lx\n",
                i, (unsigned long)proxy_chan->buffers[0], (unsigned long)proxy_chan->buffer_phys_addrs[0]);
        pr_info("Allocated dst_buffer for channel %d at %lx with phys addr %lx\n",
                i, (unsigned long)proxy_chan->buffers[1], (unsigned long)proxy_chan->buffer_phys_addrs[1]);

        return 0;
}

/**
 * @brief: Destructs a channel for the iDMA proxy device
 *
 * This function destructs a channel for the iDMA proxy device by freeing resources,
 * destroying the character device, and releasing the DMA channel.
 *
 * @param idma_proxy_dev: Pointer to the idma_proxy_device structure representing the iDMA proxy device
 * @param i: The index of the channel to destruct
 */
static void idma_proxy_destruct_channel(struct idma_proxy_device *idma_proxy_dev, int i)
{
        struct idma_proxy_channel *proxy_chan = &idma_proxy_dev->channels[i];

        if (proxy_chan->chr_dev)
        {
                device_destroy(idma_proxy_dev->device_class, idma_proxy_dev->device_num_base + i);
                proxy_chan->chr_dev = NULL;
        }
        cdev_del(&proxy_chan->cdev);
        if (proxy_chan->buffers[0])
        {
                dmam_free_coherent(proxy_chan->dev, sizeof(struct idma_proxy_kernel_buffer) * 2,
                                   proxy_chan->buffers[0], proxy_chan->buffer_phys_addrs[0]);
                proxy_chan->buffers[0] = NULL;
        }
        if (proxy_chan->chan)
        {
                dma_release_channel(proxy_chan->chan);
                proxy_chan->chan = NULL;
        }
}

/**
 * @brief: Probes the iDMA proxy platform device
 *
 * This function is called when the iDMA proxy platform device is probed.
 * It initializes the device, allocates resources, and sets up channels.
 *
 * @param pdev: Pointer to the platform device structure
 * @return: 0 on success, negative error code on failure
 */
static int idma_proxy_platform_probe(struct platform_device *pdev)
{
        struct device *dev = &pdev->dev;
        struct idma_proxy_device *idma_proxy_dev;
        int i, ret;

        pr_info("Probing IIS iDMA proxy platform device %s\n", dev_name(dev));

        idma_proxy_dev = (struct idma_proxy_device *)devm_kzalloc(dev, sizeof(struct idma_proxy_device), GFP_KERNEL);
        if (!idma_proxy_dev)
                return -ENOMEM;

        idma_proxy_dev->dev = &pdev->dev;
        platform_set_drvdata(pdev, idma_proxy_dev);

        idma_proxy_dev->n_channels = device_property_read_string_array(&pdev->dev,
                                                                       "dma-names", NULL, 0);

        if (idma_proxy_dev->n_channels <= 0)
        {
                pr_err("No channels found in device tree\n");
                return -EINVAL;
        }

        idma_proxy_dev->names = devm_kmalloc_array(dev, idma_proxy_dev->n_channels, sizeof(char *), GFP_KERNEL);
        if (!idma_proxy_dev->names)
                return -ENOMEM;

        ret = device_property_read_string_array(&pdev->dev, "dma-names",
                                                (const char **)idma_proxy_dev->names, idma_proxy_dev->n_channels);
        // should not fail, but check anyway
        if (ret < 0)
                goto of_dma_names_read_failed;

        ret = alloc_chrdev_region(&idma_proxy_dev->device_num_base, 0, idma_proxy_dev->n_channels, "idma_proxy");
        if (ret)
        {
                pr_err("Cannot allocate chrdev\n");
                goto alloc_chrdev_region_failed;
        }

        idma_proxy_dev->device_class = class_create("idma_class");
        if (IS_ERR(idma_proxy_dev->device_class))
        {
                pr_err("Cannot create device class\n");
                ret = PTR_ERR(idma_proxy_dev->device_class);
                goto unregister_chrdev;
        }

        idma_proxy_dev->channels = devm_kcalloc(dev, idma_proxy_dev->n_channels, sizeof(struct idma_proxy_channel), GFP_KERNEL);
        if (!idma_proxy_dev->channels)
        {
                pr_err("Failed to allocate memory for channels\n");
                ret = -ENOMEM;
                goto allocate_channels_failed;
        }

        for (i = 0; i < idma_proxy_dev->n_channels; ++i)
        {
                ret = idma_proxy_init_channel(idma_proxy_dev, i);
                if (ret)
                {
                        pr_err("Failed to initialize channel %d\n", i);
                        goto channel_init_failed;
                }
        }

        return 0;
channel_init_failed:
        for (i -= 1; i >= 0; --i)
                idma_proxy_destruct_channel(idma_proxy_dev, i);
allocate_channels_failed:
        class_destroy(idma_proxy_dev->device_class);
        idma_proxy_dev->device_class = NULL;
unregister_chrdev:
        unregister_chrdev_region(idma_proxy_dev->device_num_base, idma_proxy_dev->n_channels);
        idma_proxy_dev->device_num_base = 0;
alloc_chrdev_region_failed:
of_dma_names_read_failed:
        pr_err("Failed to probe iDMA proxy platform device %s\n", dev_name(dev));
        return ret;
}

/**
 * @brief: Removes the iDMA proxy platform device
 *
 * This function is called when the iDMA proxy platform device is removed.
 * It cleans up resources, destroys channels, and unregisters the character devices.
 *
 * @param pdev: Pointer to the platform device structure
 * @return: 0 on success, negative error code on failure
 */
static int idma_platform_remove(struct platform_device *pdev)
{
        struct idma_proxy_device *idma_proxy_dev = platform_get_drvdata(pdev);
        int i;

        if (!idma_proxy_dev)
        {
                pr_err("No iDMA proxy device found\n");
                return -ENODEV;
        }

        for (i = 0; i < idma_proxy_dev->n_channels; ++i)
                idma_proxy_destruct_channel(idma_proxy_dev, i);

        if (idma_proxy_dev->device_class)
                class_destroy(idma_proxy_dev->device_class);

        if (idma_proxy_dev->device_num_base)
                unregister_chrdev_region(idma_proxy_dev->device_num_base, idma_proxy_dev->n_channels);
        idma_proxy_dev->device_num_base = 0;

        pr_info("Removed IIS iDMA proxy platform device %s successfully\n", dev_name(&pdev->dev));
        return 0;
}

static const struct of_device_id idma_match[] = {
    {.compatible = "eth,idma-proxy"},
    {/* sentinel */},
};
MODULE_DEVICE_TABLE(of, idma_match);

static struct platform_driver idma_proxy_platform_driver = {
    .probe = idma_proxy_platform_probe,
    .remove = idma_platform_remove,
    .driver = {
        .name = "iis-idma-proxy",
        .of_match_table = idma_match,
    },
};
module_platform_driver(idma_proxy_platform_driver);

MODULE_DESCRIPTION("IIS iDMA DMA proxy driver");
MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Kevin Schaerer <schkevin@student.ethz.ch>");
MODULE_VERSION("0.1");
