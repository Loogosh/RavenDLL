// Early (via version.dll): reserve FrameScript stub slots.
// Widen walker end via HWBP+VEH after lea rdi (no .text write — ACG/VP 87).
// Late inject: RavenEra_ClaimSlot(i, fn).
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <direct.h>
#include <stdarg.h>

#pragma comment(lib, "psapi.lib")

static constexpr uintptr_t RVA_TABLE_SMALL = 0x2BC03B0;
static constexpr uintptr_t RVA_TABLE_BIG = 0x2BB3E00;
static constexpr uintptr_t RVA_NAME_CAVE = 0x2BC04B9;
static constexpr uintptr_t RVA_END_LEA0 = 0x19E2261;
static constexpr uintptr_t RVA_OLD_END = 0x2BC0430;
static constexpr uintptr_t RVA_CTX = 0x2D20B20;
static constexpr uintptr_t RVA_REGISTER = 0x95ECC0;
static constexpr uintptr_t END_LEAS[] = { 0x19E2261, 0x19E2501, 0x19E2CB1, 0x19E2D41 };
static constexpr int EXTRA_SLOTS = 2;
static constexpr uintptr_t RVA_NEW_END = RVA_OLD_END + 16 * EXTRA_SLOTS;
static constexpr int kMaxSlots = 3;

// Steal names chosen after checking the 1.12/Turtle addon corpus: none of these is
// called by addons (0 hits in 1.18 and 1.14 AddOns). IsUsingFixedTimeStep/ForceQuit
// are left untouched (ForceQuit is used by pfQuest, IsUsingFixedTimeStep by UI).
static constexpr int IDX_TOURNAMENT = 208;     // IsOnTournamentRealm  -> RavenEra HearthDB gate
static constexpr int IDX_TAXI_SET = 232;       // SetTaxiBenchmarkMode -> RavenEra SuperWoW gate
static constexpr int IDX_TAXI_GET = 233;       // GetTaxiBenchmarkMode -> RavenEra Ping / spare
// HWBP/DR → SINGLE_STEP #132 — never arm on live packed client.
// FILE_EXTEND: run WowClassic_endlea32 (Start-Client.ps1 -EndLea); arm [8]/[9] when end-LEA already wide.
static constexpr bool kUseHwbpWalker = false;
static constexpr bool kArmExtendedSlots = true;

struct Prep {
	uintptr_t tableRva;
	int index;
	uintptr_t gadgetRva;
	uintptr_t slotRva;
	const char *name;
	bool keepName;   // online: keep the stock name (survives /reload), swap only fn
};

static uint8_t *g_base = nullptr;
static char *g_cave = nullptr;
static size_t g_caveUsed = 0;
static volatile LONG g_bootOnce = 0;
static volatile LONG g_ready = 0;
static volatile LONG g_stop = 0;
static volatile LONG g_hwbpHits = 0;
static volatile LONG g_hwbpArmed = 0;
static PVOID g_veh = nullptr;

static void **g_slots[kMaxSlots]{};
static void *g_gadgets[kMaxSlots]{};
static void *g_fns[kMaxSlots]{};
static char *g_names[kMaxSlots]{};
static int g_indices[kMaxSlots]{};
static uintptr_t g_tableRvas[kMaxSlots]{};
static bool g_claimed[kMaxSlots]{};
static bool g_keepName[kMaxSlots]{};
static int g_n = 0;

static void Log(const char *fmt, ...) {
	_mkdir("Logs");
	FILE *f = fopen("Logs\\slotprep.log", "a");
	if(!f) return;
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(f, "[%02u:%02u:%02u] ", st.wHour, st.wMinute, st.wSecond);
	va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
	fputc('\n', f); fclose(f);
}

static int lua_Stub(void *) { return 0; }

static uintptr_t ReadEndLeaTarget() {
	uint8_t *p = g_base + RVA_END_LEA0;
	if(p[0] != 0x48 || p[1] != 0x8D || p[2] != 0x3D) return 0;
	int32_t disp = *(int32_t *)(p + 3);
	return (uintptr_t)((int64_t)(RVA_END_LEA0 + 7) + disp);
}

static bool IsAfterEndLea(DWORD64 rip) {
	if(!g_base) return false;
	for(uintptr_t lea : END_LEAS) {
		if(rip == (DWORD64)(g_base + lea + 7)) return true;
	}
	return false;
}

