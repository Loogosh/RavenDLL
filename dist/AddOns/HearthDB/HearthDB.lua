-- HearthDB shim: HDB_* → gate (RavenEra_S0 or stolen IsUsingFixedTimeStep).
-- SlotPrep often steals AFTER first Register walk → only old name is in _G.

local SEP = string.char(1)
local pending = {}
local G
local booted = false

local function gate(cmd, ...)
  if not G then return nil, "HearthDB not ready" end
  local n = select("#", ...)
  if n <= 0 then return G(cmd) end
  if n == 1 then return G(cmd, tostring(select(1, ...))) end
  local t = {}
  for i = 1, n do t[i] = tostring(select(i, ...)) end
  return G(cmd, table.concat(t, SEP))
end

local function logPumpErr(msg)
  DEFAULT_CHAT_FRAME:AddMessage("|cffff9900HDB callback: " .. tostring(msg) .. "|r")
end

-- Results arrive as one packed string; tables are built here in Lua on purpose:
-- the packer virtualizes lua_createtable, so building tables from the DLL corrupts
-- the caller and then #DE-spins the game (see HANDOFF_HEARTHDB.md).
-- Format: "S<ncols>;<len>:<col>...<nrows>;<len>:<cell>..." with -1 = SQL NULL.
local function parsePacked(s)
  if type(s) ~= "string" or string.byte(s, 1) ~= 83 then return nil, "not packed" end
  local pos = 2
  local function number(stop)
    local p = string.find(s, stop, pos, true)
    if not p then return nil end
    local v = tonumber(string.sub(s, pos, p - 1))
    pos = p + 1
    return v
  end
  local nc = number(";")
  if not nc then return nil, "ncols" end
  local cols = {}
  for i = 1, nc do
    local n = number(":")
    if not n then return nil, "col len" end
    cols[i] = string.sub(s, pos, pos + n - 1)
    pos = pos + n
  end
  local nr = number(";")
  if not nr then return nil, "nrows" end
  local rows = {}
  for r = 1, nr do
    local row = {}
    for c = 1, nc do
      local n = number(":")
      if not n then return nil, "cell len" end
      if n >= 0 then
        row[c] = string.sub(s, pos, pos + n - 1)
        pos = pos + n
      end
    end
    rows[r] = row
  end
  return cols, rows
end

local function namedRows(packed)
  local cols, rows = parsePacked(packed)
  if not cols then return nil end
  local out = {}
  for i = 1, #rows do
    local src, dst = rows[i], {}
    for c = 1, #cols do dst[cols[c]] = src[c] end
    out[i] = dst
  end
  return out
end

