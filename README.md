# SWBF2 Fusion Cutter Plugins

These are the plugins and mods I've written for Star Wars: Battlefront II using [Fusion Cutter](https://github.com/Alpha5k/FusionCutter). They can be installed separately or together. You can download the plugin DLLs from the Releases section, or build them using the instructions below.

If you want to build your own custom plugins you can modify the .cpp files in the `plugins` directory to add or exclude certain patches.

Source for everything is in this repo. Feel free to include any of these in your own projects without attribution. These are examples of the kinds of things you can do with Fusion Cutter. Most patches have been tested but I can't promise there won't be issues.

## Plugins

All patches currently target Battlefront II. The supported builds are Steam (`59EDE353`), GOG (`59EDF52B`),
Mod Tools (`43EBD102`) and Classic Collection (`66702CD2`).

**Client**, **Server** and **Both** identify the supported launch roles; **—** means unsupported.

### Multiplayer

Gameplay and networking fixes.

| Patch | What it does |
| --- | --- |
| [UnlimitedBandwidth](patches/unlimited_bandwidth) | Disable the legacy multiplayer bandwidth limit in both client and server. |
| [InactiveWindowFix](patches/inactive_window_fix) | Removes the extra ten-millisecond wait while the window is inactive. |
| [InputSync](patches/input_sync) | Keeps movement and button presses in sync between clients and servers. |
| [CrouchBugFix](patches/crouch_bug_fix) | Fixes the bug where crouching players can appear uncrouched in multiplayer. |
| [DoorCorpseFix](patches/door_corpse_fix) | Fixes the bug where corpses can hold doors open on multiplayer clients. |
| [ReloadDurationFix](patches/reload_duration_fix) | Fixes the duplicated weapon reload sounds in multiplayer on high ping. |
| [HeroAnimationFix](patches/hero_animation_fix) | Prevents repeated hero animations and sounds in multiplayer. |
| [MeleeTiming](patches/melee_timing) | Fixes missed melee combo inputs in multiplayer. |
| [HostRandomStateFix](patches/host_random_state_fix) | Preserves the host's random state in multiplayer. |
| [ShotSequence](patches/shot_sequence) | Fixes weapon cooldowns and inconsistent projectile spread in multiplayer. |
| [SoldierStateSync](patches/soldier_state_sync) | Keeps player movement and weapon actions in sync in multiplayer. |
| [CollisionHistoryFix](patches/collision_history_fix) | Fixes missed player collisions when replaying predicted movement. |
| [WeaponSwapReplayFix](patches/weapon_swap_replay_fix) | Fixes the duplicated weapon swap sounds and HUD flickering in multiplayer on high ping. |
| [SpectatorCameraSmoothing](patches/spectator_camera_smoothing) | Smooth spectator camera movement and interpolated objects with two additional turns of delay. |
| [LateUpdateRecovery](patches/late_update_recovery) | Recover multiple updates received in a single turn instead of only parsing the newest. |
| [AerialGravity](patches/aerial_gravity) | Fixes hero aerial gravity to be the correct speed in both singleplayer and multiplayer. |
| [DirectTransport](patches/direct_transport) | Restores direct UDP connections instead of using GalaxyPeer for game traffic. |
| [SaberSoundFix](patches/saber_sound_fix) | Prevents lightsaber ignition and shutdown sounds from being cut off. |
| [FootstepTrackingFix](patches/footstep_tracking_fix) | Prevents duplicated footstep sounds particularly when landing. |

| Patch | Steam | GOG | Mod Tools |
| --- | --- | --- | --- |
| UnlimitedBandwidth | Both | Both | — |
| InactiveWindowFix | Client | Client | Client |
| InputSync | Client | Client | — |
| CrouchBugFix | Client | Client | — |
| DoorCorpseFix | Client | Client | — |
| ReloadDurationFix | Client | Client | — |
| HeroAnimationFix | Client | Client | — |
| MeleeTiming | Client | Client | — |
| HostRandomStateFix | Client | Client | — |
| ShotSequence | Client | Client | — |
| SoldierStateSync | Client | Client | — |
| CollisionHistoryFix | Client | Client | — |
| WeaponSwapReplayFix | Client | Client | — |
| SpectatorCameraSmoothing | Client | Client | — |
| LateUpdateRecovery | Client | Client | — |
| AerialGravity | Client | Client | — |
| DirectTransport | Client | Client | — |
| SaberSoundFix | Client | Client | — |
| FootstepTrackingFix | Client | Client | — |

