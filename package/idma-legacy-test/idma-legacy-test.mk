################################################################################
#
# idma-legacy-test
#
################################################################################

IDMA_LEGACY_TEST_VERSION = 0.1
IDMA_LEGACY_TEST_LICENSE = GPL-2.0-only
IDMA_LEGACY_TEST_LICENSE_FILES = COPYING
IDMA_LEGACY_TEST_SITE = "$(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/idma-legacy-test/idma-legacy-test-src"
IDMA_LEGACY_TEST_SITE_METHOD = local

IDMA_LEGACY_TEST_DEPENDENCIES = idma-legacy

define IDMA_LEGACY_TEST_BUILD_CMDS
	$(MAKE) CC="$(TARGET_CC)" CFLAGS="$(TARGET_CFLAGS) -march=rv64gc_zicbom_zicboz" -C $(@D)
endef

define IDMA_LEGACY_TEST_INSTALL_TARGET_CMDS
	$(INSTALL) -D -m 0755 $(@D)/idma-legacy-test $(TARGET_DIR)/root/idma-legacy-test
	$(INSTALL) -D -m 0755 $(@D)/01-test-idma-legacy.sh $(TARGET_DIR)/etc/profile.d/01-test-idma-legacy.sh
endef

$(eval $(generic-package))
