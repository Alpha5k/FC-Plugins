# Contributing

See [STYLE.md](STYLE.md) for code and writing conventions.

## Building

Development requires Windows, a C++23-capable MSVC toolset, the Windows SDK, CMake 3.28 or later, Ninja and Git.
Use an MSVC developer shell for the target architecture, with CMake and Ninja on `PATH`.
Run commands from the repository root. Install the public Fusion Cutter SDK separately and supply its installation
prefix through `CMAKE_PREFIX_PATH`.

Initialize the pinned dependencies before configuring:

```powershell
git submodule update --init --recursive
```

For x86 builds, Opus also needs its model data. Run the upstream download script from Git Bash at the repository
root; it verifies the archive checksum before extracting the files:

```bash
cd vendor/opus
sh dnn/download_model.sh a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e
cd ../..
```

Return to the MSVC developer shell to configure and build.

Choose the preset matching your developer shell and SDK:

| Preset | Architecture | Configuration |
| --- | --- | --- |
| `x86-debug` | x86 | Debug |
| `x64-debug` | x64 | Debug |
| `x86-release` | x86 | RelWithDebInfo |
| `x64-release` | x64 | RelWithDebInfo |

For example, configure, build and test the x86 plugins with debug symbols:

```powershell
cmake --preset x86-release "-DCMAKE_PREFIX_PATH=C:/path/to/installed-fc-sdk"
cmake --build --preset x86-release --parallel
ctest --test-dir ../build/fc-plugins/x86-release --output-on-failure
```

Build output goes to `../build/fc-plugins/<preset>`. Both release presets use `RelWithDebInfo`.
The x86 build provides Multiplayer, Server, Social, Diagnostics and MPGC. The x64 build provides the Server plugin
with StablePlayerIds for Classic Collection.

Build an individual plugin with `--target FusionCutterMultiplayer`, `FusionCutterServer`, `FusionCutterSocial`,
`FusionCutterDiagnostics` or `FusionCutterMPGC` on x86. The x64 target is `FusionCutterServer`.
The `patches`, `plugins` and `tests` directories also configure independently using the same SDK prefix.

Catch2 and Opus are pinned Git submodules under `vendor/`. The root x86 build includes tests by default;
use `-DBUILD_TESTING=OFF` to omit them. The x64 build skips these x86 tests and their dependencies.

## Testing changes

Run tests for the affected patches. The test executable accepts Catch2 filters; for example, run the MPGC cases with:

```powershell
../build/fc-plugins/x86-release/tests/fc_plugin_tests.exe "[mpgc]"
```

For changes to native calls, game memory layouts, hooks or resource lifetimes, build and test the affected
architectures in Debug and RelWithDebInfo.

Check assumptions about game code and data against the supported binaries. For relocatable images, validate
patch plans at another load address: embedded addresses must use the selected module's base and RVA, and
byte checks must account for relocations.

Add focused tests for changed behavior and update affected documentation. When submitting a change, include
the relevant test results and game builds checked, distinguishing automated checks, static analysis and live testing.
For Lua changes, compile the affected addon and check its menus and callbacks in the game.

## Staging an installation

Collect the plugin DLLs and matching symbols into a local installation directory:

```powershell
cmake --install ../build/fc-plugins/x86-release --prefix ../stage/fc-plugins/x86-release
```

Build addon assets separately using the commands below. Keep plugin DLLs and their addon files from the same build.

## Lua addons

Addon sources are under `addons/mpgc` and `addons/social`. Both use the original BF2 Mod Tools and the numbered
script loaders supplied by 1.3 or Remaster 1.5.

Build the MPGC addon with an unused `custom_gc_N` slot that the installed loader reaches:

```powershell
./addons/mpgc/munge/build.ps1 -ModToolsDir C:/BF2_ModTools `
    -OutputDir ../build/mpgc-addon -EntrySlot 0
```

Slot 10 belongs to the extension loader; slots above 10 must be consecutive. The entry's script resource name
must match its slot, so changing slots requires rebuilding rather than renaming the LVL.

Build the Social addon with free shell and game slots from 0 through 9:

```powershell
./addons/social/munge/build.ps1 -ModToolsDir C:/BF2_ModTools `
    -OutputDir ../build/social-addon -ShellSlot 1 -GameSlot 0
```

Each build produces a `GameData` folder to copy into the game installation alongside the matching plugin.
Choose free slots so existing mods keep their entries. Install one shell entry for MPGC, and one shell and one game
entry for Social.
