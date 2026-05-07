################################################################################
#
# idma-legacy
#
################################################################################

IDMA_LEGACY_VERSION = 0.1
IDMA_LEGACY_LICENSE = GPL-2.0-only
IDMA_LEGACY_SITE = $(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/idma-legacy/idma-legacy-src
IDMA_LEGACY_SITE_METHOD = local

$(eval $(kernel-module))
$(eval $(generic-package))
