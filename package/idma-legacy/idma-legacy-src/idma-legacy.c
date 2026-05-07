// SPDX-License-Identifier: GPL-2.0-only

#include <linux/cdev.h>
#include <linux/compat.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/sizes.h>
#include <linux/uaccess.h>
#include <linux/version.h>
#if IS_ENABLED(CONFIG_OF)
#include <linux/of.h>
#include <linux/of_address.h>
#endif

#define DEBUG 1

#include "idma-legacy-dma.h"
#include "idma-legacy.h"

#define IDMA_LEGACY_NAME "idma_legacy"
#define IDMA_LEGACY_ABI_VERSION 1u

/* Upper bound until real DMA path exists (tune per hardware). */
#define IDMA_LEGACY_MEMCPY_MAX_SZ SZ_64M

/* Per-transfer completion wait (ns) for idma_legacy_submit_and_wait. */
#define IDMA_LEGACY_XFER_TIMEOUT_NS (30LL * NSEC_PER_SEC)

/* DT: soc idma@1000000 — compatible "eth,idma-engine", first reg bank (e.g.
 * cheshire.dtsi). */
#define IDMA_LEGACY_OF_COMPAT "eth,idma-engine"

static void __iomem *idma_legacy_mmio;

#if IS_ENABLED(CONFIG_OF)
static int idma_legacy_map_mmio(void) {
  struct device_node *np;
  struct resource res;

  np = of_find_compatible_node(NULL, NULL, IDMA_LEGACY_OF_COMPAT);
  if (!np)
    return -ENODEV;

  if (of_address_to_resource(np, 0, &res) == 0)
    pr_info("idma-legacy: iDMA regs %pR (from device tree)\n", &res);

  idma_legacy_mmio = of_iomap(np, 0);
  of_node_put(np);

  if (!idma_legacy_mmio)
    return -ENOMEM;

  return 0;
}

static void idma_legacy_unmap_mmio(void) {
  if (idma_legacy_mmio) {
    iounmap(idma_legacy_mmio);
    idma_legacy_mmio = NULL;
  }
}
#else
static int idma_legacy_map_mmio(void) {
  pr_warn("idma-legacy: CONFIG_OF disabled, no register block\n");
  return 0;
}

static void idma_legacy_unmap_mmio(void) {}
#endif

/**
 * Program iDMA in Cheshire register mode and block until DONE matches transfer
 * id. Sequence matches sw/include/dif/dma.h NAME##_dma_memcpy /
 * NAME##_dma_blk_memcpy.
 *
 * @dst_addr / @src_addr: addresses the DMA engine can use (e.g. bus/physical).
 */
static int idma_legacy_submit_and_wait(void __iomem *regs, u64 dst_addr,
                                       u64 src_addr, u64 len, u32 conf) {
  ktime_t deadline = ktime_add_ns(ktime_get(), IDMA_LEGACY_XFER_TIMEOUT_NS);
  u64 xfer_id;

  if (!regs)
    return -ENODEV;

  if (len == 0)
    return 0;

  pr_info("idma-legacy: submitting transfer %llu bytes from %llx to %llx\n",
           len, src_addr, dst_addr);
  pr_info("idma-legacy: writing to register addresses:\n"
           "src_addr: %llx\n"
           "dst_addr: %llx\n"
           "len_addr: %llx\n"
           "conf: %llx\n",
           DMA_SRC_ADDR(regs), DMA_DST_ADDR(regs), DMA_NUMBYTES_ADDR(regs), conf);

  writeq(src_addr, DMA_SRC_ADDR(regs));
  writeq(dst_addr, DMA_DST_ADDR(regs));
  writeq(len, DMA_NUMBYTES_ADDR(regs));
  wmb();
  writel(conf, DMA_CONF_ADDR(regs));
  wmb();


  xfer_id = readw(DMA_NEXT_ID_ADDR(regs));

  for (;;) {
    if (readw_relaxed(DMA_DONE_ADDR(regs)) == xfer_id)
      return 0;

    if (ktime_after(ktime_get(), deadline))
      return -ETIMEDOUT;

    cpu_relax();
  }

  return 0;
}

/**
 * legacy_dma_memcpy - copy user→user via iDMA, one pinned page pair per submit.
 */