### Server

Patches for dedicated servers. Most patches also work on selfhosts.

| Patch | What it does |
| --- | --- |
| [InfiniteSprintPatch](patches/infinite_sprint_patch) | Fixes the infinite sprint bug where players can glitch their stamina bar to run without losing stamina. |
| [SpeedpackPatch](patches/speedpack_patch) | Fixes the bug where players can throw glitched detpacks at very high speeds. |
| [SpawnDelay](patches/spawn_delay) | Sets the default delay between team spawn waves. |
| [SpawnJumpFix](patches/spawn_jump_fix) | Fixes the bug where players can spawn right as warmup timer ends and skip the first wave. |
| [DistanceLagFix](patches/distance_lag_fix) | Increases the default player move limit from 5 to 32. |
| [ObjectBudget](patches/object_budget) | Give ordinary server updates more room for object state and nearby ordnance events. |
| [NameBugFix](patches/name_bug_fix) | Fixes the bug where players that leave during warmup are not properly cleaned up and another player can inherit their name. |
| [MapHangFix](patches/map_hang_fix) | Prevents the server from getting stuck in-between maps waiting for client packets. |
| [VoteExploitFix](patches/vote_exploit_fix) | Disables in-game vote kicking to prevent server-crashing exploits. |
| [WaitlateGrace](patches/waitlate_grace) | Enables waitlate and sets its grace from the simulation turn rate. |
| [UpdateScheduling](patches/update_scheduling) | Send game updates to every client every turn instead of staggering them. |
| [ServerLoopRate](patches/server_loop_rate) | Raises the server outer loop cap from 80 to 120 iterations per second. |
| [InactiveWindowFix](patches/inactive_window_fix) | Removes the extra ten-millisecond wait while the window is inactive. |
| [AerialGravity](patches/aerial_gravity) | Fixes hero aerial gravity to be the correct speed in both singleplayer and multiplayer. |
| [SoldierStateSync](patches/soldier_state_sync) | Keeps player movement and weapon actions in sync in multiplayer. |
| [MeleeTiming](patches/melee_timing) | Fixes missed melee combo inputs in multiplayer. |
| [ShotSequence](patches/shot_sequence) | Fixes weapon cooldowns and inconsistent projectile spread in multiplayer. |
| [InputSync](patches/input_sync) | Keeps movement and button presses in sync between clients and servers. |
| [DirectTransport](patches/direct_transport) | Restores direct UDP connections instead of using GalaxyPeer for game traffic. |
| [StablePlayerIds](patches/stable_player_ids) | Uses published Photon account IDs for persistent admin keyhashes. |

| Patch | Steam | GOG | Classic Collection |
| --- | --- | --- | --- |
| InfiniteSprintPatch | — | Server | — |
| SpeedpackPatch | — | Server | — |
| SpawnDelay | — | Server | — |
| SpawnJumpFix | — | Server | — |
| DistanceLagFix | — | Server | — |
| ObjectBudget | — | Server | — |
| NameBugFix | — | Server | — |
| MapHangFix | — | Server | — |
| VoteExploitFix | — | Server | — |
| WaitlateGrace | — | Server | — |
| UpdateScheduling | — | Server | — |
| ServerLoopRate | Server | Server | — |
| InactiveWindowFix | Server | Server | — |
| AerialGravity | — | Server | — |
| SoldierStateSync | — | Server | — |
| MeleeTiming | Server | Server | — |
| ShotSequence | — | Server | — |
| InputSync | Server | Server | — |
| DirectTransport | — | Server | — |
| StablePlayerIds | — | — | Server |

### Social

Buddy, lobby and voice features. Requires the [Social addon](addons/social).

