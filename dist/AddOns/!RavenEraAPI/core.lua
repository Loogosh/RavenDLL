-- RavenEra API core: one C gate (stolen SetTaxiBenchmarkMode) -> all new globals.
-- This addon is named with "!" so it loads before normal addons.

local SEP = string.char(1)
local G
local booted = false

RAVENERA_VERSION = "0.1.0"

function RavenEra(cmd, ...)
  if not G then return nil, "RavenEra not ready" end
  local n = select("#", ...)
  if n <= 0 then return G(cmd) end
  if n == 1 then return G(cmd, tostring(select(1, ...))) end
  local t = {}
  for i = 1, n do t[i] = tostring(select(i, ...)) end
  return G(cmd, table.concat(t, SEP))
end

local function gateWorks(fn)
  if type(fn) ~= "function" then return false end
  local ok, a, b = pcall(fn, "GetVersion")
  return ok and a == 0 and b == 1 -- SuperWoW gate 0.1
end

local function tryBoot()
  if booted then return true end
  -- Online keeps the stock name (fn swapped by SlotPrep); RavenEra_S1 is offline.
  local names = { "SetTaxiBenchmarkMode", "RavenEra_S1" }
  for i = 1, #names do
    local fn = _G[names[i]]
    if gateWorks(fn) then
      G = fn
      booted = true
      RAVENERA_READY = true
      if type(RavenEra_OnGateReady) == "function" then RavenEra_OnGateReady() end
      DEFAULT_CHAT_FRAME:AddMessage("|cff55ff55RavenEra API ready|r")
      return true
    end
  end
  return false
end

if not tryBoot() then
  local f = CreateFrame("Frame", "RavenEraAPI_Wait")
  local t0 = GetTime and GetTime() or 0
  f:SetScript("OnUpdate", function()
    if tryBoot() then
      f:SetScript("OnUpdate", nil)
      return
    end
    local now = GetTime and GetTime() or t0
    if now - t0 > 60 then
      f:SetScript("OnUpdate", nil)
      DEFAULT_CHAT_FRAME:AddMessage("|cffff5555RavenEra: gate missing after 60s (SlotPrep?)|r")
    end
  end)
end
