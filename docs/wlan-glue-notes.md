# WiFi driver glue layer (os/linux) on 6.18

Scope: `wlan/mt_wifi/wlan/os/linux/{gl_init,gl_kal,gl_rst,gl_cfg80211,platform}.c` and the headers in
`os/linux/include`. All five objects compile without errors or warnings of their own against a 6.18 ARM32
configuration with `CONFIG_CFG80211=m`; the module links only when the core and HIF layers are done.

## Decisions

- **cfg80211 only, no wireless extensions.** `gl_wext.c`, `gl_wext_priv.c`, the `iwpriv` ioctls, `ndo_do_ioctl`,
  `wext_handler_def` and the `iw_statistics` bookkeeping are gone. `gl_wext.h` keeps only the WPA/WPS IE finders
  (`wextSrchDesiredWPAIE/WPSIE`, now defined in `gl_cfg80211.c`); `gl_wext_priv.h` is an empty placeholder because
  `gl_p2p_kal.h` still includes it. Scan, connect, roam, disconnect and MIC failure are indicated through
  `cfg80211_scan_done`, `cfg80211_inform_bss*`, `cfg80211_connect_bss`, `cfg80211_roamed`,
  `cfg80211_disconnected` and `cfg80211_michael_mic_failure`. The BSS reference obtained for a connect is handed
  to `cfg80211_connect_bss()`/`cfg80211_roamed()`, which release it; `wdev->connected` replaces `sme_state`.
- **cfg80211 ops kept:** change_virtual_intf (STA/IBSS), add/get/del/set_default key, set_default_mgmt_key,
  get_station, scan, connect, disconnect, join/leave_ibss, set_power_mgmt, set/del/flush_pmksa. Dropped: the
  station ops (TDLS only), `assoc`, `remain_on_channel`, `mgmt_tx`, `mgmt_frame_register`, `testmode_cmd`
  (P2P, GAS and OEM test channel). The kal indications that the AIS FSM calls (`kalReadyOnChannel`,
  `kalRemainOnChannelExpired`, `kalIndicateMgmtTxStatus`, `kalIndicateRxMgmtFrame`) stay, ported to the new
  `cfg80211_*` signatures, because the core still references them.
- **wiphy:** `REGULATORY_CUSTOM_REG` replaces `WIPHY_FLAG_CUSTOM_REGULATORY`; the 2.4 GHz band only (the stock
  code already left the 5 GHz band unset); `ops` table is `const`; no wowlan/mgmt_stypes/remain-on-channel flags.
- **Firmware:** `kalFirmwareImageMapping()` calls `request_firmware("WIFI_RAM_CODE_<chip>")`, with the chip
  suffix from `glGetChipInfo()` (`8127` when `HifInfo->ChipID` is `MTK_CHIP_ID_8127`), copies the image into a
  zeroed vmalloc buffer (the downloader may read up to the next 4-byte boundary) and releases the firmware.
  `wifi.cfg` goes through `firmware_request_nowarn()`; a missing file keeps the defaults. The stock
  `filp_open`/`set_fs` firmware code is removed. The mem-dump writer `kalWriteToFile()` uses `kernel_write()`.
  `wifi.cfg` is only read when the core defines `CFG_SUPPORT_CFG_FILE` (it is undefined in the imported
  `config.h`).
- **MAC address:** `glGetMacAddress()` (called after `glSetHifInfo`) takes `local-mac-address`/`mac-address` from
  the `of_node` of `rHifInfo.Dev`, else `eth_random_addr()` with a warning, and stores it as the MAC override that
  `kalRetrieveNetworkAddress()` returns, so `wlanUpdateNetworkAddress()` sends it to the firmware. The eFUSE
  address is therefore never used. The NVRAM accessors (`kalCfgDataRead16/Write16`) report no data;
  `glLoadNvram()` leaves the defaults and `kalIsConfigurationExist()` is FALSE. `kalScanDone()` no longer calls
  `wlanCheckSystemConfiguration()`, so the "NVRAM WARNING" fake scan results are never generated.
