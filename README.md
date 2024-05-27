# PULP Linux

This project helps you build and run GNU/Linux Kernels for various PULP-based
projects. The build flow is based on [buildroot](https://buildroot.org/). To do
further customizations study the [buildroot user
manual](https://buildroot.org/downloads/manual/manual.html).

## Getting Started

First make sure you have the following packets installed:

Requirements for AlmaLinux
TBD

Requirements Ubuntu
TBD

IIS Machines

Spawn a subshell with the required tools already installed by calling
```bash
riscv -riscv64-gcc-linux-gnu-11.2.0 bash
```


Now build a target image containing OpenSBI, GNU/Linux and a rootfs by running
the following commands:

```bash
# Initializes buildroot for your target platform
make setup
# Call buildroot's makefile based buildsystem
cd buildroot
make -j
# Workaround for issue https://github.com/pulp-platform/pulp-linux/issues/3
make opensbi-rebuild
```

If everything goes right you should find GNU/Linux and the combined OpenSBI + GNU/Linux image in

`buildroot/output/images/`.


## Customizing
The easiest way to do customizations such as adding/removing packages or
enabling/disabling features is to call

```bash
make menuconfig
```

in `buildroot/`.

To save this as a new configuration try `make savedefconfig`.

## Booting over JTAG

Make sure you flashed your FPGA with the target platform's bitstream.

Open a Terminal and connect to your FPGA with OpenOCD:

```bash
$ openocd -f target/cheshire/cheshire.cfg
```

This will provide an endpoint GNU GDB can connect which you can by calling

```bash
$ riscv64-unknown-linux-gnu-gdb
# then in GDB
gdb> target extended-remote localhost:3333
gdb> load buildroot/output/images/fw_payload.elf
gdb> c
```

If you want to have debug symbols available during execution you can call

```bash
gdb> add-symbol-file buildroot/output/build/linux-6.7/vmlinux
gdb> add-symbol-file buildroot/output/images/fw_payload.elf
```

before `load` ing `fw_payload.elf`.

## Booting from an SD card

TBD
