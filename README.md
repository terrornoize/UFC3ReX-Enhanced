# UFC Undisputed 3 Recomp: Enhanced

> **This is a fork** of [360recomper/UFC3ReX](https://github.com/360recomper/UFC3ReX). The recompilation and almost
> all of this project are 360recomper's work, and the native renderer is nefariousjosiah's (see Credits below). This fork
> adds the fixes and options listed under [What this fork adds](#what-this-fork-adds). It is offered back to the
> original project ([issue #13](https://github.com/360recomper/UFC3ReX/issues/13)). The launcher here updates from
> **this fork's** releases. The DLSS 5 add-on still comes from the original project.

## What this fork adds

- **Native renderer VRAM leak fixed:** video memory no longer keeps growing during long sessions. Retired and abandoned
  surfaces are freed, and so are idle resolve targets. Each fix can be turned off with a bit of `ufc3_native_off2`
  (2, 4, 8).
- **Real-time round clock** (Settings > Fight > Round clock, off by default). The game's clock counts 30 ticks a
  second but runs at 60 fps, so a 5:00 round was over in 2:30. With this option one clock second is one real second,
  for both the round's own timer and the HUD clock. Pauses and slow motion are respected.
- **Longer careers** (Settings > Career > Career length: 48, 96 or 192 fights). The last-year warning still comes in the
  last year. With a longer career, a safety copy of the career save is made just before fight 48 (and 96), in
  `userdata\<profile>\5451087D\ufc3-career-backup\`.
- **Ultrawide 21:9 / 32:9 Hor+ in matches** (Settings > Aspect ratio, native renderer). You see more at the sides,
  fighters keep their proportions, the HUD stays centred, and menus stay 16:9. This is experimental: some menus and
  training mode can still look wrong.
- **Cloud saves** (Settings > Saves > Choose cloud folder, for example `%OneDrive%\SaveGames\UFC3ReX-Enhanced`): one
  career shared between PCs. Before Play the launcher downloads newer saves, and when the game closes it uploads them.
  Saves, created fighters, careers, achievements and DLC are synced; highlight replays are not. If the game is still
  running on another PC you get a warning. When both copies changed, the launcher asks which one to keep and keeps the
  other as a backup. Every file is hash-checked, so a folder the cloud app has only half synced is never used. The
  career length follows the saves. Start the game from the launcher on every PC.
- F9 in game writes one frame's draws to the log (for reporting graphics glitches).
- `env.bat` finds Visual Studio 2022 or 2026 through vswhere.

The mod pack "UFC 3 Legends: Cut Fighters" is the original author's and is not distributed here.


A native Windows PC port of **UFC Undisputed 3** (Xbox 360), made by static recompilation with the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk). It runs the original game code natively. It is not an emulator.

**This download contains no game files.** You need your own legally obtained copy of UFC Undisputed 3
for Xbox 360 as a disc image (`.iso`). DLC is optional and also comes from your own copy.

`ufc3.exe` is the game's own code translated to native PC code, which is how a recompilation works.

## Features

- Runs natively at any resolution (render scale up to 4K-class and beyond)
- Native Direct3D 12 renderer (default): the game draws directly through Direct3D 12 instead of an emulated GPU.
  Optional AMD FSR 1 upscaling. The emulated renderer (Vulkan or Direct3D 12) stays as the fallback.
- **F4 in game:** quick settings and Quit
- Bloom off and depth-of-field blur off (optional)
- 60 fps cutscenes, walkouts and replays (optional)
- First-run prompts are saved properly (no repeat grappling/tutorial questions)
- Working DLC from your own packages
- **Mods folder:** files laid over the game without touching your game files
- Roster beyond the game's 600-fighter table (up to 1024), for roster mods
- Optional cheats: unlock everything, free unlock points (off by default)
- Optional per-fighter walkout music from your own audio files
- Optional DLSS 5 add-on (NVIDIA RTX)
- Alt-tab and focus switches are instant (no black screen from exclusive fullscreen)
- Blood stays where it lands on the mat at higher render scales
- Career mode opponents and referees render properly (no black or invisible fighters)
- Career mode playable start to finish: no crash at the end of the first fight or at the first UFC main-card fight
- Created fighters: correct skin, portraits, magazine covers and walkout shirts, at any render scale
- Career mode: opponents' shorts logos and tattoos stay sharp at higher render scales

With the mod pack installed (separate download, see below), the same exe also turns on:

- **Blood and Guts:** bare-knuckle fights (right stick click, R3, on SELECT FIGHTER switches gloves and bare fists,
  UFC and PRIDE) with heavy blood that builds over the fight; gloved fights stay close to the original
  (amounts in the launcher's Settings > Gameplay)
- **Attire picker:** left stick click (L3) on SELECT FIGHTER cycles through more than two attires per fighter

Without the mod pack the game plays like the original: no Blood and Guts, no bare knuckle, no attire picker.

## Install

1. Download `UFC3-Recomp-<version>.zip` from [Releases](../../releases) and unzip anywhere, for example
   `C:\Games\UFC3 Recomp\` (not Program Files).
2. Run **UFC3 Launcher.exe**.
3. **Setup** tab: choose your UFC 3 `.iso`. The launcher checks it and copies the game files into `assets\` (about 6.5 GB).
   Optionally point it at your DLC packages.
4. Pick your settings in the **Settings** tab, then press **Play**. The first Play builds the native renderer's
   shader cache from your own game files (only once).

The launcher checks for new releases and can update itself. Your saves, settings and mods are never touched by updates.

## Mod packs

Mod packs are zips with a `modpack.json` and a `mods\` folder. Install them from the launcher's **Mod Pack** tab;
uninstall removes exactly the files the pack added. Mod packs are separate downloads and are not part of this repository.

**Updating a mod pack:** the launcher doesn't download mod packs for you. When a new version comes out, download the new zip and
install it from the **Mod Pack** tab. The launcher removes the old version first, then installs the new one. Your saves and created
fighters are kept. Update the game (launcher) first if the pack's notes ask for a newer version.

## Controls

An Xbox controller is recommended. Press **F4** at any time for quick settings and Quit.
With the mod pack installed, on SELECT FIGHTER: click the right stick (**R3**) to switch between gloves and bare fists,
and the left stick (**L3**) for the next attire.

## Building from source

See [BUILDING.md](BUILDING.md).

## Credits and licences

This project is licensed under the GNU General Public License v3.0 ([LICENSE](LICENSE)). The native renderer comes
from the SmackDown vs. Raw 2010 recomp by nefariousjosiah (GPL-3.0), which builds on re:Blue's renderer core
(BSD 3-Clause), plume and XenosRecomp (MIT). These and the SDK's libraries keep their own licences: see
[CREDITS.md](CREDITS.md) and `licenses\`.

Not affiliated with or endorsed by THQ, Yuke's, Zuffa or the UFC. No game code, assets or DLC are distributed
in this repository. Please support official releases.
