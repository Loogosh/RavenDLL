// HearthDB for Ravencraft 1.14 — HDB_* via 1 SlotPrep gate + Lua shim.
// FrameScript C bindings only reliably pass ~2 stack args → payload packed in arg2.
#include "DbEngine.h"
#include "LuaWow.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void Log(const char *msg) {
	CreateDirectoryA("Logs", nullptr);
	FILE *f = fopen("Logs\\hearthdb.log", "a");
	if(!f) return;
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(f, "[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
	fclose(f);
}

static void Logf(const char *fmt, ...) {
	char msg[512];
	va_list ap; va_start(ap, fmt);
	vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
	va_end(ap);
	Log(msg);
}

static std::vector<std::string> SplitPayload(const char *p) {
	std::vector<std::string> out;
	if(!p) return out;
	const char sep = '\x01';
	const char *s = p;
	for(;;) {
		const char *e = strchr(s, sep);
		if(!e) { out.emplace_back(s); break; }
		out.emplace_back(s, e - s);
		s = e + 1;
	}
	return out;
}

// Query results are shipped to Lua as ONE length-prefixed string and the tables are
// built in Lua. Why: the launcher's packer virtualizes lua_createtable; calling it
// from this DLL makes its VM write a computed byte into our return address (our .text)
// and then #DE-spin — the client hang of 2026-10-29. lua_pushstring is safe.
// Format: S<ncols>;<len>:<col>...<nrows>;<len>:<cell>... (-1 = SQL NULL)
static void AppendNum(std::string &s, size_t v) {
	char b[32];
	sprintf_s(b, "%zu", v);
	s += b;
}

static std::string SerializeResult(const HdbResult &r) {
	std::string s;
	s.reserve(64 + r.rows.size() * (r.colNames.size() + 1) * 16);
	s += 'S';
	AppendNum(s, r.colNames.size());
	s += ';';
	for(const std::string &c : r.colNames) {
		AppendNum(s, c.size());
		s += ':';
		s += c;
	}
	AppendNum(s, r.rows.size());
	s += ';';
	for(const HdbRow &row : r.rows) {
		for(size_t c = 0; c < r.colNames.size(); c++) {
			if(c >= row.vals.size() || !row.vals[c]) {
				s += "-1:";
			} else {
				const std::string &v = *row.vals[c];
				AppendNum(s, v.size());
				s += ':';
				s += v;
			}
		}
	}
	return s;
}

