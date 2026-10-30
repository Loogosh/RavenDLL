-- OPT-IN recon dump for the engine RE work (add "Recon.lua" to the .toc).
-- Writes Logs\recon.txt with GUIDs, positions, health and camera info.
-- Also callable on demand: /run RavenEra_ReconDump()

local function join(...)
  local n = select("#", ...)
  local out = {}
  for i = 1, n do out[i] = tostring((select(i, ...))) end
  return table.concat(out, ",")
end

function RavenEra_ReconDump()
  local out = {}
  local function add(k, v) out[#out + 1] = k .. "=" .. tostring(v) end

  add("player.guid", UnitGUID("player"))
  add("player.pos", join(UnitPosition("player")))
  add("player.health", UnitHealth("player"))
  add("player.name", UnitName("player"))

  local tg = UnitGUID("target")
  add("target.guid", tg or "nil")
  if tg then
    add("target.pos", join(UnitPosition("target")))
    add("target.health", UnitHealth("target"))
    add("target.name", UnitName("target"))
    add("target.reaction", UnitReaction("target", "player"))
  end

  local ci = { pcall(GetUICameraInfo, 1) }
  add("camera.ok", ci[1])
  for i = 2, #ci do add("camera." .. (i - 2), ci[i]) end

  pcall(RavenEra, "ExportFile", "Logs\\recon.txt", table.concat(out, "\r\n"))
  DEFAULT_CHAT_FRAME:AddMessage("RECON -> Logs\\recon.txt")
end

local f = CreateFrame("Frame", "RavenEra_Recon")
local t = 0
f:SetScript("OnUpdate", function(_, dt)
  t = t + (dt or 0.05)
  if t < 2 then return end
  f:SetScript("OnUpdate", nil)
  RavenEra_ReconDump()
  pcall(RavenEra_NameplateDump)
end)

function RavenEra_NameplateDump()
  local out = {}
  local ok, err = pcall(function()
    local plates = C_NamePlate.GetNamePlates() or {}
    out[#out + 1] = "plates=" .. #plates
    for i, f in ipairs(plates) do
      out[#out + 1] = "plate " .. i ..
        " name=" .. tostring(f:GetName()) ..
        " unit=" .. tostring(f.unit) ..
        " guid=" .. tostring(type(f.unit) == "string" and UnitGUID(f.unit) or nil) ..
        " frame=" .. tostring(f)
      local uf = f.UnitFrame
      out[#out + 1] = "  unitframe=" .. tostring(uf) .. " uf0=" .. tostring(uf and uf[0])
      local ks = {}
      for k, v in pairs(f) do ks[#ks + 1] = tostring(k) .. "=" .. type(v) .. ":" .. tostring(v) end
      out[#out + 1] = "  keys: " .. table.concat(ks, ", ")
    end
  end)
  out[#out + 1] = "ok=" .. tostring(ok) .. " err=" .. tostring(err)
  pcall(RavenEra, "ExportFile", "Logs\\nameplates.txt", table.concat(out, "\r\n"))
  DEFAULT_CHAT_FRAME:AddMessage("NAMEPLATES -> Logs\\nameplates.txt")
end




