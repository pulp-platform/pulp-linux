// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
#include <linux/init.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/platform_device.h>
#include <linux/of_address.h>
#include <linux/io.h>
#include <linux/iopoll.h>
#include <linux/device.h>

#include "axi_llc_regs.h"
#include "tagger_regs.h"

struct llc {
	void __iomem *regs;
	void __iomem *tagger_regs;
	struct dentry *debugfs;
};

static u64 combine(u32 low, u32 high)
{
	return ((u64)high << 32) | low;
}

static void llc_print_config(struct device *dev)
{
	u64 set_asso, num_lines, num_blocks, version, spm, status, bist;
	struct llc *llc = dev_get_drvdata(dev);

	set_asso = combine(readl(llc->regs + AXI_LLC_SET_ASSO_LOW_REG_OFFSET),
			   readl(llc->regs + AXI_LLC_SET_ASSO_HIGH_REG_OFFSET));
	num_lines =
		combine(readl(llc->regs + AXI_LLC_NUM_LINES_LOW_REG_OFFSET),
			readl(llc->regs + AXI_LLC_NUM_LINES_HIGH_REG_OFFSET));
	num_blocks =
		combine(readl(llc->regs + AXI_LLC_NUM_BLOCKS_LOW_REG_OFFSET),
			readl(llc->regs + AXI_LLC_NUM_BLOCKS_HIGH_REG_OFFSET));
	version = combine(readl(llc->regs + AXI_LLC_VERSION_LOW_REG_OFFSET),
			  readl(llc->regs + AXI_LLC_VERSION_HIGH_REG_OFFSET));

	dev_info(
		dev,
		"%llu ways, %llu lines/way, %llu blocks/line, version 0x%016llx\n",
		set_asso, num_lines, num_blocks, version);

	spm = combine(readl(llc->regs + AXI_LLC_CFG_SPM_LOW_REG_OFFSET),
		      readl(llc->regs + AXI_LLC_CFG_SPM_HIGH_REG_OFFSET));

	dev_info(dev, "spm config 0x%016llx\n", spm);

	int err = readl_poll_timeout(llc->regs + AXI_LLC_BIST_STATUS_REG_OFFSET,
				     status,
				     status & BIT(AXI_LLC_BIST_STATUS_DONE_BIT),
				     0, 1000);
	if (err) {
		dev_err(dev, "timeout waiting for bist\n");
		return;
	}

	bist = combine(readl(llc->regs + AXI_LLC_BIST_OUT_LOW_REG_OFFSET),
		       readl(llc->regs + AXI_LLC_BIST_OUT_HIGH_REG_OFFSET));

	dev_info(dev, "bist result 0x%016llx\n", bist);
}

static void llc_print_partitioning(struct device *dev)
{
	if (!of_device_is_compatible(dev->of_node,
				     "eth,axi-llc-partitioning")) {
		dev_info(dev, "partitioning feature not available\n");
		return;
	}
	dev_info(dev, "partitioning feature available\n");
}

static void llc_print_tagger(struct device *dev)
{
	u32 commit, addr_conf;

	struct llc *llc = dev_get_drvdata(dev);

	if (!llc->tagger_regs) {
		dev_info(dev, "no transaction tagger mapped\n");
		return;
	}


	addr_conf = readl(llc->tagger_regs + TAGGER_REG_ADDR_CONF_REG_OFFSET);
	dev_info(dev, "tagger: addr_conf = 0x%08x\n", addr_conf);

	for (int i = 0; i < TAGGER_REG_PAT_ADDR_MULTIREG_COUNT; i++) {
		u32 addr_reg =
			readl(llc->tagger_regs +
			      (TAGGER_REG_PAT_ADDR_0_REG_OFFSET + i * 4));

		/*
		 * pat_addr holds an address/size encoding; the HW spec defines
		 * the exact interpretation. Here we just print the raw value
		 * and a shifted version (assuming bottom 2 bits are dropped).
		 */
		dev_info(dev,
			 "tagger: pat_addr[%02d] = 0x%08x (base<<2=0x%08x)\n",
			 i, addr_reg, addr_reg << 2);
	}

	for (int i = 0; i < TAGGER_REG_PATID_MULTIREG_COUNT; i++) {
		u32 reg = readl(llc->tagger_regs +
				(TAGGER_REG_PATID_0_REG_OFFSET + i * 4));

		dev_info(dev, "tagger: patid[%d] = 0x%08x\n", i, reg);
	}
}

