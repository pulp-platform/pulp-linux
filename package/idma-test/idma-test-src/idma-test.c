/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Userspace memcpy test for the iDMA proxy driver (IDMA_COPY_BUFFER path).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "idma-proxy-userspace.h"

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [DEVICE] [LENGTH]\n"
		"  DEVICE  default: /dev/idma_chan0 (must match platform dma-names)\n"
		"  LENGTH  transfer size in bytes (default: 1048576)\n",
		argv0);
}

int main(int argc, char **argv)
{
	const char *path = "/dev/idma_chan0";
	size_t length = 1024 * 1024;
	int fd = -1;
	void *src = NULL;
	void *dst = NULL;

	if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
		usage(argv[0]);
		return 0;
	}
	if (argc >= 2)
		path = argv[1];
	if (argc >= 3) {
		char *end = NULL;
		unsigned long v = strtoul(argv[2], &end, 0);
		if (end == argv[2] || *end != '\0' || v == 0 || v > SIZE_MAX) {
			fprintf(stderr, "Invalid LENGTH\n");
			return EXIT_FAILURE;
		}
		length = (size_t)v;
	}

	fd = open(path, O_RDWR);
	if (fd < 0) {
		perror(path);
		return EXIT_FAILURE;
	}

	src = malloc(length);
	dst = malloc(length);
	if (!src || !dst) {
		fprintf(stderr, "malloc failed\n");
		free(src);
		free(dst);
		close(fd);
		return EXIT_FAILURE;
	}

	uint32_t *s32 = src;
	uint32_t *d32 = dst;
	size_t nwords = length / sizeof(uint32_t);
	size_t i;

	for (i = 0; i < nwords; i++)
		s32[i] = (uint32_t)i;
	memset(dst, 0xab, length);

	idma_memcpy_transfer_t xfer = {
		.src = (uintptr_t)src,
		.dst = (uintptr_t)dst,
		.src_type = IDMA_COPY_BUFFER,
		.dst_type = IDMA_COPY_BUFFER,
		.length = length,
		.conf = 0,
	};

	if (ioctl(fd, IOCTL_ISSUE_MEMCPY_TRANSFER, &xfer) < 0) {
		perror("IOCTL_ISSUE_MEMCPY_TRANSFER");
		free(src);
		free(dst);
		close(fd);
		return EXIT_FAILURE;
	}

	for (i = 0; i < nwords; i++) {
		if (d32[i] != (uint32_t)i) {
			fprintf(stderr, "verify failed at word %zu: got %#x expect %#x\n",
				i, d32[i], (uint32_t)i);
			free(src);
			free(dst);
			close(fd);
			return EXIT_FAILURE;
		}
	}

	free(src);
	free(dst);
	close(fd);
	printf("idma-test: OK (%zu bytes via %s)\n", length, path);
	return 0;
}
