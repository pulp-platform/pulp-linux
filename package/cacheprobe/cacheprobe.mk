################################################################################
#
# cacheprobe
#
################################################################################

CACHEPROBE_VERSION = 1.0
CACHEPROBE_LICENSE = Apache-2.0
CACHEPROBE_LICENSE_FILES = COPYING
CACHEPROBE_SITE = "$(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/cacheprobe/cacheprobe-src"
CACHEPROBE_SITE_METHOD = local

define CACHEPROBE_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) $(TARGET_CONFIGURE_OPTS) -C $(@D) cacheprobe
endef

define CACHEPROBE_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/cacheprobe $(TARGET_DIR)/usr/bin
endef


$(eval $(generic-package))
