# Copyright 2024 ETH Zurich and University of Bologna.
# Licensed under the Apache License, Version 2.0, see LICENSE for details.
# SPDX-License-Identifier: Apache-2.0
#
# Robert Balas <balasr@iis.ee.ethz.ch>
# Luka Guzenko <lguzenko@student.ethz.ch>

.PHONY: setup
setup:
	$(MAKE) -C buildroot BR2_EXTERNAL=.. cheshire-minimal_defconfig
	$(MAKE) -C buildroot make


.PHONY: clean
clean-buildroot:
	$(MAKE) -C buildroot clean