- **Module init/exit:** `initWlan()` calls `glRegisterBus(wlanProbe, wlanRemove)` (the HIF registers its
  platform driver, whose probe calls `mtk_wcn_wmt_wlan_reg`). With the module parameter `autostart=1` (default)
  it then calls `mtk_wcn_wmt_func_on(WMTDRV_TYPE_WIFI)` and fails the load with `-EIO` if the WMT core refuses.
  `exitWlan()` calls `mtk_wcn_wmt_func_off(WMTDRV_TYPE_WIFI)` before `glUnregisterBus()`.
  `wmt_chrdev_wifi.c` is not part of the WMT module, so `register_set_p2p_mode_handler`,
  `register_set_dbg_level_handler`, `wifi_reset_start` and `wifi_reset_end` do not exist: the first two uses are
  removed, and `gl_rst.c` handles the whole chip reset itself (RESET_START sets `fgIsResetting`; RESET_END powers
  the function on again when it was running, tracked by `wlanIsRequested()`).
- **Netdev/kernel API:** `alloc_netdev_mq(..., NET_NAME_ENUM, ...)`, `ndo_select_queue(dev, skb, sb_dev)`,
  `eth_hw_addr_set()`, `netif_rx()`, `timer_setup()`/`timer_container_of()`/`timer_delete_sync()`,
  `ktime_get_boottime_ns()`, `unsigned long` irq flags (`GLUE_SPIN_LOCK_DECLARATION`, `KAL_SPIN_LOCK_DECLARATION`,
  `kalAcquireSpinLock/kalReleaseSpinLock` take `unsigned long`), `strncasecmp`, `MODULE_SUPPORTED_DEVICE` removed,
  all `LINUX_VERSION_CODE` branches resolved. Wake locks compile to nothing (`CONFIG_ANDROID` is forced to 0).
  `wlanNetRegister()` no longer registers the net_device when `wiphy_register()` failed.
- **Dropped vendor features:** AGPS, MET profiling, Hotspot 2.0, WAPI and TC1 blocks are deleted from the glue
  files; P2P, BoW and TDLS blocks keep their `CFG_ENABLE_WIFI_DIRECT`/`CFG_ENABLE_BT_OVER_WIFI`/`CFG_SUPPORT_TDLS`
  guards. `gl_p2p*.c`, `gl_bow.c`, `gl_proc.c` and `include/gl_p2p_*.h` are left untouched and unbuilt
  (`WLAN_INCLUDE_PROC` is not defined).

## Requirements

See `docs/wlan-glue-needs.md` for the core and HIF items. Kernel side: `CONFIG_CFG80211` must be enabled
(`wlan_mt` depends on the module; `net_device::ieee80211_ptr` does not exist without it). The configuration of
`port/build-sensors` and `port/build-618` has it unset, so the stock recipe fails in `gl_init.c`/`gl_kal.c`.
`CONFIG_NL80211_TESTMODE` is not needed. `WIFI_RAM_CODE_8127` (and optionally `wifi.cfg`) must be in
`/lib/firmware`, and the WiFi DT node should carry `local-mac-address`.

## Risks

- Nothing ran on hardware; only compilation was checked. The first run exercises `wiphy_register`,
  `register_netdev` with a driver-allocated `wireless_dev`, and the connect/roam indications.
- cfg80211 ops run under the wiphy mutex and block in `kalIoctl()`; the main thread only uses the
  workqueue-based `cfg80211_*` event calls, so no lock inversion is expected, but a hung OID would stall other
  nl80211 requests.
- `cfg80211_connect_bss()` needs the BSS to exist; the code looks it up and otherwise builds it from the
  target BSS descriptor, as the stock code did.
- `mtk_cfg80211_connect()` still leaves `eAuthMode` unset for a WPA request without AKM suites (stock behaviour).
- `netdev_event()` in `platform.c` dereferences `netdev_priv()` of every net_device whose name starts with
  `wlan` while suspended; it assumes no other driver uses that prefix.
- A random MAC changes on every power cycle when the DT has none.
- Wake locks are no-ops and the suspend/resume hooks (`wlanHandleSystemSuspend/Resume`) are not called by anyone.
- `.o.d` files of the import are tracked in git and every build rewrites them; they were not committed.
