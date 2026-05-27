################################################################################
#
# idma
#
################################################################################

IDMA_VERSION = 0.1
IDMA_LICENSE = GPL-2.0-only
IDMA_SITE = $(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/idma/idma-src
IDMA_SITE_METHOD = local

define IDMA_LINUX_CONFIG_FIXUPS
	$(call KCONFIG_ENABLE_OPT,CONFIG_DMA_ENGINE)
	$(call KCONFIG_ENABLE_OPT,CONFIG_DMADEVICES)
	$(call KCONFIG_ENABLE_OPT,CONFIG_DMA_VIRTUAL_CHANNELS)
	$(call KCONFIG_ENABLE_OPT,CONFIG_DMA_OF)
endef

$(eval $(kernel-module))
$(eval $(generic-package))
