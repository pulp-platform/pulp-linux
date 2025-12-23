// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
/*
 * Cheshire iDMA (register frontend) bring-up driver
 *
 * - MMIO platform driver
 * - sysfs knobs to configure a transfer and submit it
 * - optional coherent-buffer selftest
 *
 * Register-frontend programming model:
 *   write params -> read next_id[stream] to launch -> poll done_id[stream]
 *
 * See iDMA register frontend docs. (Fill regs, read next_id to launch, done_id >= id => done)
 */

#include <linux/bitops.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/iopoll.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/sched.h>

#define DRV_NAME "cheshire-idma-reg"

/* --- Register map (64-bit, 2D register frontend as documented) --- */
#define IDMA_REG_CONF               0x00

#define IDMA_REG_STATUS(n)          (0x04 + 0x04 * (n))   /* n=0..15 */
#define IDMA_REG_NEXT_ID(n)         (0x44 + 0x04 * (n))   /* n=0..15 (READ triggers submit) */
#define IDMA_REG_DONE_ID(n)         (0x84 + 0x04 * (n))   /* n=0..15 */

#define IDMA_REG_DST_ADDR_LO        0xD0
#define IDMA_REG_DST_ADDR_HI        0xD4
#define IDMA_REG_SRC_ADDR_LO        0xD8
#define IDMA_REG_SRC_ADDR_HI        0xDC
#define IDMA_REG_LEN_LO             0xE0
#define IDMA_REG_LEN_HI             0xE4

/* Only present/meaningful on ND-enabled (e.g. 2D) frontends */
#define IDMA_REG_DST_STRIDE2_LO     0xE8
#define IDMA_REG_DST_STRIDE2_HI     0xEC
#define IDMA_REG_SRC_STRIDE2_LO     0xF0
#define IDMA_REG_SRC_STRIDE2_HI     0xF4
#define IDMA_REG_REPS2_LO           0xF8
#define IDMA_REG_REPS2_HI           0xFC

/* conf bits (assumes classic 2D layout: enable_nd is bit 10, protos at 13:11 and 16:14) */
#define IDMA_CONF_DECOUPLE_AW       BIT(0)
#define IDMA_CONF_DECOUPLE_RW       BIT(1)
#define IDMA_CONF_SRC_REDUCE_LEN    BIT(2)
#define IDMA_CONF_DST_REDUCE_LEN    BIT(3)
#define IDMA_CONF_SRC_MAX_LLEN_MASK GENMASK(6, 4)
#define IDMA_CONF_DST_MAX_LLEN_MASK GENMASK(9, 7)
#define IDMA_CONF_ENABLE_ND         BIT(10)
#define IDMA_CONF_SRC_PROTO_MASK    GENMASK(13, 11)
#define IDMA_CONF_DST_PROTO_MASK    GENMASK(16, 14)

/* status busy field is commonly bits [9:0] in the generated reg frontend */
#define IDMA_STATUS_BUSY_MASK       GENMASK(9, 0)

struct cheshire_idma_variant {
	bool has_2d;
};

struct cheshire_idma {
	void __iomem *regs;
	struct mutex lock;

	const struct cheshire_idma_variant *variant;

	u32 num_streams;
	u32 stream;          /* currently selected stream for sysfs ops */

	/* shadow “staged transfer” */
	u64 src_addr;
	u64 dst_addr;
	u64 length;
	u64 src_stride2;
	u64 dst_stride2;
	u64 reps2;


	/* stress / interference generator */
	struct {
		bool enabled;
		struct task_struct *task;

		/* one contiguous pool: [src | dst] */
		void *pool_cpu;
		dma_addr_t pool_dma;
		size_t pool_bytes;

		size_t half_bytes;
		u32 poll_us;
		u32 stream;

		u64 iters;
		u64 bytes_total;
	} stress;

	u32 last_id[16];
};

