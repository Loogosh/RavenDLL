# RavenEra API — Addon API for WoW 1.14.2 (RavenCraft)

A reimplementation of the missing SuperWoW / UnitXP_SP3 / ClassicAPI-style addon
functionality for the 1.14 client, delivered entirely on the DLL side.

- **Client:** `WowClassic.exe` 1.14.2.42597 (RavenCraft 1.14)
- **No EXE modifications.** The client binary is never patched.
- **No automation.** The API only exposes read/query and player-initiated actions
  that addons could otherwise perform — it does not play the game for you.
- Every function listed as ✅ has been verified in game.

---

## 1. What this is

Version 1.14 removed a large part of the addon-facing engine API that the
1.12/Turtle WoW ecosystem depends on: unit positions, distances, line of sight,
GUID-based targeting, focus control, loot slot access, and more. Addons that use
those APIs silently break.

RavenEra restores them by calling the engine from a native DLL and exposing the
results to Lua through a thin, documented shim.

| Component | Role |
|---|---|
| `version.dll` | Proxy DLL, loads the stack before the Lua state exists |
| `SlotPrep.dll` | Reserves FrameScript dispatch slots for the gates |
| `SuperWoW.dll` | Engine layer: memory reads, traces, loot, focus, gate |
| `HearthDB.dll` | Item database (SQLite) |
| `!RavenEraAPI` (addon) | Lua shim: public `RavenEra_*` functions |

---

## 2. Installation

```
_classic_era_\
├─ version.dll
├─ SlotPrep.dll
├─ SuperWoW.dll
├─ HearthDB.dll
└─ Interface\AddOns\!RavenEraAPI\      (core.lua, SuperWoW.lua, …)
```

Start the game normally. On a successful load the chat prints:

```
RavenEra API ready
HearthDB ready
```

Gate status is also written to `Logs\superwow.log` and `Logs\slotprep.log`.

> The DLLs are loaded only when the process starts. `/reload` reloads the Lua
> addon but **not** the DLLs — a DLL update requires a full client restart.

---

## 3. Architecture

```
version.dll (proxy, early)
  └─ SlotPrep.dll      reserves 3 FrameScript slots (name→fn table)
       ├─ SuperWoW.dll claims slot 1 (RavenEra gate) and slot 2 (protected proxy)
       └─ HearthDB.dll claims slot 0
```

The client stores FrameScript functions as `[name_ptr, fn_ptr]` pairs in a static
table. SlotPrep takes three unused names, points their `fn` at a `jmp [slot]`
gadget **inside the module** (the engine rejects function pointers outside the
module image with a fatal `#134`), and hands the slots out through
`RavenEra_ClaimSlot(index, fn)`.

Protected actions are enabled per call by setting bit `0x40` at
`base + 0x3171B80` and restoring it afterwards. The "blocked from an action"
popups are suppressed by the addon.

---

## 4. Calling convention

High-level helpers (recommended):

```lua
RavenEra_UnitDistance("player", "target")
```

Low-level gate (for diagnostics and new integrations):

```lua
RavenEra("Cmd", "payload")
```

---

## 5. Lua API

### 5.1 Positions, distances, visibility

| Function | Returns | Status |
|---|---|---|
| `RavenEra_UnitPosition(unit)` | `x, y, z, o` | ✅ |
| `RavenEra_UnitFacing(unit)` | `o` (radians) | ✅ |
| `RavenEra_UnitDistance(a, b)` | number, combat reach subtracted | ✅ |
| `RavenEra_UnitInSight(a, b)` | `visible, distance, traceFactor` | ✅ |
| `RavenEra_UnitBehind(unit, target)` | `bool` | ✅ |
| `RavenEra_UnitReach(unit)` | `reach, radius` | ✅ |

```lua
/run print(RavenEra_UnitPosition("target"))
/run print(RavenEra_UnitDistance("player", "target"))
/run print(RavenEra_UnitInSight("player", "target"))
```

Data source: the object manager (`std::vector<Object*>` at `base + 0x2F90000`).
Position/orientation `obj + 0x1644`, GUID `obj + 0x58`, descriptor `obj + 0x10`
(combat reach `+0x1A0`, bounding radius `+0x19C`).
Line of sight uses the engine trace `CWorld_Intersect` (`0x1A4EAA0`, flags `0x100111`).

### 5.2 Targeting

| Function | Returns | Status |
|---|---|---|
| `RavenEra_TargetUnit(unit)` | `bool` | ✅ |
| `RavenEra_TargetLastTarget()` | `bool` | ✅ |
| `RavenEra_AssistUnit(unit)` | `bool` | ✅ |
| `RavenEra_TargetUnitByGUID(guid)` | target name, or `nil, "no token"` | ✅ |
| `RavenEra_TokenForGUID(guid)` | unit token or `nil` | ✅ |
| `RavenEra("TargetByGuid", guid)` | `bool` | ✅ |

```lua
/run local g = UnitGUID("nameplate1"); print(RavenEra("TargetByGuid", g), UnitGUID("target"))
```