static ssize_t spm_config_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 spm;

	spm = combine(readl(llc->regs + AXI_LLC_CFG_SPM_LOW_REG_OFFSET),
		      readl(llc->regs + AXI_LLC_CFG_SPM_HIGH_REG_OFFSET));

	return sysfs_emit(buf, "0x%016llx\n", spm);
}

static ssize_t spm_config_store(struct device *dev,
				struct device_attribute *attr, const char *buf,
				size_t count)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 spm;
	int ret;

	ret = kstrtou64(buf, 0, &spm);
	if (ret)
		return ret;

	writel((u32)(spm & 0xffffffff),
	       llc->regs + AXI_LLC_CFG_SPM_LOW_REG_OFFSET);
	writel((u32)(spm >> 32), llc->regs + AXI_LLC_CFG_SPM_HIGH_REG_OFFSET);

	writel(BIT(AXI_LLC_COMMIT_CFG_COMMIT_BIT),
	       llc->regs + AXI_LLC_COMMIT_CFG_REG_OFFSET);

	return count;
}

static DEVICE_ATTR_RW(spm_config);

static ssize_t flush_config_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 flush;

	flush = combine(readl(llc->regs + AXI_LLC_CFG_FLUSH_LOW_REG_OFFSET),
			readl(llc->regs + AXI_LLC_CFG_FLUSH_HIGH_REG_OFFSET));

	return sysfs_emit(buf, "0x%016llx\n", flush);
}

static ssize_t flush_config_store(struct device *dev,
				  struct device_attribute *attr,
				  const char *buf, size_t count)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 flush;
	int ret;

	ret = kstrtou64(buf, 0, &flush);
	if (ret)
		return ret;

	writel((u32)(flush & 0xffffffff),
	       llc->regs + AXI_LLC_CFG_FLUSH_LOW_REG_OFFSET);
	writel((u32)(flush >> 32),
	       llc->regs + AXI_LLC_CFG_FLUSH_HIGH_REG_OFFSET);

	writel(BIT(AXI_LLC_COMMIT_CFG_COMMIT_BIT),
	       llc->regs + AXI_LLC_COMMIT_CFG_REG_OFFSET);

	return count;
}

static DEVICE_ATTR_RW(flush_config);

static ssize_t bist_result_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 bist = combine(readl(llc->regs + AXI_LLC_BIST_OUT_LOW_REG_OFFSET),
			   readl(llc->regs + AXI_LLC_BIST_OUT_HIGH_REG_OFFSET));

	return sysfs_emit(buf, "0x%016llx\n", bist);
}

static DEVICE_ATTR_RO(bist_result);

static ssize_t partitioning_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	return sysfs_emit(buf, "%d\n",
			  of_device_is_compatible(dev->of_node,
						  "eth,axi-llc-partitioning") ?
				  1 :
				  0);
}

static DEVICE_ATTR_RO(partitioning);

static ssize_t partitioning_config_show(struct device *dev,
					struct device_attribute *attr,
					char *buf)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 part;

	part = combine(
		readl(llc->regs + AXI_LLC_CFG_SET_PARTITION_LOW_0_REG_OFFSET),
		readl(llc->regs + AXI_LLC_CFG_SET_PARTITION_HIGH_0_REG_OFFSET));

	return sysfs_emit(buf, "0x%016llx\n", part);
}