static bool selftest;
module_param(selftest, bool, 0644);
MODULE_PARM_DESC(selftest, "Run a simple coherent-buffer DMA copy selftest at probe time");

static inline void idma_write64(struct cheshire_idma *idma, u32 off_lo, u64 val)
{
	writel_relaxed(lower_32_bits(val), idma->regs + off_lo);
	writel_relaxed(upper_32_bits(val), idma->regs + off_lo + 4);
}

static inline u64 idma_read64(struct cheshire_idma *idma, u32 off_lo)
{
	u32 lo = readl_relaxed(idma->regs + off_lo);
	u32 hi = readl_relaxed(idma->regs + off_lo + 4);
	return ((u64)hi << 32) | lo;
}

static inline u32 idma_read_next_id(struct cheshire_idma *idma, u32 stream)
{
	/* Reading next_id[stream] launches the transfer and returns its ID */
	return readl_relaxed(idma->regs + IDMA_REG_NEXT_ID(stream));
}

static inline u32 idma_read_done_id(struct cheshire_idma *idma, u32 stream)
{
	return readl_relaxed(idma->regs + IDMA_REG_DONE_ID(stream));
}

static inline u32 idma_read_status(struct cheshire_idma *idma, u32 stream)
{
	return readl_relaxed(idma->regs + IDMA_REG_STATUS(stream));
}

static int idma_submit_locked(struct cheshire_idma *idma, u32 stream, u32 *out_id)
{
	u32 id;

	if (stream >= idma->num_streams)
		return -EINVAL;

	/* Program registers */
	idma_write64(idma, IDMA_REG_SRC_ADDR_LO, idma->src_addr);
	idma_write64(idma, IDMA_REG_DST_ADDR_LO, idma->dst_addr);
	idma_write64(idma, IDMA_REG_LEN_LO,      idma->length);

	if (idma->variant->has_2d) {
		idma_write64(idma, IDMA_REG_SRC_STRIDE2_LO, idma->src_stride2);
		idma_write64(idma, IDMA_REG_DST_STRIDE2_LO, idma->dst_stride2);
		idma_write64(idma, IDMA_REG_REPS2_LO,       idma->reps2);
	}

	/*
	 * Ensure all writes reach the device before the "kick" read.
	 * The frontend uses the next_id read as the submission trigger.
	 */
	wmb();

	id = idma_read_next_id(idma, stream);
	if (id == 0) {
		/*
		 * Per frontend spec: next_id returns 0 if transfer is not set up properly.
		 * (E.g., missing params / invalid config)
		 */
		return -EIO;
	}

	idma->last_id[stream] = id;
	if (out_id)
		*out_id = id;
	return 0;
}

static int idma_wait_done(struct cheshire_idma *idma, u32 stream, u32 id, u32 timeout_us)
{
	u32 done;
	int ret;

	ret = readl_poll_timeout(idma->regs + IDMA_REG_DONE_ID(stream),
				 done,
				 done >= id,
				 10, timeout_us);
	return ret;
}


/* iDMA stress workload using a kernel thread to reschedule transfers. This is
 * for benchmarking interference on caches/buses. */

/*
 * Submit a single transfer with explicit parameters.
 *
 * Note: parameter registers are global (shared by all streams), therefore any
 * concurrent users must serialize access (we reuse idma->lock for that).
 */
static int idma_submit_xfer_locked(struct cheshire_idma *idma, u32 stream,
				  u64 src, u64 dst, u64 len,
				  u64 src_stride2, u64 dst_stride2, u64 reps2,
				  u32 *out_id)
{
	u32 id;

	if (stream >= idma->num_streams)
		return -EINVAL;

	idma_write64(idma, IDMA_REG_SRC_ADDR_LO, src);
	idma_write64(idma, IDMA_REG_DST_ADDR_LO, dst);
	idma_write64(idma, IDMA_REG_LEN_LO,      len);

	if (idma->variant->has_2d) {
		idma_write64(idma, IDMA_REG_SRC_STRIDE2_LO, src_stride2);
		idma_write64(idma, IDMA_REG_DST_STRIDE2_LO, dst_stride2);
		idma_write64(idma, IDMA_REG_REPS2_LO,       reps2);
	}

	wmb();

	id = idma_read_next_id(idma, stream);
	if (id == 0)
		return -EIO;

	idma->last_id[stream] = id;
	if (out_id)
		*out_id = id;

	return 0;
}

