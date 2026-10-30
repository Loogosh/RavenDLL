-- UnitXP_SP3 compatibility shim (partial): only commands that 1.14 can do
-- without engine-memory access. distanceBetween/inSight/behind/weather/targeting
-- need the unit object or protected calls and stay unimplemented for now.
UNITXP_VERSION = 3.0
UNITXP_STRING = "UnitXP_SP3 (RavenEra 1.14, partial)"

local function num(v)
  local n = tonumber(v)
  if n == nil then return nil end
  return n
end

function UnitXP(cmd, a, b)
  cmd = tostring(cmd or "")

  if cmd == "version" or cmd == "GetVersion" then return UNITXP_VERSION end

  if cmd == "screenshot" then
    local ok = pcall(Screenshot)
    return ok and 1 or 0
  end

  if cmd == "FPScap" then
    local v = num(a)
    if v and v > 0 then SetCVar("maxFPS", tostring(math.floor(v))) end
    return num(GetCVar("maxFPS")) or 0
  end

  if cmd == "camera" then
    local id = num(a) or 0
    local ok, v = pcall(GetUICameraInfo, id)
    if ok then return v end
    return nil
  end

  if cmd == "modernNameplateDistance" then
    local v = num(a)
    if v and v > 0 then SetCVar("nameplateDistance", tostring(math.floor(v))) end
    return num(GetCVar("nameplateDistance")) or 0
  end

  -- Engine/protected: not implementable without the unit object or secure code.
  -- Native 1.14 has TargetLastTarget/TargetUnit/AssistUnit/TargetNearestEnemy,
  -- but they are PROTECTED: calling them from insecure addon code is blocked
  -- ("!RavenEraAPI has been blocked..."), so the shim must not route them.
  if cmd == "target" or cmd == "assist" or cmd == "lastTarget" or cmd == "targetNearest" then
    return nil
  end
  -- Still engine-only (unit object): nothing native covers these.
  if cmd == "distanceBetween" or cmd == "inSight" or cmd == "behind" then
    return nil
  end
  if cmd == "weather" then
    return nil
  end

  error("UnitXP: unsupported command: " .. cmd, 2)
end