static int legacy_dma_memcpy(void __iomem *regs, void __user *udst,
                             void __user *usrc, size_t size) {
  const u32 conf = DMA_CONF_DECOUPLE_NONE;
  size_t done = 0;

  if (!regs)
    return -ENODEV;

  while (done < size) {
    unsigned long s_addr = (unsigned long)usrc + done;
    unsigned long d_addr = (unsigned long)udst + done;
    size_t src_left = PAGE_SIZE - (s_addr & (PAGE_SIZE - 1));
    size_t dst_left = PAGE_SIZE - (d_addr & (PAGE_SIZE - 1));
    size_t chunk = size - done;
    struct page *src_pg, *dst_pg;
    phys_addr_t src_pa, dst_pa;
    long n;
    int ret;

    if (chunk > src_left)
      chunk = src_left;
    if (chunk > dst_left)
      chunk = dst_left;

    n = pin_user_pages_fast(s_addr, 1, 0, &src_pg);
    if (n != 1) {
      if (n < 0)
        return n;
      return -EFAULT;
    }

    n = pin_user_pages_fast(d_addr, 1, FOLL_WRITE, &dst_pg);
    if (n != 1) {
      unpin_user_page(src_pg);
      if (n < 0)
        return n;
      return -EFAULT;
    }

    src_pa = page_to_phys(src_pg) + (s_addr & (PAGE_SIZE - 1));
    dst_pa = page_to_phys(dst_pg) + (d_addr & (PAGE_SIZE - 1));

    ret = idma_legacy_submit_and_wait(regs, (u64)dst_pa, (u64)src_pa, chunk,
                                      conf);

    if (ret == 0)
      set_page_dirty_lock(dst_pg);

    unpin_user_page(dst_pg);
    unpin_user_page(src_pg);

    if (ret)
      return ret;

    done += chunk;
  }

  return 0;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
#define idma_legacy_class_create(nm) class_create(nm)
#else
#define idma_legacy_class_create(nm) class_create(THIS_MODULE, nm)
#endif

static dev_t idma_legacy_devt;
static struct cdev idma_legacy_cdev;
static struct class *idma_legacy_class;
static struct device *idma_legacy_device;

static long idma_legacy_ioctl(struct file *filp, unsigned int cmd,
                              unsigned long arg) {
  switch (cmd) {
  case IDMA_LEGACY_IOC_GET_VERSION: {
    u32 ver = IDMA_LEGACY_ABI_VERSION;

    if (copy_to_user((void __user *)arg, &ver, sizeof(ver)))
      return -EFAULT;
    return 0;
  }
  case IDMA_LEGACY_IOC_MEMCPY: {
    struct idma_legacy_memcpy_args args;
    void __user *udst;
    void __user *usrc;
    size_t len;

    if (copy_from_user(&args, (void __user *)arg, sizeof(args)))
      return -EFAULT;

    if (!args.size)
      return 0;

    if (args.size > IDMA_LEGACY_MEMCPY_MAX_SZ)
      return -EINVAL;

    len = (size_t)args.size;
    udst = u64_to_user_ptr(args.dst);
    usrc = u64_to_user_ptr(args.src);

    if (!access_ok(usrc, len) || !access_ok(udst, len))
      return -EFAULT;

    return legacy_dma_memcpy(idma_legacy_mmio, udst, usrc, len);
  }
  default:
    return -ENOTTY;
  }
}

static const struct file_operations idma_legacy_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = idma_legacy_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
};

static void idma_legacy_chrdev_cleanup(void) {
  if (idma_legacy_device) {
    device_destroy(idma_legacy_class, idma_legacy_devt);
    idma_legacy_device = NULL;
  }
  if (idma_legacy_class) {
    class_destroy(idma_legacy_class);
    idma_legacy_class = NULL;
  }
  cdev_del(&idma_legacy_cdev);
  unregister_chrdev_region(idma_legacy_devt, 1);
}

static int __init idma_legacy_init(void) {
  int ret;

  ret = idma_legacy_map_mmio();
  if (ret)
    return ret;

  ret = alloc_chrdev_region(&idma_legacy_devt, 0, 1, IDMA_LEGACY_NAME);
  if (ret)
    goto err_unmap;

  cdev_init(&idma_legacy_cdev, &idma_legacy_fops);
  idma_legacy_cdev.owner = THIS_MODULE;

  ret = cdev_add(&idma_legacy_cdev, idma_legacy_devt, 1);
  if (ret)
    goto err_cdev;

  idma_legacy_class = idma_legacy_class_create(IDMA_LEGACY_NAME);
  if (IS_ERR(idma_legacy_class)) {
    ret = PTR_ERR(idma_legacy_class);
    idma_legacy_class = NULL;
    goto err_cdev;
  }

  idma_legacy_device = device_create(idma_legacy_class, NULL, idma_legacy_devt,
                                     NULL, IDMA_LEGACY_NAME);
  if (IS_ERR(idma_legacy_device)) {
    ret = PTR_ERR(idma_legacy_device);
    idma_legacy_device = NULL;
    goto err_class;
  }

  return 0;

err_class:
  if (idma_legacy_class) {
    class_destroy(idma_legacy_class);
    idma_legacy_class = NULL;
  }
err_cdev:
  cdev_del(&idma_legacy_cdev);
  unregister_chrdev_region(idma_legacy_devt, 1);
err_unmap:
  idma_legacy_unmap_mmio();
  return ret;
}

static void __exit idma_legacy_exit(void) {
  idma_legacy_chrdev_cleanup();
  idma_legacy_unmap_mmio();
}

module_init(idma_legacy_init);
module_exit(idma_legacy_exit);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("idma-legacy (skeleton)");