static int idma_stress_thread(void *arg)
{
	struct cheshire_idma *idma = arg;

	/* Keep CPU disturbance low by running at low priority. */
	set_user_nice(current, 19);

	while (!kthread_should_stop()) {
		u32 id, done;
		u64 src, dst;
		size_t len;
		u32 stream, poll_us;
		int ret;

		mutex_lock(&idma->lock);
		if (!idma->stress.enabled) {
			mutex_unlock(&idma->lock);
			break;
		}

		stream  = idma->stress.stream;
		poll_us = idma->stress.poll_us;
		len     = idma->stress.half_bytes;

		src = (u64)idma->stress.pool_dma;
		dst = (u64)idma->stress.pool_dma + idma->stress.half_bytes;

		ret = idma_submit_xfer_locked(idma, stream, src, dst, len,
					      0, 0, 0, &id);
		mutex_unlock(&idma->lock);

		if (ret) {
			usleep_range(1000, 2000);
			continue;
		}

		/* Wait for completion with (optional) sleeping to avoid CPU hogging. */
		for (;;) {
			if (kthread_should_stop())
				return 0;

			done = idma_read_done_id(idma, stream);
			if (done >= id)
				break;

			if (poll_us)
				usleep_range(poll_us, poll_us + 20);
			else
				cpu_relax();
		}

		mutex_lock(&idma->lock);
		idma->stress.iters++;
		idma->stress.bytes_total += len;
		mutex_unlock(&idma->lock);
	}

	return 0;
}

static int idma_stress_start(struct device *dev, struct cheshire_idma *idma)
{
	size_t half, pool;

	if (idma->stress.enabled)
		return 0;

	half = idma->stress.half_bytes;
	if (half < 4096)
		return -EINVAL;
	if (half > ((~(size_t)0) / 2))
		return -EINVAL;

	if (idma->stress.stream >= idma->num_streams)
		return -EINVAL;

	pool = half * 2;

	idma->stress.pool_cpu = dma_alloc_coherent(dev, pool, &idma->stress.pool_dma,
						   GFP_KERNEL);
	if (!idma->stress.pool_cpu)
		return -ENOMEM;

	idma->stress.pool_bytes = pool;
	memset(idma->stress.pool_cpu, 0, pool);

	idma->stress.iters = 0;
	idma->stress.bytes_total = 0;

	idma->stress.enabled = true;
	idma->stress.task = kthread_run(idma_stress_thread, idma, "idma_stress");
	if (IS_ERR(idma->stress.task)) {
		int err = PTR_ERR(idma->stress.task);

		idma->stress.task = NULL;
		idma->stress.enabled = false;

		dma_free_coherent(dev, pool, idma->stress.pool_cpu, idma->stress.pool_dma);
		idma->stress.pool_cpu = NULL;
		idma->stress.pool_dma = 0;
		idma->stress.pool_bytes = 0;

		return err;
	}

	return 0;
}

/*
 * Stop the stress thread and wait for any outstanding DMA to complete before
 * freeing its backing buffer.
 *
 * Must NOT be called with idma->lock held (kthread_stop waits for the thread,
 * which also takes idma->lock).
 */
