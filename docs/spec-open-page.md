# Spec: open_page (plugin ABI 1.3)

## Why

TM Vibrant Shaders (TMVS) 2.0 runs as four Horizon plugins (`tmvs`, `tmvs-capture`, `tmvs-upscaling`, `tmvs-packs`). In the standalone mod F8 opened TMVS's own menu. As plugins their pages are Horizon panels, but a plugin could not open Horizon's menu on its page: only the user's menu key (Insert) or the bar did. `open_page` closes that gap so a plugin can register an "open my page" hotkey.

## Signature

```c
/* ABI minor 3 (check host->abi_minor >= 3), the first minor-3 field of HorizonHostApi, right after
 * host_flags; notify, bridge_request and bridge_state follow (docs/plugin-abi.md "ABI 1.3") */
HorizonResult (HORIZON_CALL* open_page)(HorizonPluginHandle plugin, const char* panel_key);
```

## Semantics

- Opens the Horizon menu on the plugin's page.
- `panel_key` `NULL` or `""`: the Settings tab (or the plain page when the plugin has no panels). Otherwise that panel's tab.
- A `panel_key` the plugin has no panel for: `HORIZON_ERR_INVALID_ARG`, the page still opens, on Settings.
- If the menu is already open on this plugin's page, the call closes it (toggle, like the menu key).
- Game thread only. `HORIZON_ERR_WRONG_STATE` outside it, and while a capture (`HorizonHost_SetCaptureActive`) is active. `HORIZON_OK` otherwise.
- Additive: older hosts lack the field, so check `host->abi_minor >= 3` (or `host->size`) first.

## What TMVS does with it

When the host reports `abi_minor >= 3`, each TMVS plugin that has a page registers a hotkey with key `menu`, default F8 (the user can rebind it in the Hotkeys page), whose callback calls `open_page(plugin, NULL)`. On older hosts it registers nothing and the page stays reachable through Horizon's menu.
