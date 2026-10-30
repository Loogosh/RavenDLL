-- OPT-IN: inspect the UnitExists closure from Lua (what kind of function it is,
-- what its upvalues are). Writes Logs\closure.txt via the gate.
local f = CreateFrame("Frame", "RavenEra_Info")
local t = 0
f:SetScript("OnUpdate", function(_, dt)
  t = t + (dt or 0.05)
  if t < 2 then return end
  f:SetScript("OnUpdate", nil)

  local out = {}
  local function add(k, v) out[#out + 1] = k .. "=" .. tostring(v) end
  add("type", type(UnitExists))
  if type(debug) == "table" and type(debug.getinfo) == "function" then
    local ok, info = pcall(debug.getinfo, UnitExists, "S")
    add("getinfo.ok", ok)
    if ok and type(info) == "table" then
      add("what", info.what)
      add("name", info.name)
      add("source", info.source)
      add("linedefined", info.linedefined)
    end
    local ok2, n, v = pcall(debug.getupvalue, UnitExists, 1)
    add("upvalue.ok", ok2)
    add("upvalue.name", n)
    add("upvalue.val", type(v) .. ":" .. tostring(v))
    local ok3, n2, v2 = pcall(debug.getupvalue, UnitExists, 2)
    add("upvalue2.ok", ok3)
    add("upvalue2.name", n2)
    add("upvalue2.val", type(v2) .. ":" .. tostring(v2))
  else
    add("debug", "nil")
  end
  pcall(RavenEra, "ExportFile", "Logs\\closure.txt", table.concat(out, "\r\n"))
end)