static void idma_stress_stop(struct device *dev, struct cheshire_idma *idma)
{
	struct task_struct *t;
	void *cpu;
	dma_addr_t dma;
	size_t bytes;
	u32 stream;
	u32 last;
	int ret;

	mutex_lock(&idma->lock);
	if (!idma->stress.task) {
		idma->stress.enabled = false;
		mutex_unlock(&idma->lock);
		return;
	}

	idma->stress.enabled = false;

	t = idma->stress.task;
	cpu = idma->stress.pool_cpu;
	dma = idma->stress.pool_dma;
	bytes = idma->stress.pool_bytes;
	stream = idma->stress.stream;
	last = idma->last_id[stream];
	mutex_unlock(&idma->lock);

	/* Ensure the thread stops submitting new work. */
	kthread_stop(t);

	/* Wait for the last submitted transfer to complete (best-effort). */
	if (last) {
		ret = idma_wait_done(idma, stream, last, 5 * 1000 * 1000);
		if (ret)
			dev_warn(dev, "stress: timeout waiting for stream%u id=%u (%d)\n",
				 stream, last, ret);
	}

	if (cpu)
		dma_free_coherent(dev, bytes, cpu, dma);

	mutex_lock(&idma->lock);
	idma->stress.task = NULL;
	idma->stress.pool_cpu = NULL;
	idma->stress.pool_dma = 0;
	idma->stress.pool_bytes = 0;
	mutex_unlock(&idma->lock);
}
static void idma_print_config(struct device *dev, struct cheshire_idma *idma)
{
	u32 conf = readl_relaxed(idma->regs + IDMA_REG_CONF);
	u32 src_max = (conf & IDMA_CONF_SRC_MAX_LLEN_MASK) >> 4;
	u32 dst_max = (conf & IDMA_CONF_DST_MAX_LLEN_MASK) >> 7;
	u32 src_proto = (conf & IDMA_CONF_SRC_PROTO_MASK) >> 11;
	u32 dst_proto = (conf & IDMA_CONF_DST_PROTO_MASK) >> 14;
	bool nd = !!(conf & IDMA_CONF_ENABLE_ND);
	unsigned int i;

	dev_info(dev, "iDMA reg-frontend @%p\n", idma->regs);
	dev_info(dev,
		 "conf=0x%08x (dec_aw=%d dec_rw=%d src_red=%d dst_red=%d src_max_llen=%u dst_max_llen=%u enable_nd=%d src_proto=%u dst_proto=%u)\n",
		 conf,
		 !!(conf & IDMA_CONF_DECOUPLE_AW),
		 !!(conf & IDMA_CONF_DECOUPLE_RW),
		 !!(conf & IDMA_CONF_SRC_REDUCE_LEN),
		 !!(conf & IDMA_CONF_DST_REDUCE_LEN),
		 src_max, dst_max, nd, src_proto, dst_proto);

	for (i = 0; i < idma->num_streams; i++) {
		u32 st = idma_read_status(idma, i);
		u32 done = idma_read_done_id(idma, i);
		u32 busy = st & IDMA_STATUS_BUSY_MASK;

		dev_info(dev, "stream%u: status=0x%08x busy=%u done_id=%u\n",
			 i, st, busy, done);
	}

	dev_info(dev, "addr/len regs now: src=0x%016llx dst=0x%016llx len=%llu\n",
		 (unsigned long long)idma_read64(idma, IDMA_REG_SRC_ADDR_LO),
		 (unsigned long long)idma_read64(idma, IDMA_REG_DST_ADDR_LO),
		 (unsigned long long)idma_read64(idma, IDMA_REG_LEN_LO));

	if (idma->variant->has_2d) {
		dev_info(dev, "2D regs now: src_stride2=%llu dst_stride2=%llu reps2=%llu\n",
			 (unsigned long long)idma_read64(idma, IDMA_REG_SRC_STRIDE2_LO),
			 (unsigned long long)idma_read64(idma, IDMA_REG_DST_STRIDE2_LO),
			 (unsigned long long)idma_read64(idma, IDMA_REG_REPS2_LO));
	}
}

