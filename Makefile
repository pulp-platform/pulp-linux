# Copyright 2024 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Robert Balas <balasr@iis.ee.ethz.ch>
# Cyril Koenig <cykoenig@iis.ee.ethz.ch>

# Avoid surprises by disabling default rules
MAKEFLAGS += --no-builtin-rules
.SUFFIXES:

THIS_PATH := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))

#
# Buildroot external output folder
#

OUTPUT_BASEDIR = $(THIS_PATH)/output
OUTPUT_BOARDNAME = $(basename $(notdir $@))
OUTPUT_DIR = $(OUTPUT_BASEDIR)/$(OUTPUT_BOARDNAME)

MAKE_BUILDROOT = $(MAKE) -C $(THIS_PATH)/buildroot BR2_EXTERNAL=$(THIS_PATH)

output/% $(OUTPUT_BASEDIR)/%: $(THIS_PATH)/configs/%_defconfig
		$(MAKE_BUILDROOT) O=$(OUTPUT_DIR) $(basename $(notdir $@))_defconfig
		# sed -i /^BR2_DL_DIR=.*/s%%BR2_DL_DIR=$(BR2_DL_DIR)% $(OUTPUT_DIR)/.config

#
# Device Tree Compiler
#

DTC = dtc

%.dtb: %.dts
	$(DTC) -o $@ $^

.PHONY: setup
setup: target/cheshire/cheshire.dtb
	$(MAKE) -C buildroot BR2_EXTERNAL=.. cheshire_defconfig


.PHONY: clean
clean-buildroot:
	$(MAKE) -C buildroot clean

.PHONY: clean-all
clean: clean-buildroot
	$(RM) target/cheshire/*.dtb
