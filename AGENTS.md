# AGENTS.md

## Project

This repository is the native **s2k_Tweaks** addon for **DOOM: The Dark Ages**.
It is an in-process Windows x64 addon with a Dear ImGui interface, Vulkan renderer,
configurable GUI hotkey, and reversible console/CVAR unlock.

The current hot-reload architecture baseline is `s2k_Tweaks-v40`.

## Product goals

1. Provide a stable, interactive Dear ImGui addon menu inside DDA.
2. Remain installable as a simple Vortex archive.
3. Avoid external executables, administrator privileges, and .NET runtime requirements.
4. Keep every memory modification narrow, validated, reversible, and process-only.
5. Preserve normal game behavior when the GUI is closed.

## Supported runtime

The verified game executable has:

- PE timestamp: `0x6A627F03`
- `SizeOfImage`: `0x0B748000`
- renderer: Vulkan
- observed swapchain format: `64`
- multiple swapchains may be created during menu/level transitions

Do not enable memory patches or hooks for an unknown executable build. Update support
only after collecting new runtime evidence and validating signatures against that build.

## Required architecture

- Loader/proxy filename: `XINPUT1_4.dll`.
- Keep the XInput/Vulkan/ImGui/input/console-patch core in `XINPUT1_4.dll`.
- Keep reloadable menu composition and tweak logic in `s2k_Tweaks.dll`.
- Load the tweak module from a private shadow copy so its deployed source DLL can be replaced while DDA runs.
- Validate `S2K_PLUGIN_ABI_VERSION` and all required callbacks before replacing the active module.
- If a reload candidate fails, keep the previous working module loaded.
- Forward the real XInput exports to the Windows system DLL.
- Keep named and ordinal exports working. `xinput_smoke` must report:
  `named=1 ordinals=1`.
- Use MinHook only for the Vulkan proc-address entry points currently required.
- Render through the official Dear ImGui Vulkan backend.
- Use the official Dear ImGui Win32 backend for GUI integration.
- Default GUI toggle hotkey: `F10`.
- Save user configuration only to `s2k_Tweaks.ini` in the game directory.
- Write diagnostics to `s2k_Tweaks.log` in the game directory.

## Vulkan rules

1. Never assume that the last-created swapchain is the presented swapchain.
2. Track swapchain metadata by `VkSwapchainKHR` and select it from
   `VkPresentInfoKHR::pSwapchains` at presentation time.
3. Track queues by `VkQueue`; the verified presentation queue was family `0`, index `1`,
   but do not hard-code that value.
4. Release overlay resources before their owning swapchain is destroyed.
5. Rebuild framebuffers, command buffers, fences, semaphores, render pass, and ImGui
   Vulkan state after a swapchain change.
6. Never use `vkDeviceWaitIdle` in the normal frame path. Wait only for overlay-owned
   fences when cleaning up overlay resources.
7. Consume the game's present wait semaphores in the overlay submission, signal an
   overlay-owned semaphore, and pass that semaphore to the original present call.
8. If any required Vulkan object or function is missing, skip drawing and call the
   original presentation function unchanged.
9. Do not submit overlay commands when neither the GUI nor a notification is visible.
10. When an exclusive swapchain is presented on a non-graphics queue family, transfer
    ownership present-to-graphics and graphics-to-present with matched release/acquire
    barriers and binary semaphores; never submit ImGui draw commands to a non-graphics queue.

## Input rules

1. The GUI toggle must work even when the ImGui renderer has been torn down during a
   swapchain transition. Poll the configured hotkey independently on every present.
2. DDA uses raw input; do not rely exclusively on classic Win32 mouse messages.
3. Feed cursor position and mouse-button states to ImGui from direct polling each GUI frame.
4. Keep Win32 message handling for text/keyboard integration and suppress game window
   input messages while the GUI is open.
5. Hotkey capture must support modifiers such as `Ctrl`, `Alt`, and `Shift`.
6. `Escape` cancels hotkey capture.
7. Protect hotkey configuration shared between the window and render threads.
8. Warn about, but do not silently forbid, hotkeys that may conflict with game controls.

## Console/CVAR unlock rules

The implementation is based on the MIT-licensed `bowsr/TDAUnlocker` behavior.
Keep its attribution and license in `packaging/THIRD_PARTY_NOTICES.txt`.

- Locked signature: `08 4C 8B 0E BA 01`
- Unlocked signature: `08 4C 8B 0E BA 00`
- Patch location: signature match `+5`
- Unlock transition: `01 -> 00`
- Restrict transition: `00 -> 01`