static int idma_selftest(struct device *dev, struct cheshire_idma *idma)
{
	const size_t sz = 4096;
	dma_addr_t src_dma, dst_dma;
	u8 *src, *dst;
	u32 id;
	int ret;
	size_t i;

	src = dma_alloc_coherent(dev, sz, &src_dma, GFP_KERNEL);
	if (!src)
		return -ENOMEM;

	dst = dma_alloc_coherent(dev, sz, &dst_dma, GFP_KERNEL);
	if (!dst) {
		dma_free_coherent(dev, sz, src, src_dma);
		return -ENOMEM;
	}

	for (i = 0; i < sz; i++)
		src[i] = (u8)(i ^ 0xA5);
	memset(dst, 0, sz);

	mutex_lock(&idma->lock);

	idma->src_addr = (u64)src_dma;
	idma->dst_addr = (u64)dst_dma;
	idma->length   = (u64)sz;

	/* For 2D frontend: use 1D semantics by setting reps2=1 and strides=0 (or just leave 0). */
	idma->src_stride2 = 0;
	idma->dst_stride2 = 0;
	idma->reps2       = 0;

	ret = idma_submit_locked(idma, 0, &id);
	if (!ret)
		ret = idma_wait_done(idma, 0, id, 1000 * 1000); /* 1s */

	mutex_unlock(&idma->lock);

	if (ret) {
		dev_err(dev, "selftest: DMA submit/wait failed: %d\n", ret);
		goto out;
	}

	if (memcmp(src, dst, sz) != 0) {
		dev_err(dev, "selftest: data mismatch\n");
		ret = -EIO;
	} else {
		dev_info(dev, "selftest: OK (copied %zu bytes)\n", sz);
		ret = 0;
	}

out:
	dma_free_coherent(dev, sz, dst, dst_dma);
	dma_free_coherent(dev, sz, src, src_dma);
	return ret;
}

/* sysfs entries */

static struct cheshire_idma *dev_to_idma(struct device *dev)
{
	return dev_get_drvdata(dev);
}

#define IDMA_SYSFS_U64_RW(_name, _field)                                      \
static ssize_t _name##_show(struct device *dev,                               \
			    struct device_attribute *attr, char *buf)         \
{                                                                             \
	struct cheshire_idma *idma = dev_to_idma(dev);                         \
	u64 v;                                                                \
	mutex_lock(&idma->lock);                                              \
	v = idma->_field;                                                     \
	mutex_unlock(&idma->lock);                                            \
	return sysfs_emit(buf, "0x%016llx\n", (unsigned long long)v);          \
}                                                                             \
static ssize_t _name##_store(struct device *dev,                              \
			     struct device_attribute *attr,                  \
			     const char *buf, size_t count)                   \
{                                                                             \
	struct cheshire_idma *idma = dev_to_idma(dev);                         \
	u64 v;                                                                \
	if (kstrtoull(buf, 0, &v))                                            \
		return -EINVAL;                                               \
	mutex_lock(&idma->lock);                                              \
	idma->_field = v;                                                     \
	mutex_unlock(&idma->lock);                                            \
	return count;                                                         \
}                                                                             \
static DEVICE_ATTR_RW(_name)

IDMA_SYSFS_U64_RW(src_addr, src_addr);
IDMA_SYSFS_U64_RW(dst_addr, dst_addr);
IDMA_SYSFS_U64_RW(length,   length);
IDMA_SYSFS_U64_RW(src_stride2, src_stride2);
IDMA_SYSFS_U64_RW(dst_stride2, dst_stride2);
IDMA_SYSFS_U64_RW(reps2, reps2);

static ssize_t stream_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 v;

	mutex_lock(&idma->lock);
	v = idma->stream;
	mutex_unlock(&idma->lock);

	return sysfs_emit(buf, "%u\n", v);
}

