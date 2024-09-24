#!/bin/bash

## Post-image script which produces a u-boot compatible kernel image and generates a bootable SD card image.

set -e

UIMAGE_LOAD_ADDRESS=0x80200000
UIMAGE_ENTRY_POINT=0x80200000
MKIMAGE="${BUILD_DIR}/uboot-cheshire/tools/mkimage"
GENIMAGE_TEMPLATE="${BR2_EXTERNAL_PULP_PATH}/target/common/genimage.cfg.in"
GENIMAGE_TMP="${BUILD_DIR}/genimage.tmp"
GENIMAGE_CFG="${BINARIES_DIR}/genimage.cfg"

## Generate u-boot image from compressed kernel
$MKIMAGE                        \
    -A riscv                    \
    -O linux                    \
    -T kernel                   \
    -a $UIMAGE_LOAD_ADDRESS     \
    -e $UIMAGE_ENTRY_POINT      \
    -C gzip                     \
    -n "PULP-Linux"             \
    -d $BINARIES_DIR/Image.gz   \
    $BINARIES_DIR/uImage

## Generate genimage.cfg from template file

# Determine FAT32 partition size based on uImage filesize
# Make number of sectors a multiple of 32 (for FAT) also add 1MB as buffer
PART_SEC=$((($(stat -c '%s' $BINARIES_DIR/uImage) / 512 + 2048) / 32 * 32))

sed "s|#SIZE#|${PART_SEC}s|" "${GENIMAGE_TEMPLATE}" \
		> "${GENIMAGE_CFG}"

trap 'rm -rf "${ROOTPATH_TMP}"' EXIT
ROOTPATH_TMP="$(mktemp -d)"

genimage \
    --rootpath "${ROOTPATH_TMP}"    \
    --tmppath "${GENIMAGE_TMP}"     \
    --inputpath "${BINARIES_DIR}"   \
    --outputpath "${BINARIES_DIR}"  \
    --config "${GENIMAGE_CFG}"

rm -rf "${GENIMAGE_TMP}"

exit $?
