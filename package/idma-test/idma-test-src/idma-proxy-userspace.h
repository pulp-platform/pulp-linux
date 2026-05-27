/*
 * ABI shared with drivers/idma-proxy (idma-proxy.h). Duplicated here because
 * userspace cannot include the kernel module header.
 */
#ifndef IDMA_PROXY_USERSPACE_H
#define IDMA_PROXY_USERSPACE_H

#include <stddef.h>
#include <stdint.h>
#include <sys/ioctl.h>

#define KERNEL_BUFFER_SIZE (2 * 1024 * 1024) /* 2 MiB */

typedef enum {
	IDMA_KERNEL_BUFFER = 0,
	IDMA_COPY_BUFFER = 1,
	IDMA_USER_DIRECT = 2,
} idma_port_type_t;

typedef struct idma_memcpy_transfer {
	uintptr_t src;
	uintptr_t dst;
	idma_port_type_t src_type;
	idma_port_type_t dst_type;
	size_t length;
	size_t conf;
} idma_memcpy_transfer_t;

#define IOCTL_MAGIC '{' /* 0x7b */
#define IOCTL_ISSUE_MEMCPY_TRANSFER _IOW(IOCTL_MAGIC, 1, idma_memcpy_transfer_t *)

#endif /* IDMA_PROXY_USERSPACE_H */
