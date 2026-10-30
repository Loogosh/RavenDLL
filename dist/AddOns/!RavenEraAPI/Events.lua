-- Custom events for SuperWoW compatibility: UNIT_CASTEVENT, RAW_COMBATLOG.
-- The 1.14 client does not know these names, so we intercept frame:RegisterEvent
-- and dispatch ourselves. Everything else is passed to the stock widget methods.

local CUSTOM = {
  UNIT_CASTEVENT = true,
  RAW_COMBATLOG = true,
  CREATE_CHATBUBBLE = true,
}

local registry = {} -- event -> { [frame] = true }

local function addFrame(event, frame)
  local set = registry[event]
  if not set then set = {}; registry[event] = set end
  set[frame] = true
end

local function removeFrame(event, frame)
  local set = registry[event]
  if set then set[frame] = nil end
end

local hooked, hookError

local function hookWidgetApi()
  local probe = CreateFrame("Frame")
  local mt = getmetatable(probe)
  local methods = mt and mt.__index
  if type(methods) ~= "table" then return false, "no method table" end
  local origRegister = methods.RegisterEvent
  local origUnregister = methods.UnregisterEvent
  local origIsRegistered = methods.IsEventRegistered
  local origUnregisterAll = methods.UnregisterAllEvents
  if type(origRegister) ~= "function" or type(origUnregister) ~= "function" then
    return false, "RegisterEvent not a function"
  end

  methods.RegisterEvent = function(self, event, ...)
    if CUSTOM[event] then addFrame(event, self); return end
    return origRegister(self, event, ...)
  end
  methods.UnregisterEvent = function(self, event, ...)
    if CUSTOM[event] then removeFrame(event, self); return end
    return origUnregister(self, event, ...)
  end
  if type(origIsRegistered) == "function" then
    methods.IsEventRegistered = function(self, event, ...)
      if CUSTOM[event] then
        local set = registry[event]
        return (set and set[self]) and 1 or nil
      end
      return origIsRegistered(self, event, ...)
    end
  end
  if type(origUnregisterAll) == "function" then
    methods.UnregisterAllEvents = function(self, ...)
      for _, set in pairs(registry) do set[self] = nil end
      return origUnregisterAll(self, ...)
    end
  end
  return true
end

hooked, hookError = hookWidgetApi()

-- Fire a custom event to every registered frame (pcall: one bad handler must not
-- break the others). Exposed for the self-test and for C-side event pumping later.
function RavenEra_FireEvent(event, ...)
  local set = registry[event]
  if not set then return 0 end
  local frames = {}
  for frame in pairs(set) do frames[#frames + 1] = frame end
  local delivered = 0
  for i = 1, #frames do
    local frame = frames[i]
    if set[frame] then
      local handler = frame.GetScript and frame:GetScript("OnEvent")
      if handler then
        local ok, err = pcall(handler, frame, event, ...)
        if not ok then
          if geterrorhandler then geterrorhandler()(err) end
        end
        delivered = delivered + 1
      end
    end
  end
  return delivered
end

function RavenEra_EventHookInfo()
  return hooked and "hooked" or ("not hooked: " .. tostring(hookError))
end

-- UNIT_CASTEVENT synthesis --------------------------------------------------
-- SuperWoW: (casterGuid, targetGuid, eventType, spellId, castTimeMs)
-- eventType: START | CAST | FAIL | CHANNEL | MAINHAND | OFFHAND
local castFrame = CreateFrame("Frame", "RavenEra_CastEvents")
castFrame:RegisterEvent("UNIT_SPELLCAST_START")
castFrame:RegisterEvent("UNIT_SPELLCAST_STOP")
castFrame:RegisterEvent("UNIT_SPELLCAST_FAILED")
castFrame:RegisterEvent("UNIT_SPELLCAST_SUCCEEDED")
castFrame:RegisterEvent("UNIT_SPELLCAST_INTERRUPTED")
castFrame:RegisterEvent("UNIT_SPELLCAST_CHANNEL_START")
castFrame:RegisterEvent("UNIT_SPELLCAST_CHANNEL_STOP")

local pendingCast = {} -- unit -> { spellId, castTime, targetGuid }

castFrame:SetScript("OnEvent", function(_, event, unit, castGUID, spellID)
  local caster = UnitGUID(unit)
  local target = UnitGUID("target")
  if event == "UNIT_SPELLCAST_START" or event == "UNIT_SPELLCAST_CHANNEL_START" then
    local _, _, _, startMS, endMS = UnitCastingInfo(unit)
    if event == "UNIT_SPELLCAST_CHANNEL_START" then
      _, _, _, startMS, endMS = UnitChannelInfo(unit)
    end
    local castTime = (endMS or 0) - (startMS or 0)
    local kind = (event == "UNIT_SPELLCAST_CHANNEL_START") and "CHANNEL" or "START"
    pendingCast[unit] = { spellId = spellID, castTime = castTime, targetGuid = target }
    RavenEra_FireEvent("UNIT_CASTEVENT", caster, target, kind, spellID, castTime)
  elseif event == "UNIT_SPELLCAST_SUCCEEDED" then
    local p = pendingCast[unit]
    RavenEra_FireEvent("UNIT_CASTEVENT", caster, (p and p.targetGuid) or target, "CAST", spellID, 0)
    pendingCast[unit] = nil
  elseif event == "UNIT_SPELLCAST_FAILED" or event == "UNIT_SPELLCAST_INTERRUPTED" then
    RavenEra_FireEvent("UNIT_CASTEVENT", caster, target, "FAIL", spellID, 0)
    pendingCast[unit] = nil
  elseif event == "UNIT_SPELLCAST_STOP" or event == "UNIT_SPELLCAST_CHANNEL_STOP" then
    pendingCast[unit] = nil
  end
end)

-- RAW_COMBATLOG synthesis ---------------------------------------------------
local logFrame = CreateFrame("Frame", "RavenEra_RawCombatLog")
logFrame:RegisterEvent("COMBAT_LOG_EVENT_UNFILTERED")
local function pack(...)
  return { n = select("#", ...), ... }
end

logFrame:SetScript("OnEvent", function()
  if type(CombatLogGetCurrentEventInfo) ~= "function" then return end
  local info = pack(CombatLogGetCurrentEventInfo())
  if info.n == 0 then return end
  RavenEra_FireEvent("RAW_COMBATLOG", unpack(info, 1, info.n))
end)