static int lua_HdbGate(lua_State *L) {
	if(!L) return 0;
	const int top = lua_gettop(L);
	if(top < 1 || !lua_isstring(L, 1)) {
		// We stole IsOnTournamentRealm; if Blizzard UI still calls it, answer `false`.
		lua_pushboolean(L, 0);
		return 1;
	}
	const char *cmd = lua_tostring(L, 1);
	if(!cmd) return 0;
	const char *payload = (top >= 2 && lua_isstring(L, 2)) ? lua_tostring(L, 2) : "";
	auto parts = SplitPayload(payload);

	// Pump is called every frame; everything else is rare and worth a log line.
	if(_stricmp(cmd, "Pump") && _stricmp(cmd, "Reset")) {
		std::string brief = payload ? payload : "";
		for(char &ch : brief) if(ch == '\x01') ch = '|';
		if(brief.size() > 140) brief.resize(140);
		Logf("gate cmd=%s payload=%s", cmd, brief.c_str());
	}

	if(!_stricmp(cmd, "GetVersion") || !_stricmp(cmd, "ver")) {
		lua_pushinteger(L, HdbVersionMaj());
		lua_pushinteger(L, HdbVersionMin());
		lua_pushinteger(L, HdbVersionPat());
		return 3;
	}
	if(!_stricmp(cmd, "Open")) {
		std::string err;
		int h = HdbOpen(payload ? payload : "", err);
		if(!h) { lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2; }
		lua_pushinteger(L, h);
		return 1;
	}
	if(!_stricmp(cmd, "OpenAddon")) {
		if(parts.size() < 2) { lua_pushnil(L); lua_pushstring(L, "args"); return 2; }
		std::string err;
		int h = HdbOpenAddon(parts[0], parts[1], err);
		if(!h) { lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2; }
		lua_pushinteger(L, h);
		return 1;
	}
	if(!_stricmp(cmd, "Close")) {
		if(!payload || !*payload || !HdbClose(atoi(payload))) {
			lua_pushnil(L); lua_pushstring(L, "invalid or already-closed handle"); return 2;
		}
		return 0;
	}
	if(!_stricmp(cmd, "Execute")) {
		if(parts.size() < 2) { lua_pushnil(L); lua_pushstring(L, "args"); return 2; }
		std::string err;
		// Turtle sync Execute returns 0 values; sql may contain SEP → rejoin parts[1..]
		std::string sql = parts[1];
		for(size_t i = 2; i < parts.size(); i++) { sql.push_back('\x01'); sql += parts[i]; }
		char buf[160];
		sprintf_s(buf, "Execute h=%s sql_len=%zu", parts[0].c_str(), sql.size());
		Log(buf);
		if(!HdbExecute(atoi(parts[0].c_str()), sql, err, nullptr)) {
			Logf("Execute FAIL h=%s err=%s", parts[0].c_str(), err.c_str());
			lua_pushnil(L); lua_pushstring(L, err.c_str()); return 2;
		}
		Log("Execute OK");
		return 0;
	}
	if(!_stricmp(cmd, "Query") || !_stricmp(cmd, "QueryRaw")) {
		if(parts.size() < 2) { lua_pushnil(L); return 1; }
		std::string sql = parts[1];
		for(size_t i = 2; i < parts.size(); i++) { sql.push_back('\x01'); sql += parts[i]; }
		HdbResult r;
		bool ok = !_stricmp(cmd, "Query")
			? HdbQuery(atoi(parts[0].c_str()), sql, r)
			: HdbQueryRaw(atoi(parts[0].c_str()), sql, r);
		if(!ok) {
			Logf("%s FAIL h=%s err=%s", cmd, parts[0].c_str(), r.err.c_str());
			lua_pushnil(L); lua_pushstring(L, r.err.c_str()); return 2;
		}
		Logf("%s OK h=%s rows=%zu cols=%zu", cmd, parts[0].c_str(), r.rows.size(), r.colNames.size());
		const std::string packed = SerializeResult(r);
		lua_pushstring(L, packed.c_str());
		return 1;
	}
	if(!_stricmp(cmd, "ExecuteAsync")) {
		if(parts.size() < 2) { lua_pushnil(L); lua_pushstring(L, "args"); return 2; }
		std::string sql = parts[1];
		for(size_t i = 2; i < parts.size(); i++) { sql.push_back('\x01'); sql += parts[i]; }
		int t = HdbExecuteAsync(atoi(parts[0].c_str()), sql);
		if(t < 0) {
			lua_pushnil(L);
			lua_pushstring(L, "handle has a pending failure -- call HDB_GetResult to retrieve it");
			return 2;
		}
		if(!t) { lua_pushnil(L); lua_pushstring(L, "invalid handle"); return 2; }
		lua_pushinteger(L, t);
		return 1;
	}
	if(!_stricmp(cmd, "QueryAsync") || !_stricmp(cmd, "QueryRawAsync")) {
		if(parts.size() < 2) { lua_pushnil(L); lua_pushstring(L, "args"); return 2; }
		std::string sql = parts[1];
		for(size_t i = 2; i < parts.size(); i++) { sql.push_back('\x01'); sql += parts[i]; }
		int t = HdbQueryAsync(atoi(parts[0].c_str()), sql, !_stricmp(cmd, "QueryRawAsync"));
		if(t < 0) {
			lua_pushnil(L);
			lua_pushstring(L, "handle has a pending failure -- call HDB_GetResult to retrieve it");
			return 2;
		}
		if(!t) { lua_pushnil(L); lua_pushstring(L, "invalid handle"); return 2; }
		lua_pushinteger(L, t);
		return 1;
	}
	if(!_stricmp(cmd, "GetResult")) {
		HdbResult r;
		if(!payload || !*payload || !HdbGetResult(atoi(payload), r)) {
			lua_pushnil(L);
			return 1;
		}
		if(!r.ok) { lua_pushnil(L); lua_pushstring(L, r.err.c_str()); return 2; }
		if(r.kind == HdbResult::Exec) { lua_pushinteger(L, r.rowsAffected); return 1; }
		const std::string packed = SerializeResult(r);
		lua_pushstring(L, packed.c_str());
		return 1;
	}
	if(!_stricmp(cmd, "ClearPoison")) {
		if(payload && *payload) HdbClearPoison(atoi(payload));
		return 0;
	}
	if(!_stricmp(cmd, "Pump") || !_stricmp(cmd, "Reset")) {
		if(!_stricmp(cmd, "Reset")) HdbResetAll();
		HdbPumpAsync();
		return 0;
	}
	lua_pushnil(L);
	lua_pushstring(L, "unknown cmd");
	return 2;
}

static DWORD WINAPI BootThread(LPVOID) {
	for(int i = 0; i < 600; i++) {
		HMODULE prep = GetModuleHandleA("SlotPrep.dll");
		if(prep) {
			auto count = (int (*)())GetProcAddress(prep, "RavenEra_SlotCount");
			auto claim = (int (*)(int, void *))GetProcAddress(prep, "RavenEra_ClaimSlot");
			if(claim && count && count() > 0) {
				if(claim(0, (void *)lua_HdbGate)) {
					Log("ClaimSlot[0]=HdbGate OK");
					return 0;
				}
			}
		}
		Sleep(100);
	}
	Log("FAIL: SlotPrep ClaimSlot timeout");
	return 1;
}

extern "C" __declspec(dllexport) DWORD Load() {
	Log("HearthDB Load()");
	CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
	if(reason == DLL_PROCESS_ATTACH) {
		Log("HearthDB attach");
		CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr);
	} else if(reason == DLL_PROCESS_DETACH) {
		HdbShutdownWorker();
		HdbResetAll();
	}
	return TRUE;
}