GUID strings are resolved through the standard unit tokens, so targeting works
for every unit that has a token (target, pet, party, raid, boss, nameplates).

### 5.3 Focus

| Function | Returns | Status |
|---|---|---|
| `RavenEra_SetFocus(unit)` | new `UnitGUID("focus")` | ✅ |
| `RavenEra_SetFocus(nil)` | `nil` (clears focus) | ✅ |
| `RavenEra("FocusByGuid", guid)` | `bool` | ✅ |
| `RavenEra("FocusDesc")` | `lo=… hi=…` | ✅ |

```lua
/run print(RavenEra_SetFocus("pet"), RavenEra("FocusDesc"), UnitGUID("focus"))
/run print(RavenEra_SetFocus(nil), RavenEra("FocusDesc"))
```

Focus is written directly into the client's focus descriptor
(`base + 0x2D24768`).

### 5.4 Unit tokens, marks, owners

| Function | Returns | Status |
|---|---|---|
| `RavenEra_UnitCandidates()` | array of ~60 unit tokens | ✅ |
| `RavenEra_UnitTokenForMark(n)` | token carrying raid mark `n` (1–8) | ✅ with nameplates |
| `RavenEra_UnitOwner(unit)` | owner token or `nil` | ✅ |
| `RavenEra_UnitOwnerGUIDLow(unit)` | hex GUID of the owner | ✅ |
| `RavenEra_CanLootUnit(unit)` | `bool` | ✅ |

```lua
/run print(#RavenEra_UnitCandidates())
/run print(RavenEra_UnitOwner("pet"))
/run print(RavenEra_UnitTokenForMark(8))
```

Raid marks: `1` star, `2` circle, `3` diamond, `4` triangle, `5` moon,
`6` square, `7` cross, `8` skull.

### 5.5 Loot

| Function | Returns | Status |
|---|---|---|
| `RavenEra_LootSlot(n)` | `bool` (1-based) | ✅ |
| `RavenEra_LootAll()` | `taken, total` | ✅ |
| auto-collect on `LOOT_OPENED` | `RAVENERA_AUTOLOOT_ON_OPEN = true` | ✅ |

```lua
/run print(RavenEra_LootSlot(1))
/run print(RavenEra_LootAll())
```

Engine entry point: `0x16FDFB0`. Looting a **closed** corpse is not supported
(see Limitations).

### 5.6 Files, CVars, service

| Function | Returns |
|---|---|
| `RavenEra("ImportFile", path)` | file contents |
| `RavenEra("ExportFile", path, data)` | `true` |
| `RavenEra_GetCVar(name)` / `RavenEra_SetCVar(name, value)` | value |
| `RavenEra("GetVersion")` | `0, 1` |
| `RavenEra("Ping")` | `true` |
| `RavenEra("Caps")` | feature bitmask |
| `RavenEra("ObjCount")` | number of objects |
| `RavenEra("ObjMgr")` | `vec=… end=… cnt=…` |
| `RavenEra("GetLocalPlayer")` | local player object address |

---

### 5.7 HearthDB (item database)

`HearthDB.dll` is a SQLite 3 engine exposed to Lua. It is an independent gate
(slot 0) with its own `HDB_*` namespace; multiple databases can be open at once,
each referenced by an integer handle.

| Function | Returns | Notes |
|---|---|---|
| `HDB_GetVersion()` | `major, minor, patch` | DLL version |
| `HDB_Open(filename)` | `handle` | opens (or creates) a DB file; raises on error |
| `HDB_OpenAddon(addon, path)` | `handle` | opens a DB shipped inside an addon, read-only |
| `HDB_Close(handle)` | ? | |
| `HDB_Execute(handle, sql)` | ? | DDL/DML; raises on error |
| `HDB_Query(handle, sql)` | `rows[]` keyed by column name | |
| `HDB_QueryRaw(handle, sql)` | `cols[], rows[][]` | raw arrays |
| `HDB_ExecuteAsync(handle, sql, cb)` | `ticket` | runs on the worker thread |
| `HDB_QueryAsync(handle, sql, cb)` | `ticket` | |
| `HDB_QueryRawAsync(handle, sql, cb)` | `ticket` | |
| `HDB_GetResult(ticket)` | `rows` / `value, err` | `nil` while the ticket is pending |
| `HDB_ClearPoison(handle)` | ? | resets a handle stuck after a failed statement |
| `HDB_Pump()` | ? | called automatically every frame (callback frame) |

Synchronous example:

```lua
local h = HDB_Open("hdb_smoke.db")
HDB_Execute(h, "CREATE TABLE IF NOT EXISTS kv (k TEXT PRIMARY KEY, v TEXT)")
HDB_Execute(h, "INSERT INTO kv VALUES ('hello','world')")
local rows = HDB_Query(h, "SELECT * FROM kv")
print(rows[1].k, rows[1].v)      -- hello world
```

Asynchronous example:

```lua
local t = HDB_QueryAsync(h, "SELECT * FROM kv", function(rows) print(rows[1].v) end)
print(HDB_GetResult(t))          -- nil while pending, rows after completion
```

