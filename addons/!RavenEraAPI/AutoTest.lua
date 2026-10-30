-- OPT-IN: add "AutoTest.lua" to !RavenEraAPI.toc to run this 3 s after load.
-- Writes Logs\ravenapi_selftest.txt via the C gate (ExportFile) — no chat needed.
local f = CreateFrame("Frame", "RavenEra_AutoTest")
local t = 0

f:SetScript("OnUpdate", function(_, dt)
  t = t + (dt or 0.05)
  if t < 3 then return end
  f:SetScript("OnUpdate", nil)
  if type(RavenEra) ~= "function" then return end

  local out = {}
  local function call(name, fn, ...)
    if type(fn) ~= "function" then
      out[#out + 1] = name .. " -> MISSING"
      return
    end
    local r = { pcall(fn, ...) }
    if r[1] then
      local vals = {}
      for i = 2, #r do vals[#vals + 1] = tostring(r[i]) end
      out[#out + 1] = name .. " -> " .. table.concat(vals, ", ")
    else
      out[#out + 1] = name .. " !! " .. tostring(r[2])
    end
  end

  -- slots and gates (stock names kept; fn pointer swapped by SlotPrep)
  out[#out + 1] = "type(IsOnTournamentRealm) = " .. type(IsOnTournamentRealm)
  out[#out + 1] = "type(SetTaxiBenchmarkMode) = " .. type(SetTaxiBenchmarkMode)
  out[#out + 1] = "type(GetTaxiBenchmarkMode) = " .. type(GetTaxiBenchmarkMode)
  out[#out + 1] = "type(RavenEra_S0..S2) = " .. type(RavenEra_S0) .. "/" .. type(RavenEra_S1) .. "/" .. type(RavenEra_S2)
  call("IsOnTournamentRealm(GetVersion)", IsOnTournamentRealm, "GetVersion") -- HearthDB 0.4.x
  call("SetTaxiBenchmarkMode(GetVersion)", SetTaxiBenchmarkMode, "GetVersion") -- SuperWoW 0.1
  call("RavenEra.GetVersion", RavenEra, "GetVersion")

  -- SuperWoW API (first batch)
  call("SUPERWOW_VERSION", function() return SUPERWOW_VERSION, SUPERWOW_STRING end)
  call("SpellInfo(133)", SpellInfo, 133)
  call("GetPlayerBuffID(1)", GetPlayerBuffID, 1)
  call("UnitPosition(player)", UnitPosition, "player")
  call("GetSpeed(player)", GetSpeed, "player")
  call("CursorPosition", CursorPosition)
  call("UnitNameplate(target)", UnitNameplate, "target")
  call("CanLootUnit(target)", CanLootUnit, "target")
  call("TrackUnit(player)", TrackUnit, "player")
  call("SetAutoloot(1)", SetAutoloot, 1)
  call("SetAutoloot(0)", SetAutoloot, 0)
  call("ExportFile(imports)", ExportFile, "ravenapi_test.txt", "hello from raven")
  call("ImportFile(imports)", ImportFile, "ravenapi_test.txt")
  call("CombatLogAdd", CombatLogAdd, "raven selftest")

  -- batch 2: CVars, clickthrough, quest helpers
  call("GetCVar(LootSparkle)", GetCVar, "LootSparkle")
  call("SetCVar(LootSparkle,1)", SetCVar, "LootSparkle", "1")
  call("GetCVar(LootSparkle)#2", GetCVar, "LootSparkle")
  call("SetCVar(LootSparkle,0)", SetCVar, "LootSparkle", "0")
  call("Clickthrough(1)", Clickthrough, 1)
  call("Clickthrough()", Clickthrough)
  call("Clickthrough(0)", Clickthrough, 0)
  call("GetQuestIDForLogIndex(1)", GetQuestIDForLogIndex, 1)
  call("GetQuestLinkForLogIndex(1)", GetQuestLinkForLogIndex, 1)
  call("GetQuestLinkForID(1)", GetQuestLinkForID, 1)

  local qn = GetNumQuestLogEntries and GetNumQuestLogEntries() or 0
  out[#out + 1] = "questlog n=" .. tostring(qn)
  for i = 1, math.min(qn, 3) do
    local a = { GetQuestLogTitle(i) }
    local s = {}
    for j = 1, 12 do s[#s + 1] = j .. ":" .. tostring(a[j]) end
    out[#out + 1] = "quest " .. i .. " = " .. table.concat(s, " ")
    local id = (not a[4]) and a[8] or nil
    if id then
      local ok, info = pcall(C_QuestLog.GetQuestInfo, id)
      out[#out + 1] = "  GetQuestInfo(" .. tostring(id) .. ") ok=" .. tostring(ok) ..
        " type=" .. type(info) .. " v=" .. tostring(info)
      if type(info) == "table" then
        local ks = {}
        for k, v in pairs(info) do ks[#ks + 1] = tostring(k) .. "=" .. tostring(v) end
        out[#out + 1] = "  info " .. table.concat(ks, ", ")
      end
    end
  end
  call("GetQuestIDForLogIndex(1)[header]", GetQuestIDForLogIndex, 1)
  call("GetQuestLinkForLogIndex(1)[header]", GetQuestLinkForLogIndex, 1)
  call("GetQuestIDForLogIndex(2)", GetQuestIDForLogIndex, 2)
  call("GetQuestLinkForLogIndex(2)", GetQuestLinkForLogIndex, 2)
  call("GetQuestLinkForID(41224)", GetQuestLinkForID, 41224)
  call("GetQuestLinkForID(1)[unknown]", GetQuestLinkForID, 1)
  out[#out + 1] = "C_QuestLog=" .. type(C_QuestLog) ..
    " GetTitle=" .. tostring(type(C_QuestLog) == "table" and type(C_QuestLog.GetTitleForQuestID))

  call("SetAutoloot()", SetAutoloot)
  call("UnitExists(player)", UnitExists, "player")
  call("CombatLogAdd(raw)", CombatLogAdd, "raw line", 1)

  local qi = { pcall(C_QuestLog.GetQuestInfo, 1) }
  out[#out + 1] = "QuestInfo(1) ok=" .. tostring(qi[1]) ..
    " a=" .. tostring(qi[2]) .. " b=" .. tostring(qi[3]) .. " c=" .. tostring(qi[4])

  -- batch 2: custom event hook and dispatch
  local got = {}
  local ef = CreateFrame("Frame", "RavenEra_TestEventFrame")
  ef:RegisterEvent("UNIT_CASTEVENT")
  ef:SetScript("OnEvent", function(_, event, caster, target, kind, spellId, castTime)
    got[#got + 1] = tostring(event) .. ":" .. tostring(kind) .. ":" .. tostring(spellId) .. ":" .. tostring(castTime)
  end)
  local delivered = RavenEra_FireEvent("UNIT_CASTEVENT", "0xA", "0xB", "START", 133, 1500)
  out[#out + 1] = "event hook: " .. tostring(RavenEra_EventHookInfo())
  out[#out + 1] = "event delivered=" .. tostring(delivered) .. " got=" .. table.concat(got, ",")
  out[#out + 1] = "IsEventRegistered(UNIT_CASTEVENT)=" .. tostring(ef:IsEventRegistered("UNIT_CASTEVENT"))
  ef:UnregisterEvent("UNIT_CASTEVENT")
  out[#out + 1] = "after unregister delivered=" .. tostring(RavenEra_FireEvent("UNIT_CASTEVENT", "0xA", "0xB", "CAST", 133, 0))

  -- HearthDB still works through its own slot
  call("HDB_GetVersion", HDB_GetVersion)

  -- C_* namespace inventory (for the next API batch)
  local dump = {}
  for _, n in ipairs({ "C_QuestLog", "C_Map", "C_NamePlate", "C_Spell", "C_UnitAuras", "C_CVar" }) do
    local tb = _G[n]
    if type(tb) == "table" then
      for k, v in pairs(tb) do dump[#dump + 1] = n .. "." .. tostring(k) .. "=" .. type(v) end
    end
  end
  table.sort(dump)
  pcall(RavenEra, "ExportFile", "Logs\\c_api_dump.txt", table.concat(dump, "\r\n"))

  -- live UNIT_CASTEVENT log (cast something in-game, then read the file)
  local liveLines = {}
  local lf = CreateFrame("Frame", "RavenEra_LiveCastTest")
  lf:RegisterEvent("UNIT_CASTEVENT")
  lf:SetScript("OnEvent", function(_, event, caster, target, kind, spellId, castTime)
    liveLines[#liveLines + 1] = string.format("%.2f %s spell=%s castTime=%s",
      GetTime(), tostring(kind), tostring(spellId), tostring(castTime))
    pcall(RavenEra, "ExportFile", "Logs\\ravenapi_events.txt", table.concat(liveLines, "\r\n"))
  end)

  local ok, err = pcall(RavenEra, "ExportFile", "Logs\\ravenapi_selftest.txt", table.concat(out, "\r\n"))
  if ok then
    DEFAULT_CHAT_FRAME:AddMessage("RAVENAPI SELFTEST -> Logs\\ravenapi_selftest.txt")
  else
    DEFAULT_CHAT_FRAME:AddMessage("RAVENAPI SELFTEST write failed: " .. tostring(err))
  end
end)