local function bindApi()
  function HDB_GetVersion()
    return gate("GetVersion")
  end

  function HDB_Open(filename)
    local h, err = gate("Open", filename)
    if not h then error(err or "HDB_Open failed", 2) end
    return h
  end

  function HDB_OpenAddon(addon, path)
    local h, err = gate("OpenAddon", addon, path)
    if not h then error(err or "HDB_OpenAddon failed", 2) end
    return h
  end

  function HDB_Close(handle)
    local _, err = gate("Close", handle)
    if err then error(err, 2) end
  end

  function HDB_Execute(handle, sql)
    local _, err = gate("Execute", handle, sql)
    if err then error(err, 2) end
  end

  function HDB_Query(handle, sql)
    local packed, err = gate("Query", handle, sql)
    if type(packed) ~= "string" then error(err or "HDB_Query failed", 2) end
    local rows = namedRows(packed)
    if not rows then error("HDB_Query: bad result", 2) end
    return rows
  end

  function HDB_QueryRaw(handle, sql)
    local packed, err = gate("QueryRaw", handle, sql)
    if type(packed) ~= "string" then error(err or "HDB_QueryRaw failed", 2) end
    local cols, rows = parsePacked(packed)
    if not cols then error("HDB_QueryRaw: bad result", 2) end
    return cols, rows
  end

  function HDB_ExecuteAsync(handle, sql, cb)
    local t, err = gate("ExecuteAsync", handle, sql)
    if not t then error(err or "HDB_ExecuteAsync failed", 2) end
    if cb then pending[t] = { kind = "exec", cb = cb } end
    return t
  end

  function HDB_QueryAsync(handle, sql, cb)
    local t, err = gate("QueryAsync", handle, sql)
    if not t then error(err or "HDB_QueryAsync failed", 2) end
    if cb then pending[t] = { kind = "query", cb = cb } end
    return t
  end

  function HDB_QueryRawAsync(handle, sql, cb)
    local t, err = gate("QueryRawAsync", handle, sql)
    if not t then error(err or "HDB_QueryRawAsync failed", 2) end
    if cb then pending[t] = { kind = "raw", cb = cb } end
    return t
  end

  function HDB_GetResult(ticket)
    if pending[ticket] then return nil end
    local v, err = gate("GetResult", ticket)
    if type(v) == "string" then return namedRows(v) end   -- packed query result
    return v, err
  end

  function HDB_ClearPoison(handle)
    gate("ClearPoison", handle)
  end

  function HDB_Pump()
    gate("Pump")
    for t, meta in pairs(pending) do
      local a, b = gate("GetResult", t)
      if a ~= nil or b ~= nil then
        pending[t] = nil
        local ok, emsg = pcall(function()
          if meta.kind == "exec" then
            meta.cb(a, b)
          elseif meta.kind == "raw" then
            if a == nil then meta.cb(nil, nil, b)
            else
              local cols, rows = parsePacked(a)
              meta.cb(cols, rows, nil)
            end
          else
            if a == nil then meta.cb(nil, b) else meta.cb(namedRows(a), nil) end
          end
        end)
        if not ok then logPumpErr(emsg) end
      end
    end
  end

  CreateFrame("Frame", "HDB_CallbackFrame"):SetScript("OnUpdate", HDB_Pump)

  function HDB_SmokeTest()
    local function ok(name) DEFAULT_CHAT_FRAME:AddMessage("|cff00ff00HDB OK|r " .. name) end
    local function fail(name, e) DEFAULT_CHAT_FRAME:AddMessage("|cffff0000HDB FAIL|r " .. name .. ": " .. tostring(e)) end

    local maj, min, pat = HDB_GetVersion()
    if maj ~= 0 or min ~= 4 then fail("GetVersion", tostring(maj) .. "." .. tostring(min) .. "." .. tostring(pat)) return end
    ok("GetVersion " .. maj .. "." .. min .. "." .. pat)

    local h = HDB_Open("hdb_smoke.db")
    ok("Open h=" .. tostring(h))

    HDB_Execute(h, [[
      CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, name TEXT, n REAL, b BLOB);
      DELETE FROM t;
      INSERT INTO t(name,n) VALUES ('a',1),('b',2.5),(NULL, NULL);
    ]])
    ok("Execute multi")

    local rows = HDB_Query(h, "SELECT name, n FROM t ORDER BY id")
    if type(rows) ~= "table" or (table.getn and table.getn(rows) or #rows) < 3 then fail("Query", "rows") return end
    if rows[1].name ~= "a" or rows[1].n ~= "1" then fail("Query vals", tostring(rows[1].name) .. "/" .. tostring(rows[1].n)) return end
    if rows[3].name ~= nil then fail("Query NULL", "expected nil") return end
    ok("Query named+string+nil")

    local cols, rrows = HDB_QueryRaw(h, "SELECT name, n FROM t WHERE name='b'")
    if cols[1] ~= "name" or rrows[1][1] ~= "b" or rrows[1][2] ~= "2.5" then
      fail("QueryRaw", "mismatch") return
    end
    ok("QueryRaw")

    local ah = HDB_OpenAddon("HearthDB", "data/smoke.db")
    local arows = HDB_Query(ah, "SELECT v FROM kv WHERE k='hello'")
    if not arows[1] or arows[1].v ~= "world" then fail("OpenAddon", "kv") return end
    local ro_ok, ro_err = pcall(HDB_Execute, ah, "DELETE FROM kv")
    if ro_ok then fail("OpenAddon RO", "execute should fail") return end
    ok("OpenAddon RO")
    HDB_Close(ah)

    HDB_ExecuteAsync(h, "INSERT INTO t(name,n) VALUES ('async',9)", function(aff, err)
      if err then fail("ExecuteAsync cb", err) HDB_ClearPoison(h) return end
      ok("ExecuteAsync cb aff=" .. tostring(aff))
    end)

    local tq = HDB_QueryAsync(h, "SELECT name FROM t WHERE name='a'")
    local pollFrame = CreateFrame("Frame")
    local elapsed = 0
    pollFrame:SetScript("OnUpdate", function(_, e)
      elapsed = elapsed + (e or 0.05)
      local res, err = HDB_GetResult(tq)
      if res == nil and err == nil then
        if elapsed > 5 then pollFrame:SetScript("OnUpdate", nil) fail("QueryAsync poll", "timeout") end
        return
      end
      pollFrame:SetScript("OnUpdate", nil)
      if err then fail("QueryAsync", err) HDB_ClearPoison(h) return end
      if res[1].name ~= "a" then fail("QueryAsync row", res[1].name) return end
      ok("QueryAsync poll")
      HDB_QueryRawAsync(h, "SELECT name FROM t WHERE name='b'", function(c, r, err2)
        if err2 then fail("QueryRawAsync", err2) HDB_ClearPoison(h) return end
        if c[1] ~= "name" or r[1][1] ~= "b" then fail("QueryRawAsync data", "?") return end
        ok("QueryRawAsync cb")
        HDB_Close(h)
        ok("Close")
        ok("=== ALL SMOKE PASSED ===")
      end)
    end)
  end
end

local function gateWorks(fn)
  if type(fn) ~= "function" then return false end
  local a, b, c = fn("GetVersion")
  return a == 0 and b == 4 -- HearthDB 0.4.x
end

local function tryBoot()
  if booted then return true end
  -- Online: SlotPrep swaps the fn of IsOnTournamentRealm but keeps the stock name
  -- (so the name survives /reload). RavenEra_S0 is the FILE_EXTEND/offline name.
  if IsOnTournamentRealm and gateWorks(IsOnTournamentRealm) then
    G = IsOnTournamentRealm
  elseif RavenEra_S0 and gateWorks(RavenEra_S0) then
    G = RavenEra_S0
  else
    return false
  end
  booted = true
  G("Reset")
  bindApi()
  DEFAULT_CHAT_FRAME:AddMessage("|cff55ff55HearthDB ready|r")
  return true
end

if not tryBoot() then
  local f = CreateFrame("Frame", "HearthDB_Wait")
  local t0 = GetTime and GetTime() or 0
  f:SetScript("OnUpdate", function()
    if tryBoot() then
      f:SetScript("OnUpdate", nil)
      return
    end
    local now = GetTime and GetTime() or t0
    if now - t0 > 60 then
      f:SetScript("OnUpdate", nil)
      DEFAULT_CHAT_FRAME:AddMessage("|cffff5555HearthDB: gate missing after 60s (SlotPrep?)|r")
    end
  end)
end