Notes:

- results are packed by the DLL and unpacked by the addon; `HDB_Query` returns
  rows keyed by column name, `HDB_QueryRaw` returns column list + array rows;
- async tickets are drained by `HDB_Pump()`, which is registered on a frame's
  `OnUpdate` ? no manual polling is required;
- `HDB_OpenAddon` is read-only by design (shipped reference data);
- low-level gate commands: `GetVersion`, `Open`, `OpenAddon`, `Close`, `Execute`,
  `Query`, `QueryRaw`, `ExecuteAsync`, `QueryAsync`, `QueryRawAsync`, `GetResult`,
  `ClearPoison`, `Pump`.

## 6. Low-level gate commands

For diagnostics and custom integrations. `payload` is always a string.

| Command | Payload | Returns |
|---|---|---|
| `UnitPos` | hex GUID | `r=1 x y z o` |
| `UnitDist` | `hexA,hexB` | number |
| `UnitReach` | hex GUID | `ok=… reach=… radius=…` |
| `UnitOwner` | hex GUID | `ok=… lo=… hi=…` |
| `Los` | — | `ctx=… open=… ground=…` (trace self-test) |
| `LosUnits` | `hexA,hexB` | `insight=… hit=… d=… dist=…` |
| `Loot` | slot number (1-based) | `1` / `-1` |
| `SetTargetToken` | token | `bool` |
| `SetFocusToken` / `ClearFocus` | token / — | `bool` |
| `TargetByGuid` / `FocusByGuid` | GUID string | `bool` |
| `FocusDesc` | — | `lo=… hi=…` |
| `ArmNextTarget` | `target` / `focus` / … | `true` |

---|---|

---

## 7. Limitations

1. **No loot request (`CMSG_LOOT_UNIT`).**
   Consequently `LootAllCorpses`, `LootUnit`, and `LootUnitItem` are not available.
   - `InteractUnit` is virtualized/obfuscated by the client's protector and crashes
     (`INT_DIVIDE_BY_ZERO`) when invoked out of context;
   - the client's traffic is TLS, so opcodes cannot be observed;
   - hardware breakpoints are rejected by the anti-cheat (client crash).
2. **`CastSpellByName`** — same virtualization, casting must use secure buttons.
3. **The global `TargetUnit` / `FocusUnit` are not patched.**
   - patching the FrameScript table triggers `#134 Invalid function pointer`
     (the engine requires the function pointer to be inside the module image);
   - patching Lua closures in the heap hangs the client.
   Use `TargetByGuid` / `FocusByGuid` instead.
4. **Names, marks and auras require a unit token** (i.e. enabled nameplates).
   Position/HP/guid reads work for any GUID present in the object manager.

---

## 8. Testing

Every function can be exercised from chat with a single `/run` line — no addon
code required:

```lua
/run print("ver", RavenEra("GetVersion"), "cnt", RavenEra("ObjCount"))
/run print(RavenEra_UnitPosition("player"))
/run local g = UnitGUID("target"); print(RavenEra("TargetByGuid", g), UnitGUID("target"))
/run local g = UnitGUID("pet"); print(RavenEra("FocusByGuid", g), RavenEra("FocusDesc"), UnitGUID("focus"))
```

---

## 9. Building

Requirements: Visual Studio (MSVC x64) and CMake.

```
call VsDevCmd.bat -arch=amd64 -host_arch=amd64
cd build
nmake SuperWoW      # also: SlotPrep, HearthDB
```

Output goes to `build\out\`. Copy the DLLs into `_classic_era_\` and restart the
client.

---

## 10. Protection and reverse engineering

| Object | State |
|---|---|
| `WowClassic.exe` | packed (ACG/VP), `.text`/`.rdata` read-only, parts of the Lua API virtualized |
| RavenEra DLLs | plain x64 PE, **not encrypted**, not obfuscated |
| Exports | `SlotPrep.dll`: `Load`, `RavenEra_ClaimSlot`, `RavenEra_SlotCount`; `HearthDB.dll`: `Load`; `SuperWoW.dll`: none |

Our DLLs can be disassembled or decompiled (Ghidra / IDA / Binary Ninja) without
source; without symbols the result is readable but slow to reconstruct. No keys
or secrets are stored in the binaries.

---

## 11. FAQ

**Why is `LootAllCorpses` missing?**
Because opening a corpse requires sending `CMSG_LOOT_UNIT`, and the client's
loot-request path cannot be invoked from outside (virtualized function, TLS
traffic, anti-cheat blocks debugging). Looting an **open** window works fully.

**Do I need nameplates for `nameplateN` tokens?**
Yes. Token-based APIs (names, marks, auras) only see units that have a token.

**Does this modify the game client?**
No. Only external DLLs and a normal addon are added.

**Will it survive a client patch?**
The stack relies on a handful of client RVAs and on the FrameScript table layout.
A client update will most likely break `SlotPrep.dll` first — check for
`READY slots=3` in `Logs\slotprep.log` after every patch.