static ssize_t stream_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 v;

	if (kstrtou32(buf, 0, &v))
		return -EINVAL;
	if (v >= idma->num_streams)
		return -EINVAL;

	mutex_lock(&idma->lock);
	idma->stream = v;
	mutex_unlock(&idma->lock);

	return count;
}
static DEVICE_ATTR_RW(stream);

static ssize_t conf_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 conf = readl_relaxed(idma->regs + IDMA_REG_CONF);

	return sysfs_emit(buf, "0x%08x\n", conf);
}

static ssize_t conf_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 conf;

	if (kstrtou32(buf, 0, &conf))
		return -EINVAL;

	mutex_lock(&idma->lock);
	writel_relaxed(conf, idma->regs + IDMA_REG_CONF);
	wmb();
	mutex_unlock(&idma->lock);

	return count;
}
static DEVICE_ATTR_RW(conf);

static ssize_t submit_store(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 one, id, stream;
	int ret;

	if (kstrtou32(buf, 0, &one))
		return -EINVAL;
	if (one != 1)
		return count;

	if (idma->stress.task)
		return -EBUSY;

	mutex_lock(&idma->lock);
	stream = idma->stream;
	ret = idma_submit_locked(idma, stream, &id);
	mutex_unlock(&idma->lock);

	if (ret)
		return ret;

	dev_info(dev, "submitted stream%u id=%u src=0x%016llx dst=0x%016llx len=%llu\n",
		 stream, id,
		 (unsigned long long)idma->src_addr,
		 (unsigned long long)idma->dst_addr,
		 (unsigned long long)idma->length);

	return count;
}
static DEVICE_ATTR_WO(submit);

static ssize_t last_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 stream, id;

	mutex_lock(&idma->lock);
	stream = idma->stream;
	id = idma->last_id[stream];
	mutex_unlock(&idma->lock);

	return sysfs_emit(buf, "%u\n", id);
}
static DEVICE_ATTR_RO(last_id);

static ssize_t done_id_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 stream, done;

	mutex_lock(&idma->lock);
	stream = idma->stream;
	done = idma_read_done_id(idma, stream);
	mutex_unlock(&idma->lock);

	return sysfs_emit(buf, "%u\n", done);
}
static DEVICE_ATTR_RO(done_id);

static ssize_t busy_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 stream, st, busy;

	mutex_lock(&idma->lock);
	stream = idma->stream;
	st = idma_read_status(idma, stream);
	mutex_unlock(&idma->lock);

	busy = st & IDMA_STATUS_BUSY_MASK;
	return sysfs_emit(buf, "%u\n", busy);
}
static DEVICE_ATTR_RO(busy);

static ssize_t wait_store(struct device *dev, struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 one, stream, id;
	int ret;

	if (kstrtou32(buf, 0, &one))
		return -EINVAL;
	if (one != 1)
		return count;

	mutex_lock(&idma->lock);
	stream = idma->stream;
	id = idma->last_id[stream];
	mutex_unlock(&idma->lock);

	if (id == 0)
		return -EINVAL;

	ret = idma_wait_done(idma, stream, id, 5 * 1000 * 1000); /* 5s */
	if (ret)
		return ret;

	return count;
}
static DEVICE_ATTR_WO(wait);


static ssize_t stress_enable_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "%u\n", idma->stress.enabled ? 1 : 0);
}

static ssize_t stress_enable_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 v;
	int ret = 0;

	if (kstrtou32(buf, 0, &v))
		return -EINVAL;

	if (v) {
		mutex_lock(&idma->lock);
		ret = idma_stress_start(dev, idma);
		mutex_unlock(&idma->lock);
	} else {
		idma_stress_stop(dev, idma);
	}

	return ret ? ret : count;
}
static DEVICE_ATTR_RW(stress_enable);

static ssize_t stress_half_bytes_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "%zu\n", idma->stress.half_bytes);
}