static LONG CALLBACK VehHandler(EXCEPTION_POINTERS *ep) {
	if(!ep || !ep->ExceptionRecord || !ep->ContextRecord) return EXCEPTION_CONTINUE_SEARCH;
	if(ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP)
		return EXCEPTION_CONTINUE_SEARCH;
	if(!IsAfterEndLea(ep->ContextRecord->Rip))
		return EXCEPTION_CONTINUE_SEARCH;
	ep->ContextRecord->Rdi = (DWORD64)(g_base + RVA_NEW_END);
	ep->ContextRecord->Dr6 = 0;
	InterlockedIncrement(&g_hwbpHits);
	return EXCEPTION_CONTINUE_EXECUTION;
}

static bool SetThreadHwbp(HANDLE th) {
	CONTEXT c{};
	c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
	if(!GetThreadContext(th, &c)) return false;
	c.Dr0 = (DWORD64)(g_base + END_LEAS[0] + 7);
	c.Dr1 = (DWORD64)(g_base + END_LEAS[1] + 7);
	c.Dr2 = (DWORD64)(g_base + END_LEAS[2] + 7);
	c.Dr3 = (DWORD64)(g_base + END_LEAS[3] + 7);
	// Local enable DR0..DR3; R/W=00 exec, LEN=00
	c.Dr7 = 0x55;
	c.Dr6 = 0;
	return SetThreadContext(th, &c) != 0;
}

static int ApplyHwbpAllThreads() {
	if(!g_base) return 0;
	DWORD pid = GetCurrentProcessId();
	DWORD self = GetCurrentThreadId();
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
	if(snap == INVALID_HANDLE_VALUE) return 0;
	THREADENTRY32 te{};
	te.dwSize = sizeof(te);
	int ok = 0;
	if(Thread32First(snap, &te)) {
		do {
			if(te.th32OwnerProcessID != pid) continue;
			HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_SET_CONTEXT,
				FALSE, te.th32ThreadID);
			if(!th) continue;
			if(te.th32ThreadID != self) {
				SuspendThread(th);
				if(SetThreadHwbp(th)) ok++;
				ResumeThread(th);
			} else {
				if(SetThreadHwbp(th)) ok++;
			}
			CloseHandle(th);
		} while(Thread32Next(snap, &te));
	}
	CloseHandle(snap);
	return ok;
}

static bool InstallWalkerEndHook() {
	if(!g_veh) {
		g_veh = AddVectoredExceptionHandler(1, VehHandler);
		if(!g_veh) { Log("FAIL AddVEH"); return false; }
	}
	int n = ApplyHwbpAllThreads();
	InterlockedExchange(&g_hwbpArmed, 1);
	Log("HWBP after-lea armed threads=%d new_end=%X", n, (unsigned)RVA_NEW_END);
	return n > 0;
}

static bool RegisterLooksReady() {
	uint8_t *p = g_base + RVA_REGISTER;
	return p[0] == 0x48 && p[1] == 0x89 && p[2] == 0x5C;
}

static bool TableReady() {
	void **t = (void **)(g_base + RVA_TABLE_SMALL);
	const char *n = (const char *)t[0];
	if(!n || n < (const char *)g_base || n > (const char *)g_base + 0x4000000) return false;
	return n[0] == 'G';
}

static const char *EntryName(uintptr_t tableRva, int idx) {
	void **t = (void **)(g_base + tableRva);
	const char *n = (const char *)t[idx * 2];
	if(!n || n < (const char *)g_base || n > (const char *)g_base + 0x4000000) return nullptr;
	return n;
}

static bool EntryNameIs(uintptr_t tableRva, int idx, const char *expect) {
	const char *n = EntryName(tableRva, idx);
	if(!n) return false;
	for(int i = 0; i < 64; i++) {
		if(n[i] != expect[i]) return false;
		if(n[i] == 0) return true;
	}
	return false;
}

static char *CaveAlloc(const char *s) {
	size_t n = strlen(s) + 1;
	if(g_caveUsed + n > 120) return nullptr;
	char *p = g_cave + g_caveUsed;
	memcpy(p, s, n);
	g_caveUsed += n;
	return p;
}

