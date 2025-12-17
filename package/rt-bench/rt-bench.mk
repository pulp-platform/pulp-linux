################################################################################
#
# rt-bench
#
################################################################################

RT_BENCH_VERSION = OSPERT25
RT_BENCH_SITE    = https://gitlab.com/rt-bench/rt-bench.git
RT_BENCH_SITE_METHOD = git

RT_BENCH_GIT_SUBMODULES = YES

RT_BENCH_LICENSE = MIT
RT_BENCH_LICENSE_FILES = LICENSE

# rt-bench uses json-c
RT_BENCH_DEPENDENCIES = json-c

# Build
define RT_BENCH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CROSS_COMPILE="$(TARGET_CROSS)" \
		compile-vision
endef
#		compile-tacle \
#		compile-image-filters

# 		compile-isolbench \
# 		compile-utils


# Install in /usr/bin
# Only install real ELF binaries for the target arch
define RT_BENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/usr/bin
	cd $(@D) && \
	for d in IsolBench rt-tacle-bench vision image-filters utils; do \
		[ -d "$$d" ] || continue; \
		find "$$d" -type f -perm -111 -print; \
	done | while read f; do \
		if $(TARGET_READELF) -h "$$f" >/dev/null 2>&1 && \
		   $(TARGET_READELF) -h "$$f" | grep -q "RISC-V"; then \
			bn=$$(basename "$$f"); \
			$(INSTALL) -m 0755 "$$f" "$(TARGET_DIR)/usr/bin/$$bn"; \
		fi; \
	done
endef

$(eval $(generic-package))
