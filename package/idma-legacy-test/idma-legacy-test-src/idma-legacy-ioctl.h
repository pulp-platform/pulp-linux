/*
 * Userspace ioctl definitions for idma-legacy — must stay in sync with
 * package/idma-legacy/idma-legacy-src/idma-legacy.h
 */

#ifndef IDMA_LEGACY_IOCTL_H
#define IDMA_LEGACY_IOCTL_H

#include <stdint.h>
#include <sys/ioctl.h>

#define IDMA_LEGACY_IOC_MAGIC		0x9d

#define IDMA_LEGACY_IOC_GET_VERSION	_IOR(IDMA_LEGACY_IOC_MAGIC, 0, uint32_t)

struct idma_legacy_memcpy_args {
	uint64_t dst;
	uint64_t src;
	uint64_t size;
};

#define IDMA_LEGACY_IOC_MEMCPY \
	_IOW(IDMA_LEGACY_IOC_MAGIC, 1, struct idma_legacy_memcpy_args)

#endif