Required safeguards:

1. Scan only the verified main executable image.
2. Accept exactly one locked or exactly one unlocked match.
3. Refuse ambiguous, missing, or contradictory results.
4. Verify the current byte immediately before writing.
5. Change page protection only for the write and restore it afterward.
6. Flush the instruction cache and verify the final byte.
7. Modify only process memory. Never patch `DOOMTheDarkAges.exe` on disk.
8. The GUI checkbox must reflect the byte currently present in memory, not a cached value.
9. Do not include Atlan's resource-hash or launch-parameter patches.
10. Do not add `god`/`noclip` function-pointer replacement without a separate explicit request
    and new safety analysis.

Finding a CVAR or command does not prove that its retail implementation works. Do not claim
functionality until it has been tested in the user's runtime.

## Atlan and DAMM boundaries

- This addon is not an Atlan mod and must not require Atlan Mod Loader or DAMM.
- Vortex may display a generic “Run DAMM” notification after deployment; it is not required
  for this DLL addon.
- DAMM can patch the executable on disk. An `ALREADY UNLOCKED` result may therefore come
  from an earlier Atlan patch rather than this addon.
- Do not package `version.dll`, Atlan files, extracted game resources, or DAMM output.

## Prohibited legacy directions

Do not reintroduce any of the following without new explicit runtime evidence and approval:

- Quake 3 movement profile, F9 movement toggle, or movement notification code.
- `winmm.dll` proxy loading; it prevented the DDA launcher/game from starting reliably.
- DDA internal AIGUI or internal `ImGui::NewFrame` hooks; the retail frame paths were dormant.
- external .NET overlay/notifier executables.
- old loader diagnostics, frame probes, hard-coded internal ImGui RVAs, or versioned probe logs.
- a single global “last swapchain” state.

## Source organization

Keep responsibilities separated:

- `xinput_proxy.*`: system XInput forwarding only.
- `dllmain.cpp`: executable validation and initialization scheduling only.
- `vulkan_hook.*`: Vulkan discovery, queue/swapchain tracking, and present orchestration.
- `imgui_overlay.*`: Vulkan resources and ImGui frame rendering.
- `addon_ui.*`: menu, Win32 input, hotkey capture, and config persistence.
- `plugin_manager.*` and `plugin_api.h`: validated shadow loading and the C ABI boundary.
- `plugin/s2k_tweaks_plugin.cpp`: reloadable menu composition and high-level tweak controls.
- `console_unlock.*`: signature scan and reversible console restriction patch.
- `notification_state.*`: short generic notifications only.
- `log.*`: thread-safe file logging.

Do not place game-specific memory patches in UI or rendering code.

## Build and verification

Build with:

```powershell
./scripts/build.ps1 -Configuration Release
```

Before packaging:

1. Build Release successfully.
2. Run:
   `./build/Release/xinput_smoke.exe ./build/Release/XINPUT1_4.dll`
3. Confirm `named=1 ordinals=1`.
4. Run `plugin_smoke` against `s2k_Tweaks.dll` and require `export=1 abi=1 init=1`.
5. Run `notification_queue_smoke` and require `queued=12 ordered=1 dismissed=1`.
6. When a clean supported executable is available, run `signature_file_smoke` and require
   exactly one total locked/unlocked signature.
7. Full-install packages contain only:
   - `XINPUT1_4.dll`
   - `s2k_Tweaks.dll`
   - `INSTALL.txt`
   - `THIRD_PARTY_NOTICES.txt`
   Plugin-only hot-update packages contain only `s2k_Tweaks.dll`.
8. Test at minimum:
   - GUI opens and closes in the main menu.
   - GUI opens after loading a level.
   - GUI survives menu-to-level and level-to-menu transitions.
   - checkbox restricts/unlocks CVAR listing.
   - hotkey capture and persistence work.
   - closing the GUI restores normal input behavior.

## Repository hygiene

- Do not commit extracted copyrighted game files or third-party binaries used only for analysis.
- Do not execute attached unknown DLLs; inspect them statically.
- Keep generated builds, packages, logs, analysis output, and downloaded tools ignored.
- Remove obsolete experiments from the active CMake target rather than leaving dormant code.
- Use descriptive, version-independent log messages in source. Put release numbers only in
  packaging metadata and archive names.
- Route user-facing state messages through the reusable host notification API. The core must
  copy message text and own its lifetime; plugins must not expose transient string pointers.