static ssize_t stress_half_bytes_store(struct device *dev,
				       struct device_attribute *attr,
				       const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	unsigned long v;

	if (kstrtoul(buf, 0, &v))
		return -EINVAL;

	mutex_lock(&idma->lock);
	if (idma->stress.enabled) {
		mutex_unlock(&idma->lock);
		return -EBUSY;
	}
	idma->stress.half_bytes = (size_t)v;
	mutex_unlock(&idma->lock);

	return count;
}
static DEVICE_ATTR_RW(stress_half_bytes);

static ssize_t stress_poll_us_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "%u\n", idma->stress.poll_us);
}

static ssize_t stress_poll_us_store(struct device *dev,
				    struct device_attribute *attr,
				    const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 v;

	if (kstrtou32(buf, 0, &v))
		return -EINVAL;

	mutex_lock(&idma->lock);
	idma->stress.poll_us = v;
	mutex_unlock(&idma->lock);

	return count;
}
static DEVICE_ATTR_RW(stress_poll_us);

static ssize_t stress_stream_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "%u\n", idma->stress.stream);
}

static ssize_t stress_stream_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u32 v;

	if (kstrtou32(buf, 0, &v))
		return -EINVAL;

	mutex_lock(&idma->lock);
	if (idma->stress.enabled) {
		mutex_unlock(&idma->lock);
		return -EBUSY;
	}
	if (v >= idma->num_streams) {
		mutex_unlock(&idma->lock);
		return -EINVAL;
	}
	idma->stress.stream = v;
	mutex_unlock(&idma->lock);

	return count;
}
static DEVICE_ATTR_RW(stress_stream);

static ssize_t stress_pool_dma_show(struct device *dev,
				    struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "0x%016llx\n",
			  (unsigned long long)idma->stress.pool_dma);
}
static DEVICE_ATTR_RO(stress_pool_dma);

static ssize_t stress_pool_bytes_show(struct device *dev,
				      struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "%zu\n", idma->stress.pool_bytes);
}
static DEVICE_ATTR_RO(stress_pool_bytes);

static ssize_t stress_src_dma_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "0x%016llx\n",
			  (unsigned long long)idma->stress.pool_dma);
}
static DEVICE_ATTR_RO(stress_src_dma);

static ssize_t stress_dst_dma_show(struct device *dev,
				   struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);

	return sysfs_emit(buf, "0x%016llx\n",
			  (unsigned long long)(idma->stress.pool_dma +
					       idma->stress.half_bytes));
}
static DEVICE_ATTR_RO(stress_dst_dma);

static ssize_t stress_iters_show(struct device *dev,
				 struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u64 v;

	mutex_lock(&idma->lock);
	v = idma->stress.iters;
	mutex_unlock(&idma->lock);

	return sysfs_emit(buf, "%llu\n", (unsigned long long)v);
}
static DEVICE_ATTR_RO(stress_iters);

static ssize_t stress_bytes_total_show(struct device *dev,
				       struct device_attribute *attr, char *buf)
{
	struct cheshire_idma *idma = dev_to_idma(dev);
	u64 v;

	mutex_lock(&idma->lock);
	v = idma->stress.bytes_total;
	mutex_unlock(&idma->lock);

	return sysfs_emit(buf, "%llu\n", (unsigned long long)v);
}
static DEVICE_ATTR_RO(stress_bytes_total);

static struct attribute *cheshire_idma_attrs[] = {
	&dev_attr_conf.attr,
	&dev_attr_stream.attr,

	&dev_attr_src_addr.attr,
	&dev_attr_dst_addr.attr,
	&dev_attr_length.attr,

	/* harmless even on 1D if you keep compatible=reg64-2d; guarded in submit */
	&dev_attr_src_stride2.attr,
	&dev_attr_dst_stride2.attr,
	&dev_attr_reps2.attr,

	&dev_attr_submit.attr,
	&dev_attr_wait.attr,

	&dev_attr_last_id.attr,
	&dev_attr_done_id.attr,
	&dev_attr_busy.attr,