| Patch | What it does |
| --- | --- |
| [GalaxyBuddies](patches/galaxy_buddies) | Restores the GameSpy buddy system using the Galaxy Friends backend. |
| [LobbyPlayerList](patches/lobby_player_list) | Restores the server player list in the multiplayer lobby browser. |
| [VoiceChat](patches/voice_chat) | Restores working in-game voice chat using the Opus codec. Requires accompanying mod. |

| Patch | Steam | GOG |
| --- | --- | --- |
| GalaxyBuddies | Client | Client |
| LobbyPlayerList | Both | Both |
| VoiceChat | Both | Both |

### Diagnostics

Optional diagnostic captures for troubleshooting.

| Patch | What it does |
| --- | --- |
| [NetworkDiagnostics](patches/network_diagnostics) | Records multiplayer timing, packet intake and sends, prediction, and combat state for troubleshooting. |
| [HeroDiagnostics](patches/hero_diagnostics) | Records melee state, movement, animation, network activity, and input data for troubleshooting. |
| [VoiceDiagnostics](patches/voice_diagnostics) | Records native voice routing, codec and jitter decisions for troubleshooting. |
| [SoundDiagnostics](patches/sound_diagnostics) | Records native sound lifetimes, output service and device results for troubleshooting. |

| Patch | Steam | GOG |
| --- | --- | --- |
| NetworkDiagnostics | Client | Both |
| HeroDiagnostics | Client | Both |
| VoiceDiagnostics | Client | Both |
| SoundDiagnostics | Client | Client |

### MPGC

Multiplayer Galactic Conquest. Requires the [MPGC addon](addons/mpgc).

| Patch | What it does |
| --- | --- |
| [MultiplayerGC](patches/multiplayer_gc) | Enables multiplayer Galactic Conquest campaigns. |

| Patch | Steam | GOG |
| --- | --- | --- |
| MultiplayerGC | Client | Client |

Custom campaign Lua can pass an optional fourth argument to
`ScriptCB_SetMetagameBattleReady(setupId, firstSide, secondSide, rolesBySlot)`.
Use native player slots from `ScriptCB_UpdateMPMetagame().participants` as the table keys, including slot 0.
Give every admitted player role `1` for `firstSide` or `2` for `secondSide`. The host's Lua sends the same
table to each peer, and each peer supplies it when acknowledging the same setup. The roles apply to that
mission's actual teams. Three-argument calls use MPGC's campaign faction assignments.

## Installation

1. Install the latest release of [Fusion Cutter](https://github.com/Alpha5k/FusionCutter).
   Follow the README.txt to copy the correct files to your game folder.
2. Copy the chosen plugin DLLs into the `plugins` directory beside the Fusion Cutter runtime.
   Use Multiplayer on clients and Server on dedicated servers; add Social, Diagnostics or MPGC as needed.
3. For Social's client UI or MPGC, install 1.3 or Remaster 1.5 and copy the matching addon's `GameData` contents
   into your game.
   See [addon instructions](CONTRIBUTING.md#lua-addons) for building entries in free loader slots.
4. Start the game and check `FusionCutter.txt` for the loaded plugins and patch results. Close the game before
   editing the generated `FC.<PluginId>.ini` files, then restart to apply changes.

Keep plugin DLLs and addon files from the same build.

## Building

Build on Windows with a C++23-capable MSVC toolset, the Windows SDK, CMake 3.28 or later, Ninja and Git.
Use an MSVC developer shell and an installed Fusion Cutter SDK matching the target architecture.

Initialize dependencies with `git submodule update --init --recursive`. For x86 builds, also fetch the Opus model
data using the [setup instructions](CONTRIBUTING.md#building).

From the repository root:

```powershell
cmake --preset x86-release "-DCMAKE_PREFIX_PATH=C:/path/to/installed-fc-sdk"
cmake --build --preset x86-release --parallel
ctest --test-dir ../build/fc-plugins/x86-release --output-on-failure
```

Output goes to `../build/fc-plugins/x86-release`. Use `x64-release` in an x64 developer shell for the Classic Collection
Server plugin. Both release presets use `RelWithDebInfo`, which includes debug symbols.

See [CONTRIBUTING.md](CONTRIBUTING.md) for other presets, installation staging and addon build commands.

## License

FC-Plugins uses the [MIT license](LICENSE). Third-party dependencies retain their own licenses.
