# Requests from the glue layer to the core and HIF layers

No signature declared in the core or HIF headers was changed. Each item says what the glue assumes.

## Core (config.h, common/, nic/, mgmt/, include/)

1. `config.h` must set to 0 (or leave undefined) the features the glue no longer carries:
   `CFG_ENABLE_WIFI_DIRECT`, `CFG_ENABLE_WIFI_DIRECT_CFG_80211`, `CFG_ENABLE_BT_OVER_WIFI`, `CFG_SUPPORT_TDLS`,
   `CFG_SUPPORT_HOTSPOT_2_0`, `CFG_SUPPORT_WAPI`, `CFG_SUPPORT_AGPS_ASSIST`, `CFG_SUPPORT_MET_PROFILING`.
   The `GLUE_INFO_T` members of the last four are removed.
2. Optional: `CFG_SUPPORT_CFG_FILE 1` makes the glue read `wifi.cfg` through `request_firmware`;
   `CFG_SUPPORT_TSF_USING_BOOTTIME` is read with `#if` and is undefined today.
3. With the features above off (checked with a local `config.h` override, not committed) core files still fail
   to build because of unguarded uses:
   - `mgmt/rsn.c`: `prP2pSpecificBssInfo`, `struct _P2P_SPECIFIC_BSS_INFO_T`
   - `mgmt/scan.c`: `kalP2PIndicateBssInfo` (the P2P kal functions live in `gl_p2p_kal.c`, not built)
   - `mgmt/assoc.c`: `p2pFuncGenerateWSC_IEForAssocRsp`, `p2pFuncCalculateWSC_IELenForAssocRsp`
   - `mgmt/ais_fsm.c`: `ASSERT_BREAK` is defined in `include/mgmt/p2p_fsm.h` only
   - `common/wlan_lib.c`: `rBowFsmInfo`, `fgTdlsIsSup`; `mgmt/bss.c`, `nic/nic.c`: `eHiddenSsidType`,
     `ENUM_HIDDEN_SSID_LEN`
4. Independent of the features: `mgmt/stats.c` needs `<linux/sched/clock.h>` for `sched_clock()`,
   `mgmt/swcr.c` uses `__DATE__`/`__TIME__` (`-Werror=date-time`), `common/wlan_oid.c` includes `<stddef.h>`.
5. `wlanCheckSystemConfiguration()` (fake "NVRAM WARNING" scan results) is no longer called; it can be deleted.
   The glue reports "no NVRAM" (`kalIsConfigurationExist()` FALSE, `kalCfgDataRead16()` FALSE) and zero config
   versions.
6. `mgmt/tdls.h` declares `int wlanHardStartXmit(...)`; the glue defines `netdev_tx_t wlanHardStartXmit(...)`.
   Only relevant if TDLS were enabled.

## HIF (os/linux/hif/ahb/**)

1. `glSetHifInfo()` must set `rHifInfo.Dev` to the `struct device *` of the WiFi platform device (its `of_node`
   supplies `local-mac-address`, and it is the device for `request_firmware`) and `rHifInfo.ChipID` to
   `MTK_CHIP_ID_8127`, so that `glGetChipInfo()` yields `8127` and the image is `WIFI_RAM_CODE_8127`.
2. `wlanProbe(pvData)` takes a `struct device *`. The stock `CONF_HIF_DEV_MISC` branch of `HifAhbProbe()` passes
   `&MtkAhbDriver.this_device`, a `struct device **`; use the platform device branch.
3. `glRegisterBus()` must register the platform driver whose probe calls `mtk_wcn_wmt_wlan_reg()` with
   `HifAhbProbe`/`HifAhbRemove`; the glue then calls `mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI)` (module parameter
   `autostart`). `HifAhbRemove()` must end up in `wlanRemove` and tolerate `glUnregisterBus()` calling it again
   (`wlanRemove` returns early when no device is registered).
4. The glue uses these `GL_HIF_INFO_T` members: `Dev`, `ChipID`, `HifLoopbkFlg`, `DmaOps->DmaRegDump`.
5. `ahb.c` and `ahb_pdma.c` still include `<mach/mt_pm_ldo.h>` and `<mach/mt_clkmgr.h>`.

## Kernel configuration

`CONFIG_CFG80211` (m or y) is required to build and load `wlan_mt`. The `build-sensors` and `build-618`
configurations have it disabled. The glue was checked with a copy of `build-sensors` plus `CONFIG_CFG80211=m`.
