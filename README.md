# Minimal KVM VMM

A minimal x86-64 hypervisor written in C using the Linux KVM API.

<img width="1278" height="687" alt="Image" src="https://github.com/user-attachments/assets/1d39ac8e-146a-4c48-acb7-44c9e461e9b3" />

**Note:** This project currently supports booting a Linux `bzImage` with a BusyBox-based initramfs into an interactive serial console. It does not currently support disks, networking, or multiple vCPUs.

## Directory Structure

* **`code/`** - Contains all C source code, headers, and the guest `init` script.
* **`bzImage`** - Linux kernel image used by the VMM.
* **`initramfs.cpio.gz`** - Initial RAM filesystem containing BusyBox and the guest init script.

## Compilation

This project requires a Linux system with KVM support and `gcc`.

To compile the VMM:

```bash
gcc code/*.c -o out
```

The host must have `/dev/kvm` available and accessible.

The Linux kernel used by the VMM must have serial console support enabled, including:

```text
CONFIG_SERIAL_8250=y
CONFIG_SERIAL_8250_CONSOLE=y
CONFIG_DEVTMPFS=y
CONFIG_DEVTMPFS_MOUNT=y
CONFIG_BLK_DEV_INITRD=y
```

## How to Run

Place `bzImage` and `initramfs.cpio.gz` in the project directory, then run:

```bash
./out
```

The VMM will boot the Linux kernel and provide an interactive BusyBox shell through the host terminal.

Press `Ctrl-A` followed by `x` to exit the virtual machine.

## Features

* KVM virtual machine and vCPU setup
* Linux `bzImage` booting
* Guest physical memory mapping
* CPUID configuration
* E820 memory map
* 16550-compatible serial UART emulation
* Serial interrupts through KVM
* Interactive guest terminal
