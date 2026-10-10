# Requests to the other layers (wlan-hif)

## Glue layer (os/linux/*.c, os/linux/include/*.h)

1. `gl_init.c` (around line 3044) calls
   `prGlueInfo->rHifInfo.DmaOps->DmaRegDump` without checking `DmaOps`. `DmaOps` is NULL when
   the device node has no `apdma-hif` window (PIO mode) and after `glClearHifInfo()`.

   Before:
   ```c
   if (prGlueInfo->rHifInfo.DmaOps->DmaRegDump != NULL)
       prGlueInfo->rHifInfo.DmaOps->DmaRegDump(&prGlueInfo->rHifInfo);
   ```
   After:
   ```c
   if (prGlueInfo->rHifInfo.DmaOps && prGlueInfo->rHifInfo.DmaOps->DmaRegDump != NULL)
       prGlueInfo->rHifInfo.DmaOps->DmaRegDump(&prGlueInfo->rHifInfo);
   ```
2. To read `local-mac-address`, use the new `struct device *glGetHifDevice(void)` (declared in
   `hif.h`, NULL when no device is bound), for example
   `of_get_mac_address(glGetHifDevice()->of_node, addr)` or `device_get_mac_address()`. The device
   is bound before the WMT core calls `wlanProbe()`, so it can be used there and in
   `glSetHifInfo()`-time code.
3. `wlanProbe()` receives `struct device *` (the platform device) as `pvData`, as in the stock
   platform-device path; `GL_HIF_INFO_T::Dev` carries the same device and is the one used for
   `dma_map_single()`. Buffers passed to `kalDevPortRead/Write` must be DMA-able (linear
   mapping, not vmalloc or stack) because the DMA path maps them with `dma_map_single()`.
4. Symbols this layer expects from glue/core and does not define: `kalHifAhbKalWakeLockTimeout`,
   `IsrCnt`, `IsrPassCnt`, `fgIsResetting`, `fgIsBusAccessFailed`, `HifIsFwOwn`, `HifRegDump`.
