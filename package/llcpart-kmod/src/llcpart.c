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

struct llc {
	void __iomem *regs;
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

	llc = devm_kzalloc(&pdev->dev, sizeof(*llc), GFP_KERNEL);
	if (!llc)
		return -ENOMEM;

	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	llc->regs = devm_ioremap_resource(&pdev->dev, res);
	if (IS_ERR(llc->regs))
		return PTR_ERR(llc->regs);

	platform_set_drvdata(pdev, llc);

	llc_print_config(&pdev->dev);
	llc_print_partitioning(&pdev->dev);

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
