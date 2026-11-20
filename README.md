# Refunct Utility v1.0.0

## Installation

**Install in minutes — no installer or compilation required.**

Download the compiled Windows player package from this repository's **Releases** section. Do not download GitHub's automatically generated `Source code (zip)` unless you intend to build the mod yourself.

**ASI Loader:** [Click here to download the Ultimate ASI Loader (d3d11.dll, 32-bit)](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases/download/Win32-latest/d3d11-Win32.zip). Extract the DLL from the ZIP. This is only needed if the mod package does not already include the loader and you do not already have a compatible ASI loader installed.

### Manual installation

1. Close Refunct and download `RefunctUtility-v1.0.0-Win32.zip` from **Releases**. Extract the archive.
2. In Steam, right-click Refunct → **Manage → Browse local files**.
3. Open the nested `Refunct\Binaries\Win32` directory. Confirm it contains `Refunct-Win32-Shipping.exe`.
4. Back up any existing utility ASI, loader DLL and diagnostic INI outside the active game directory before replacing files.
5. Copy `RefunctUtility.asi` and `d3d11.dll` (Ultimate ASI Loader x86, bundled or downloaded above) into that directory. If a compatible ASI loader is already installed, copy only the ASI. Do not blindly overwrite an unrelated DLL or install multiple proxy loaders.
6. Ensure there is only one active utility ASI. Back up and remove any old `RefunctUtility.diagnostic.ini` (normal operation defaults to Mode 6 when absent).
7. Start Refunct and press **F5**.

No installer, batch script or compilation is required. If testing your own build, use `build-win32/Release/RefunctUtility.asi` instead of the bundled ASI and install a compatible x86 loader separately if needed.

Example directory layout (the Steam library prefix may differ):

```text
steamapps/common/Refunct/
├── Refunct/
│   └── Binaries/
│       └── Win32/
│           ├── Refunct-Win32-Shipping.exe    # existing game executable
│           ├── RefunctUtility.asi            # mod
│           └── d3d11.dll                     # Ultimate ASI Loader (32-bit)
└── RefunctUtility/                          # created automatically at runtime
    ├── settings.ini
    ├── Profiles/
    └── Logs/
```

The outer `Refunct` folder is the Steam installation root; the inner `Refunct` folder contains the game's binaries. Install the ASI next to the **actual shipping executable**, not the outer game's root, the content `.pak` folder, or a shortcut to a previous build directory.

Optional compatibility check using PowerShell from the `Win32` directory:

```powershell
Get-FileHash -LiteralPath '.\Refunct-Win32-Shipping.exe' -Algorithm SHA256
```

