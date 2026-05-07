################################################################################
#
# idma-test
#
################################################################################

IDMA_TEST_VERSION = 0.1
IDMA_TEST_LICENSE = GPL-2.0-only
IDMA_TEST_LICENSE_FILES = COPYING
IDMA_TEST_SITE = "$(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/idma-test/idma-test-src"
IDMA_TEST_SITE_METHOD = local

IDMA_TEST_DEPENDENCIES = idma

define IDMA_TEST_BUILD_CMDS
	$(MAKE) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS)" -C $(@D)
endef

define IDMA_TEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/idma-test $(TARGET_DIR)/root/idma-test
endef

$(eval $(generic-package))
