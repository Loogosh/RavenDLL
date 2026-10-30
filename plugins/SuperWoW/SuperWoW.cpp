// SuperWoW compatibility layer for RavenCraft 1.14.2 — C gate on SlotPrep slot 1.
// The stolen FrameScript name is SetTaxiBenchmarkMode (unused by addons); all public
// SuperWoW globals are defined by the !RavenEraAPI addon and dispatch here.
#include "../HearthDB/LuaWow.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>

static void Log(const char *msg) {
	CreateDirectoryA("Logs", nullptr);
	FILE *f = fopen("Logs\\superwow.log", "a");
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

using LuaCFn = int(__cdecl *)(lua_State *);

static uint8_t *g_wow = nullptr;
static bool g_armRequested = false;
static char g_pendingOp[16] = { 0 };

// --- payload helpers --------------------------------------------------------
static std::vector<std::string> Split(const char *p) {
	std::vector<std::string> out;
	if(!p) return out;
	const char *s = p;
	for(;;) {
		const char *e = strchr(s, '\x01');
		if(!e) { out.emplace_back(s); break; }
		out.emplace_back(s, e - s);
		s = e + 1;
	}
	return out;
}

static bool SafeRel(const std::string &s) {
	if(s.empty() || s.size() > 240) return false;
	if(s.find("..") != std::string::npos) return false;
	if(s[0] == '/' || s[0] == '\\') return false;
	if(s.find(':') != std::string::npos) return false;
	if(s.find('*') != std::string::npos || s.find('?') != std::string::npos) return false;
	return true;
}

static bool ReadText(const std::string &path, std::string &out, std::string &err) {
	if(!SafeRel(path)) { err = "unsafe path"; return false; }
	FILE *f = fopen(path.c_str(), "rb");
	if(!f) { err = "open failed"; return false; }
	fseek(f, 0, SEEK_END);
	const long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	out.resize(n > 0 ? (size_t)n : 0);
	if(n > 0) { const size_t got = fread(&out[0], 1, (size_t)n, f); out.resize(got); }
	fclose(f);
	return true;
}

static bool WriteText(const std::string &path, const std::string &data, std::string &err) {
	if(!SafeRel(path)) { err = "unsafe path"; return false; }
	FILE *f = fopen(path.c_str(), "wb");
	if(!f) { err = "open failed"; return false; }
	if(!data.empty()) fwrite(data.data(), 1, data.size(), f);
	fclose(f);
	return true;
}

static int PushErr(lua_State *L, const std::string &err) {
	lua_pushnil(L);
	lua_pushstring(L, err.c_str());
	return 2;
}

// --- engine helpers ---------------------------------------------------------
// Target descriptor resolver: fills {low, high} for a unit token.
static int ResolveTargetDesc(const char *token, unsigned long long *a, unsigned long long *b) {
	struct Desc { unsigned long long lo, hi; } d = { 0, 0 };
	__try {
		auto fn = (int (__fastcall *)(void *, const char *, int, int, int, int, float, int))(g_wow + 0x15D8230);
		fn(&d, token, 1, 8, 1, 0, 0.0f, 1);
	} __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
	*a = d.lo; *b = d.hi;
	return (d.lo || d.hi) ? 1 : 0;
}

static int CallSetTargetCore(unsigned long long lo, unsigned long long hi, int flags) {
	struct Desc { unsigned long long lo, hi; } d = { lo, hi };
	__try {
		auto fn = (void (__fastcall *)(void *, void *, int))(g_wow + 0x9DA8A0);
		fn((void *)(g_wow + 0x2BA35D0), &d, flags);
		return 1;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// Object manager: std::vector<Object*> at base+0x2F90000 (begin/end).
// Object: vtable +0x00, GUID {low,high} +0x58, position/orientation +0x1644,
// descriptor array pointer +0x10 (reach +0x1A0, radius +0x19C, owner +0x5C).
static int FindObjPos(unsigned long long guid, float out[4]) {
	__try {
		uint8_t *begin = *(uint8_t **)(g_wow + 0x2F90000);
		uint8_t *end = *(uint8_t **)(g_wow + 0x2F90008);
		if(!begin || !end || end <= begin) return 0;
		for(uint8_t *p = begin; p + 8 <= end; p += 8) {
			uint8_t *obj = *(uint8_t **)p;
			if(!obj) continue;
			if(*(unsigned long long *)(obj + 0x58) != guid) continue;
			float *pos = (float *)(obj + 0x1644);
			out[0] = pos[0]; out[1] = pos[1]; out[2] = pos[2]; out[3] = pos[3];
			return 1;
		}
		return 0;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int ObjCountRaw() {
	__try {
		uint8_t *begin = *(uint8_t **)(g_wow + 0x2F90000);
		uint8_t *end = *(uint8_t **)(g_wow + 0x2F90008);
		if(!begin || !end || end <= begin) return 0;
		return (int)((end - begin) / 8);
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int FindObjReach(unsigned long long guid, float *reach, float *radius) {
	__try {
		uint8_t *begin = *(uint8_t **)(g_wow + 0x2F90000);
		uint8_t *end = *(uint8_t **)(g_wow + 0x2F90008);
		if(!begin || !end || end <= begin) return 0;
		for(uint8_t *p = begin; p + 8 <= end; p += 8) {
			uint8_t *obj = *(uint8_t **)p;
			if(!obj) continue;
			if(*(unsigned long long *)(obj + 0x58) != guid) continue;
			uint8_t *desc = *(uint8_t **)(obj + 0x10);
			if(!desc) return 0;
			*reach = *(float *)(desc + 0x1A0);
			*radius = *(float *)(desc + 0x19C);
			return 1;
		}
		return 0;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int FindOwnerGuid(unsigned long long guid, unsigned long long *lo, unsigned long long *hi) {
	__try {
		uint8_t *begin = *(uint8_t **)(g_wow + 0x2F90000);
		uint8_t *end = *(uint8_t **)(g_wow + 0x2F90008);
		if(!begin || !end || end <= begin) return 0;
		for(uint8_t *p = begin; p + 8 <= end; p += 8) {
			uint8_t *obj = *(uint8_t **)p;
			if(!obj) continue;
			if(*(unsigned long long *)(obj + 0x58) != guid) continue;
			uint8_t *desc = *(uint8_t **)(obj + 0x10);
			if(!desc) return 0;
			*lo = *(unsigned long long *)(desc + 0x5C);
			*hi = *(unsigned long long *)(desc + 0x64);
			return 1;
		}
		return 0;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static int WriteFocusDesc(unsigned long long lo, unsigned long long hi) {
	__try {
		uint8_t *dst = (uint8_t *)(g_wow + 0x2D24768);
		*(unsigned long long *)dst = lo;
		*(unsigned long long *)(dst + 8) = hi;
		return 1;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// LOS: client trace wrapper 0x1A4EAA0(ctx, p1, p2, hit, float* dist, flags).
static void *LosContext() {
	__try {
		uint8_t *w = *(uint8_t **)(g_wow + 0x32539F8);
		if(!w) return nullptr;
		return *(void **)(w + 0x348);
	} __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static int LosCall(void *ctx, const float *a, const float *b, float *outDist) {
	__try {
		auto fn = (bool (__fastcall *)(void *, const void *, const void *, void *, float *, unsigned int))(g_wow + 0x1A4EAA0);
		float d = 1.0f;
		const bool r = fn(ctx, a, b, nullptr, &d, 0x100111u);
		if(outDist) *outDist = d;
		return r ? 1 : 0;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

// Loot: core 0x16FDFB0(slotIndex0based, flag).
static int LootSlotCore(int slotIndex) {
	__try {
		auto fn = (void (__fastcall *)(int, int))(g_wow + 0x16FDFB0);
		fn(slotIndex, 0);
		return 1;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return -1; }
}

static void *GetObjMgr() {
	__try {
		auto fn = (void *(__cdecl *)(void))(g_wow + 0x16B4720);
		return fn();
	} __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

static void *PlayerObject() {
	__try {
		uint8_t *begin = *(uint8_t **)(g_wow + 0x2F90000);
		uint8_t *end = *(uint8_t **)(g_wow + 0x2F90008);
		if(!begin || !end || end <= begin) return nullptr;
		for(uint8_t *p = begin; p + 8 <= end; p += 8) {
			uint8_t *obj = *(uint8_t **)p;
			if(!obj) continue;
			if(*(unsigned long long *)obj == (unsigned long long)(g_wow + 0x27C1BF8)) return obj;
		}
		return nullptr;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

// --- GUID-based targeting / focus (direct engine calls, no global hooks) ----
static bool IsGuidString(const char *s) {
	if(!s) return false;
	const char *dash = strrchr(s, '-');
	if(!dash || !dash[1]) return false;
	for(const char *p = dash + 1; *p; ++p) {
		const char c = *p;
		if(!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
	}
	return true;
}

static unsigned long long GuidLowFromString(const char *s) {
	const char *dash = strrchr(s, '-');
	if(!dash) return 0;
	return _strtoui64(dash + 1, nullptr, 16);
}

static int FindObjDescByGuid(unsigned long long low, unsigned long long *lo, unsigned long long *hi) {
	__try {
		uint8_t *begin = *(uint8_t **)(g_wow + 0x2F90000);
		uint8_t *end = *(uint8_t **)(g_wow + 0x2F90008);
		if(!begin || !end || end <= begin) return 0;
		for(uint8_t *p = begin; p + 8 <= end; p += 8) {
			uint8_t *obj = *(uint8_t **)p;
			if(!obj) continue;
			if(*(unsigned long long *)(obj + 0x58) != low) continue;
			*lo = *(unsigned long long *)(obj + 0x58);
			*hi = *(unsigned long long *)(obj + 0x60);
			return 1;
		}
		return 0;
	} __except(EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

static int ResolveTokenLow(const char *token, unsigned long long low) {
	unsigned long long lo = 0, hi = 0;
	if(!ResolveTargetDesc(token, &lo, &hi)) return 0;
	return lo == low ? 1 : 0;
}

static const char *TokenForGuidLow(unsigned long long low, char *buf, size_t bufLen) {
	static const char *fixed[] = { "player", "target", "focus", "mouseover", "pet", "targettarget", nullptr };
	for(int i = 0; fixed[i]; i++) if(ResolveTokenLow(fixed[i], low)) return fixed[i];
	for(int i = 1; i <= 4; i++) { sprintf_s(buf, bufLen, "party%d", i); if(ResolveTokenLow(buf, low)) return buf; }
	for(int i = 1; i <= 40; i++) { sprintf_s(buf, bufLen, "raid%d", i); if(ResolveTokenLow(buf, low)) return buf; }
	for(int i = 1; i <= 8; i++) { sprintf_s(buf, bufLen, "boss%d", i); if(ResolveTokenLow(buf, low)) return buf; }
	for(int i = 1; i <= 40; i++) { sprintf_s(buf, bufLen, "nameplate%d", i); if(ResolveTokenLow(buf, low)) return buf; }
	return nullptr;
}

static int CallCore2(uint32_t rva, void *ctx, unsigned long long lo, unsigned long long hi) {
	struct Desc { unsigned long long lo, hi; } d = { lo, hi };
	uint8_t *gate = (uint8_t *)(g_wow + 0x3171B80);
	const uint8_t old = *gate;
	*gate = (uint8_t)(old | 0x40);
	int ok = 0;
	__try {
		auto fn = (void (__fastcall *)(void *, void *, int))(g_wow + rva);
		fn(ctx, &d, 3);
		ok = 1;
	} __except(EXCEPTION_EXECUTE_HANDLER) { ok = 0; }
	*gate = old;
	return ok;
}

// --- protected proxy: native Lua function with the secure bit set -----------
static int __cdecl lua_ProtectedProxy(lua_State *L) {
	if(!g_wow) g_wow = (uint8_t *)GetModuleHandleW(nullptr);
	const char *unit = lua_tostring(L, 1);
	const char *op = (lua_gettop(L) >= 2) ? lua_tostring(L, 2) : (g_pendingOp[0] ? g_pendingOp : nullptr);
	Logf("ProtectedProxy unit=%s op=%s", unit ? unit : "(null)", op ? op : "(null)");
	if(!unit || !unit[0]) { lua_pushboolean(L, 0); return 1; }
	uint32_t rva = 0;
	if(!op || !_stricmp(op, "target")) rva = 0x15CABF0;
	else if(!_stricmp(op, "focus")) rva = 0x15CB380;
	else if(!_stricmp(op, "last")) rva = 0x15CB0C0;
	else if(!_stricmp(op, "assist")) rva = 0x15CB1F0;
	else if(!_stricmp(op, "cast")) rva = 0x163C880;
	if(!rva) { lua_pushboolean(L, 0); return 1; }
	uint8_t *gate = (uint8_t *)(g_wow + 0x3171B80);
	const uint8_t oldFlag = *gate;
	if(g_armRequested) *gate = (uint8_t)(oldFlag | 0x40);
	Logf("ProtectedProxy gate=%p old=0x%02X new=0x%02X armed=%d", gate, (int)oldFlag, (int)*gate, (int)g_armRequested);
	auto fn = (LuaCFn)(g_wow + rva);
	const int n = fn(L);
	if(g_armRequested) *gate = oldFlag;
	Logf("ProtectedProxy rva=0x%X returned %d", rva, n);
	return n;
}

// --- gate -------------------------------------------------------------------
static int __cdecl Gate(lua_State *L) {
	if(!g_wow) g_wow = (uint8_t *)GetModuleHandleW(nullptr);
	const char *cmd = lua_tostring(L, 1);
	if(!cmd) return 0;
	const char *payload = (lua_gettop(L) >= 2 && lua_isstring(L, 2)) ? lua_tostring(L, 2) : "";

	if(!_stricmp(cmd, "GetVersion") || !_stricmp(cmd, "ver")) {
		lua_pushinteger(L, 0);
		lua_pushinteger(L, 1);
		return 2;
	}
	if(!_stricmp(cmd, "Ping")) { lua_pushboolean(L, 1); return 1; }
	if(!_stricmp(cmd, "Caps")) { lua_pushinteger(L, 0xF); return 1; }
	if(!_stricmp(cmd, "Pump") || !_stricmp(cmd, "Reset")) return 0;

	if(!_stricmp(cmd, "ArmNextTarget")) {
		g_armRequested = true;
		if(payload[0]) strncpy_s(g_pendingOp, sizeof(g_pendingOp), payload, _TRUNCATE);
		else g_pendingOp[0] = 0;
		lua_pushboolean(L, 1);
		return 1;
	}

	if(!_stricmp(cmd, "SetTargetCore") || !_stricmp(cmd, "SetTargetToken")) {
		unsigned long long a = 0, b = 0;
		const int resolved = payload[0] ? ResolveTargetDesc(payload, &a, &b) : 0;
		const int ok = resolved ? CallSetTargetCore(a, b, 3) : 0;
		Logf("SetTargetCore %s resolved=%d desc=%llx,%llx ok=%d", payload, resolved, a, b, ok);
		lua_pushboolean(L, ok);
		return 1;
	}

	if(!_stricmp(cmd, "SetFocusToken")) {
		unsigned long long lo = 0, hi = 0;
		const int resolved = payload[0] ? ResolveTargetDesc(payload, &lo, &hi) : 0;
		int ok = 0;
		if(resolved) ok = WriteFocusDesc(lo, hi);
		Logf("SetFocusToken %s desc=%llx,%llx ok=%d", payload, lo, hi, ok);
		lua_pushboolean(L, ok == 1);
		return 1;
	}
	if(!_stricmp(cmd, "ClearFocus")) {
		const int ok = WriteFocusDesc(0, 0);
		Logf("ClearFocus ok=%d", ok);
		lua_pushboolean(L, ok == 1);
		return 1;
	}

	if(!_stricmp(cmd, "ObjCount")) { lua_pushinteger(L, ObjCountRaw()); return 1; }
	if(!_stricmp(cmd, "ObjDbg")) {
		char buf[256];
		sprintf_s(buf, "cnt=%d begin=%p end=%p", ObjCountRaw(),
			*(void **)(g_wow + 0x2F90000), *(void **)(g_wow + 0x2F90008));
		Logf("ObjDbg %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}
	if(!_stricmp(cmd, "ObjMgr")) {
		char buf[160];
		sprintf_s(buf, "vec=%p end=%p cnt=%d eng=%p",
			*(void **)(g_wow + 0x2F90000), *(void **)(g_wow + 0x2F90008), ObjCountRaw(), GetObjMgr());
		Logf("ObjMgr %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}

	if(!_stricmp(cmd, "UnitPos")) {
		Logf("UnitPos payload=[%s] cnt=%d", payload, ObjCountRaw());
		float v[4] = { 0, 0, 0, 0 };
		const int r = FindObjPos(_strtoui64(payload, nullptr, 16), v);
		char buf[160];
		sprintf_s(buf, "r=%d %.4f %.4f %.4f %.4f", r, v[0], v[1], v[2], v[3]);
		Logf("UnitPos %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}
	if(!_stricmp(cmd, "UnitDist")) {
		char a[32] = { 0 }, b[32] = { 0 };
		sscanf_s(payload, "%31[^,],%31s", a, (unsigned)sizeof(a), b, (unsigned)sizeof(b));
		float pa[4] = { 0 }, pb[4] = { 0 };
		const int ra = FindObjPos(_strtoui64(a, nullptr, 16), pa);
		const int rb = FindObjPos(_strtoui64(b, nullptr, 16), pb);
		if(ra != 1 || rb != 1) { lua_pushnumber(L, -1); return 1; }
		const double dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
		const double d = sqrt(dx * dx + dy * dy + dz * dz);
		float reachA = 0, reachB = 0, radA = 0, radB = 0;
		FindObjReach(_strtoui64(a, nullptr, 16), &reachA, &radA);
		FindObjReach(_strtoui64(b, nullptr, 16), &reachB, &radB);
		const double ranged = d - (reachA > 0 ? reachA : 0) - (reachB > 0 ? reachB : 0);
		char buf[128];
		sprintf_s(buf, "dist=%.3f raw=%.3f reach=%.2f+%.2f", ranged > 0 ? ranged : 0, d, reachA, reachB);
		Logf("UnitDist %s", buf);
		lua_pushnumber(L, ranged > 0 ? ranged : 0);
		return 1;
	}
	if(!_stricmp(cmd, "UnitReach")) {
		float r = 0, b = 0;
		const int ok = FindObjReach(_strtoui64(payload, nullptr, 16), &r, &b);
		char buf[128];
		sprintf_s(buf, "ok=%d reach=%.3f radius=%.3f", ok, r, b);
		Logf("UnitReach %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}
	if(!_stricmp(cmd, "UnitOwner")) {
		unsigned long long lo = 0, hi = 0;
		const int ok = FindOwnerGuid(_strtoui64(payload, nullptr, 16), &lo, &hi);
		char buf[128];
		sprintf_s(buf, "ok=%d lo=%llx hi=%llx", ok, lo, hi);
		Logf("UnitOwner %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}

	if(!_stricmp(cmd, "Los")) {
		char buf[256];
		void *ctx = LosContext();
		float pos[4] = { 0, 0, 0, 0 };
		uint8_t *me = (uint8_t *)PlayerObject();
		const int found = me ? FindObjPos(*(unsigned long long *)(me + 0x58), pos) : 0;
		if(!ctx || found != 1) {
			sprintf_s(buf, "ctx=%p found=%d", ctx, found);
			lua_pushstring(L, buf);
			return 1;
		}
		float a[3] = { pos[0], pos[1], pos[2] + 1.5f };
		float open[3] = { a[0] + (float)cos(pos[3]) * 5.0f, a[1] + (float)sin(pos[3]) * 5.0f, a[2] };
		float ground[3] = { pos[0], pos[1], pos[2] - 50.0f };
		float d1 = 0, d2 = 0;
		const int rOpen = LosCall(ctx, a, open, &d1);
		const int rGround = LosCall(ctx, a, ground, &d2);
		sprintf_s(buf, "ctx=%p open=%d(%.2f) ground=%d(%.2f)", ctx, rOpen, d1, rGround, d2);
		Logf("Los %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}
	if(!_stricmp(cmd, "LosUnits")) {
		char a[32] = { 0 }, b[32] = { 0 };
		sscanf_s(payload, "%31[^,],%31s", a, (unsigned)sizeof(a), b, (unsigned)sizeof(b));
		float pa[4] = { 0 }, pb[4] = { 0 };
		const int ra = FindObjPos(_strtoui64(a, nullptr, 16), pa);
		const int rb = FindObjPos(_strtoui64(b, nullptr, 16), pb);
		char buf[200];
		if(ra != 1 || rb != 1) {
			sprintf_s(buf, "err=%d,%d", ra, rb);
			lua_pushstring(L, buf);
			return 1;
		}
		void *ctx = LosContext();
		if(!ctx) { lua_pushstring(L, "err=noctx"); return 1; }
		float p1[3] = { pa[0], pa[1], pa[2] + 1.5f };
		float p2[3] = { pb[0], pb[1], pb[2] + 1.5f };
		float d = 0;
		const int r = LosCall(ctx, p1, p2, &d);
		const double dx = pa[0] - pb[0], dy = pa[1] - pb[1], dz = pa[2] - pb[2];
		const double dist = sqrt(dx * dx + dy * dy + dz * dz);
		const int insight = (r == 0) ? 1 : ((r == 1 && d > 1.0f) ? 1 : 0);
		sprintf_s(buf, "insight=%d hit=%d d=%.3f dist=%.2f", insight, r, d, dist);
		Logf("LosUnits %s", buf);
		lua_pushstring(L, buf);
		return 1;
	}

	if(!_stricmp(cmd, "TargetByGuid")) {
		const unsigned long long low = GuidLowFromString(payload);
		char tb[32];
		const char *tok = TokenForGuidLow(low, tb, sizeof(tb));
		unsigned long long lo = 0, hi = 0;
		int ok = 0;
		if(tok && ResolveTargetDesc(tok, &lo, &hi)) ok = CallCore2(0x9C6360, (void *)(g_wow + 0x2D24720), lo, hi);
		else if(FindObjDescByGuid(low, &lo, &hi)) ok = CallCore2(0x9C6360, (void *)(g_wow + 0x2D24720), lo, hi);
		Logf("TargetByGuid %s low=%llx tok=%s desc=%llx,%llx ok=%d", payload, low, tok ? tok : "-", lo, hi, ok);
		lua_pushboolean(L, ok);
		return 1;
	}
	if(!_stricmp(cmd, "FocusByGuid")) {
		const unsigned long long low = GuidLowFromString(payload);
		char tb[32];
		const char *tok = TokenForGuidLow(low, tb, sizeof(tb));
		unsigned long long lo = 0, hi = 0;
		int ok = 0;
		if(tok && ResolveTargetDesc(tok, &lo, &hi)) ok = WriteFocusDesc(lo, hi);
		else if(FindObjDescByGuid(low, &lo, &hi)) ok = WriteFocusDesc(lo, hi);
		Logf("FocusByGuid %s low=%llx tok=%s desc=%llx,%llx ok=%d", payload, low, tok ? tok : "-", lo, hi, ok);
		lua_pushboolean(L, ok == 1);
		return 1;
	}
	if(!_stricmp(cmd, "FocusDesc")) {
		char buf[96];
		sprintf_s(buf, "lo=%llx hi=%llx",
			*(unsigned long long *)(g_wow + 0x2D24768),
			*(unsigned long long *)(g_wow + 0x2D24770));
		lua_pushstring(L, buf);
		return 1;
	}
	if(!_stricmp(cmd, "Loot")) {
		const int slot = payload[0] ? atoi(payload) : 1;
		uint8_t *gate = (uint8_t *)(g_wow + 0x3171B80);
		const uint8_t oldFlag = *gate;
		*gate = (uint8_t)(oldFlag | 0x40);
		const int r = LootSlotCore(slot - 1);
		*gate = oldFlag;
		Logf("Loot slot=%d r=%d", slot, r);
		lua_pushinteger(L, r);
		return 1;
	}


	if(!_stricmp(cmd, "GetLocalPlayer")) {
		void *p = PlayerObject();
		char buf[64];
		sprintf_s(buf, "%p", p);
		lua_pushstring(L, buf);
		return 1;
	}

	if(!_stricmp(cmd, "ImportFile")) {
		std::string data, err;
		if(!ReadText(payload, data, err)) return PushErr(L, err);
		lua_pushstring(L, data.c_str());
		return 1;
	}
	if(!_stricmp(cmd, "ExportFile")) {
		auto parts = Split(payload);
		if(parts.size() < 2) return PushErr(L, "Missing path or content");
		std::string err;
		if(!WriteText(parts[0], parts[1], err)) return PushErr(L, err);
		lua_pushboolean(L, 1);
		return 1;
	}
	if(!_stricmp(cmd, "CombatLogAdd")) { lua_pushboolean(L, 1); return 1; }
	if(!_stricmp(cmd, "SetAutoloot")) { Logf("SetAutoloot %s", payload); lua_pushboolean(L, 1); return 1; }
	if(!_stricmp(cmd, "TrackUnit") || !_stricmp(cmd, "UntrackUnit")) { lua_pushboolean(L, 1); return 1; }

	return 0;
}

static void EarlyProbe() {}

static DWORD WINAPI BootThread(LPVOID) {
	bool gateClaimed = false, proxyClaimed = false;
	for(int i = 0; i < 1200; i++) {
		HMODULE sp = GetModuleHandleA("SlotPrep.dll");
		if(sp) {
			auto claim = (int(__cdecl *)(int, void *))GetProcAddress(sp, "RavenEra_ClaimSlot");
			if(claim) {
				if(!gateClaimed && claim(1, (void *)&Gate)) { gateClaimed = true; Log("ClaimSlot[1]=SuperWoW gate OK"); }
				if(!proxyClaimed && claim(2, (void *)&lua_ProtectedProxy)) { proxyClaimed = true; Log("ClaimSlot[2]=ProtectedProxy OK"); }
				if(gateClaimed && proxyClaimed) return 0;
			}
		}
		Sleep(100);
	}
	Log("ClaimSlot timeout");
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
	if(reason == DLL_PROCESS_ATTACH) {
		g_wow = (uint8_t *)GetModuleHandleW(nullptr);
		EarlyProbe();
		Log("SuperWoW attach");
		CreateThread(nullptr, 0, BootThread, nullptr, 0, nullptr);
	}
	return TRUE;
}
