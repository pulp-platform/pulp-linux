################################################################################
#
# idma-reg-kmod
#
################################################################################

IDMA_REG_KMOD_VERSION = 1.0
IDMA_REG_KMOD_SITE = $(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/idma-reg-kmod
IDMA_REG_KMOD_SITE_METHOD = local
IDMA_REG_KMOD_LICENSE = GPL-2.0-or-later OR MIT
IDMA_REG_KMOD_MODULE_SUBDIRS = src

$(eval $(kernel-module))
$(eval $(generic-package))