static ssize_t partitioning_config_store(struct device *dev,
					 struct device_attribute *attr,
					 const char *buf, size_t count)
{
	struct llc *llc = dev_get_drvdata(dev);
	u64 part;
	int ret;

	ret = kstrtou64(buf, 0, &part);
	if (ret)
		return ret;

	writel((u32)(part & 0xffffffff),
	       llc->regs + AXI_LLC_CFG_SET_PARTITION_LOW_0_REG_OFFSET);
	writel((u32)(part >> 32),
	       llc->regs + AXI_LLC_CFG_SET_PARTITION_HIGH_0_REG_OFFSET);

	writel(BIT(AXI_LLC_COMMIT_PARTITION_CFG_COMMIT_BIT),
	       llc->regs + AXI_LLC_COMMIT_PARTITION_CFG_REG_OFFSET);

	return count;
}

static DEVICE_ATTR_RW(partitioning_config);


/* mode encoding is PMP-like: 0=OFF, 1=TOR, 2=NA4, 3=NAPOT.
 * We do a few cheap sanity checks before programming the HW.
 */
static int tagger_check_addr_mode(unsigned int mode, u64 addr)
{
	/* All modes: address must be 4-byte aligned (encoding uses >> 2). */
	if (addr & 0x3)
		return -EINVAL;

	switch (mode & 0x3) {
	case 0: /* OFF */
		/* For OFF, insist the address is 0 to avoid garbage config. */
		if (addr != 0)
			return -EINVAL;
		break;
	case 1: /* TOR */
		/* TOR uses an upper bound; allow any aligned addr. */
		break;
	case 2: /* NA4 */
		/* NA4 = single 4-byte word; aligned addr is enough here. */
		break;
	case 3: /* NAPOT */
		/*
		 * For NAPOT we at least require non-zero (otherwise it's
		 * indistinguishable from OFF-ish encodings). A proper size
		 * check would inspect the encoded bits, but we keep it simple.
		 */
		if (!addr)
			return -EINVAL;
		break;
	default:
		return -EINVAL;
	}

	return 0;
}

static ssize_t tagger_addr_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct llc *llc = dev_get_drvdata(dev);
	int i;
	ssize_t len = 0;

	if (!llc->tagger_regs)
		return sysfs_emit(buf, "no_tagger\n");

	for (i = 0; i < TAGGER_REG_PAT_ADDR_MULTIREG_COUNT; i++) {
		u32 addr_reg = readl(llc->tagger_regs +
				     (TAGGER_REG_PAT_ADDR_0_REG_OFFSET +
				      i * 4));
		u32 addr_conf = readl(llc->tagger_regs +
				      TAGGER_REG_ADDR_CONF_REG_OFFSET);
		unsigned int shift = (i % 16) * 2;
		unsigned int mode = (addr_conf >> shift) & 0x3;
		u64 base = (u64)addr_reg << 2;

		len += scnprintf(buf + len, PAGE_SIZE - len,
				 "%2d: mode=%u addr_reg=0x%08x base=0x%016llx\n",
				 i, mode, addr_reg, base);
		if (len >= PAGE_SIZE)
			break;
	}

	return len;
}

