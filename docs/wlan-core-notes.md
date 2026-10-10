# wlan core layer (common, nic, mgmt, include)

All 34 objects in `wlan/Kbuild.core` compile for 6.18 ARM32 with no errors
(`make -k -j8`). Remaining diagnostics in these files are `-Wundef`,
`-Wmissing-prototypes`, `-Wcast-function-type` and a few fallthrough warnings
from the stock code.

## Feature switches (`include/config.h`)

| Macro | Value |
|---|---|
| `CFG_ENABLE_WIFI_DIRECT` | 0 |
| `CFG_ENABLE_WIFI_DIRECT_CFG_80211` | 0 |
| `CFG_SUPPORT_AAA` | 0 (AP/GO association only serves P2P) |
| `CFG_ENABLE_BT_OVER_WIFI` | 0 |
| `CFG_SUPPORT_TDLS` | 0 |
| `CFG_SUPPORT_WFD`, `CFG_SUPPORT_WFD_COMPOSE_IE` | 0 |
| `CFG_SUPPORT_HOTSPOT_OPTIMIZATION`, `CFG_SUPPORT_HOTSPOT_2_0` | 0 |
| `CFG_SUPPORT_WAPI` | 0 |
| `CFG_SUPPORT_802_11W` | 0 (already off, kept) |
| `CFG_SUPPORT_AGPS_ASSIST`, `CFG_SUPPORT_AEE`, `CFG_SUPPORT_XLOG`, `CFG_SUPPORT_MET_PROFILING` | 0 (now defined explicitly) |

Kept: infrastructure STA, scan, WPA/WPA2/RSN, open/WEP, power save,
regulatory domain, roaming FSM, statistics, ad-hoc (`CFG_SUPPORT_ADHOC`).
No object was dropped from `Kbuild.core`; the p2p/bow/tdls/wapi/hs20 sources
are simply not listed.

## Interface changes

No function or struct signature changed. Struct/macro layout changes that
follow from the switches:

- `include/nic/adapter.h`: `ADAPTER_T.fgTdlsIsSup` is now always present
  (before: only with `CFG_SUPPORT_TDLS == 1`; `wlan_lib.c` writes it in the
  `#else` branch).
- `include/nic/adapter.h`: `BSS_INFO_T.eHiddenSsidType` is now always present
  (before: only with `CFG_ENABLE_WIFI_DIRECT`); `ENUM_HIDDEN_SSID_TYPE_T` is
  defined in adapter.h when `p2p_fsm.h` is not built.
- `include/debug.h`: `ASSERT_BREAK()` moved here from `include/mgmt/p2p_fsm.h`
  so it exists without P2P.
- `include/wlan_oid.h`: `PARAM_PACKET_FILTER_P2P_MASK` is 0 when
  `CFG_ENABLE_WIFI_DIRECT_CFG_80211` is 0.
- `include/typedef.h` includes `<linux/ieee80211.h>` before undefining
  `WLAN_STATUS_SUCCESS` (the kernel enum otherwise collides with it).
- `include/config.h` defines `strnicmp` as `strncasecmp` for LINUX, because
  `gl_kal.h` (`kalStrniCmp`) still uses the removed function.

## What glue/hif can rely on

- With the switches above `p2p_precomp.h`, `gl_p2p_*.h`, `gl_bow.h` are not
  needed by the core. The core never references `kalP2P*`, BOW or WAPI symbols.
- Undefined symbols left in the core objects are only `kal*`, `gl*`,
  `fgIsResetting`, `aucDebugModule` (defined in `gl_init.c`) and kernel
  symbols. `nm` the core objects against your list to find the exact set.
- `swcr.c` no longer embeds `__DATE__`/`__TIME__` (`-Werror=date-time`).

## Errors in files of other layers (not touched)

- `os/linux/include/gl_cfg80211.h:504` and `gl_p2p_ioctl.h:860`: `#error` when
  `CONFIG_NL80211_TESTMODE` is unset (the build kernel does not set it). Hits
  `gl_init.c`/`gl_cfg80211.c`. Glue must drop the `#error` or guard it with the
  P2P switch.
- `os/version.h:140`: `MODULE_SUPPORTED_DEVICE(NIC_NAME)` in `gl_init.c` is
  gone from 6.18.
- `platform.c`: `set_fs`/`KERNEL_DS`/`mm_segment_t` removed.
- The module `Kbuild` passes `-D_HIF_SDIO=1`; the core does not use it for
  anything compiled here.

## Risks

- `CFG_SUPPORT_AAA 0` and `CFG_ENABLE_WIFI_DIRECT 0` change the
  `bss.c`/`assoc.c` IE tables; STA behaviour is unchanged but only compile
  tested. Nothing in the core was run on hardware.
- `PARAM_PACKET_FILTER_P2P_MASK == 0` makes `wlanoidSetPacketFilter` store only
  the new filter bits, which is the P2P-less behaviour.
- Unchanged stock code assumes 32-bit `ULONG`/pointer casts, which holds on
  ARM32 but not on a 64-bit build.
- Warning noise: `-Wundef` for macros the Android makefile used to define;
  all are false (0) here.
