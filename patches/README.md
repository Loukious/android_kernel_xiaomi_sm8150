# Kernel patch series

The `17.0` and `nethunter` branches track crDroid's `17.0` kernel source.
The sync workflow first applies `common/*.patch`, then the selected branch's
patches in filename order. The former `16.0` branch and patch folder were
renamed to `17.0`.

## Shared patches

- `00_kernelsu_susfs.patch` preserves the KernelSU-Next and SUSFS integration
  from crDroid 16.0 commit `c11c2e4785a3c39857e462f03ce566bfaabd1e92`,
  rebased onto crDroid 17.0 commit `f3f07e3a41785202e990bcd87e6087264fe31d96`.
  This is needed because 17.0 does not include that integration. The existing
  per-branch clean/mrproper fix still applies afterward.
- `01_ir_legacy_compat.patch` adds Xiaomi's `/dev/ir_spi` interface while
  preserving standard `/dev/lirc0`. The legacy HAL writes already modulated
  bytes with 32-bit SPI words at 1.92 MHz and uses `LIRC_SET_SEND_MODE` to set
  the byte-buffer length. Both interfaces serialize hardware transfers.
  Both `ir-spi-led` and `ir-spi` device-tree compatibles are supported, so
  the existing device tree and `CONFIG_IR_SPI=y` remain appropriate.

The legacy IR format is based on the working drivers in
[AnymoreProject](https://github.com/AnymoreProject/kernel_xiaomi_vayu/blob/Anymore/drivers/media/rc/ir-spi.c)
and [Infinity-X's maintainer kernel](https://github.com/tejas101k/kernel_xiaomi_vayu/blob/sixteen/drivers/media/rc/ir-spi.c).
Use the native legacy node; a symlink between it and the LIRC node cannot
translate their different ioctl/write protocols.

- `02_credential_counter_compat.patch` retains 17.0's `atomic_long_t`
  credential reference counts and upstream `group_info` layout. This keeps
  the `get_cred_rcu` backport consistent with its atomic-long operations and
  avoids importing the unrelated 16.0 APatch layout changes.

## Branch patches and validation

`17.0/` retains the regular vayu configuration, build script, and workflow.
`nethunter/` retains its wireless, Bluetooth, CAN, and build customizations.
The packet-injection patch omits an IBSS connection argument already present
in upstream 17.0. The Docker patch remains disabled.

Before publishing a branch, the sync workflow verifies the vayu, IR,
KernelSU and SUSFS configuration and compiles the IR driver, KernelSU, and
key SUSFS integration objects. It uses force-with-lease to avoid overwriting
a branch changed during the run. A kernel build and physical IR test are
still required before treating a flashable artifact as validated.
