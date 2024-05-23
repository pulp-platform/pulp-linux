################################################################################
#
# vitetris
#
################################################################################

VITETRIS_VERSION = 0.58.0-a922c8b
VITETRIS_LICENSE = BSD-2-Clause
VITETRIS_LICENSE_FILES = COPYING
VITETRIS_SITE = "$(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/vitetris/vitetris-src"
VITETRIS_SITE_METHOD = local

define VITETRIS_BUILD_CMDS
    cd $(@D) && ./configure CC="$(TARGET_CC)"
    $(MAKE) -C $(@D)
endef

define VITETRIS_INSTALL_TARGET_CMDS
    $(INSTALL) -D -m 0755 $(@D)/tetris $(TARGET_DIR)/usr/bin
endef


$(eval $(generic-package))