static ssize_t tagger_addr_store(struct device *dev,
				 struct device_attribute *attr,
				 const char *buf, size_t count)
{
	struct llc *llc = dev_get_drvdata(dev);
	unsigned int idx, mode;
	u64 addr;
	int ret;

	if (!llc->tagger_regs)
		return -ENODEV;

	/* Expected format: "<idx> <addr> <mode>" */
	ret = sscanf(buf, "%u %llx %u", &idx, &addr, &mode);
	if (ret != 3)
		return -EINVAL;

	if (idx >= TAGGER_REG_PAT_ADDR_MULTIREG_COUNT)
		return -EINVAL;

	ret = tagger_check_addr_mode(mode, addr);
	if (ret)
		return ret;

	/* Encode address for HW: drop lower 2 bits (PMP-style). */
	if (addr >> 34)	/* conservative: make sure it fits in 32 bits after >>2 */
		return -EINVAL;
	writel((u32)(addr >> 2),
	       llc->tagger_regs +
		       (TAGGER_REG_PAT_ADDR_0_REG_OFFSET + idx * 4));

	/* Update mode bits in addr_conf register (2 bits per entry). */
	{
		u32 addr_conf = readl(llc->tagger_regs +
				      TAGGER_REG_ADDR_CONF_REG_OFFSET);
		unsigned int shift = (idx % 16) * 2;
		u32 mask = 0x3u << shift;

		addr_conf = (addr_conf & ~mask) |
			    ((mode & 0x3u) << shift);
		writel(addr_conf,
		       llc->tagger_regs + TAGGER_REG_ADDR_CONF_REG_OFFSET);
	}

	return count;
}

static DEVICE_ATTR_RW(tagger_addr);



static struct attribute *llc_attrs[] = {
	&dev_attr_spm_config.attr,	    &dev_attr_flush_config.attr,
	&dev_attr_bist_result.attr,	    &dev_attr_partitioning.attr,
	&dev_attr_partitioning_config.attr, NULL, /* terminator */
};

static const struct attribute_group llc_group = {
	.attrs = llc_attrs,
};

static const struct attribute_group *llc_groups[] = {
	&llc_group,
	NULL,
};




static int llc_probe(struct platform_device *pdev)
{
	struct llc *llc;
	struct resource *res;
	struct device_node *tagger_np;
	struct resource tagger_res;
	int ret;

	struct device *dev = &pdev->dev;

	llc = devm_kzalloc(dev, sizeof(*llc), GFP_KERNEL);
	if (!llc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	llc->regs = devm_ioremap_resource(dev, res);
	if (IS_ERR(llc->regs))
		return PTR_ERR(llc->regs);

	llc->tagger_regs = NULL;

	tagger_np = of_parse_phandle(dev->of_node, "tagger", 0);
	if (!tagger_np) {
		dev_info(dev, "no 'tagger' phandle in device tree\n");
	} else {
		ret = of_address_to_resource(tagger_np, 0, &tagger_res);
		of_node_put(tagger_np);

		if (ret) {
			dev_warn(dev, "failed to get tagger resource: %d\n",
				 ret);
		} else {
			llc->tagger_regs =
				devm_ioremap_resource(dev, &tagger_res);
			if (IS_ERR(llc->tagger_regs)) {
				dev_warn(dev, "failed to map tagger regs\n");
				llc->tagger_regs = NULL;
			} else {
				dev_info(dev,
					 "mapped transaction tagger @ %pa\n",
					 &tagger_res.start);
			}
		}
	}

	platform_set_drvdata(pdev, llc);

	llc_print_config(dev);
	llc_print_partitioning(dev);
	llc_print_tagger(dev);

	return 0;
}

static int llc_remove(struct platform_device *pdev)
{
	return 0;
}

static const struct of_device_id llc_of_match[] = {
	{ .compatible = "eth,axi-llc-partitioning" },
	{ .compatible = "eth,axi-llc" },
	{ /* sentinel */ }
};

MODULE_DEVICE_TABLE(of, llc_of_match);

static struct platform_driver llc_driver = {
	.probe = llc_probe,
	.remove = llc_remove,
	.driver = {
		.name = "axi-llc-part",
		.of_match_table = llc_of_match,
		.dev_groups = llc_groups,
	},
};

module_platform_driver(llc_driver);

MODULE_DESCRIPTION("Cheshire AXI LLC partitioning driver");
MODULE_AUTHOR("Robert Balas <balasr@is.ee.ethz.ch>");
MODULE_LICENSE("Dual MIT/GPL");