	/* interference / stress mode */
	&dev_attr_stress_enable.attr,
	&dev_attr_stress_half_bytes.attr,
	&dev_attr_stress_poll_us.attr,
	&dev_attr_stress_stream.attr,
	&dev_attr_stress_pool_dma.attr,
	&dev_attr_stress_pool_bytes.attr,
	&dev_attr_stress_src_dma.attr,
	&dev_attr_stress_dst_dma.attr,
	&dev_attr_stress_iters.attr,
	&dev_attr_stress_bytes_total.attr,
	NULL,
};

static const struct attribute_group cheshire_idma_group = {
	.name  = "idma",
	.attrs = cheshire_idma_attrs,
};

static const struct attribute_group *cheshire_idma_groups[] = {
	&cheshire_idma_group,
	NULL,
};

/* ---------------- probe/remove ---------------- */

static const struct cheshire_idma_variant idma_var_reg64_2d = {
	.has_2d = true,
};

static const struct cheshire_idma_variant idma_var_reg64_1d = {
	.has_2d = false,
};

static const struct of_device_id cheshire_idma_of_match[] = {
	{ .compatible = "eth,idma-reg64-2d", .data = &idma_var_reg64_2d },
	{ .compatible = "eth,idma-reg64-1d", .data = &idma_var_reg64_1d },
	{ .compatible = "eth,idma-reg",      .data = &idma_var_reg64_2d }, /* default */
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, cheshire_idma_of_match);

static int cheshire_idma_probe(struct platform_device *pdev)
{
	struct cheshire_idma *idma;
	u32 nstreams = 1;
	int ret;

	idma = devm_kzalloc(&pdev->dev, sizeof(*idma), GFP_KERNEL);
	if (!idma)
		return -ENOMEM;

	mutex_init(&idma->lock);

	idma->variant = of_device_get_match_data(&pdev->dev);
	if (!idma->variant)
		idma->variant = &idma_var_reg64_2d;

	idma->regs = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(idma->regs))
		return PTR_ERR(idma->regs);

	/* How many streams to print/use (default 1, max 16) */
	if (device_property_read_u32(&pdev->dev, "dma-channels", &nstreams))
		nstreams = 1;
	if (nstreams == 0)
		nstreams = 1;
	if (nstreams > 16)
		nstreams = 16;

	idma->num_streams = nstreams;
	idma->stream = 0;

	idma->stress.half_bytes = 2 * 1024 * 1024;
	idma->stress.poll_us = 100;
	idma->stress.stream = 0;

	platform_set_drvdata(pdev, idma);

	idma_print_config(&pdev->dev, idma);

	ret = devm_device_add_groups(&pdev->dev, cheshire_idma_groups);
	if (ret)
		return ret;

	if (selftest) {
		ret = idma_selftest(&pdev->dev, idma);
		if (ret)
			dev_warn(&pdev->dev, "selftest failed (%d)\n", ret);
	}

	dev_info(&pdev->dev, "loaded (%u stream(s), %s)\n",
		 idma->num_streams,
		 idma->variant->has_2d ? "reg64-2d" : "reg64-1d");
	return 0;
}


static int cheshire_idma_remove(struct platform_device *pdev)
{
	struct cheshire_idma *idma = platform_get_drvdata(pdev);

	idma_stress_stop(&pdev->dev, idma);

	return 0;
}

static struct platform_driver cheshire_idma_driver = {
	.probe = cheshire_idma_probe,
	.remove = cheshire_idma_remove,
	.driver = {
		.name           = DRV_NAME,
		.of_match_table = cheshire_idma_of_match,
	},
};

module_platform_driver(cheshire_idma_driver);

MODULE_DESCRIPTION("Cheshire iDMA driver");
MODULE_AUTHOR("Robert Balas <balasr@iis.ee.ethz.ch>");
MODULE_LICENSE("Dual MIT/GPL");
