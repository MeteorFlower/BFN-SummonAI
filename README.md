# BFN SummonAI - source code

Source for the DLL and injector shipped with the Nexus release
**"Summon AI Anywhere"** for *Plants vs. Zombies: Battle for Neighborville*.

This repository exists so that anyone - players, and Nexus Mods staff reviewing
the upload - can read exactly what the files do before running them. Both files
that ship as binaries, `SummonAI.dll` and `inject_summonai.exe`, are built from
the sources here, and both builds are reproducible.

---

## ⚠️ Read this before anything else: the Base Mod is required

SummonAI does not work on its own. It needs the Base Mod already installed:

<https://github.com/MeteorFlower/BFN-Base-Mod-DLL>

The Base Mod is what lets the game start and reach a map. SummonAI is an
injected DLL, not a replacement for `RtWorkQ.dll`: that slot belongs to the Base
Mod. Without it the game never gets far enough for SummonAI to attach to
anything, and every summon fails.

---

## ⚠️ Why antivirus tools may complain

**1. The DLL is injected into a running process.**

The injector - `inject_summonai.exe`, built from `inject_summonai.py` - opens
`PVZBattleforNeighborville.exe`, writes the path of `SummonAI.dll` into its
memory, and starts a thread inside the game that calls `LoadLibraryW`. That is
what every DLL injector does - and it is also a technique malware uses, so it
trips pattern-based scanners. There is no way to inject a DLL without tripping
them.

**2. The DLL hooks game code and writes into the game's memory.**

It installs four inline hooks with MinHook (vendored under
`src/third_party/minhook`, BSD-2-Clause) after the packed executable has
decrypted. When you press F5 or F11 it temporarily points an AI spawner at the
selected character template, calls the game's own spawn function, and restores
the spawner's fields. Writing into another process and inline-hooking its code
are the other half of what the scanners look for.

**3. What this does *not* do.**

- It does **not** connect to the network. There is no socket, no HTTP, no WinINet.
- It does **not** read, collect, or transmit any personal data.
- It does **not** modify or inject into any process other than the game.
- It does **not** modify any game file. It writes one log file,
  `summonai_log.txt`, next to itself.
- It does **not** patch the game's executable or assets on disk. The hooks live
  only in the running process and disappear when the game closes.
- It does **not** replace `RtWorkQ.dll`. The Base Mod owns that.

The injector ships as `inject_summonai.exe`, a PyInstaller build of
`inject_summonai.py`, which is in this repository: the thing to read is the
script, and because the build is reproducible you can rebuild the exe from it
and compare hashes instead of trusting the binary. Nothing is obfuscated and
nothing is downloaded at run time.

---

## What's in here

| Path | What it is |
|---|---|
| `src/SummonAI.cpp` | The DLL source, 7,479 lines. |
| `src/build.bat` | Builds `SummonAI.dll` with MSVC. |
| `src/build_debug.bat` | Development build with the extra logging compiled in. It writes the same `SummonAI.dll`; do not ship it. |
| `src/unit_names.inc` | Display names for the character list, taken from the game's localization data. |
| `src/spawn_buff_table.inc` | Generated table of the spawn buffs applied to summoned units. |
| `src/third_party/minhook/` | Vendored MinHook (BSD-2-Clause). |
| `helper-scripts/inject_main.bat` | Double-click this one. Asks for admin and runs the injector. |
| `helper-scripts/inject_summonai.py` | The injector's source. The exe that ships is built from it. |
| `helper-scripts/build_injector.bat` | Builds `inject_summonai.exe` from that source. |

`SummonAI.dll` and `inject_summonai.exe` themselves are **not** in this
repository: they are build products, so they live on the **Releases** page and
in the release zip instead.

### Getting the files

