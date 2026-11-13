<p align="center">
  <img width="96" height="96" alt="RomManager icon" src="RomManager/icon.png" />
</p>

<h1 align="center">RomManager for spruceOS</h1>

<p align="center"><b>A ROM downloader and ROM manager for the TrimUI Brick, Brick Pro and Smart Pro, running spruceOS.</b></p>

RomManager is an on-device ROM downloader: browse your own game sources - archive.org items, web folders, or an FTP server on your PC - download your games over Wi-Fi with their box art, and play them, all on the handheld. No PC, no taking the SD card out.

This is the spruceOS port of [plusCloud's RomManager](https://github.com/Plus-Cloud/RomManager), made for the Miyoo Mini on OnionOS. The app is the same; underneath, its screen, button and sound engines were rewritten for spruce's 64-bit handhelds, and games start through spruce, so its game switcher, Recents and resume-after-power-off work with them.

<p align="center">
  <img width="400" alt="RomManager home screen" src="screenshots/02_home.png" />
  <img width="400" alt="Your games in cover flow" src="screenshots/14_installed_covers.png" />
</p>

> ⚖️ **RomManager doesn't include, host or link to any games.** It only downloads from sources you add yourself. Only download games you own, and follow the copyright laws where you live.

## 💾 Installation

1. Download `RomManager-<version>.zip` from the **Releases** page.
2. Copy the `RomManager` folder inside it to `App/` on your SD card, so you have `App/RomManager/launch.sh`.
3. Put the card back in, turn on Wi-Fi in spruce (**Settings > WiFi**) and open **RomManager** from spruce's **Apps**.

**Updating:** RomManager updates itself from this repository's releases (**Settings > Auto Update**), or copy a new `RomManager` folder over the old one. Your repos, favorites and settings are kept.

**Coming from the Miyoo Mini?** Copy `settings.ini`, `repos.txt` (or the older `archives.txt`) and `caches/favorites.txt` from the old card's `App/RomManager` into the new one.

**Devices:** TrimUI Brick, Brick Pro and Smart Pro on spruceOS 4.6 or newer. spruce hides the app on other devices (`devices` in `config.json`).

## ✨ Features

### Looks and feel
- **Home screen** of six big tiles (or a plain list, if you prefer), with live counts - and live progress while something downloads.
- **Boot screen** with a loading bar that follows the real start-up, your library at a glance, and a tip.
- **Slides, fades and pop-ups** between screens; notices slide in and go away by themselves, so nothing makes you wait.
- **Cover flow** in every game and console list (press **R**): covers slide past with reflections, and the selected one floats gently.
- **Its own icons** for consoles and menus, so RomManager looks the same whatever spruce theme you use.
- **Animations can be turned off** in Settings.

### Under the hood
- A **screen engine written in C** that draws straight to the screen: page flipping (no tearing), box art decoded on a second CPU core so scrolling never waits for it, and the original 640x480 layout scaled up sharp to the Brick's 1024x768.
- **No CPU used when idle** - the screen only redraws when something changes.
- Separate small programs for the **buttons** (presses are queued, never missed) and the **sound** (music and button sounds mixed, so the music never pauses; played through spruce's audio, so Bluetooth and USB audio work too).
- **Crash-safe:** problems are logged to `caches/rommanager.log` and RomManager restarts itself.

### Your sources
- Add **archive.org items, web folders or FTP folders**, and see every game they have for a console in one merged list, without duplicates.
- **Multi-console packs:** add a whole collection at once; RomManager matches its folders (or per-console zips) to spruce's consoles - N64, Dreamcast, PSP and Saturn included - and marks any spruce can't run.
- **Help while typing:** start an archive.org ID and a matching item is suggested.
- **archive.org login** for items that need an account - only the login cookie is kept, never your password.

### Downloading
- **Background downloads:** pick a game and keep browsing. Queue as many as you like; **X** on the home screen opens the queue with speed, time left, retry and cancel.
- **Resume:** downloads stopped by a game or a power-off carry on where they left off.
- **Before you download:** the game's size and your free space, and a check mark on games you already have.
- **Sort & Filter (Y):** A-Z or biggest first, one region, hide betas / demos / hacks, hide games you have.
- **Box art** with every game, and disc games get their `.cue` too. Preview covers for games you haven't downloaded are fetched three at a time, nearest first, and stored small.

### Playing
- **A** plays any installed game, favorite or recent download, with spruce's emulator for that console.
- **Default Launcher:** a game started from RomManager comes back to it when it ends - straight to the screen you left, no boot screen. Games you start show up in spruce's Recents and game switcher, and a power-off in a game resumes that game. **Menu** opens spruce's game switcher.

### Your library
- Search with the on-screen keyboard, **L2 / R2** to jump by letter, and every list remembers where you were.
- Favorites across all consoles, and a Recent Downloads list.
- Get box art for one game or every game missing it; delete a game together with its disc files and art.

## 📸 Screenshots

| | |
| :---: | :---: |
| <img width="380" alt="Boot screen" src="screenshots/01_boot.png" /><br><sub>Boot screen</sub> | <img width="380" alt="Consoles in cover flow" src="screenshots/03_consoles.png" /><br><sub>Consoles in cover flow, with RomManager's own icons</sub> |
| <img width="380" alt="A source's game list" src="screenshots/05_download_list.png" /><br><sub>A source's games - check marks on the ones you have</sub> | <img width="380" alt="Download pop-up" src="screenshots/06_download_confirm.png" /><br><sub>Size and free space before you download</sub> |
| <img width="380" alt="Download queue" src="screenshots/08_download_queue.png" /><br><sub>The download queue, running in the background</sub> | <img width="380" alt="Settings" src="screenshots/10_settings_launcher.png" /><br><sub>Every setting explained on the status line</sub> |

<p align="center"><sub>Screenshots from the original Miyoo Mini version - the spruce port keeps its look. The games shown are homebrew, with box art from the Libretro thumbnails project; game counts and sources are sample data.</sub></p>

## 🗺️ Adding sources

| Source | What you type | Example |
| --- | --- | --- |
| archive.org item | The ID after `archive.org/details/` | `homebrew-console-pack` |
| Web folder | An `http(s)://` address that shows a file list | `https://myserver.local/roms/psx/` |
| FTP folder | An `ftp://` address, with a login if needed | `ftp://user:password@192.168.1.20/roms/` |

Open **Repositories > + Add a repository**, type the address (**L** shows symbols such as `: / . @`), and pick a console or **Multi-console pack**. A pop-up shows what was found: **Save**, **Edit** or **Cancel**.

Sources live in `RomManager/repos.txt`, so you can also set them up on a PC:

```text
# CONSOLE | SOURCE | FORMAT (optional extra file types)
MD  | homebrew-console-pack
PS  | ftp://user:password@192.168.1.20/psx/ | chd
*   | https://myserver.local/roms/
```

`CONSOLE` is spruce's ROM folder name (`PS`, `MD`, `MS`, `PCE`, `N64`, `PSP`...), or `*` for a pack.

## 🎮 Controls

| Button | Game lists | Keyboard |
| --- | --- | --- |
| **D-Pad / left stick** | Move (list: left / right jump a page; cover flow: up / down jump) | Move |
| **L2 / R2** | Previous / next letter | - |
| **A** | Download / Play | Type |
| **B** | Back | Delete a letter |
| **X** | Download lists: re-read the sources. Installed: get the box art | Done |
| **Y** | Download lists: sort & filter. Installed: delete the game | - |
| **Start** | Add / remove favorite | Done, or accept the suggestion |
| **Select** | Search | Cancel |
| **R** | List / cover flow | Capitals |
| **L** | - | Space (search) / symbols (addresses) |
| **Menu** | spruce's game switcher | spruce's game switcher |

**Home screen:** **X** opens the download queue; **B** twice exits.
**Repositories:** **A** add or edit, **Y** remove, **X** file format, **Select** move to another console, **Start** review a pack.

## ⚙️ Settings

| Setting | What it does |
| --- | --- |
| Music Volume / Sound Effects | Volume bars from off to full |
| Home Screen | Big tiles or a plain list |
| Animations | On or off |
| Open To | What RomManager opens on from spruce: Main Menu, Installed ROMs, or where you left off. Back from a game, it always returns where you were. |
| Default Launcher | Games started here come back to RomManager when they end (off: back to spruce's menu) |
| List Rows | How many rows a list shows |
| Auto Scrape Covers | Download box art with every game |
| Repo Cache | How many days source lists are kept before being read again |
| Auto Update | Install new versions of this port by itself |
| Tools | Scrape Missing Box Art, Prefetch Repo Previews, Manage Preview Cache, Clear Repo Cache, Rebuild Local Database |
| Credits & About | Who made RomManager |

`settings.ini` only: `PREVIEW_CACHE_MB` (space for preview covers, default 40), `SHOW_PERF=1` (frames per second and CPU use on screen).

## 🛠️ Building

RomManager is a shell script (`RomManager/launch.sh`, with `lib/`) plus four small C programs in `src/`: `rmui` (the screen engine), `rmkey` (buttons), `rmaudio` (the sound mixer) and `rmls` (fast ROM folder listing). The lines of text `launch.sh` sends `rmui` for each screen are described at the top of `src/rmui.c`.

- `sh src/build.sh` builds the four programs for spruce's 64-bit handhelds (static aarch64) into `RomManager/bin`. It compiles in Docker, so Docker is all you need.
- `sh src/build.sh preview` builds `rmui` for your computer, to draw screens as PNG files without the device: `src/rmui-preview --preview 1024 768 out.png frame.txt`.

If something looks wrong on a device, these can be set before `rmui` starts: `RMUI_NOFLIP=1` (draw without page flipping), `RMUI_NOTHREAD=1` (decode pictures on the main thread), `RMUI_FB=/dev/fbN`; and `RMKEY_DEV=/dev/input/eventN` for `rmkey`.

**Versions:** the port has its own [semantic version](https://semver.org), starting at 1.0.0 (based on RomManager 2.1): fixes bump the patch (1.0.1), new features - including ones brought in from the original - the minor (1.1.0), and changes that break settings or repos the major.

**Releasing:** set `RM_VERSION` in `launch.sh`, zip the `RomManager` folder (`zip -r RomManager-<version>.zip RomManager -x '*.DS_Store' 'RomManager/caches/*'`) and attach it to a GitHub release tagged `v<version>`. Auto Update installs the newest release's first `.zip`.

## 🤝 Credits

**RomManager is made by plusCloud** - designed, built, tested and debugged as an ADHD special interest after realizing nothing like it existed: the app, the screen, button and sound engines, sources, downloads and the look.

**spruceOS port by [brenonsc](https://github.com/brenonsc):** new 64-bit screen, button and sound engines, starting games through spruce, and spruce's consoles. Earlier contributions: automatic file-format detection, bulk cover prefetching, choosing which consoles' cached previews to delete, `.cue` downloads for disc games, case-insensitive archive.org IDs, and reliability fixes.

**Icons** by **Bernig**.

**Also:** box art from the Libretro thumbnails project; `stb_image` / `stb_truetype` / `stb_image_write` by Sean Barrett; `minimp3` by lieff; and the OnionOS and spruce teams.

## ⚖️ Disclaimer

RomManager doesn't include, host or link to any games - it downloads only from the sources you add. Only download games you own, and follow the copyright laws where you live. RomManager isn't affiliated with any console maker, with OnionOS or with spruceOS.