Compare the result against the fingerprint in [Requirements](#requirements).

### Updating and uninstalling

**Update:** Close Refunct, back up the existing ASI and entire `RefunctUtility/` data folder, then overwrite the installed ASI with the new release. Keep your existing compatible loader unless an update specifically requires changing it. Do not copy build caches or old test files into the game.

**Uninstall:** Close the game and remove `RefunctUtility.asi` from `Win32`. Keep the loader if other mods use it; otherwise remove it according to its own instructions. Keep the data folder if you want to reinstall later. Deleting `RefunctUtility/` erases the utility's settings, profiles, replays and saved records. It is not necessary for a normal uninstall.

**Rollback:** Restore the files you manually backed up outside the active game directory while the game is closed. Restore your separately backed-up data folder if reverting across incompatible changes.

**A native utility, speedrunning, practice, replay and visual customisation mod for Refunct v9.30.**

Refunct Utility extends the original game with an in-game keyboard-controlled menu, configurable movement, practice tools, detailed split timing, persistent personal bests, ghosts, replay cameras, custom maps and independent settings profiles. It is written in C++20 for the 32-bit Windows version of Refunct, using MinHook and native Unreal Engine 4 integration. **UE4SS is not required.**

| Release | Supported game | Architecture | Mod format |
| --- | --- | --- | --- |
| Utility v1.0.0 | Refunct v9.30 (verified executable only) | Windows x86 (32-bit) | ASI |

> This is an unofficial community mod, not an official Refunct feature. Gameplay-changing options are intended for practice and experimentation. Check the rules of the speedrunning category you intend to submit to; the utility's internal record eligibility is not an external leaderboard ruling.

## Contents

- [Installation](#installation)
  - [Manual installation](#manual-installation)
  - [Updating and uninstalling](#updating-and-uninstalling)
- [Requirements](#requirements)
- [Getting started](#getting-started)
- [Default hotkeys](#default-hotkeys)
- [Module reference](#module-reference)
  - [Movement](#movement)
  - [World](#world)
  - [Maps](#maps)
  - [Customisation](#customisation)
  - [HUD](#hud)
  - [Splits](#splits)
  - [Inputs](#inputs)
  - [Ghost](#ghost)
  - [Replay](#replay)
  - [Profiles](#profiles)
  - [Settings](#settings)
- [Recording rules and assisted runs](#recording-rules-and-assisted-runs)
- [Saved files and backups](#saved-files-and-backups)
- [Performance and diagnostics](#performance-and-diagnostics)
- [Troubleshooting](#troubleshooting)
- [Building from source](#building-from-source)
- [Creating a player release](#creating-a-player-release)
- [Source architecture](#source-architecture)
- [Testing and known limitations](#testing-and-known-limitations)
- [Contributing and reporting bugs](#contributing-and-reporting-bugs)
- [Credits and licensing](#credits-and-licensing)

## Requirements

- A legitimate Steam installation of Refunct on Windows 10 or 11.
- The supported **32-bit** Refunct v9.30 executable. A 64-bit Windows installation is fine; the game, mod and loader themselves must be x86.
- A compatible 32-bit ASI loader installed beside the game executable, unless included in the player package.
- Microsoft Visual C++ 2015–2022 Redistributable (x86), if not already installed.
- Write access to the game installation directory for settings, records and logs.

Players do **not** need Visual Studio, CMake, Python, UE4SS or the source code to use a compiled release.

The mod uses version-specific native addresses and validates the game executable. The supported SHA-256 fingerprint is:

```text
f9145dc602580414a81c56ad9c23b618e1ee164a2fb9911f53399c67711027de
```

Other builds may not work, even if they display the same version number. Do not bypass the compatibility check or replace your game executable with an unknown download.

## Getting started

Press **F5** in Refunct to open or hide the utility menu. The menu is drawn inside the game and uses the keyboard; no separate application is required.

| Menu control | Action |
| --- | --- |
| F5 | Open/hide menu (default binding) |
| Up / Down | Move the selection |
| Left / Right | Cycle choices, adjust values and toggle supported settings |
| Enter | Open a submenu, execute an action or toggle the selected option |
| Escape | Return to the previous page |
| `/` | Search settings from the main menu using the search entry |

Rows ending in `>` open another page; rows marked `< >` support arrow adjustments. Some numeric settings use Enter to restore their default value; read the option description. Enable **HUD → Show option descriptions** for additional in-game guidance. Hotkeys can be remapped or unbound under Settings.

A sensible first setup is to enable the run timer, FPS and record status HUDs, select an appropriate Replay / Split profile, and leave movement and World gameplay settings at defaults. Visual customisations can be configured independently. Use a separate practice profile if you intend to record assisted runs.

## Default hotkeys

The following are factory bindings, not necessarily your current settings. All other actions are unbound initially. See Settings → Hotkeys for the complete rebindable action list.

| Key | Action |
| --- | --- |
| F1 | Load saved state |
| F2 | Load / replay selected practice section |
| F3 | Toggle run timer |
| F4 | Save current state |
| F5 | Utility menu |
| F6 | Toggle flying |
| F7 | Toggle noclip |
| F8 | Save player position |
| F9 | Restore saved player position |
| F10 | Toggle player coordinates HUD |
| R | Restart run |
| G | Preloaded restart |
| H | Restart with progress |
| B | Load last button checkpoint |
| N | Load second-last button checkpoint |
| L | Full level reload |
| Page Up | Increase flight speed |
| Page Down | Decrease flight speed |

F2 is section **practice**, not the main saved-run replay viewer. Start spectator replays from **Replay** or assign the separate Start / stop ghost replay action in Hotkeys.

## Module reference

The F5 main menu contains the following feature modules in order.

| Module | Purpose |
| --- | --- |
| Movement | Flight, physics and jump controls |
| World | Level progression, practice and object interactions |
| Maps | Load supported custom layouts or restore the original world |
| Customisation | Appearance, camera, environment and crosshair settings |
| HUD | General information overlays and presentation |
| Splits | Timing, record comparisons and split displays |
| Inputs | Keyboard input display |
| Ghost | Race against saved movement recordings |
| Replay | Spectate PB and fastest-segment recordings |
| Profiles | Manage independent sets of records and settings |
| Settings | Hotkeys, persistence and logs |
| Search settings | Locate options across the menu |

### Movement

Controls how the native character moves. Most physics values use percentages, with 100% representing the game's normal value. Extreme values can cause unusual physics; change gradually.

| Submenu | Features |
| --- | --- |
| Movement Modes | Flying, noclip, preserve momentum |
| Speed & Physics | Fly speed, movement speed, swim speed, climb speed, slide speed, slide distance, jump height, acceleration, game speed |
| Jump Controls | Scroll-wheel jump, hold-Space jump, air jump |
| Reset movement settings | Restore normal movement settings |

- **Flying:** Move with WASD, Space to ascend and Shift to descend. Adjust speed using Page Up/Page Down or the menu.
- **Noclip:** Enables movement through collision geometry. Be careful when returning to normal collision inside an object.
- **Preserve momentum:** Changes momentum handling for movement practice.
- **Scroll jump:** Allows the mouse wheel to trigger jumps in the selected contact states.
- **Hold Space jump:** Repeats eligible jumps while Space is held.
- **Jump targeting:** Both assisted jump controls support Off, ground, wall, climb and combinations of those three contact states.
- **Air jump:** Allows additional airborne jumping when enabled.

Movement changes and jump assists are gameplay-affecting and can affect record eligibility. Control whether they persist after restarting the map or relaunching the game under Settings → Persistence.

### World

World changes native level objects, physical properties and practice state. Unlike purely visual changes, many World actions affect the course or progression.

#### Sections

- Raise or lower all sections, or reset their placement to Auto (native progression).
- Choose Auto, Raised or Lowered for individual sections.
- Enable/disable section rise animations and adjust rise speed.
- Adjust X, Y and Z offsets for all sections or individual sections; restore their original positions.
- Inspect button synchronisation status for section overrides.

The native map has 31 progression clusters and 37 physical buttons; a cluster can contain multiple buttons. Button count and section count are therefore not the same thing.

#### Run & Practice

| Submenu | Actions |
| --- | --- |
| Restart Controls | Normal restart, preloaded restart, restart with progress, rise-animation preference, full level reload |
| Positions & Teleport | Save/load position, set a target XYZ, use current position as target, teleport to last global button |
| Save States | Save/load a checkpoint containing position and button progress |
| Button Checkpoints | Load last or second-last button; optional momentum preservation |
| Section Practice | Choose a button and load/replay its practice section |

Normal restart starts a fresh attempt. Preloaded restart provides an alternative restart path. Restart with progress restores the previous progression using the mod's existing restore logic. Full level reload reloads the current level without closing the entire game and uses a confirmation prompt in the menu.

A saved position is not the same thing as a saved checkpoint: a position is a location; a checkpoint captures progression information as well. Practice loads are designed for repeated attempts at a particular part of the course, not for submitting a clean full-game PB.

#### Buttons

- Restore game-default button states.
- Enable buttons above water or all buttons, including underwater.
- Disable buttons above water, underwater or everywhere.
- Press all available buttons.
- Repair ghost buttons now (buttons with inconsistent visual/contact state, unrelated to the race ghost).
- Prevent button tile selection, or remove buttons.
- Access button colours and beacon customisations.

#### Tiles

Select or deselect tiles above water, or all tiles including underwater. Disable tile selection through contact or remove tile geometry. Tile and wall appearance controls are also accessible here. The original level contains 251 tiles/platforms. Selecting tiles and changing their colour are different operations.

#### Cubes

Disable cubes, collect all cubes, restore their default visibility, spawn them above water or including underwater, and remove cubes. Removing a cube is not equivalent to collecting it. The original map contains 18 cubes.

#### Tubes, Platforms and Jump Pads

| Object | Gameplay options | Visual options |
| --- | --- | --- |
| Tubes | Travel speed, reset to 100%, remove tubes | Tube colour |
| Moving platforms | Platform speed, reset to 100%, remove platforms | Platform colour |
| Jump pads | Launch height, reset to 100%, remove jump pads | Jump pad colour |

#### Water

Remove the water surface, disable swimming/water gravity behaviour, or use the master water-removal setting. Adjust the water surface height in Unreal units and reset it to its original height. Water appearance is available in a separate customisation page. Changes to water physics or height affect gameplay.

#### World profiles and reset

Use Remove all objects or Restore all removed objects for broad object removal controls. World Profiles save persistent World gameplay settings. Reset World gameplay settings restores the corresponding defaults; it is not the same as wiping the saved game or resetting movement and visual profiles.

### Maps

The Maps module loads supported Refunct TAS map v1 layouts from the utility's `Maps/` directory. It changes existing native world objects rather than importing arbitrary Unreal Engine levels.

1. Put a compatible map file in `RefunctUtility/Maps/`.
2. Open F5 → Maps → Refresh maps folder.
3. Browse the list using Previous page and Next page.
4. Select `Load: <map name>`.
5. Use Restore original world to return to the built-in layout.

The parser accepts version-1 JSON map data (files with `.json`, `.map`, `.txt`, or no extension are considered). It expects exactly 31 clusters and validates the original object totals: 251 platforms, 18 cubes, 37 buttons, 3 lifts, 9 pipes and 4 springpads. Arbitrary `.pak` files or general UE4 maps are not supported by this importer. Refer to `include/refunct/custom_map_format.hpp` for the precise schema and validation rules. Back up your data before experimenting and do not assume custom layouts are eligible for vanilla speedrun categories.

### Customisation

The Customisation module contains visual settings, environment controls, camera FOV and crosshair options. Many colour controls offer a six-digit RGB HEX input (for example `#00FFFF`) or the Windows colour picker; use the indicated arrows to switch input methods. Restore individual settings to Game default or use the module-wide Restore default customisations action.

| Option | Description |
| --- | --- |
| Button Colours | Separate unpressed/pressed button colours and border colours |
| Button Beacons | Show/hide beacons and change their colour |
| Tile Colours | Selected and unselected colours; random RGB for newly selected tiles |
| Wall Colours | Selected and unselected wall appearance |
| Tube Colours | Override the tube colour |
| Platform Colours | Override moving-platform colour |
| Jump Pad Colours | Override jump-pad colour |
| Water | Surface and underwater colour/intensity, underwater tint and splash visibility |
| Time | Set the in-game clock, lock it, change time speed and restore native time |
| Sky | Zenith, horizon, sun and cloud colours; sun/moon, cloud and star visibility; general and directional fog colours and visibility |
| Shadows | Shadow visibility, ambient shadow colour and default restoration |
| Pillars | Decorative pillar colour and removal |
| Camera FOV | Adjust normal gameplay field of view in degrees or restore the game default |
| Water Reflections | Global and category-specific reflection controls |
| Crosshair | Native default, editable replacement styles and visibility |
| Customisation Profiles | Save and switch appearance presets |
| Restore default customisations | Reset visual settings without deliberately erasing records or progression |

**Water Reflections:** All seven controls are on one page: the global switch and reflection visibility for walls/platforms, buttons, pipes, jump pads, cubes and button beacon beams. These are native reflection controls; some water appearance can also originate from screen-space effects or the water shader. Individual exclusions depend on verified native component layout and are not a guarantee that every visible reflection will disappear.

**Crosshair:** Choose Game default, Dot, Cross, Cross + dot, Diamond or Off. Adjust colour, size (1–100 px), thickness (1–20 px where applicable), gap (0–50 px where applicable) and opacity (0–100%). Solid dots do not have a separately adjustable thickness; dots and diamonds do not use a gap. Editing the game-default appearance selects a replacement style (gap adjustment selects Cross). Custom styles suppress the native reticle and draw a replacement, rather than modifying the original material shader. If native suppression fails, the replacement may not draw; inspect the status displayed in the menu. Restore default crosshair returns native rendering.

**Time:** Lock Time freezes the chosen clock value; leaving the lock off allows the clock to progress. Time Speed controls the day/night cycle rather than the overall physics speed, which is found in Movement.

### HUD

General information overlays can be enabled independently in **Side HUD Elements**:

| Element | Information shown |
| --- | --- |
| Player XYZ | Current world coordinates |
| Cubes collected | Cube counter |
| Tiles coloured | Tile counter |
| Frame rate (FPS) | Frame rate |
| Enabled features | Active utility features |
| Practice status | Current practice state |
| PB recording status | Eligibility/record status |
| Movement speed | Player speed in game units per second |
| Level completion (%) | Progress through the level |
| Active gameplay cheats | Gameplay-changing modifications |
| Active visual settings | Cosmetic customisations |
| Game day/night clock | Native in-game time |
| PC local clock | Local system time |
| Current button number | Current progression count |

The run timer and saved split-related HUDs live in the **Splits** module. The keyboard display lives in **Inputs**.

Under HUD → Customisations, adjust each general overlay's position (corners or centre), colour, text scale (50–200%), header visibility and supported displayed-value modes. The utility menu itself has its own corner position. Toggle option descriptions for help, restore all HUD display settings (including splits), or save a complete layout under HUD Profiles.

### Splits

Built-in speedrunning timing and record comparison tools. Splits are attached to individual button progression events.

| Split HUD element | What it shows |
| --- | --- |
| Run timer | Elapsed run time |
| Full-run PB | Saved personal-best total |
| Sum of best segments | Sum of independently fastest saved segments |
| Run time at last button | Elapsed time when the most recent button was reached |
| Other profile time | Reference time from the selected comparison profile |
| Fastest button time | Best individual segment or supported cumulative view |
| PB time at button | PB reference at the corresponding button |

- **PB:** A saved eligible completed run.
- **Split:** The cumulative time at a progression point.
- **Segment:** The time between two progression points.
- **Sum of best:** A theoretical combination of independently fastest segments; not necessarily a completed run.

In **Split Popups & Comparisons**, enable button split popups and choose rows for the button number, elapsed run time, time since previous button, ahead/behind PB, fastest segment delta, other-profile comparison, section vs PB, section vs other profile and time gained/lost. Supported row modes let you choose whether a value and/or its header appears. Negative time differences normally indicate being ahead; positive differences indicate being behind.

Set a reference profile and choose PB splits or cumulative best splits under Compare Profile & Mode. Use Reload compared profile after external record-file changes. Saved Split Times provides the corresponding persistent time HUD controls; Split HUD Customisations adjusts their presentation.

Record-saving options live under **Profiles → Replay / Split Profiles → Record Saving**. See [Recording rules and assisted runs](#recording-rules-and-assisted-runs).

### Inputs

An optional read-only display of WASD, Space and Shift. Held keys light up; released keys dim. It reads input while the game is focused and does not inject or remap gameplay input. Use Customise key display to configure its position, colour, size and header. Input display settings are included in HUD Profiles.

### Ghost

Race against movement recorded in your active Replay / Split profile while playing normally.

| Setting | Description |
| --- | --- |
| Show ghost | Show/hide the race ghost; eligible recording can continue while hidden |
| Ghost mode | 2D screen-space silhouette or 3D world-space no-collision ghost |
| 2D transparency | Translucent or solid 2D rendering |
| 3D transparency | Translucent or solid 3D rendering |
| Ghost colour | Shared colour using HEX or the Windows picker |
| Start next ghost on button | Synchronise segments to your button progression; otherwise play continuously |
| Use fastest sections | Prefer the fastest saved segment tracks, with available fallback recordings |

With fastest sections off, the normal ghost prefers the full PB, or the furthest saved run when no PB exists. Recording depends on Record Saving permissions, not the visibility toggle. Movement samples are recorded at a nominal 20 Hz. The recording format supports horizontal yaw, vertical pitch and native button identity in current runs. Older files remain readable but cannot recover data they never recorded.

Ghost is for racing a reference **while you play**; Replay is for watching a reference **as a spectator**.

### Replay

Replay is a separate main-menu module. It reconstructs recorded character movement and native world progression in the game, with several camera modes. It is not a video file exporter.

#### Watch a replay

1. Complete and save an eligible run, or use a profile containing an appropriate saved recording.
2. Open F5 → Replay.
3. Select **Replay profile** and use Left/Right to choose the saved profile. This also changes the active split/ghost profile.
4. Set **Source** to PB or Best segments.
5. Choose a camera and select **Replay: Start**. The mod restarts/prepares the level before playback.
6. Hide the menu with F5 to watch, or keep it open to adjust playback settings.

| Control | Function |
| --- | --- |
| Replay: Start / Stop | Start or safely stop spectator playback |
| Replay profile | Switch the active saved run profile; stops any active replay |
| Source | PB (completed personal best) or Best segments (saved gold tracks with PB fallbacks) |
| Pause / resume | Pause/resume replay motion and progression |
| Restart from beginning | Replay from the start |
| Seek backward 5 seconds | Rewind; may require a native level reset |
| Seek forward 5 seconds | Advance while applying required button progression |
| Playback speed | 0.5x, 1x or 2x |
| Playback time | Replay clock, duration or preparation status |
| Camera | Cycle the four viewing modes |

#### Camera modes

| Camera | Behaviour |
| --- | --- |
| Locked third person | Follows recorded yaw and pitch around the ghost |
| Third person | Manual mouse orbit; scroll up/down to zoom (including while the menu is open) |
| First person | Eye-level view using recorded yaw and pitch and a fixed 130° FOV |
| Free cam | Independent flight: WASD, Space up, Shift down, mouse look and arrow-key look with the menu closed; uses configured fly speed |

The first-person replay FOV does not overwrite the normal gameplay FOV preference. When leaving that mode, normal FOV handling resumes.

#### World interaction during replay

The replay uses native button activation, reconstructs tile contact and attempts native cube pickup as the recorded player passes through objects. New recordings store physical button IDs; older tracks rely on position matching, especially in multi-button sections. Replay movement, platform rise, tile contact and cube collection are distinct systems. Seeking can require resetting/reconstructing the world, so do not expect arbitrary seek operations to behave exactly like a recorded video timeline.

Stopping or restarting playback may reset the level again to clean up replay progression. Saved PBs and ghost files are not intentionally changed by viewing a replay. Independently earned fastest segments may have incompatible button orders. Tile and cube contact is reconstructed from samples and native geometry, not a complete recording of every original collision event. Old recordings may therefore be visually approximate.

### Profiles

Profiles are independent groups of related settings and records. Switching a HUD profile, for example, does not switch your PB or World profile.

| Profile type | Contents | Also accessible from |
| --- | --- | --- |
| Replay / Split | PB totals, button splits, best segments, saved ghost/replay tracks, record metadata | Replay (profile selector) |
| Customisation | World appearance, time, sky, fog, shadows, FOV, crosshair and reflection settings | Customisation |
| HUD | General HUD, split displays, comparison presentation, menu appearance and Inputs | HUD |
| World | Persistent World gameplay settings, including section animation/speed, interaction toggles, object removal, tube/platform/jump-pad tuning and water settings | World |
| Hotkey | Assignments for the utility's rebindable actions, including unassigned keys | Settings → Hotkeys |

**Replay / Split Profiles** has a list of saved profiles, Create new profile, Rename active profile, Refresh files from disk, Reload active split file and Record Saving. Select the desired profile with Enter. Changing the Replay profile in the Replay module selects this same active record profile, not a separate spectator-only profile.

The four settings-profile managers support creating, renaming, selecting, explicitly saving, reloading, refreshing and deleting profiles. Deletion has a confirmation screen. Save the active profile before making experimental changes if you want a known baseline, and keep an external backup of particularly important configurations. Existing live settings can be imported into the initial Default profile.

World Profiles are not complete world snapshots: transient section placement/offsets, already pressed buttons, selected tiles, collected cubes and saved practice checkpoints are not stored as a separate run state in each World profile. Use the dedicated practice/save-state tools for progress. Persistence options may reset live World settings at startup or restart without intentionally overwriting the stored profile.

Example setup:

- Replay / Split: `Any%` for normal attempts and `Practice` for experimentation.
- Customisation: `Default` and a personal colour scheme.
- HUD: `Minimal` for playing and `Analysis` for detailed split comparisons.
- World: `Vanilla` and a separate practice configuration.
- Hotkey: `Default` and a preferred practice layout.

These example names are suggestions, not preinstalled profiles.

### Settings

#### Hotkeys

The release defines bindings for movement, restarts, practice, HUD and timers, time/maps/world actions and ghost/replay actions. Hotkeys are grouped into six pages:

- General & Restarts
- Movement & Jump
- Saves & Practice
- HUD & Timers
- Time, Maps & World
- Ghost & Replay

Select a binding and use Enter to enter key capture. Follow the prompts to assign an unused key. Backspace on a hotkey row clears the assignment. Conflicting or reserved bindings are checked. F5 remains a menu recovery key when the menu binding has deliberately been unassigned; if you remap the menu to another key, use the assigned key instead.

Use Hotkey Profiles to maintain different layouts, or Restore default hotkeys to return to the factory assignments.

#### Persistence

Four independent options determine whether live movement and World gameplay modifications survive:

| Setting | When enabled |
| --- | --- |
| Keep movement on game launch | Movement tuning, flight and jump helpers persist across launches |
| Keep movement on map restart | Movement settings remain active on normal map restart |
| Keep world on game launch | Persistent World gameplay settings remain active across launches |
| Keep world on map restart | Persistent World gameplay settings remain active on normal map restart |

When corresponding persistence options are off, normal launch/restart paths restore the affected group to defaults. Practice loads have their own behaviour. Visual customisations, including camera FOV, are independent of these gameplay persistence controls.

#### Logs

| Setting | Purpose |
| --- | --- |
| Save Logs | Master runtime logging switch, including performance/optimisation reports |
| Startup Logs | Controls the startup trace, subject to the master switch |
| Clear Logs | Delete eligible runtime logs after confirmation |

There is no separate Optimisation Log toggle in v1.0.0. Performance reports are part of the main runtime log and follow Save Logs. Startup logging starts on application initialisation; enable its preference before relaunching when investigating startup issues. When Save Logs is off, startup logging is paused without discarding its individual preference.

Clearing logs is asynchronous and does not intentionally erase settings, records, profiles or replays. Locked files may remain. If logging is enabled, new log output can appear immediately after clearing. Back up any log you intend to attach to a bug report before clearing it.

## Recording rules and assisted runs

The utility separates gameplay-changing settings from recognised visual customisations. In the v1.0.0 implementation, camera FOV and appearance-only controls belong to the visual category; movement tuning, jump assists, altered section progression, World physics changes, custom maps and synthetic progression actions can affect gameplay eligibility. Do not infer a setting's classification solely from which menu contains it. Consult the Active gameplay cheats and PB recording status HUDs.

Under **Profiles → Replay / Split Profiles → Record Saving**, configure:

| Option | Purpose |
| --- | --- |
| Save personal bests | Allow an eligible faster completed run to replace the profile PB |
| Save fastest segments | Save individual faster button-to-button times when eligible |
| Save PB ghosts | Save PB movement tracks and eligible furthest unfinished recordings |
| Save fastest-section ghosts | Save movement tracks for newly earned fastest segments |
| Save with cheats | Off, PB only, Fastest segments only, or Both |
| Create profile on new PB | Create a numbered PB snapshot profile alongside the active profile |
| Copy fastest segments to new profile | Include independently earned best segments in that automatic PB snapshot |

Turning off saving does not delete existing data. Ghost-saving permissions are independent of showing the ghost, but remain subject to the record rules. Allowing cheat-assisted saving does not override every restriction: practice state, synthetic progression and whether the run started from an appropriate state are also considered. A run may remain tainted even after a gameplay modification is disabled; start a fresh normal attempt with the desired settings.

**Recommended clean-run setup:**

1. Select your normal category profile.
2. Reset Movement and World gameplay settings, and restore the original map if a custom one is active.
3. Set Save with cheats to Off. Enable PB and segment saving if required.
4. Check Active gameplay cheats and PB recording status.
5. Start a fresh run using the ordinary restart path.
6. Follow the external leaderboard/category rules independently of the mod's internal eligibility checks.

Use a separate profile for practice or assisted records to avoid mixing them with normal attempts.

## Saved files and backups

In a standard Steam installation, persistent utility data is stored in the `RefunctUtility/` folder at the outer game root, not inside the Win32 binary folder. The storage path is logged on startup. For a nonstandard installation layout, storage can be created beside the game executable instead.

Typical layout (directories and files are created as needed):

```text
RefunctUtility/
├── settings.ini
├── save-state.ini
├── Logs/
│   ├── RefunctUtility.log
│   ├── RefunctUtility.startup.txt
│   └── ... archived or migrated logs ...
├── Profiles/
│   └── Any%/
│       ├── Splits/
│       │   └── records.ini
│       └── Ghosts/
│           ├── PB.ghost
│           └── BestSegments/
│               ├── gold01.ghost
│               ├── gold02.ghost
│               └── ...
├── CustomisationProfiles/
│   └── Default.ini
├── HUDProfiles/
│   └── Default.ini
├── WorldProfiles/
│   └── Default.ini
├── HotkeyProfiles/
│   └── Default.ini
└── Maps/
    └── ... user-supplied maps ...
```

- `settings.ini` holds live preferences and selected settings-profile information.
- `save-state.ini` holds persisted checkpoint data.
- `records.ini` contains profile record information, including splits and independently earned best segments.
- `PB.ghost` stores the completed PB movement record, or the furthest eligible incomplete recording before a completed PB exists.
- `BestSegments/goldNN.ghost` files hold independent segment tracks.
- The four settings-profile directories hold named `.ini` profiles.
- `Logs/` contains runtime information, not your PBs or gameplay settings.

Older versions may have legacy `Splits/`, `Ghosts/` or separate `furthest.ghost` files. The current source includes migration paths; retain old files until you confirm the new profile loads correctly. Some older AppData settings can also be imported when no local file exists.

**Backup:** Close the game normally so pending asynchronous saves can finish, then copy the entire utility data folder to a separate location. Keep a copy of your working ASI and loader if preparing a new release. When sharing a replay or profile, keep its associated record and ghost files together. Do not edit INI or ghost files while the game is running, since the game can write its own settings and records.

## Performance and diagnostics

The mod uses asynchronous logging and selected settings/record writes, cached actor and reflection discovery, incremental work, batched cosmetic updates and throttled retries for expensive native checks. Static customisations avoid unnecessary work where possible; dynamic effects and native object-lifetime validation remain active where needed.

These are implementation techniques, not guarantees of a particular FPS or a stutter-free session. The native game, graphics driver, hardware, enabled features and loaded objects can all affect frame pacing. The performance monitor measures selected utility callbacks and execution intervals, not the entire GPU pipeline.

### Investigating stuttering

1. Enable Save Logs in Settings → Logs before the test.
2. Start a fresh session and play normally, using the settings that reproduce the issue.
3. Continue until the hitching or deterioration occurs, if possible.
4. Close Refunct normally and collect `RefunctUtility/Logs/RefunctUtility.log`.
5. Compare initial and later samples, noting whether you changed maps, profiles, replay mode or visual settings.

Performance reports are written approximately every five seconds while the instrumented callbacks run. They include average FPS estimates, estimated 1% and 0.1% lows, worst intervals and selected module timings. Look for `Performance` entries in the main log. Native and utility callback timings are not equivalent to comprehensive render profiling. A stable average FPS can still conceal poor frame pacing, and memory growth requires separate evidence rather than being inferred from stutters alone.

Disable Save Logs if you do not need diagnostics. Startup Logs can be disabled independently, or enabled before a test that investigates loading or initialization. Build logs generated by CMake/MSBuild remain in the source build directory and are not cleared by the runtime log menu.

## Troubleshooting

| Symptom | What to check |
| --- | --- |
| F5 does nothing | Confirm the ASI and compatible x86 loader are next to the shipping EXE; check the menu binding, game focus, runtime dependency and startup log. |
| Mod loads in settings-only mode | Remove the development `RefunctUtility.diagnostic.ini` after backing it up. Mode 2 does not enable normal gameplay hooks. |
| Mod rejects the executable | Verify its SHA-256 against the supported fingerprint. Do not bypass the check. |
| Architecture or DLL error | Use the x86 ASI, x86 loader and x86 VC++ runtime, even on 64-bit Windows. |
| New features do not appear | Check the timestamp/hash of the actual installed ASI; ensure no shortcut is pointing at an older build and no old utility ASI remains loaded. |
| Game crashes during initialization | Remove duplicate or incompatible mods/loaders, verify executable integrity, backup and remove diagnostic overrides, and collect the startup trace if available. |
| Settings or profiles appear missing | Check the actual utility storage directory, selected profile, restrictive permissions and launch/restart persistence settings. |
| Replay cannot start | Ensure the selected profile contains a compatible completed PB or eligible segment recordings for the selected source. |
| Old replay looks straight ahead | Old files may not contain vertical pitch. Record a new eligible run to capture it. |
| Replay presses the wrong button | New recordings contain native button identities; older recordings use position matching. Send the exact profile and relevant log if it remains reproducible. |
| Replay misses a tile or cube | Contacts are reconstructed from movement samples and live native geometry; report the recording and playback timestamp. |
| Crosshair colour or size does not appear | Choose an editable style, check native-suppression status and reset to Game default if necessary. |
| Objects still appear in water reflections | Native planar reflection exclusions might not affect other water rendering sources; report the toggle and screenshot. |
| No optimisation log file | Optimisation reports are inside the main `Logs/RefunctUtility.log`, not a separate file. Enable Save Logs. |
| Clear Logs leaves a file | Close other applications using the log and retry; current logging can create a new log immediately. |
| PB is not being saved | Check Record Saving, active profile, gameplay-cheat status and practice/run eligibility. Restart a clean run. |
| Build succeeds but game behaves like an old release | Check that you actually copied the newly generated `build-win32/Release/RefunctUtility.asi` to the executable directory. Rebuilding alone does not install it. |

If you need to check installation integrity, compare the installed ASI with your release/build copy using PowerShell:

```powershell
Get-FileHash -Algorithm SHA256 -LiteralPath '.\RefunctUtility.asi'
```

The two matching copies should have the same SHA-256. This checks file equality, not whether the loader successfully loaded the ASI into the running process. If no runtime logs are created, loader initialization or disabled logging may be the reason; absence of a log alone does not prove either.

## Building from source

The public source archive contains the C++ implementation, headers, tests, CMake configuration, Windows build scripts, installer source and bundled MinHook dependency. It does not require the original Refunct source code or UE4SS.

### Developer requirements

- Windows 10/11.
- Visual Studio 2022 with **Desktop development with C++**, MSVC x86 tools and a Windows SDK.
- CMake 3.24 or newer.
- Python 3 for Python regression scripts (not required for the ordinary native build).
- A compatible C++ compiler for tests that compile portable mocks.

The bundled `third_party/minhook/` source is preferred automatically, so the normal build need not fetch MinHook online. A local alternative may be selected using the `REFUNCT_MINHOOK_SOURCE` CMake cache variable. Without either, CMake can fetch the pinned MinHook v1.3.4 release, which requires Git and network access.

### Build using the supplied script

Extract the complete source archive, or clone the published repository using GitHub's Code / Clone instructions.

From the project root, run:

```bat
build_win32.bat
```

Output:

```text
build-win32/Release/RefunctUtility.asi
```

The script builds for Win32 and writes compilation/configuration logs. It does not copy the ASI to the game and does not prompt to install it. Follow [Manual installation](#manual-installation) to test the resulting binary.

### Build using CMake directly

Open a terminal where CMake and Visual Studio 2022 are available, and run from the project root:

```bat
cmake -S . -B build-win32 -G "Visual Studio 17 2022" -A Win32
cmake --build build-win32 --config Release --target RefunctUtility
```

Use `-A Win32`, **not** `-A x64`. The project deliberately rejects a 64-bit Windows configuration. Non-Windows configurations build portable test targets only, not the native ASI. If switching architecture or moving a build directory, delete only the generated build directory or let the supplied script repair the cache; do not delete source files or player data.

The utility release and CMake project version are both `1.0.0`. The supported Refunct game version remains `9.30`.

## Creating a player release

Build the mod using the instructions above if the source has changed. If the latest working ASI is already compiled, no rebuild is needed merely to package it. Create `RefunctUtility-v1.0.0-Win32.zip` with the following contents at its root:

```text
RefunctUtility.asi
d3d11.dll
INSTALL.txt
licenses/
    MinHook-LICENSE.txt
    Ultimate-ASI-Loader-LICENSE.txt
```

Use the tested x86 Ultimate ASI Loader, preserve all applicable third-party license notices and verify that the packaged ASI is the intended release build. Optionally publish a SHA-256 checksum alongside the ZIP. No installer is necessary. The source archive may retain older optional installer-generation tools; these are not needed for the manual release.

Before publishing, test the ZIP on a clean supported Windows/Refunct installation, confirm F5 works, test a fresh run and restart, switch profiles, verify a replay and check that uninstalling preserves saved files. Upload the compiled ZIP to GitHub Releases, alongside the source repository. Do not upload game executables, `.pak` assets, private logs, personal settings or saved records. GitHub's automatically generated source archives are not the compiled player package.

## Source architecture

The project is split into native feature modules, common state, storage and game-integration layers. This is an approximate architecture overview; see the implementation for exact call order and native signatures.

```text
ASI loader
    |
    v
bootstrap + executable compatibility + MinHook
    |
    +-- native character tick ---> gameplay, practice, world, timer, records
    +-- native input hooks ------> hotkeys and keyboard menu
    +-- native HUD hook ---------> overlays, menu and ghost/crosshair rendering
    |
    +-- shared state / reflection / game ABI
    +-- settings / profiles / recordings / asynchronous log writer
```

| Source area | Responsibility |
| --- | --- |
| `src/bootstrap.cpp`, `src/compatibility.cpp` | Initialisation, pinned executable validation and hook setup |
| `src/core.cpp`, `src/state.cpp` | Shared infrastructure, state and logging |
| `src/game_tick.cpp`, `src/input.cpp` | Tick orchestration and input handling |
| `src/menu.cpp`, `src/hud.cpp` | Menu entries, navigation, descriptions and HUD rendering |
| `src/movement.cpp`, `src/restart.cpp` | Movement, camera FOV and restart implementation |
| `src/checkpoint.cpp`, `src/practice.cpp` | Saved checkpoints and section practice |
| `src/records.cpp`, `src/timer.cpp` | Run timing, record eligibility, splits and PB data |
| `src/ghost.cpp` | Ghost recording, race playback, spectator cameras and replay contacts |
| `src/sections.cpp`, `src/section_heights.cpp` | Native progression, buttons and section placements |
| `src/tiles.cpp`, `src/cubes.cpp` | Native tile and cube interactions |
| `src/tubes.cpp`, `src/platforms.cpp`, `src/jumppads.cpp` | Traversal objects, speed/height and appearance |
| `src/world_object_removal.cpp`, `src/maps.cpp` | Object removal and custom map loading |
| `src/water.cpp`, `src/time_of_day.cpp`, `src/sky.cpp`, `src/fog.cpp`, `src/shadows.cpp`, `src/pillars.cpp` | Environment and visual/physics customisation |
| `src/storage.cpp`, `src/*_profiles.cpp` | Persistent preferences and independent settings profiles |
| `src/reflection.cpp`, `include/refunct/game_abi.hpp` | Native UE4 lookups, object layouts and memory integration |
| `src/performance.cpp` | Timing instrumentation and performance reporting |
| `src/pure.cpp`, `include/refunct/` | Portable algorithms, format definitions, feature headers and interfaces |
| `tests/` | Portable C++ tests and Python regression scripts, including compiled mocks |
| `third_party/minhook/` | Bundled MinHook source and upstream license |
| `release/` | Inspectable installer and package-generation scripts |

The native integration uses fixed RVAs (relative virtual addresses), native function hooks and reflected Unreal objects/properties. Game updates may invalidate these assumptions. Do not port by simply replacing the executable hash; verify the native layouts, addresses and calling conventions first. Preserve Win32 calling conventions and object-lifetime safety checks when modifying hooks. Add new translation units to `CMakeLists.txt` where necessary.

## Testing and known limitations

Portable tests can be built without the original game. On Windows, configure with the Win32 generator:

```bat
cmake -S . -B build-tests -G "Visual Studio 17 2022" -A Win32
cmake --build build-tests --config Release
ctest --test-dir build-tests -C Release --output-on-failure
```

On Linux or another supported non-Windows development host, use a C++20-capable compiler:

```sh
cmake -S . -B build-tests
cmake --build build-tests
ctest --test-dir build-tests --output-on-failure
```

Python regression scripts under `tests/` can be run individually with Python 3. Some check source integration; others compile mocks. For example:

```text
python tests/<test-script>.py
```

The last documented release-cleanup baseline reported 206 of 221 Python checks and all three portable C++ tests passing. The 15 failing checks were also reported on the pre-cleanup baseline. These figures describe the recorded development result, not a fresh Windows validation of the manual release. Real Windows compilation and clean-install/in-game validation of the final package are still necessary before publishing a tested release. Source-level checks and mock tests cannot prove that native hooks work in a running game.

Known limitations:

- Only the pinned Refunct v9.30 Win32 executable is supported.
- Replay reconstruction of older tracks depends on information actually saved; old files lack vertical pitch or physical button identity if originally recorded without those fields.
- Replays made from independent fastest segments can have inconsistent multi-button ordering.
- Tile and cube contacts are estimated from movement and verified native geometry rather than losslessly recorded collision events.
- Individual reflection exclusions and custom crosshair suppression depend on native component/property validation; visual effects supplied by other rendering paths may remain.
- Custom map loading expects the supported TAS v1 schema and original object totals; it does not load arbitrary UE4 assets.
- Extreme physics values and unsupported combinations of world edits are experimental.
- The compiled player package should be tested on a clean Windows/game installation before distribution.

## Contributing and reporting bugs

Use the repository's Issues section to report reproducible bugs and its Pull Requests section to propose changes. Please explain the problem and include sufficient evidence rather than only reporting that something is broken.

Useful report information:

- Utility version, game version/fingerprint and Windows version.
- Whether installed from the compiled player ZIP or from a local build.
- Exact menu path, setting and expected versus actual behaviour.
- Steps to reproduce, including active profiles, replay source and playback timestamp if applicable.
- Runtime log from `RefunctUtility/Logs/RefunctUtility.log` and startup trace for initialization issues.
- Compilation errors for build problems, including the first relevant compiler/linker error.
- Screenshot/video or a minimal compatible replay/profile when it helps reproduce the problem.

Review logs before uploading: they can include local folder paths, profile names and gameplay details. Remove unrelated private information. Do not attach the game EXE, PAK files, license-restricted assets or personal data unnecessarily.

Development guidelines: keep changes modular, preserve saved-data compatibility, distinguish cosmetics from gameplay assistance, avoid expensive per-frame discovery or disk writes, validate native memory access, and add regression coverage for changes. Describe exactly what was tested and whether any native Windows or gameplay validation remains outstanding.

## Credits and licensing

- **Refunct Utility:** community-developed native mod for Refunct.
- **Refunct:** the original game and its assets belong to their respective owners. This project is not affiliated with the original game's developers.
- **MinHook:** native hooking library by its upstream contributors. See the included license under `third_party/minhook/` and the notice included in release packages. Upstream: [TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook).
- **Ultimate ASI Loader:** bundled x86 `d3d11.dll` by ThirteenAG, under its MIT license. See the included notice. Upstream: [ThirteenAG/Ultimate-ASI-Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader).

The source is provided for inspection. The supplied source archive does not assign a license to the utility's own code. Until the repository owner publishes a license granting additional rights, do not assume permission to redistribute modified versions or reuse the source merely because it is publicly visible. Third-party components retain their respective licenses. No original Refunct executable or game asset is distributed in the source package.
