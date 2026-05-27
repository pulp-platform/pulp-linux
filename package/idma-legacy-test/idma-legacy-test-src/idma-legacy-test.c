/*
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * Userspace test for the idma-legacy character device (IOCTL MEMCPY).
 */

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "idma-legacy-ioctl.h"

#ifndef IDMA_LEGACY_TEST_EXPECT_CMO
#define IDMA_LEGACY_TEST_EXPECT_CMO 0
#endif

static void usage(const char *argv0) {
  fprintf(stderr,
          "Usage: %s [DEVICE] [LENGTH] [SEED]\n"
          "  DEVICE  default: /dev/idma_legacy\n"
          "  LENGTH  transfer size in bytes (default: 4096)\n"
          "  SEED    64-bit seed for pseudo-random src fill (default: 0)\n",
          argv0);
}

/* 64-bit mix; good enough for deterministic test pattern. */
static uint64_t mix_hash(uint64_t x) {
  x += 0x9e3779b97f4a7c15ULL;
  x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
  x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
  return x ^ (x >> 31);
}

/* Zicbom cache block size for Cheshire / this test platform */
#define ZICBOM_BLOCK_SZ 16

static void zicbom_flush_range(const void *base, size_t len) {
  uintptr_t p = (uintptr_t)base & ~(uintptr_t)(ZICBOM_BLOCK_SZ - 1);
  uintptr_t e = ((uintptr_t)base + len + ZICBOM_BLOCK_SZ - 1) &
                ~(uintptr_t)(ZICBOM_BLOCK_SZ - 1);

  asm volatile("bgeu %[p], %[e], 2f\n\t"
               "1:\n\t"
               "cbo.flush (%[p])\n\t"
               "addi %[p], %[p], %[step]\n\t"
               "bltu %[p], %[e], 1b\n"
               "2:"
               : [p] "+&r"(p)
               : [e] "r"(e), [step] "I"(ZICBOM_BLOCK_SZ)
               : "memory");
}

static void zicbom_inval_range(void *base, size_t len) {
  uintptr_t p = (uintptr_t)base & ~(uintptr_t)(ZICBOM_BLOCK_SZ - 1);
  uintptr_t e = ((uintptr_t)base + len + ZICBOM_BLOCK_SZ - 1) &
                ~(uintptr_t)(ZICBOM_BLOCK_SZ - 1);

  asm volatile("bgeu %[p], %[e], 2f\n\t"
               "1:\n\t"
               "cbo.inval (%[p])\n\t"
               "addi %[p], %[p], %[step]\n\t"
               "bltu %[p], %[e], 1b\n"
               "2:"
               : [p] "+&r"(p)
               : [e] "r"(e), [step] "I"(ZICBOM_BLOCK_SZ)
               : "memory");
}

int test_idma_legacy(const int fd, void *src, void *dst, size_t length, uint64_t seed,
                     bool use_zicbom) {
  int ret;
  struct idma_legacy_memcpy_args args;

  uint8_t *sb = src;
  uint8_t *db = dst;
  size_t i;

  for (i = 0; i < length; i++)
    sb[i] = (uint8_t)mix_hash(seed + (uint64_t)i);

  memset(dst, 0xab, length);

  asm volatile("fence iorw, iorw" ::: "memory");

  if (use_zicbom) {
    zicbom_flush_range(sb, length);
    zicbom_inval_range(db, length);
  }

  args.dst = (uint64_t)(uintptr_t)dst;
  args.src = (uint64_t)(uintptr_t)src;
  args.size = (uint64_t)length;

  ret = ioctl(fd, IDMA_LEGACY_IOC_MEMCPY, &args);
  if (ret < 0) {
    perror("IDMA_LEGACY_IOC_MEMCPY");
    free(src);
    free(dst);
    close(fd);
    return EXIT_FAILURE;
  }

  int errors = 0;
  for (i = 0; i < length; i++) {
    if (sb[i] != db[i]) {
      errors++;
    }
  }

  return errors;
}

int main(int argc, char **argv) {
  const char *path = "/dev/idma_legacy";
  size_t length = 4096;
  uint64_t seed = 0;
  int fd = -1;
  void *src = NULL;
  void *dst = NULL;
  int ret;
  uint32_t ver = 0;

  if (argc > 1 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
    usage(argv[0]);
    return 0;
  }
  if (argc >= 2)
    path = argv[1];
  if (argc >= 3) {
    char *end = NULL;
    unsigned long v = strtoul(argv[2], &end, 0);

    if (end == argv[2] || *end != '\0' || v == 0 || v > (256UL * 1024 * 1024)) {
      fprintf(stderr, "Invalid LENGTH (1 .. 256 MiB)\n");
      return EXIT_FAILURE;
    }
    length = (size_t)v;
  }
  if (argc >= 4) {
    char *end = NULL;
    unsigned long long sv = strtoull(argv[3], &end, 0);

    if (end == argv[3] || *end != '\0') {
      fprintf(stderr, "Invalid SEED (64-bit unsigned)\n");
      return EXIT_FAILURE;
    }
    seed = sv;
  }

  fd = open(path, O_RDWR);
  if (fd < 0) {
    perror(path);
    return EXIT_FAILURE;
  }

  ret = ioctl(fd, IDMA_LEGACY_IOC_GET_VERSION, &ver);
  if (ret < 0) {
    perror("IDMA_LEGACY_IOC_GET_VERSION");
    close(fd);
    return EXIT_FAILURE;
  }
  printf("idma-legacy ABI version: %u\n", ver);

  src = malloc(length);
  dst = malloc(length);
  if (!src || !dst) {
    fprintf(stderr, "malloc failed\n");
    free(src);
    free(dst);
    close(fd);
    return EXIT_FAILURE;
  }

#if IDMA_LEGACY_TEST_EXPECT_CMO
  printf("Testing CMO flushing...");
  ret = test_idma_legacy(fd, src, dst, length, seed, true);
  if (ret == 0) {
    printf("OK\n");
  } else {
    printf("FAILED\n");
  }
  printf("Testing only fence (no flush expected)...");
  ret = test_idma_legacy(fd, src, dst, length, seed, false);
  if (ret > 0) {
    printf("OK\n");
  } else {
    printf("FAILED\n");
  }
#else
  printf("Testing fence flushing...");
  ret = test_idma_legacy(fd, src, dst, length, seed, false);
  if (ret == 0) {
    printf("OK\n");
  } else {
    printf("FAILED\n");
  }
#endif

  free(src);
  free(dst);
  close(fd);
  return EXIT_SUCCESS;
}