Either take them from the **Releases** page of this repository, or build them
yourself: the DLL from `src/` (see [Building](#building)), the injector exe from
`helper-scripts/build_injector.bat`. Neither build product is committed, so a
plain checkout has neither.

The release zip is flat: `SummonAI.dll`, `inject_main.bat`, and
`inject_summonai.exe` in one folder. The DLL writes `summonai_log.txt` next to
itself.

**The injector only looks next to itself.** It resolves `SummonAI.dll` from its
own folder, never anywhere else. In a checkout the scripts live in
`helper-scripts/` while the DLL builds into `src/`, so running
`helper-scripts/inject_main.bat` from a checkout stops with `DLL not found`.
That is expected, not a bug. To run from a checkout, build the DLL, build the
exe (or have Python installed), then copy `SummonAI.dll` in beside the scripts.

---

## Using it

1. Install the Base Mod first.
2. Start the game and get into a level. The menu and loading screens are not
   enough; the mod needs a local player and a loaded level.
3. Run `inject_main.bat` (it asks for administrator rights), or run
   `inject_summonai.exe` directly.
4. The HUD appears in the top-left corner. The log `summonai_log.txt` is
   written next to the DLL and is overwritten each run.

Hotkeys:

- **F1 / F2** - previous / next ally character
- **F3 / F4** - previous / next ally tier
- **F5** - summon the selected ally
- **F7 / F8** - previous / next enemy character
- **F9 / F10** - previous / next enemy tier
- **F11** - summon the selected enemy
- **F6** - show / hide the HUD

The left two columns are your team; the right two are the enemy team or gnomes.
The role columns show playable characters first in roster order, then the rest by
display name; the tier columns are sorted by health.

The list is built from the loaded `Template_*` assets. A template is kept only
if its own `AISpawnData` is present. Templates without one are dropped: the
fallback would use the carrier's `Blueprint` and `BehaviorTree`, so the summoned
unit would have the wrong appearance and AI. This is why a small number of units
with irregular or missing spawn data, such as Dreadwood, never appear in the
list; Dreadwood's final-boss template is spawned directly by its boss-fight
prefab and has no `AISpawnData`.

A tier is the character variant, like base, Champion and Elite. Some Champions
and Elites have a prefix like `BattleLeader`, which means the occasion where they appear.

Summoned units are the game's own AI characters. They use the game's AI, loadout, 
and behavior; they are not player-controlled or the kind of AI that simulates players in
Team Vanquish.

The game sometimes has a limited pool of player/AI slots. When the pool is nearly full,
the mod recycles the oldest AI on your team to make room. That is a game limit, not a
bug; the log records which unit was recycled.

---

## Building

**Requirements**

- Windows 10 / 11
- **Build Tools for Visual Studio 2026** with the *Desktop development with C++*
  workload. It is free and includes no IDE:

  <https://visualstudio.microsoft.com/downloads/#build-tools-for-visual-studio-2026>

  The installer is `vs_BuildTools.exe`. By default `vcvars64.bat` ends up in
  `C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\`.

  Or via winget:

  ```
  winget install Microsoft.VisualStudio.BuildTools --force --override "--wait --passive --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64"
  ```

  Older toolsets (VS 2019 / 2022) work too - they just produce a different
  binary.

**Steps**

```
cd src
build.bat
```

`build.bat` calls `vcvars64.bat` and then:

```
cl /nologo /LD /O2 /MT /EHsc /W3 /Brepro /D_CRT_SECURE_NO_WARNINGS ^
   SummonAI.cpp third_party\minhook\src\hook.c ^
   third_party\minhook\src\trampoline.c ^
   third_party\minhook\src\buffer.c ^
   third_party\minhook\src\hde\hde64.c ^
   /Fe:SummonAI.dll /Fo:obj\ ^
   /link kernel32.lib user32.lib gdi32.lib
```

`build.bat` locates the toolchain itself: it asks `vswhere.exe` first and falls
back to scanning the install roots. If it finds nothing it stops and tells you
what to install.

`/Brepro` makes the build reproducible, so the same source always produces the
same bytes on the same toolchain. Without it the linker stamps the build time
into the binary - twice, in the COFF header and again in the
`IMAGE_DEBUG_TYPE_REPRO` debug entry - and no two builds would ever match.

MinHook is compiled straight into the DLL; there is no separate `.lib` and no
extra runtime dependency. Only the four files x64 needs are built; `hde32.c`
is 32-bit only.

**Toolchain**

For reference, the released binary was built with the MSVC 14.51 toolchain from
Build Tools for Visual Studio 2026. `build.bat` prints its own versions when
you run it. Any current MSVC builds this source. A different one links different
library code and so produces a different binary - expected, and harmless. What
is worth checking is the source, not the bytes.

---

## How it works

The full explanation is in the comment block at the top of `src/SummonAI.cpp`
(the source comments are in Chinese). In short:

**1. Injecting.** The injector finds `PVZBattleforNeighborville.exe`, opens it
with administrator rights, writes the path of `SummonAI.dll` into its memory,
and starts a thread that calls `LoadLibraryW`. `DllMain` starts four threads:
the worker, the keyboard thread, the HUD thread, and the spawn probe.

**2. Waiting for decryption.** The executable is packed, and its code is
decrypted at runtime. Patching too early would hook still-encrypted bytes and
silently do nothing once they decrypt. The DLL polls until the expected
original bytes appear at its four target addresses, then installs the hooks. If
they never appear, it installs nothing and the HUD reports that summoning is
not ready.

**3. The hooks.** MinHook detours four functions: the spawn function
(`ctx->vt[232]`), the world-insert function, the AI dispatch tick, and the
player-spawn entry. The dispatch detour is where the actual summon runs, on the
game thread. That is required: the engine's TLS context exists only on the game
thread, so a key press only sets a flag and the game thread does the work.

**4. The character list.** The worker scans the loaded `Template_*` assets and
splits each name into a character and a tier. It groups them by your team and
the enemy team. It rescans on level changes and every 30 seconds, so the list
stays current without a restart.

**5. Summoning.** The mod temporarily points a valid AI spawner's template slot
at the selected template, calls the game's own spawn function, then restores
the spawner's fields. The game creates a real AI character, not a fake model.
It tries to spawn at your position first and falls back to the game's own
placement if that fails. If the spawner is not ready, it retries a few times.

**6. Slots.** Summoned units occupy the game's player/AI slots. When the pool is
nearly full, the mod recycles the oldest AI on your team. The local player is never
touched. The log records the recycle.

**7. Spawn buffs.** After a summon succeeds, the mod applies the spawn buffs that
Explore Mode applies to that unit when it spawns. The table is generated from
those Explore Mode spawn rules (`spawn_buff_table.inc`); invincibility buffs are
left out. No place outside Explore Mode has been found that applies buffs on
unit spawn, so the table is based on Explore Mode behavior.

**8. The HUD and the log.** The HUD is a layered, click-through window that does
not take focus. `summonai_log.txt` is overwritten each run and records the scan,
hook installation, each summon, and every failure.

---

## The helper scripts

**No Python needed.** The release zip ships `inject_summonai.exe`: the injector
with a Python interpreter packed inside it, so `inject_main.bat` works on a
machine that has no Python at all. `SummonAI.dll` itself is an ordinary DLL, so
any injector works as well.

**The exe is not in this repository.** Exactly like the DLL, it is a build
product: `.gitignore` keeps it out and it is published on the **Releases** page
instead. A git checkout therefore contains only the source -
`inject_summonai.py` is what the exe is built from, not something you are
expected to run. The exe is the injector that ships.

Building it needs Python 3 (64-bit) and PyInstaller on the build machine; run
`helper-scripts/build_injector.bat` and it lands next to the script.
`inject_main.bat` uses the exe when it is present and falls back to the script
otherwise - that fallback exists for development, and it is the only case where
Python is needed. <https://www.python.org/downloads/windows/>

The exe builds reproducibly on a fixed toolchain: `build_injector.bat` pins
`SOURCE_DATE_EPOCH` (the timestamp in the PE header) and `PYTHONHASHSEED`
(the iteration order PyInstaller walks its dictionaries in), and both are
needed - with only the first one, two builds still differ. Same Python and
PyInstaller versions plus the same paths give byte-identical files; a different
PyInstaller version still gives different bytes, so compare hashes only when
the versions match.

`inject_main.bat` asks for administrator rights - `OpenProcess` on the game
fails without them - and then runs the injector. The window closes by itself
when it succeeds; it only stays open if something failed, so you can read why.

`inject_summonai.py` is the injector. It declares every Win32 function it
calls, so ctypes cannot silently truncate a 64-bit pointer, and it verifies the
injection by reading the log the DLL writes rather than assuming it worked.

---

## If it does not work

- **`DLL not found`** - the injector only looks next to itself. In a checkout,
  copy `SummonAI.dll` beside the helper scripts, or use the release layout.
- **`hooks are not installed yet`** - the game has not decrypted the target
  functions yet; wait a few seconds. If it never installs, the game build may
  not match the hardcoded addresses; the log lists what it found.
- **A summon fails with no carrier** - some modes do not expose a usable AI
  spawner, or you are not in a level yet. Try another mode or level; Ops is
  supported through the built-in fallback.
- **The log file is not created** - the DLL opens it with the ANSI code page,
  so a folder path outside that code page fails silently. Put the DLL in an
  ASCII-only path.
- **Injecting a second time does nothing** - Windows runs `DllMain` once per DLL
  path per process. Close the game and start it again.
- **A summoned unit disappeared** - the slot pool was nearly full and the mod
  recycled the oldest AI on your team; check `[RECYCLE]` in the log.
- **The HUD is gone** - press F6.

---

## License

MIT - see [LICENSE](LICENSE).

This project is not affiliated with, endorsed by, or sponsored by Electronic
Arts Inc. It is an independent, community-developed mod for educational and
preservation purposes. *Plants vs. Zombies* is a trademark of its respective
owners.
