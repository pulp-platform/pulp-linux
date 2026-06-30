#!/bin/sh
set -eu

image_dir="${BINARIES_DIR:-${1:-}}"
mkimage="${HOST_DIR:?HOST_DIR is not set}/bin/mkimage"

if [ -z "${image_dir}" ]; then
	echo "BINARIES_DIR is not set" >&2
	exit 1
fi

if [ ! -f "${image_dir}/Image.gz" ]; then
	echo "Compressed Linux Image not found: ${image_dir}/Image.gz" >&2
	exit 1
fi

if [ ! -x "${mkimage}" ]; then
	echo "mkimage not found or not executable: ${mkimage}" >&2
	exit 1
fi

"${mkimage}" \
	-A riscv \
	-O linux \
	-T kernel \
	-C gzip \
	-a 0x80200000 \
	-e 0x80200000 \
	-n "Linux" \
	-d "${image_dir}/Image.gz" \
	"${image_dir}/uImage"
