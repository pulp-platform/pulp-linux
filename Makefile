# Copyright 2024 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Robert Balas <balasr@iis.ee.ethz.ch>

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
