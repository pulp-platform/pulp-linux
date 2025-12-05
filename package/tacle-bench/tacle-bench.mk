################################################################################
#
# tacle-bench
#
################################################################################

# Use a specific commit from the bluew/build branch of
# https://github.com/pulp-platform/tacle-bench
TACLE_BENCH_VERSION = 576ec51f1a2e81b64b708260cfc1492e63acb548
TACLE_BENCH_SITE    = $(call github,pulp-platform,tacle-bench,$(TACLE_BENCH_VERSION))

TACLE_BENCH_LICENSE = unknown
# TACLE_BENCH_LICENSE_FILES =

TACLE_BENCH_DEPENDENCIES =

define TACLE_BENCH_BUILD_CMDS
	$(TARGET_MAKE_ENV) $(MAKE) -C $(@D) \
		CC="$(TARGET_CC)" \
		CFLAGS="$(TARGET_CFLAGS)" \
		LDFLAGS="$(TARGET_LDFLAGS)"
endef

define TACLE_BENCH_INSTALL_TARGET_CMDS
	$(INSTALL) -d $(TARGET_DIR)/usr/bin
	if [ -d $(@D)/bin ]; then \
		for f in $(@D)/bin/*; do \
			[ -f "$$f" ] || continue; \
			$(INSTALL) -m 0755 "$$f" $(TARGET_DIR)/usr/bin/; \
		done; \
	fi
endef

$(eval $(generic-package))
