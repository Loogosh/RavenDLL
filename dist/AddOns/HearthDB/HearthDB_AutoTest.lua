-- OPT-IN boot self-test: add "HearthDB_AutoTest.lua" to HearthDB.toc to enable.
-- Runs HDB_SmokeTest() ~3 s after the addon loads (in-world), with no chat input.
-- Handy on launcher sessions where typing /run into chat is unreliable.
local f = CreateFrame("Frame", "HDB_AutoTest")
local elapsed = 0
local ran = false

f:SetScript("OnUpdate", function(_, e)
  elapsed = elapsed + (e or 0.05)
  if elapsed < 3 or ran then return end
  if type(HDB_SmokeTest) ~= "function" or type(HDB_GetVersion) ~= "function" then return end
  ran = true
  f:SetScript("OnUpdate", nil)
  local ok, err = pcall(HDB_SmokeTest)
  if not ok then
    DEFAULT_CHAT_FRAME:AddMessage("HDBTEST harness error: " .. tostring(err))
  end
end)
