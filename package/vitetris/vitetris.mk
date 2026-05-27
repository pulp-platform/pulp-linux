################################################################################
#
# vitetris
#
################################################################################

VITETRIS_VERSION = 0.57
VITETRIS_SITE = http://www.victornils.net/tetris/
VITETRIS_LICENSE = BSD
VITETRIS_LICENSE_FILES = licence.txt

define VITETRIS_CONFIGURE_CMDS
    (cd $(@D) && ./configure)
endef

define VITETRIS_BUILD_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) CC="$(TARGET_CC)" -C $(@D)
endef

define VITETRIS_INSTALL_TARGET_CMDS
    $(TARGET_MAKE_ENV) $(MAKE) PREFIX=$(TARGET_DIR)/usr -C $(@D) install
endef

$(eval $(generic-package))
