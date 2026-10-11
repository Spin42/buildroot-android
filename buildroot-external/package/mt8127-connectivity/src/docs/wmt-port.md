# WMT/STP core port (module `mtk_stp_wmt_soc`)

Scope: `wmt/` (core, linux/pri, linux/pub, include), ported from the stock 3.10.54 sources to Linux 6.18 ARM32. The
platform glue (`wmt/mt8127/*`) is not built here; the `consys` module provides it. The BTIF driver comes from `btif`.

The module builds warning-free (apart from one warning in `btif/inc/mtk_btif_exp.h`, which belongs to the btif port) and
links except for the symbols listed below.

## Build setup

- `wmt/Kbuild`: `-I$(src)/compat` added. `MTK_WCN_WMT_STP_EXP_SYMBOL_ABSTRACT` is no longer defined: with it, the
  `mtk_wcn_wmt_*` / `mtk_wcn_stp_*` API would be reachable only through callback tables registered with the stock
  `common_detect` module (which does not exist here). Without it the module exports the real symbols
  (`mtk_wcn_wmt_func_on/off`, `mtk_wcn_wmt_wlan_reg/unreg`, `mtk_wcn_wmt_msgcb_reg/unreg`, `mtk_wcn_wmt_hwver_get`,
  `mtk_wcn_wmt_therm_ctrl`, `mtk_wcn_wmt_dsns_ctrl`, `mtk_wcn_wmt_assert`, `mtk_wcn_wmt_co_clock_flag_get`,
  `mtk_wcn_stp_*`), which is what the stock gen2 WiFi driver links against.
- `-Wno-missing-prototypes -Wno-missing-declarations -Wno-old-style-declaration -Wno-undef`: the vendor code has
  many non-static helpers without prototypes and `static` after the type; they are left untouched.
- `wmt/include/wmt_stp_exp.h` replaces the `common_detect/wmt_stp_exp.h` header that the stock headers include. It only
  carries the common Linux includes and the configuration macros (`CFG_WMT_LTE_COEX_HANDLING` is 0, no IDC).
- `wmt/compat/mach/mtk_wcn_cmb_stub.h` is a **temporary** copy of the stock `mach/mtk_wcn_cmb_stub.h`. It is found
  before `../compat` on the include path; delete it once `compat/mach/mtk_wcn_cmb_stub.h` (consys) exists. The wmt module
  uses only its types (`CMB_STUB_AIF_X`, `CMB_STUB_AIF_CTRL`, `COMBO_IF`, ...) and
  `mt_combo_plt_enter/exit_deep_idle()`, `mtk_wcn_wmt_chipid_query()`.

## API drift fixes (all mechanical)

| Area | Change |
| --- | --- |
| includes | `linux/earlysuspend.h`, `linux/wakelock.h`, `linux/aee.h`, `asm/system.h`, `system.h` removed; `asm/uaccess.h` -> `linux/uaccess.h`; `asm/atomic.h` -> `linux/atomic.h` |
| wake locks | `OSAL_WAKE_LOCK` wraps a `struct wakeup_source *` (`wakeup_source_register`, `__pm_stay_awake`, `__pm_relax`). With `CONFIG_PM_SLEEP=n` (the current kernel config) the pointer is NULL and all calls are no-ops. The source is never unregistered (module unload leaks it) |
| timers | `timer_setup()` with a trampoline that calls the stock `void (*)(unsigned long)` handler with its stored cookie; `del_timer*` -> `timer_delete*` |
| time | `struct timeval`/`do_gettimeofday()` -> `struct osal_timeval`/`osal_do_gettimeofday()` (`ktime_get_real_ts64`, `osal_typedef.h`); `tv_sec` is a 32-bit `long`, only used for logging and relative intervals |
| procfs | `/proc/driver/wmt_dbg` and `/proc/driver/wmt_aee` use `struct proc_ops`. The stock code stored the AEE entry in the wrong variable, so it was never removed; fixed. `proc_create("driver/...")` needs the `driver` proc directory to exist, otherwise the entries are not created (non-fatal) |
| char device | `alloc_chrdev_region()` (no fixed major 190), `class_create("stpwmt")` + `device_create()`, so devtmpfs provides `/dev/stpwmt`. Handlers were already `unlocked_ioctl` |
| genetlink (`stp_dbg.c`) | `struct genl_family` with `small_ops`/`policy`/`module`; the coredump notify code is unchanged |
| AEE | `aee_kernel_dal_show`, `aed_combo_exception*`, `aee_kernel_warning_api` removed (`STP_DBG_AEE_EXP_API` = 0). Firmware coredumps are collected, compressed and discarded; the assert string is logged |
| early suspend | handler code removed; `g_early_suspend_flag` stays 0 (so the quick-sleep path in `wmt_lib.c` is never taken) |
| types | `mtk_wcn_btif_open()` takes `unsigned long *`; `mtk_wcn_stp_send_data_raw()` prototype matched to its definition (`const UINT8 *`); `mtk_wcn_stp_psm_notify_stp()` definition takes `const UINT32`; const casts in `wmt_core_tx` |

