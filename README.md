# RavenDLL — addon API for WoW 1.14.2 (RavenCraft)

A small DLL stack that restores the addon-facing engine API missing in the 1.14 client:
unit positions, exact distances, line of sight, GUID targeting, focus, loot slots and
an embedded SQLite item database.

- **No EXE modification** — the client binary is never patched.
- **No automation** — read/query APIs and player-initiated actions only.
- **Verified** on `WowClassic.exe` 1.14.2.42597.

Documentation:

- **[RAVENERA_API_EN.md](RAVENERA_API_EN.md)** — full API reference (English)
- **[RAVENERA_API.md](RAVENERA_API.md)** — same in Russian

## Components

| DLL | Role |
|---|---|
| `version.dll` | sideload proxy: loads the stack before the Lua state exists |
| `SlotPrep.dll` | reserves FrameScript dispatch slots for the gates |
| `SuperWoW.dll` | engine layer: object manager, traces, loot slots, focus, public gate |
| `HearthDB.dll` | SQLite 3 engine exposed to Lua as `HDB_*` |

Lua API lives in two addons: `!RavenEraAPI` (`RavenEra_*`) and `HearthDB` (`HDB_*`).

## Install

Prebuilt binaries and addons are in [`dist/`](dist):

```
dist\
├─ version.dll
├─ SlotPrep.dll
├─ SuperWoW.dll
├─ HearthDB.dll
└─ AddOns\
   ├─ !RavenEraAPI\
   └─ HearthDB\
```

Copy the four DLLs next to `WowClassic.exe` (into `_classic_era_\`) and both addon
folders into `_classic_era_\Interface\AddOns\`. Restart the client.

On success the chat shows `RavenEra API ready` / `HearthDB ready`, and
`Logs\slotprep.log` contains `READY slots=3`.

## Build

Requirements: Windows x64, Visual Studio 2022+ with the C++ workload, CMake 3.16+.
No external packages — `third_party\sqlite` is bundled.

```bat
build.bat
```

or manually:

```bat
call "C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
cd build
cmake -G "NMake Makefiles" -DCMAKE_BUILD_TYPE=Release ..
nmake
```

Individual targets:

```bat
nmake SuperWoW      :: engine layer + gate
nmake SlotPrep      :: slot reservation
nmake HearthDB      :: SQLite item database
nmake VersionProxy  :: produces version.dll
```

Artifacts land in `build\out\`.

Deploy into a client folder:

```bat
deploy.bat "C:\Games\RavenCraft\_classic_era_"
```

## Layout

```
plugins\SlotPrep      FrameScript slot reservation (name→fn table, module-internal gadgets)
plugins\SuperWoW      engine layer: object manager, trace, loot slots, focus, gate
plugins\HearthDB      SQLite engine (HDB_*)
plugins\VersionProxy  version.dll sideload proxy
addons\!RavenEraAPI   public Lua API (RavenEra_*)
addons\HearthDB       public Lua API (HDB_*)
dist\                 prebuilt DLLs + addons
tools\                small RE helpers (disassembly, memory scan)
```

## Client coupling

All offsets target **1.14.2.42597**:

| What | Where |
|---|---|
| FrameScript table, stolen-name indices, gadget slots | `plugins\SlotPrep\SlotPrep.cpp` |
| object manager, unit descriptor, trace, loot slot, focus descriptor | `plugins\SuperWoW\SuperWoW.cpp` |
| protected-action flag (`base + 0x3171B80`) | `plugins\SuperWoW\SuperWoW.cpp` |

After a client patch, expect `SlotPrep.dll` to break first — check for
`READY slots=3` in `Logs\slotprep.log`.

## Limits

- No corpse loot request (`CMSG_LOOT_UNIT`): looting a **closed** corpse is not available.
- `CastSpellByName` / `InteractUnit` are virtualized by the client protector.
- The global `TargetUnit` / `FocusUnit` are not patched; use `*ByGuid` helpers.
- Names, marks and auras require unit tokens (enabled nameplates).

See [RAVENERA_API_EN.md](RAVENERA_API_EN.md) §7 for details.

## Notes

The DLLs are plain x64 PE files (not packed, not obfuscated); the protected part is
the game client itself. No keys or credentials are stored in the binaries.
