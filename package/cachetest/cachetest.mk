################################################################################
#
# cachetest
#
################################################################################

CACHETEST_VERSION = 1.0
CACHETEST_LICENSE = Apache-2.0
CACHETEST_LICENSE_FILES = COPYING
CACHETEST_SITE = "$(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/cachetest/cachetest-src"
CACHETEST_SITE_METHOD = local

define CACHETEST_BUILD_CMDS
    $(MAKE) CC="$(TARGET_CC)" LD="$(TARGET_LD)" -C $(@D) cachetest
endef

define CACHETEST_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/cachetest $(TARGET_DIR)/usr/bin
endef


$(eval $(generic-package))
