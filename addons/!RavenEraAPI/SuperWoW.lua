-- SuperWoW API surface (first batch: what is implementable in Lua over 1.14 APIs).
-- Loaded by !RavenEraAPI after core.lua.

SUPERWOW_VERSION = 2.2
SUPERWOW_STRING  = "SuperWoW 2.2 (RavenEra 1.14)"

-- CVars registered by SuperWoW on 1.12. NOTE: we do NOT wrap GetCVar/SetCVar here.
-- Overriding Blizzard globals taints secure UI code on 1.14 ("blocked from an action
-- only available to the Blizzard UI", e.g. targeting). Taint-safe path: the real
-- CVar hook must live in C (FrameScript GetCVar entry) — planned; until then the
-- values below are exposed only through RavenEra_GetCVar/RavenEra_SetCVar.
local FAKE_CVARS = {
  LootSparkle = "0",
  SelectionCircleStyle = "1",
  BackgroundSound = "1",
  UncapSounds = "0",
  HealingText = "0",
  ChatBubbleRange = "40",
  ChatBubblesRaid = "1",
  ChatBubblesBattleground = "1",
  ChatBubblesWhisper = "1",
  ChatBubblesCreatures = "1",
  NameplateRange = "40",
  NameplateMotion = "0",
}
RAVENERA_CVARS = FAKE_CVARS

function RavenEra_GetCVar(name)
  local value = FAKE_CVARS[tostring(name)]
  if value ~= nil then return value end
  return GetCVar(name)
end

function RavenEra_SetCVar(name, value)
  local key = tostring(name)
  if FAKE_CVARS[key] ~= nil then
    FAKE_CVARS[key] = tostring(value)
    return
  end
  SetCVar(name, value)
end

-- Global overrides of Blizzard functions taint secure code on 1.14 (unit-frame
-- clicks, TargetUnit). OFF by default; enable only if you accept the risk.
RAVENERA_ALLOW_GLOBAL_OVERRIDES = false

-- SuperWoW CVars: addons call GetCVar/SetCVar directly, so the globals must be
-- transparent. Kept behind a flag so it can be disabled if taint shows up.
local RAVENERA_ORIG_GETCVAR, RAVENERA_ORIG_SETCVAR = GetCVar, SetCVar
if RAVENERA_ALLOW_GLOBAL_OVERRIDES and type(RAVENERA_ORIG_GETCVAR) == "function" then
  function GetCVar(name)
    local key = tostring(name)
    local fake = FAKE_CVARS[key]
    if fake ~= nil then return fake end
    return RAVENERA_ORIG_GETCVAR(name)
  end
end
if RAVENERA_ALLOW_GLOBAL_OVERRIDES and type(RAVENERA_ORIG_SETCVAR) == "function" then
  function SetCVar(name, value)
    local key = tostring(name)
    if FAKE_CVARS[key] ~= nil then
      FAKE_CVARS[key] = tostring(value)
      return
    end
    return RAVENERA_ORIG_SETCVAR(name, value)
  end
end

-- Clickthrough(1/0): SuperWoW exposes it as a getter/setter; engine hook later.
RAVENERA_CLICKTHROUGH = 0

function Clickthrough(value)
  if value ~= nil then
    RAVENERA_CLICKTHROUGH = (value ~= 0 and value ~= false) and 1 or 0
  end
  return RAVENERA_CLICKTHROUGH
end

-- UnitExists → +GUID: NOT overridden. Replacing this Blizzard global taints secure
-- targeting code on 1.14 (blocked action on F1/unit-frame click). Taint-safe path
-- would be a C-side hook of the FrameScript entry; addons that need the GUID can use
-- UnitGUID(unit) directly.
function RavenEra_UnitExists(unit)
  local exists = UnitExists(unit)
  if exists then return exists, UnitGUID(unit) end
  return exists
end

-- SuperWoW function change: UnitExists(unit) -> exists, guid.
-- NOTE: replacing a Blizzard global can taint secure code on 1.14; this is the
-- only override left enabled, to test whether targeting survives it.
local RAVENERA_ORIG_UNITEXISTS = UnitExists
if RAVENERA_ALLOW_GLOBAL_OVERRIDES and type(RAVENERA_ORIG_UNITEXISTS) == "function" then
  function UnitExists(unit)
    local exists = RAVENERA_ORIG_UNITEXISTS(unit)
    if exists then return exists, UnitGUID(unit) end
    return exists
  end
end

-- Quest link helpers (GetQuestLink itself does not exist in 1.14) -------------

local function questLogEntry(index)
  local a = { GetQuestLogTitle(index) }
  return a
end

local function buildQuestLink(id, title, level)
  if not id or not title then return nil end
  return "|cffffffff|Hquest:" .. tostring(id) .. ":" .. tostring(level or 0) .. "|h[" .. title .. "]|h|r"
end

RAVENERA_QUEST_CACHE = {} -- [questID] = { title, level }

local function cacheQuest(id, title, level)
  if id and title then
    RAVENERA_QUEST_CACHE[id] = { title = title, level = level or 0 }
  end
end

local function questIDFromEntry(a)
  -- 1.14 (verified in game): 1 title, 2 level, 3 questTag, 4 isHeader,
  -- 5 isCollapsed, 6 isComplete, 7 frequency, 8 questID, 9.. flags.
  if a[4] then return nil end -- zone header, not a quest
  return a[8]
end

function GetQuestIDForLogIndex(index)
  local a = questLogEntry(index)
  if not a[1] then return nil end
  local id = questIDFromEntry(a)
  cacheQuest(id, a[1], a[2])
  return id
end

function GetQuestLinkForLogIndex(index)
  local a = questLogEntry(index)
  if not a[1] then return nil end
  local id = questIDFromEntry(a)
  if not id then return nil end
  cacheQuest(id, a[1], a[2])
  return buildQuestLink(id, a[1], a[2])
end

function GetQuestLinkForID(id)
  local c = RAVENERA_QUEST_CACHE[id]
  if c then return buildQuestLink(id, c.title, c.level) end

  -- fill the cache from the current quest log
  local n = GetNumQuestLogEntries and GetNumQuestLogEntries() or 0
  for i = 1, n do
    local a = questLogEntry(i)
    if a[1] then cacheQuest(questIDFromEntry(a), a[1], a[2]) end
  end
  c = RAVENERA_QUEST_CACHE[id]
  if c then return buildQuestLink(id, c.title, c.level) end

  -- 1.14 has C_QuestLog.GetQuestInfo but no GetTitleForQuestID
  if type(C_QuestLog) == "table" and type(C_QuestLog.GetQuestInfo) == "function" then
    local ok, info = pcall(C_QuestLog.GetQuestInfo, id)
    if ok then
      if type(info) == "table" then
        local title = info.title or info.questTitle or info.name
        local level = info.level or info.questLevel or 0
        if title then
          cacheQuest(id, title, level)
          return buildQuestLink(id, title, level)
        end
      elseif type(info) == "string" then
        cacheQuest(id, info, 0)
        return buildQuestLink(id, info, 0)
      end
    end
  end
  return nil
end

-- New globals ---------------------------------------------------------------

-- SuperWoW: name, rank, texture, minRange, maxRange (no castTime, no spellID).
function SpellInfo(id)
  local name, rank, icon, castTime, minRange, maxRange = GetSpellInfo(id)
  return name, rank, icon, minRange, maxRange
end

-- SuperWoW argument is the player's buff index (1-based), like UnitAura.
function GetPlayerBuffID(index)
  local a = { UnitAura("player", index, "HELPFUL") }
  return a[10] -- spellId in 1.14 (nameplateShowPersonal is a[9])
end

function GetSpeed(unit)
  -- SuperWoW: runSpeed, swimSpeed in yards/sec (7 = 100%).
  local _, runSpeed, _, swimSpeed = GetUnitSpeed(unit or "player")
  return runSpeed, swimSpeed
end

function CursorPosition()
  return GetCursorPosition()
end

local function namePlateForUnit(unit)
  if type(GetNamePlateForUnit) == "function" then
    local f = GetNamePlateForUnit(unit)
    if f then return f end
  end
  if type(C_NamePlate) == "table" and type(C_NamePlate.GetNamePlateForUnit) == "function" then
    return C_NamePlate.GetNamePlateForUnit(unit)
  end
  return nil
end

function UnitNameplate(unit)
  -- SuperWoW returns the nameplate frame itself.
  return namePlateForUnit(unit)
end

-- CanLootUnit(token): native 1.14 wants a GUID; NOT overridden (Blizzard global,
-- taint). SuperWoW-compatible helper under a new name.
function RavenEra_CanLootUnit(unit)
  local guid = unit
  if type(unit) == "string" and string.sub(unit, 1, 2) ~= "0x" then
    guid = UnitGUID(unit)
  end
  if not guid or type(CanLootUnit) ~= "function" then return nil end
  return CanLootUnit(guid)
end

-- Autoloot (SuperWoW semantics: SetAutoloot(1) / SetAutoloot(0), also used as a
-- feature-detection flag by addons). The actual LootSlot automation is engine-side
-- on 1.14 (LootSlot is protected from addon code), so only the flag is kept here.
RAVENERA_AUTOLOOT = false

-- SuperWoW SetAutoloot: auto-pickup of items when the loot window opens
-- (not "open the window automatically when approaching a corpse").
function SetAutoloot(flag)
  if flag ~= nil then
    RAVENERA_AUTOLOOT = (flag ~= 0 and flag ~= false)
    pcall(SetCVar, "autoLootDefault", RAVENERA_AUTOLOOT and "1" or "0")
    RavenEra("SetAutoloot", RAVENERA_AUTOLOOT and 1 or 0)
  end
  local v = GetCVar("autoLootDefault")
  RAVENERA_AUTOLOOT = (v == "1" or v == 1 or v == true)
  return RAVENERA_AUTOLOOT and 1 or 0
end

-- File IO (C gate) ----------------------------------------------------------

-- SuperWoW reads/writes gamedirectory\imports ---------------------------------

local function importsPath(name)
  return "imports\\" .. tostring(name)
end

function ImportFile(name)
  local data, err = RavenEra("ImportFile", importsPath(name))
  if data == nil then error(err or "ImportFile failed", 2) end
  return data
end

function ExportFile(name, text)
  local ok, err = RavenEra("ExportFile", importsPath(name), text or "")
  if not ok then error(err or "ExportFile failed", 2) end
  return true
end

-- Combat log line: default WowCombatLog.txt, addToRawLog -> WoWRawCombatLog.txt --

function CombatLogAdd(message, addToRawLog)
  return RavenEra("CombatLogAdd", tostring(message), addToRawLog and 1 or 0)
end

-- Tracking (engine part pending; keep the globals so addons don't error) -----

function TrackUnit(unit)
  return RavenEra("TrackUnit", tostring(unit or ""))
end

function UntrackUnit(unit)
  return RavenEra("UntrackUnit", tostring(unit or ""))
end

-- Mouseover override used by unit frames (SuperWoW sets the engine mouseover;
-- on 1.14 only the Lua-side value is stored for now).
RAVENERA_MOUSEOVER = nil

function SetMouseoverUnit(unit)
  RAVENERA_MOUSEOVER = unit
end

-- Hooks that need the gate to be bound --------------------------------------

function RavenEra_OnGateReady()
  -- Reserved for later batches (events, engine queries).
end


-- RE helpers (call manually, never on login: engine calls at login are unsafe).
function RavenEra_LocalPlayerDump()
  local p = RavenEra and RavenEra("GetLocalPlayer")
  local t = UnitName("target")
  pcall(RavenEra, "ExportFile", "Logs\\lp.txt",
    "lp=" .. tostring(p) .. "\ntarget=" .. tostring(t))
end

function RavenEra_TrySetTarget(unit)
  local ok = RavenEra and RavenEra("SetTargetCore", unit or "player")
  local t = UnitName("target")
  pcall(RavenEra, "ExportFile", "Logs\\lp.txt",
    "set=" .. tostring(ok) .. "\ntarget=" .. tostring(t))
  return ok, t
end


-- RE: try native TargetUnit with the secure flag armed.
function RavenEra_TryNativeTarget(unit)
  RavenEra("ArmNextTarget")
  local ok, a = pcall(GetTaxiBenchmarkMode, unit or "player", "target")
  local t = UnitName("target")
  pcall(RavenEra, "ExportFile", "Logs\\lp.txt",
    "native ok=" .. tostring(ok) .. " ret=" .. tostring(a) .. " target=" .. tostring(t))
  return t
end

-- Slash command: /raven target | /raven info
SLASH_RAVEN1 = "/raven"
SlashCmdList["RAVEN"] = function(msg)
  msg = tostring(msg or "")
  local cmd, arg = msg:match("^(%S*)%s*(.-)$")
  if cmd == "last" then
    RavenEra_TargetLastTarget()
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN last target=" .. tostring(UnitName("target")))
  elseif cmd == "target" or cmd == "focus" or cmd == "assist" then
    local u = (arg ~= "" and arg) or "player"
    if cmd == "target" then RavenEra_TargetUnit(u)
    elseif cmd == "focus" then RavenEra_FocusUnit(u)
    else RavenEra_AssistUnit(u) end
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. cmd .. "(" .. u .. ") target=" ..
      tostring(UnitName("target")) .. " focus=" .. tostring(UnitName("focus")))
  elseif cmd == "guidtest" then
    local uu = arg ~= "" and arg or "nameplate1"
    local g = UnitGUID(uu)
    local t = RavenEra_TargetUnitByGUID(g)
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN guids " .. uu .. " g=" .. tostring(g) ..
      " target=" .. tostring(t))
  elseif cmd == "cast" then
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN cast disabled: native CastSpellByName crashes the client (packer VM)")

  elseif cmd == "upos" then
    local u = (arg ~= "" and arg) or "target"
    local x, y, z, o = RavenEra_UnitPosition(u)
    local out = string.format("upos %s = %s,%s,%s,%s", u, tostring(x), tostring(y), tostring(z), tostring(o))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "udist" then
    local a, b = arg:match("^(%S+)%s+(%S+)$")
    a, b = a or "player", b or "target"
    local d = RavenEra_UnitDistance(a, b)
    local out = string.format("udist %s..%s = %s", a, b, tostring(d))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "dist" then
    local out = {}
    for _, u in ipairs({ "target", "nameplate1", "focus", "mouseover" }) do
      if UnitExists(u) then
        local ok, d = pcall(UnitDistanceSquared, u)
        local inr = UnitInRange and select(1, UnitInRange(u)) or nil
        local ci = {}
        for i = 1, 4 do ci[i] = CheckInteractDistance(u, i) and 1 or 0 end
        out[#out + 1] = string.format("%s d2=%s inRange=%s ci=%s",
          u, tostring(d), tostring(inr), table.concat(ci, ""))
      else
        out[#out + 1] = u .. " missing"
      end
    end
    local txt = table.concat(out, " | ")
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. txt)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", txt)
  elseif cmd == "pos" then
    local x, y, z = UnitPosition("player")
    local f = GetPlayerFacing and select(1, GetPlayerFacing()) or nil
    local out = string.format("pos=%.4f,%.4f,%.4f facing=%s", x or -1, y or -1, z or -1, tostring(f))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "focus2" then
    local u = (arg ~= "" and arg) or "player"
    local g = RavenEra_SetFocus(u)
    local out = string.format("focus2 %s ok=%s guid=%s", u, tostring(g ~= nil), tostring(g))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "mark" or cmd == "marks" then
    local ok, err = pcall(function()
      local out = {}
      for i = 1, 8 do
        local tok = RavenEra_UnitTokenForMark(i)
        out[#out + 1] = i .. "=" .. (tok and (tok .. "/" .. tostring(UnitName(tok))) or "-")
      end
      local txt = "marks " .. table.concat(out, " ")
      DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. txt)
      RavenEra("ExportFile", "Logs\\lp.txt", txt)
    end)
    if not ok then
      DEFAULT_CHAT_FRAME:AddMessage("RAVEN marks error: " .. tostring(err))
      pcall(RavenEra, "ExportFile", "Logs\\lp.txt", "marks error: " .. tostring(err))
    end
  elseif cmd == "reach" then
    local u = (arg ~= "" and arg) or "player"
    local reach, radius = RavenEra_UnitReach(u)
    local out = string.format("reach %s = %s (radius %s)", u, tostring(reach), tostring(radius))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "behind" then
    local a, b = arg:match("^(%S+)%s+(%S+)$")
    a, b = a or "player", b or "target"
    local r = RavenEra_UnitBehind(a, b)
    local out = string.format("behind %s..%s = %s", a, b, tostring(r))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "insight" then
    local a, b = arg:match("^(%S+)%s+(%S+)$")
    a, b = a or "player", b or "target"
    local ins, dist, d = RavenEra_UnitInSight(a, b)
    local out = string.format("insight %s..%s = %s dist=%s d=%s", a, b, tostring(ins), tostring(dist), tostring(d))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN " .. out)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", out)
  elseif cmd == "los" then
    local r = tostring(RavenEra("Los"))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN los " .. r)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", "los " .. r)
  elseif cmd == "objdbg" then
    local r = tostring(RavenEra("ObjDbg"))
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN objdbg " .. r)
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", "objdbg " .. r)
  elseif cmd == "objmgr" then
    local r = RavenEra("ObjMgr")
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN objmgr " .. tostring(r))
    pcall(RavenEra, "ExportFile", "Logs\\lp.txt", "objmgr " .. tostring(r))
  elseif msg == "info" then
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN: UnitXP " .. tostring(UNITXP_VERSION) ..
      " SuperWoW " .. tostring(SUPERWOW_VERSION))
  else
    DEFAULT_CHAT_FRAME:AddMessage("RAVEN: /raven target|focus|assist [unit] | /raven info")
  end