static bool ArmOne(const Prep &p, int idx) {
	void **t = (void **)(g_base + p.tableRva);
	void **entry = &t[p.index * 2];
	static const uintptr_t kAltSlots[] = {
		0x2CA8DA8, 0x2CADB20, 0x2CADB28, 0x2CADB30, 0x2CADB38,
		0x2CAEB18, 0x2CAEB20, 0x2CAEB28, 0x2CAEB30
	};
	static const uintptr_t kAltGadgets[] = {
		0x2B8571, 0x325C71, 0x325C81, 0x325C91, 0x325CA1,
		0x342EF1, 0x342DA1, 0x342D91, 0x342DB1
	};
	void **slot = nullptr;
	void *gadget = nullptr;
	uintptr_t sr = 0, gr = 0;
	for(int ai = 0; ai < 9; ai++) {
		sr = (ai == 0) ? p.slotRva : kAltSlots[ai];
		gr = (ai == 0) ? p.gadgetRva : kAltGadgets[ai];
		void **cand = (void **)(g_base + sr);
		bool taken = false;
		for(int j = 0; j < idx; j++) if(g_slots[j] == cand) { taken = true; break; }
		if(taken || *cand != nullptr) continue;
		slot = cand;
		gadget = g_base + gr;
		break;
	}
	if(!slot) { Log("FAIL %s no free gadget slot", p.name); return false; }
	char *nameImg = CaveAlloc(p.name);
	if(!nameImg) { Log("FAIL cave %s", p.name); return false; }

	const char *oldName = (const char *)entry[0];
	g_fns[idx] = (void *)lua_Stub;
	g_slots[idx] = slot;
	g_gadgets[idx] = gadget;
	g_names[idx] = nameImg;
	g_indices[idx] = p.index;
	g_tableRvas[idx] = p.tableRva;
	g_keepName[idx] = p.keepName;
	*slot = g_fns[idx];
	if(!p.keepName) entry[0] = nameImg;
	entry[1] = gadget;
	Log("prep %s keepName=%d was=%s table=%X[%d] slot=%X gadget=%X", p.name, (int)p.keepName,
		(oldName && oldName > (const char *)g_base && oldName < (const char *)g_base + 0x4000000) ? oldName : "?",
		(unsigned)p.tableRva, p.index, (unsigned)sr, (unsigned)gr);
	return true;
}

static void Repoke() {
	// Each slot lives in its own table: table 25 entries must be re-poked in table 25,
	// not in the small table (the old code wrote 208*16 bytes past 0x2BC03B0).
	for(int k = 0; k < g_n; k++) {
		if(!g_slots[k] || !g_gadgets[k] || !g_names[k] || !g_tableRvas[k]) continue;
		*g_slots[k] = g_fns[k];
		void **t = (void **)(g_base + g_tableRvas[k]);
		void **entry = &t[g_indices[k] * 2];
		if(!g_keepName[k]) entry[0] = g_names[k];
		entry[1] = g_gadgets[k];
	}
}

static DWORD WINAPI Refresh(LPVOID) {
	while(!g_stop) {
		for(int i = 0; i < g_n; i++) {
			if(g_slots[i] && g_fns[i]) *g_slots[i] = g_fns[i];
		}
		if(kUseHwbpWalker && g_hwbpArmed && !g_ready)
			ApplyHwbpAllThreads();
		Sleep(5);
	}
	return 0;
}

