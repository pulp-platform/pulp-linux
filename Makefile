# Copyright 2024 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Robert Balas <balasr@iis.ee.ethz.ch>
# Cyril Koenig <cykoenig@iis.ee.ethz.ch>

# Avoid surprises by disabling default rules
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

PL_ROOT := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

#
# Nonfree components (CI)
#

CHS_NONFREE_REMOTE ?= git@iis-git.ee.ethz.ch:chency/pulp-linux-nonfree.git
CHS_NONFREE_COMMIT ?= ba14962

.PHONY: nonfree-init
nonfree-init:
	git clone $(CHS_NONFREE_REMOTE) $(PL_ROOT)/nonfree
	cd $(PL_ROOT)/nonfree && git checkout $(CHS_NONFREE_COMMIT)

-include $(PL_ROOT)/nonfree/nonfree.mk

#
# Buildroot external output folder
#

OUTPUT_BASEDIR = $(PL_ROOT)/output
OUTPUT_BOARDNAME = $(basename $(notdir $@))
OUTPUT_DIR = $(OUTPUT_BASEDIR)/$(OUTPUT_BOARDNAME)

MAKE_BUILDROOT = $(MAKE) -C $(PL_ROOT)/buildroot BR2_EXTERNAL=$(PL_ROOT)

output/% $(OUTPUT_BASEDIR)/%: $(PL_ROOT)/configs/%_defconfig
		$(MAKE_BUILDROOT) O=$(OUTPUT_DIR) $(basename $(notdir $@))_defconfig
		# sed -i /^BR2_DL_DIR=.*/s%%BR2_DL_DIR=$(BR2_DL_DIR)% $(OUTPUT_DIR)/.config