The BT, GPS, FM and WiFi character devices (`stp_chrdev_bt.c`, `stp_chrdev_gps.c`, `wmt_chrdev_wifi.c`,
`wmt_idc.c`, `dbg_core.c`) are not in `Kbuild`. The WMT core reaches BT/GPS/FM only through the function-on/off handlers in `wmt_func.c`, which run when
some client calls `mtk_wcn_wmt_func_on()` for those types; `WMT_IDC_SUPPORT=0`. The BGW desense netlink
socket (`bgw_desense.c`) is still created at init (stock behaviour, used for BT/GPS vs WiFi desense) and is harmless
without a user-space peer.

## Running without `wmt_launcher`

The stock launcher did three things; all three are now done in the kernel.

1. **Host interface.** The launcher called `WMT_IOCTL_SET_STP_MODE`. `WMT_init()` now calls the same code
   (`wmt_dev_set_stp_mode(STP_BTIF_FULL | WMT_FM_COMM << 4)`) after `wmt_lib_init()`. The ioctl still exists and shares
   that helper.
2. **Patch search (`srh_patch`).** `wmt_ctrl_patch_search()` no longer asks user space. `wmt_ic_soc.c` passes the chip
   hardware version (`gp_soc_info->u4HwVer`, read from the chip) and `wmt_dev_patch_search()` builds a one-entry patch
   list, the same structure the launcher delivered through `WMT_IOCTL_SET_PATCH_NUM/INFO`:
   - number of patches: 1
   - 0x8A00 (E1) -> `ROMv2_patch_1_0_hdr.bin`, 0x8A01 (E2) -> `ROMv2_patch_1_1_hdr.bin`; other hardware versions fail
     with an error
   - address bytes: `ff ff ff ff` (see open questions)
   - fragment size: unchanged, 1000 bytes (`DEFAULT_PATCH_FRAG_SIZE`), the two patch-address commands and the
     fragment loop in `mtk_wcn_soc_patch_dwn()` are untouched
3. **Files.** `wmt_dev_read_file()`/`wmt_dev_patch_get()`/`wmt_dev_is_file_exist()` use `request_firmware_direct()`
   (no user-space fallback). Directory parts of the requested name are dropped, so the stock absolute names
   (`/system/etc/firmware/WMT_SOC.cfg`) become `WMT_SOC.cfg` in the firmware search path. The buffer layout (8 bytes of
   headroom, `vmalloc`) is the stock one.

Firmware under `/lib/firmware`:

| File | Used by |
| --- | --- |
| `WMT_SOC.cfg` | `wmt_conf_read_file()` at module init; a missing file fails the module init, as before |
| `ROMv2_patch_1_0_hdr.bin` | hw version 0x8A00, downloaded in `mtk_wcn_soc_sw_init()` at the first function-on |
| `ROMv2_patch_1_1_hdr.bin` | hw version 0x8A01 |
| `WIFI_RAM_CODE_8127` | loaded by the WiFi driver, not by this module |

Flow at first `mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI)`: wmtd thread -> `opfunc_pwr_on` -> `wmt_plat_pwr_ctrl(FUNC_ON)`
(consys) -> BTIF open (btif) -> chip id/hw version check -> init scripts -> `WMT_CTRL_PATCH_SEARCH` ->
`request_firmware` of the patch -> patch download over STP -> function on.

`/dev/stpwmt` is only for debugging: `open()` waits up to 2 s for init. The core never needs it.

## External symbols (unresolved at link, to be provided by other modules)

**btif** (`mtk_btif_exp.h`): `mtk_wcn_btif_open`, `mtk_wcn_btif_close`, `mtk_wcn_btif_write`,
`mtk_wcn_btif_rx_cb_register`, `mtk_wcn_btif_wakeup_consys`, `mtk_wcn_btif_dpidle_ctrl`,
`mtk_wcn_btif_loopback_ctrl`, `mtk_wcn_btif_dbg_ctrl`.

**consys** (`wmt_plat.h`): `wmt_plat_init`, `wmt_plat_deinit`, `wmt_plat_pwr_ctrl`, `wmt_plat_gpio_ctrl`,
`wmt_plat_eirq_ctrl`, `wmt_plat_wake_lock_ctrl`, `wmt_plat_irq_cb_reg`, `wmt_plat_aif_cb_reg`,
`wmt_plat_func_ctrl_cb_reg`, `wmt_plat_thermal_ctrl_cb_reg`, `wmt_plat_deep_idle_ctrl_cb_reg`,
`wmt_plat_soc_paldo_ctrl`, `wmt_plat_get_emi_virt_add`, `wmt_plat_get_emi_phy_add`, `wmt_plat_jtag_flag_ctrl`,
`wmt_plat_read_cpupcr`, `wmt_plat_read_dmaregs`, `wmt_plat_set_host_dump_state`, `wmt_plat_force_trigger_assert`,
`wmt_plat_update_host_sync_num`, `wmt_plat_get_dump_info`.

**consys** (cmb_stub): `mt_combo_plt_enter_deep_idle`, `mt_combo_plt_exit_deep_idle`, `mtk_wcn_wmt_chipid_query`.

No prototype was changed. The `mtk_wcn_consys_hw_*` functions are not referenced by this module.

## Open questions and risks

- **Patch address bytes.** The launcher source is not available. `ff ff ff ff` is the value in the stock
  `WMT_PATCH_P_ADDRESS_CMD` template and the patch files hold a single image, but if the firmware rejects the partial
  address command, the bytes (and `WMT_PATCH_NAME_FMT`) in `wmt_dev_patch_search()` are the place to adjust.
- The hardware version -> file mapping covers 0x8A00/0x8A01 only.
- Nothing has run on hardware yet. Untested at runtime: the in-kernel HIF setup at init (it waits for the wmtd thread),
  `request_firmware_direct` from the wmtd thread (the files must be reachable when the first function is switched on,
  i.e. in the rootfs, not only after a later mount), and the genetlink family registration.
- `mtk_wcn_wmt_ic_info_get` and the `common_detect` callback registration (`mtk_wcn_wmt_exp_cb_reg`) are not provided; a
  WiFi driver that expects them needs a small adaptation.
- `wmt_dev.c` still contains the launcher ioctls (`SET_PATCH_NAME/NUM/INFO`, `SET_LAUNCHER_KILL`); they are harmless
  but unused. `SET_PATCH_NUM/INFO` from a user program would replace the kernel's patch list.
- Wakeup sources are no-ops until `CONFIG_PM_SLEEP` is enabled; PSM (power save) then does not hold a wake lock.
