/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _IDMA_LEGACY_H
#define _IDMA_LEGACY_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define IDMA_LEGACY_IOC_MAGIC		0x9d

#define IDMA_LEGACY_IOC_GET_VERSION	_IOR(IDMA_LEGACY_IOC_MAGIC, 0, __u32)

struct idma_legacy_memcpy_args {
	__u64 dst;
	__u64 src;
	__u64 size;
};

#define IDMA_LEGACY_IOC_MEMCPY							\
	_IOW(IDMA_LEGACY_IOC_MAGIC, 1, struct idma_legacy_memcpy_args)

#endif