static DWORD WINAPI Boot(LPVOID) {
	if(InterlockedCompareExchange(&g_bootOnce, 1, 0) != 0) return 0;
	_mkdir("Logs");
	Log("SlotPrep boot HWBP=%d ARM=%d EXTRA=%d",
		(int)kUseHwbpWalker, (int)kArmExtendedSlots, EXTRA_SLOTS);

	if(!kUseHwbpWalker && !kArmExtendedSlots) {
		Log("idle — no DR / no table extend (avoid ERROR#132 SINGLE_STEP)");
		return 0;
	}

	MODULEINFO mi{};
	GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(NULL), &mi, sizeof(mi));
	g_base = (uint8_t *)mi.lpBaseOfDll;
	g_cave = (char *)(g_base + RVA_NAME_CAVE);
	g_caveUsed = 0;

	// Patch the exact entries we steal as soon as their names are readable. The walker
	// scans a flat block that starts before RVA_TABLE_BIG, so waiting for the small
	// table (RVA_TABLE_SMALL) can be too late for table 25 entries.
	int readyMs = -1;
	for(int i = 0; i < 20000; i++) {
		if(EntryNameIs(RVA_TABLE_BIG, IDX_TOURNAMENT, "IsOnTournamentRealm") &&
		   EntryNameIs(RVA_TABLE_BIG, IDX_TAXI_SET, "SetTaxiBenchmarkMode") &&
		   EntryNameIs(RVA_TABLE_BIG, IDX_TAXI_GET, "GetTaxiBenchmarkMode")) {
			readyMs = i;
			break;
		}
		Sleep(1);
	}
	if(readyMs >= 0) {
		Log("entry names ready %d ms (RegisterLooks=%d)", readyMs, (int)RegisterLooksReady());
	} else {
		Log("entry names not matched in 20s — fallback to TableReady");
		for(int i = 0; i < 100000; i++) {
			if(TableReady()) { Log("table ready %d ms", i); break; }
			Sleep(1);
		}
		if(!TableReady() && !EntryName(RVA_TABLE_BIG, IDX_TOURNAMENT)) { Log("FAIL table"); return 0; }
	}

	if(kUseHwbpWalker) {
		if(!InstallWalkerEndHook()) {
			Log("FAIL HWBP — packer may clear DR / OpenThread denied");
			return 0;
		}
	}

	// Peek end-LEA once — do NOT spin 20s here. Waiting lets walker Register
	// stock names; later steal only renames the table, Lua keeps old C fn.
	uintptr_t end = ReadEndLeaTarget();
	Log("end-LEA target=%X stock=%X spare=%u", (unsigned)end, (unsigned)RVA_OLD_END,
		end > RVA_OLD_END ? (unsigned)((end - RVA_OLD_END) / 16) : 0u);

	Prep plan[kMaxSlots];
	int planN = 0;
	if(end >= RVA_OLD_END + 32) {
		plan[planN++] = { RVA_TABLE_SMALL, 8, 0x325C71, 0x2CADB20, "RavenEra_S0", false };
		plan[planN++] = { RVA_TABLE_SMALL, 9, 0x325C81, 0x2CADB28, "RavenEra_S1", false };
		Log("plan=FILE_EXTEND S0/S1 at [8]/[9]");
	} else {
		// Online: three rare names in table 25, all with 0 references in the 1.12/1.14
		// addon corpora. IsUsingFixedTimeStep (UI) and ForceQuit (pfQuest) stay stock.
		// keepName=true: stock name survives /reload, only the fn pointer is swapped.
		plan[planN++] = { RVA_TABLE_BIG, IDX_TOURNAMENT, 0x325C71, 0x2CADB20, "IsOnTournamentRealm", true };
		plan[planN++] = { RVA_TABLE_BIG, IDX_TAXI_SET, 0x325C81, 0x2CADB28, "SetTaxiBenchmarkMode", true };
		plan[planN++] = { RVA_TABLE_BIG, IDX_TAXI_GET, 0x325C91, 0x2CADB30, "GetTaxiBenchmarkMode", true };
		Log("plan=STEAL IsOnTournamentRealm/SetTaxiBenchmarkMode/GetTaxiBenchmarkMode (keep names)");
	}

	g_n = 0;
	for(int i = 0; i < planN; i++) {
		if(ArmOne(plan[i], g_n)) g_n++;
	}
	if(!g_n) { Log("FAIL no slots"); return 0; }

	HANDLE th = CreateThread(nullptr, 0, Refresh, nullptr, 0, nullptr);
	if(th) CloseHandle(th);

	for(int i = 0; i < 900; i++) {
		Repoke();
		// Never arm DR/HWBP on stock packed client — causes ERROR #132 SINGLE_STEP.
		if(kUseHwbpWalker && i && (i % 50) == 0)
			ApplyHwbpAllThreads();
		if(*(void **)(g_base + RVA_CTX)) break;
		Sleep(10);
	}
	InterlockedExchange(&g_ready, 1);
	Log("READY slots=%d hwbp_hits=%ld (ClaimSlot later)", g_n, (long)g_hwbpHits);
	return 0;
}

extern "C" {

__declspec(dllexport) int RavenEra_SlotCount(void) {
	return g_ready ? g_n : 0;
}

__declspec(dllexport) int RavenEra_ClaimSlot(int i, void *fn) {
	if(!g_ready || i < 0 || i >= g_n || !fn || !g_slots[i]) return 0;
	if(g_claimed[i]) return 0;   // one owner per slot: HearthDB=0, SuperWoW=1, Ping=2
	g_claimed[i] = true;
	g_fns[i] = fn;
	*g_slots[i] = fn;
	Log("ClaimSlot[%d] -> %p", i, fn);
	return 1;
}

__declspec(dllexport) DWORD __cdecl Load(void) {
	HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
	if(t) CloseHandle(t);
	return 0;
}

} // extern C

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
	if(reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(h);
		HANDLE t = CreateThread(nullptr, 0, Boot, nullptr, 0, nullptr);
		if(t) CloseHandle(t);
	} else if(reason == DLL_PROCESS_DETACH) {
		InterlockedExchange(&g_stop, 1);
		if(g_veh) {
			RemoveVectoredExceptionHandler(g_veh);
			g_veh = nullptr;
		}
	}
	return TRUE;
}
