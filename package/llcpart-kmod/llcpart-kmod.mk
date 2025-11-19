################################################################################
#
# llcpart-kmod
#
################################################################################

LLCPART_KMOD_VERSION = 1.0
LLCPART_KMOD_SITE = $(BR2_EXTERNAL_CVA6_LINUX_PATH)/package/llcpart-kmod
LLCPART_KMOD_SITE_METHOD = local
LLCPART_KMOD_LICENSE = GPL-2.0-or-later OR MIT
LLCPART_KMOD_MODULE_SUBDIRS = src

$(eval $(kernel-module))
$(eval $(generic-package))