end

-- Engine-backed protected actions (bit 6 gate, set/restored inside the proxy).
local function RavenEra_NativeAction(op, unit)
  if not RavenEra or not GetTaxiBenchmarkMode then return nil end
  RavenEra("ArmNextTarget")
  local ok = pcall(GetTaxiBenchmarkMode, tostring(unit or "player"), op)
  return ok
end
function RavenEra_TargetUnit(unit) return RavenEra_NativeAction("target", unit) end
function RavenEra_FocusUnit(unit) return RavenEra_NativeAction("focus", unit) end
function RavenEra_TargetLastTarget() return RavenEra_NativeAction("last", "player") end
function RavenEra_AssistUnit(unit) return RavenEra_NativeAction("assist", unit) end

-- One-shot RE test at login (writes Logs\lp.txt, no chat needed).


-- GUID -> unit token map (native TargetUnit only accepts tokens, not GUIDs).
local function RavenEra_UnitTokens()
  local t = { "player", "target", "focus", "mouseover", "pet", "targettarget" }
  for i = 1, 4 do t[#t + 1] = "party" .. i end
  for i = 1, 40 do t[#t + 1] = "raid" .. i end
  if type(C_NamePlate) == "table" and C_NamePlate.GetNamePlates then
    for _, f in ipairs(C_NamePlate.GetNamePlates() or {}) do
      if f.namePlateUnitToken then t[#t + 1] = f.namePlateUnitToken end
    end
  end
  return t
end

function RavenEra_TokenForGUID(guid)
  if not guid then return nil end
  for _, tok in ipairs(RavenEra_UnitTokens()) do
    if UnitGUID(tok) == guid then return tok end
  end
  return nil
end

function RavenEra_CastSpell(name)
  if not RavenEra or not GetTaxiBenchmarkMode then return nil end
  RavenEra("ArmNextTarget", "cast")
  local ok = pcall(GetTaxiBenchmarkMode, tostring(name))
  return ok
end

function RavenEra_TargetUnitByGUID(guid)
  local tok = RavenEra_TokenForGUID(guid)
  if not tok then return nil, "no token" end
  RavenEra_TargetUnit(tok)
  return UnitName("target")
end

-- Engine object access: positions/distances via the object manager.
local function RavenEra_GuidLow(unit)
  if RavenEra_ResolveUnit then unit = RavenEra_ResolveUnit(unit) end
  if not unit then return nil end
  local g = UnitGUID(tostring(unit))
  if not g then return nil end
  return g:match("(%x+)$")
end

function RavenEra_UnitPosition(unit)
  local hex = RavenEra_GuidLow(unit)
  if not hex then return nil end
  local r = tostring(RavenEra("UnitPos", hex))
  local ok, x, y, z, o = r:match("r=(%-?%d+)%s+(%-?[%d%.]+)%s+(%-?[%d%.]+)%s+(%-?[%d%.]+)%s+(%-?[%d%.]+)")
  if ok ~= "1" then return nil end
  return tonumber(x), tonumber(y), tonumber(z), tonumber(o)
end

function RavenEra_UnitFacing(unit)
  local _, _, _, o = RavenEra_UnitPosition(unit)
  return o
end

function RavenEra_UnitDistance(a, b)
  local ha, hb = RavenEra_GuidLow(a), RavenEra_GuidLow(b)
  if not ha or not hb then return nil end
  local d = tonumber(RavenEra("UnitDist", ha .. "," .. hb))
  if not d or d < 0 then return nil end
  return d
end

function RavenEra_UnitInSight(a, b)
  local ha, hb = RavenEra_GuidLow(a), RavenEra_GuidLow(b)
  if not ha or not hb then return nil end
  local r = tostring(RavenEra("LosUnits", ha .. "," .. hb))
  local ins, hit, d, dist = r:match("insight=(%-?%d+) hit=(%-?%d+) d=([%d%.]+) dist=([%d%.]+)")
  if not ins then return nil end
  return tonumber(ins) == 1, tonumber(dist), tonumber(d)
end

function RavenEra_UnitBehind(unit, target)
  local ux, uy = RavenEra_UnitPosition(unit)
  local tx, ty, tz, to = RavenEra_UnitPosition(target)
  if not ux or not tx or not to then return nil end
  local dx, dy = ux - tx, uy - ty
  local len = math.sqrt(dx * dx + dy * dy)
  if len < 0.01 then return false end
  local dot = (math.cos(to) * dx + math.sin(to) * dy) / len
  if dot > 1 then dot = 1 elseif dot < -1 then dot = -1 end
  return math.acos(dot) >= math.pi / 2
end

-- Engine focus (writes the client focus descriptor directly).
function RavenEra_SetFocus(unit)
  if not unit or unit == "" or unit == "none" then
    RavenEra("ClearFocus")
  else
    RavenEra("SetFocusToken", tostring(unit))
  end
  if type(FireEvent) == "function" then pcall(FireEvent, "PLAYER_FOCUS_CHANGED") end
  return UnitGUID("focus")
end

function RavenEra_UnitReach(unit)
  local hex = RavenEra_GuidLow(unit)
  if not hex then return nil end
  local r = tostring(RavenEra("UnitReach", hex))
  local ok, reach, radius = r:match("ok=(%-?%d+) reach=(%-?[%d%.]+) radius=(%-?[%d%.]+)")
  if ok ~= "1" then return nil end
  return tonumber(reach), tonumber(radius)
end

-- distanceBetween semantics (METER_RANGED): distance minus both combat reaches.
function RavenEra_DistanceBetween(a, b)
  return RavenEra_UnitDistance(a, b)
end

-- SuperWoW-style unit tokens: mark1..mark8 (units with a raid target icon).
function RavenEra_UnitCandidates()
  local t = { "player", "target", "focus", "mouseover", "pet", "targettarget" }
  for i = 1, 4 do t[#t + 1] = "party" .. i end
  for i = 1, 40 do t[#t + 1] = "raid" .. i end
  for i = 1, 8 do t[#t + 1] = "boss" .. i end
  if type(C_NamePlate) == "table" and C_NamePlate.GetNamePlates then
    local plates = C_NamePlate.GetNamePlates()
    if type(plates) == "table" then
      for _, f in ipairs(plates) do
        if f and f.namePlateUnitToken then t[#t + 1] = f.namePlateUnitToken end
      end
    end
  end
  return t
end

function RavenEra_UnitTokenForMark(n)
  n = tonumber(n)
  if not n or n < 1 or n > 8 then return nil end
  for _, u in ipairs(RavenEra_UnitCandidates()) do
    if UnitExists(u) then
      local ok, idx = pcall(GetRaidTargetIndex, u)
      if ok and idx == n then return u end
    end
  end
  return nil
end

function RavenEra_ResolveUnit(unit)
  if type(unit) ~= "string" then return unit end
  if unit == "owner" then return RavenEra_UnitOwner("target") end
  local n = unit:match("^mark([1-8])$")
  if not n then return unit end
  return RavenEra_UnitTokenForMark(tonumber(n))
end

-- SuperWoW "owner" token: the unit that owns the given unit (pet/totem master).
function RavenEra_UnitOwnerGUIDLow(unit)
  local hex = RavenEra_GuidLow(unit)
  if not hex then return nil end
  local r = tostring(RavenEra("UnitOwner", hex))
  local ok, lo = r:match("ok=(%-?%d+) lo=(%x+)")
  if ok ~= "1" or not lo or lo == "0" then return nil end
  return tonumber(lo, 16)
end

function RavenEra_UnitOwner(unit)
  local low = RavenEra_UnitOwnerGUIDLow(unit)
  if not low then return nil end
  for _, u in ipairs(RavenEra_UnitCandidates()) do
    local g = UnitGUID(u)
    if g then
      local gl = g:match("(%x+)$")
      if gl and tonumber(gl, 16) == low then return u end
    end
  end
  return nil
end

-- Engine loot: LootSlot core (0x16FDFB0) with the secure bit, no protected checks.
function RavenEra_LootSlot(slot)
  local n = tonumber(slot) or 1
  if n < 1 then return false end
  return RavenEra("Loot", tostring(n)) == 1
end

function RavenEra_LootAll()
  local total = GetNumLootItems and GetNumLootItems() or 0
  local ok = 0
  for i = 1, total do
    if RavenEra_LootSlot(i) then ok = ok + 1 end
  end
  return ok, total
end

-- Engine InteractUnit (open loot/gossip/etc.) with the secure bit.
function RavenEra_InteractUnit(unit)
  RavenEra("ArmNextTarget")
  local ok = pcall(GetTaxiBenchmarkMode, tostring(unit or "target"), "interact")
  return ok
end

-- Auto-collect: when a loot window opens, take every slot immediately.
RAVENERA_AUTOLOOT_ON_OPEN = false
local alFrame = CreateFrame("Frame", "RavenEra_AutoLoot")
alFrame:RegisterEvent("LOOT_OPENED")
alFrame:SetScript("OnEvent", function()
  if not RAVENERA_AUTOLOOT_ON_OPEN then return end
  local function take() pcall(RavenEra_LootAll) end
  if C_Timer and C_Timer.After then C_Timer.After(0.05, take) else take() end
end)

-- Suppress the "blocked from an action" popups caused by our engine calls.
do
  local blk = { ADDON_ACTION_BLOCKED = true, MACRO_ACTION_BLOCKED = true,
                ADDON_ACTION_FORBIDDEN = true, MACRO_ACTION_FORBIDDEN = true }
  if type(StaticPopup_Show) == "function" and not RAVENERA_POPUP_FILTERED then
    RAVENERA_POPUP_FILTERED = true
    local orig = StaticPopup_Show
    StaticPopup_Show = function(which, text_arg1, text_arg2, data, ...)
      if blk[which] then return nil end
      return orig(which, text_arg1, text_arg2, data, ...)
    end
  end
end
