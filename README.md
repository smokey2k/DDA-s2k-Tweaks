# s2k_Tweaks

Native in-process Dear ImGui tweaks addon for DOOM: The Dark Ages.

Clone with dependencies:

```powershell
git clone --recurse-submodules <repository-url>
```

For an existing clone, run `git submodule update --init --recursive` once.

## Features

- `XINPUT1_4.dll` proxy with forwarding to the Windows system DLL.
- Vulkan rendering through the game's presentation queue.
- Dear ImGui Vulkan renderer and Win32 input backend.
- Stable `XINPUT1_4.dll` core with a shadow-loaded, hot-reloadable `s2k_Tweaks.dll` UI module.
- Persistent addon settings saved to `s2k_Tweaks.ini`.
- Keyboard movement remains available while the addon GUI is open; mouse input stays captured by the GUI.
- Reversible, process-only console/CVAR unlock for the verified DDA executable.
- No Atlan Mod Loader or external executable required.

The addon does not modify `DOOMTheDarkAges.exe` on disk. Unsupported executable
versions and ambiguous console signatures are refused without patching memory.

## Build

Requirements: Windows, a Visual Studio C++ toolchain, Vulkan SDK headers (vendored),
and CMake 3.20 or newer available in `PATH`.

```powershell
./scripts/build.ps1 -Configuration Release
```

The output DLL is `build/Release/XINPUT1_4.dll`.
The reloadable module is `build/Release/s2k_Tweaks.dll`.

## Test

```powershell
./build/Release/xinput_smoke.exe ./build/Release/XINPUT1_4.dll
./build/Release/plugin_smoke.exe ./build/Release/s2k_Tweaks.dll
./build/Release/signature_file_smoke.exe <path-to-DOOMTheDarkAges.exe>
```

## Automated releases

The version has a single source of truth in [`VERSION`](VERSION). Pushes and pull
requests to `main` are built and tested automatically. To publish a release, update
`VERSION`, commit and push it, then create the matching tag:

```powershell
$version = Get-Content VERSION -Raw
$version = $version.Trim()
git tag "v$version"
git push origin main "v$version"
```

The tag workflow builds and tests the project, then publishes a Vortex-ready ZIP and
its SHA-256 checksum on the GitHub Releases page. A mismatched tag is rejected.

## License

This project is available under the [MIT License](LICENSE). Third-party components
retain their respective licenses; see [THIRD_PARTY_NOTICES.txt](packaging/THIRD_PARTY_NOTICES.txt).
