// SPDX-License-Identifier: GPL-2.0-or-later OR MIT
#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static inline uint64_t now_ns(void)
{
	struct timespec ts;
#ifdef CLOCK_MONOTONIC_RAW
	clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#else
	clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void pin_to_cpu0(void)
{
	cpu_set_t set;
	CPU_ZERO(&set);
	CPU_SET(0, &set);
	(void)sched_setaffinity(0, sizeof(set), &set);
}

static uint32_t xorshift32(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* Parse sizes like 256K, 64M, 1G, or plain bytes */
static size_t parse_size(const char *s)
{
	char *end = NULL;
	unsigned long long v = strtoull(s, &end, 0);
	if (end == s || v == 0)
		return 0;

	if (*end == 'K' || *end == 'k')
		return (size_t)v * 1024ull;
	if (*end == 'M' || *end == 'm')
		return (size_t)v * 1024ull * 1024ull;
	if (*end == 'G' || *end == 'g')
		return (size_t)v * 1024ull * 1024ull * 1024ull;
	if (*end == '\0')
		return (size_t)v;

	return 0;
}

/* Build one single-cycle permutation for pointer chasing.
 * next[perm[i]] = perm[i+1], and last points to first.
 */
static void build_single_cycle(uint32_t *next, uint32_t n)
{
	uint32_t *perm = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
	if (!perm) {
		fprintf(stderr, "malloc perm failed\n");
		exit(1);
	}

	for (uint32_t i = 0; i < n; i++)
		perm[i] = i;

	uint32_t rng = 0x12345678u;
	for (uint32_t i = n - 1; i > 0; i--) {
		uint32_t j = xorshift32(&rng) % (i + 1);
		uint32_t tmp = perm[i];
		perm[i] = perm[j];
		perm[j] = tmp;
	}

	for (uint32_t i = 0; i < n - 1; i++)
		next[perm[i]] = perm[i + 1];
	next[perm[n - 1]] = perm[0];

	free(perm);
}

/* Pointer chase benchmark.
 * Steps ~ n * iters to traverse the whole working set repeatedly.
 */
static double bench_chase(uint32_t *next, uint32_t n, int iters,
			  uint64_t *out_sum)
{
	uint64_t steps = (uint64_t)n * (uint64_t)iters;
	uint32_t idx = 0;
	uint64_t sum = 0;

	/* Warmup: one full cycle */
	for (uint64_t i = 0; i < (uint64_t)n; i++) {
		idx = next[idx];
		sum += idx;
	}

	uint64_t t0 = now_ns();
	for (uint64_t i = 0; i < steps; i++) {
		idx = next[idx];
		sum += idx;
	}
	uint64_t t1 = now_ns();

	*out_sum = sum;
	return (double)(t1 - t0) / (double)steps; /* ns per dependent load */
}

/* Streaming bandwidth benchmark.
 * Touch each cache line (default stride = 64B). Optional write.
 */
static double bench_stream(uint8_t *buf, size_t size, int iters, int do_write,
			   uint64_t *out_sum)
{
	const size_t stride = 64;
	volatile uint64_t sum = 0;

	/* Warmup pass */
	for (size_t i = 0; i < size; i += stride)
		sum += buf[i];

	uint64_t t0 = now_ns();
	for (int it = 0; it < iters; it++) {
		for (size_t i = 0; i < size; i += stride) {
			uint8_t v = buf[i];
			sum += v;
			if (do_write)
				buf[i] = (uint8_t)(v + 1);
		}
	}
	uint64_t t1 = now_ns();

	*out_sum = (uint64_t)sum;
	double bytes = (double)size * (double)iters;
	double secs = (double)(t1 - t0) * 1e-9;
	return bytes / secs; /* bytes/sec */
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage:\n"
		"  %s -m chase  -s <MB> -i <iters>\n"
		"  %s -m stream -s <MB> -i <iters> [-w]\n"
		"  %s -m sweep  -min <size> -max <size> -i <iters>\n"
		"\n"
		"Options:\n"
		"  -m chase|stream|sweep\n"
		"  -s <MB>           working set size in MiB (for chase/stream)\n"
		"  -i <iters>        iterations (chase: cycles over array; stream: passes)\n"
		"  -w                stream: enable writes\n"
		"  -min <size>       sweep min (e.g., 256K)\n"
		"  -max <size>       sweep max (e.g., 64M)\n"
		"  -nopin            don't pin to CPU0\n",
		argv0, argv0, argv0);
}

int main(int argc, char **argv)
{
	const char *mode = "chase";
	size_t size = 2 * 1024 * 1024;
	size_t min_size = 256 * 1024;
	size_t max_size = 64 * 1024 * 1024;
	int iters = 20;
	int do_write = 0;
	int pin = 1;

	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-m") && a + 1 < argc) {
			mode = argv[++a];
		} else if (!strcmp(argv[a], "-s") && a + 1 < argc) {
			size = (size_t)strtoull(argv[++a], NULL, 0) * 1024ull *
			       1024ull;
		} else if (!strcmp(argv[a], "-i") && a + 1 < argc) {
			iters = atoi(argv[++a]);
		} else if (!strcmp(argv[a], "-w")) {
			do_write = 1;
		} else if (!strcmp(argv[a], "-min") && a + 1 < argc) {
			min_size = parse_size(argv[++a]);
		} else if (!strcmp(argv[a], "-max") && a + 1 < argc) {
			max_size = parse_size(argv[++a]);
		} else if (!strcmp(argv[a], "-nopin")) {
			pin = 0;
		} else {
			usage(argv[0]);
			return 1;
		}
	}

	if (pin)
		pin_to_cpu0();

	if (!strcmp(mode, "chase") || !strcmp(mode, "stream")) {
		/* Allocate aligned buffer. */
		void *p = NULL;
		if (posix_memalign(&p, 64, size) != 0 || !p) {
			fprintf(stderr, "posix_memalign(%zu) failed: %s\n",
				size, strerror(errno));
			return 1;
		}
		memset(p, 0xA5, size);

		uint64_t sum = 0;
		if (!strcmp(mode, "chase")) {
			if (size < 4096) {
				fprintf(stderr, "size too small for chase\n");
				return 1;
			}
			uint32_t n = (uint32_t)(size / sizeof(uint32_t));
			uint32_t *next = (uint32_t *)p;
			build_single_cycle(next, n);
			double ns = bench_chase(next, n, iters, &sum);
			printf("mode=chase size=%zu bytes iters=%d -> %.2f ns/step (sum=%" PRIu64
			       ")\n",
			       size, iters, ns, sum);
		} else {
			double bps = bench_stream((uint8_t *)p, size, iters,
						  do_write, &sum);
			printf("mode=stream size=%zu bytes iters=%d write=%d -> %.1f MB/s (sum=%" PRIu64
			       ")\n",
			       size, iters, do_write, bps / (1024.0 * 1024.0),
			       sum);
		}

		free(p);
		return 0;
	}

	if (!strcmp(mode, "sweep")) {
		if (min_size == 0 || max_size == 0 || min_size > max_size) {
			fprintf(stderr, "invalid sweep range\n");
			return 1;
		}

		printf("# sweep iters=%d (chase: ns/step), (stream: MB/s)\n",
		       iters);
		printf("# size_bytes  chase_ns_per_step  stream_MBps\n");

		for (size_t sz = min_size; sz <= max_size; sz <<= 1) {
			void *p = NULL;
			if (posix_memalign(&p, 64, sz) != 0 || !p) {
				fprintf(stderr, "alloc failed at %zu bytes\n",
					sz);
				break;
			}
			memset(p, 0xA5, sz);

			uint64_t sum1 = 0, sum2 = 0;

			/* chase */
			double chase_ns = 0.0;
			if (sz >= 4096) {
				uint32_t n = (uint32_t)(sz / sizeof(uint32_t));
				build_single_cycle((uint32_t *)p, n);
				chase_ns = bench_chase((uint32_t *)p, n, iters,
						       &sum1);
			}

			/* stream */
			double mbps = bench_stream((uint8_t *)p, sz, iters, 0,
						   &sum2) /
				      (1024.0 * 1024.0);

			printf("%zu %.2f %.1f  # sums=%" PRIu64 "/%" PRIu64
			       "\n",
			       sz, chase_ns, mbps, sum1, sum2);

			free(p);
		}
		return 0;
	}

	usage(argv[0]);
	return 1;
}
