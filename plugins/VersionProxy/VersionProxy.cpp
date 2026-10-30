// Proxy VERSION.dll — forwards to System32\\version.dll, loads SlotPrep.dll (stubs only).
#include <windows.h>
#include <stdio.h>

static HMODULE g_real = nullptr;

static void Log(const char *msg) {
	CreateDirectoryA("Logs", nullptr);
	FILE *f = fopen("Logs\\version_proxy.log", "a");
	if(!f) return;
	SYSTEMTIME st; GetLocalTime(&st);
	fprintf(f, "[%02u:%02u:%02u] %s\n", st.wHour, st.wMinute, st.wSecond, msg);
	fclose(f);
}

// SlotPrep: FILE_EXTEND on bootable dump / STEAL on stock.
static constexpr bool kLoadSlotPrep = true;

// Packer: div0 then SMC writes (needs writable code). Soften both, but NEVER strip
// EXECUTE from a code page: repairing a code AV with PAGE_READWRITE de-executes the
// region, and the next instruction fetch becomes an endless AV storm (HearthDB hang,
// 2026-10-29: rip/addr in HearthDB .text, "prot 4" repeated → client frozen).
static ULONG_PTR g_vehRip = 0;
static int g_vehHits = 0;

static volatile LONG g_avSeq = 0;
static volatile LONG g_sameCount = 0;
static ULONG_PTR g_lastRip = 0, g_lastAddr = 0, g_lastWrite = 0;
static volatile LONG g_execFix = 0, g_execFail = 0;

static bool IsExecProt(DWORD p) {
	p &= 0xFF;
	return p == PAGE_EXECUTE || p == PAGE_EXECUTE_READ ||
	       p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY;
}

static void DescribeAddr(ULONG_PTR a, char *out, size_t n) {
	MEMORY_BASIC_INFORMATION m{};
	if(VirtualQuery((LPCVOID)a, &m, sizeof(m)) != sizeof(m)) { sprintf_s(out, n, "unmapped"); return; }
	if(m.Type == MEM_IMAGE && m.AllocationBase) {
		char path[MAX_PATH] = {};
		GetModuleFileNameA((HMODULE)m.AllocationBase, path, MAX_PATH);
		const char *bn = strrchr(path, '\\');
		bn = bn ? bn + 1 : path;
		sprintf_s(out, n, "%s+%llX", bn, (unsigned long long)(a - (ULONG_PTR)m.AllocationBase));
		return;
	}
	sprintf_s(out, n, "%s", m.Type == MEM_MAPPED ? "mapped" : (m.Type == MEM_PRIVATE ? "private" : "other"));
}

static void HexBytes(const void *p, char *out, size_t n) {
	out[0] = 0;
	MEMORY_BASIC_INFORMATION m{};
	if(VirtualQuery(p, &m, sizeof(m)) != sizeof(m) || m.State != MEM_COMMIT ||
	   (m.Protect & 0xFF) == PAGE_NOACCESS || (m.Protect & PAGE_GUARD)) return;
	const BYTE *b = (const BYTE *)p;
	size_t k = 0;
	for(int i = 0; i < 12; i++)
		k += sprintf_s(out + k, n - k, "%02X ", b[i]);
}

static LONG CALLBACK PackerVeh(EXCEPTION_POINTERS *ep) {
	const DWORD code = ep->ExceptionRecord->ExceptionCode;
	CONTEXT *c = ep->ContextRecord;
	char buf[256];
	// #DE only came from calling the packer-virtualized lua_createtable out of this DLL:
	// the VM divides using stack slots our foreign frame does not provide. "Fix" RCX=1
	// corrupted the division, made the VM write the return-address byte over our .text
	// and then spin on the same div (client hang, 2026-10-29). HearthDB no longer calls
	// lua_createtable (results are packed strings), so a #DE here means a real VM fault:
	// pass it to the app — it raises ERROR #132 instead of silently corrupting code.
	static constexpr bool kHandleDiv0 = false;
	if(code == EXCEPTION_INT_DIVIDE_BY_ZERO) {
		static volatile LONG s_div0 = 0;
		const LONG n0 = InterlockedIncrement(&s_div0);
		if(n0 <= 24) {
			sprintf_s(buf, "VEH div0 #%ld rip=%p — %s", (long)n0, (void *)c->Rip,
				kHandleDiv0 ? "rcx=1" : "pass to app");
			Log(buf);
		}
		if(!kHandleDiv0) return EXCEPTION_CONTINUE_SEARCH;
		if(c->Rip == g_vehRip)
			++g_vehHits;
		else {
			g_vehRip = c->Rip;
			g_vehHits = 1;
		}
		if(g_vehHits <= 2) {
			sprintf_s(buf, "VEH div0 rip=%p hit=%d — rcx=1", (void *)c->Rip, g_vehHits);
			Log(buf);
			c->Rcx = 1;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		// Spin escape: unwind to a real call-return in .text (skip packer SMC zone).
		ULONG_PTR *sp = (ULONG_PTR *)c->Rsp;
		for(int i = 0; i < 64; i++) {
			const ULONG_PTR cand = sp[i];
			if(cand < 0x140001000ull || cand >= 0x1425D0000ull)
				continue;
			if(cand >= 0x140950000ull && cand < 0x140A00000ull)
				continue; // packer SMC scratch
			const BYTE *p = (const BYTE *)cand;
			if(p[-5] != 0xE8 && !(p[-2] == 0xFF && p[-1] == 0xD0)) // call rel32 / call rax
				continue;
			sprintf_s(buf, "VEH div0 unwind rip=%p → %p slot=%d",
				(void *)c->Rip, (void *)cand, i);
			Log(buf);
			c->Rip = cand;
			c->Rsp = (ULONG_PTR)&sp[i + 1];
			g_vehHits = 0;
			return EXCEPTION_CONTINUE_EXECUTION;
		}
		c->Rcx = 1;
		return EXCEPTION_CONTINUE_EXECUTION;
	}
	if(code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
		const ULONG_PTR access = ep->ExceptionRecord->ExceptionInformation[0]; // 0=read 1=write 8=exec
		const ULONG_PTR addr = ep->ExceptionRecord->ExceptionInformation[1];
		const LONG seq = InterlockedIncrement(&g_avSeq);

		// A repair that repeats the very same fault means the "fix" did not help (or made
		// it worse). Never spin forever: pass the exception to the app after 64 tries.
		if(c->Rip == g_lastRip && addr == g_lastAddr && access == g_lastWrite) {
			const LONG n = InterlockedIncrement(&g_sameCount);
			if(n > 64) {
				if(n == 65) Log("VEH storm: identical AV x65 — stop repairing, pass to app");
				return EXCEPTION_CONTINUE_SEARCH;
			}
		} else {
			g_lastRip = c->Rip; g_lastAddr = addr; g_lastWrite = access;
			InterlockedExchange(&g_sameCount, 1);
		}

		char at[128], ip[128];
		DescribeAddr(addr, at, sizeof(at));
		DescribeAddr(c->Rip, ip, sizeof(ip));
		MEMORY_BASIC_INFORMATION am{};
		VirtualQuery((LPCVOID)addr, &am, sizeof(am));

		if(seq <= 48) {
			sprintf_s(buf, "VEH AV #%ld rip=%p[%s] acc=%llu addr=%p[%s] prot=%lx state=%lx rsz=%zx tid=%lu",
				(long)seq, (void *)c->Rip, ip, (unsigned long long)access, (void *)addr, at,
				(unsigned long)am.Protect, (unsigned long)am.State, (size_t)am.RegionSize,
				(unsigned long)GetCurrentThreadId());
			Log(buf);
			char hx1[64], hx2[64];
			HexBytes((const void *)c->Rip, hx1, sizeof(hx1));
			HexBytes((const void *)addr, hx2, sizeof(hx2));
			sprintf_s(buf, "  rip bytes: %-36s addr bytes: %s", hx1, hx2);
			Log(buf);
		}

		if(addr >= 0x10000 && addr != (ULONG_PTR)-1) {
			// Code page: only ever add WRITE, keep EXECUTE. Repair one page at a time so a
			// stray write cannot de-execute a whole section.
			if(am.State == MEM_COMMIT && IsExecProt(am.Protect)) {
				void *pg = (void *)(addr & ~(ULONG_PTR)0xFFF);
				const SIZE_T avail = (SIZE_T)((ULONG_PTR)am.BaseAddress + am.RegionSize - (ULONG_PTR)pg);
				const SIZE_T psz = avail < 0x1000 ? avail : 0x1000;
				DWORD old = 0;
				if(psz && VirtualProtect(pg, psz, PAGE_EXECUTE_READWRITE, &old)) {
					const LONG n = InterlockedIncrement(&g_execFix);
					if(n <= 48 || (n % 500) == 0) {
						sprintf_s(buf, "VEH exec-fix prot %lx→RWX %p[%s] n=%ld",
							(unsigned long)am.Protect, pg, at, (long)n);
						Log(buf);
					}
					return EXCEPTION_CONTINUE_EXECUTION;
				}
				InterlockedIncrement(&g_execFail);
				sprintf_s(buf, "VEH exec-fix FAIL err=%lu prot=%lx addr=%p[%s] rip=%p[%s] — pass to app",
					GetLastError(), (unsigned long)am.Protect, (void *)addr, at, (void *)c->Rip, ip);
				Log(buf);
				return EXCEPTION_CONTINUE_SEARCH;
			}

			DWORD old = 0;
			SIZE_T sz = am.RegionSize ? am.RegionSize : 0x1000;
			if(am.State == MEM_COMMIT && VirtualProtect(am.BaseAddress, sz, PAGE_READWRITE, &old)) {
				if(seq <= 48) {
					sprintf_s(buf, "VEH data-fix prot %lx→RW %p[%s]", (unsigned long)am.Protect, (void *)addr, at);
					Log(buf);
				}
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// Unmapped / reserved stale dump pointer: map the page (old packer contract).
			void *page = (void *)(addr & ~(ULONG_PTR)0xFFF);
			void *m = VirtualAlloc(page, 0x20000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if(!m)
				m = VirtualAlloc(page, 0x20000, MEM_COMMIT, PAGE_READWRITE);
			if(m) {
				if(seq <= 48) {
					sprintf_s(buf, "VEH alloc RW %p[%s]", (void *)addr, at);
					Log(buf);
				}
				return EXCEPTION_CONTINUE_EXECUTION;
			}
			// Readonly low map: skip a few stos; then stack-unwind out of SMC spin.
			if(access == 1 && addr < 0x01000000ull &&
			   c->Rip > 0x140000000ull && c->Rip < 0x150000000ull) {
				const BYTE *ipb = (const BYTE *)c->Rip;
				static int skipN = 0;
				if(ipb[0] == 0xF3 && ipb[1] == 0xAA) {
					if(skipN++ >= 4) {
						DWORD old2 = 0;
						VirtualProtect((LPVOID)c->Rip, 16, PAGE_EXECUTE_READWRITE, &old2);
						BYTE *ipw = (BYTE *)c->Rip;
						ipw[0] = 0xC3; // kill site permanently
						sprintf_s(buf, "VEH stos patch ret @ %p", (void *)c->Rip);
						Log(buf);
						skipN = 0;
						return EXCEPTION_CONTINUE_EXECUTION;
					}
					c->Rcx = 0;
					c->Rip += 2;
					if(skipN <= 3)
						Log("VEH skip rep stosb (low RO map)");
					return EXCEPTION_CONTINUE_EXECUTION;
				}
			}
		}
		if(seq <= 48) {
			sprintf_s(buf, "VEH AV unresolved rip=%p[%s] addr=%p[%s] state=%lx prot=%lx err=%lu",
				(void *)c->Rip, ip, (void *)addr, at, (unsigned long)am.State,
				(unsigned long)am.Protect, GetLastError());
			Log(buf);
		}
	}
	return EXCEPTION_CONTINUE_SEARCH;
}

static void InstallPackerVeh() {
	if(AddVectoredExceptionHandler(1, PackerVeh))
		Log("VEH packer installed");
	else
		Log("VEH packer FAIL");
}

// dumpimage .data keeps absolute heap bases from the dump session (e.g. 0x1AD89A80000).
// Map those pages early so packer checks like [base+0x208]/[base+0x250] don't AV.
static void MapStaleDumpHeaps() {
	// Full dump ptr + 32-bit truncation (mov ebx loads low dword → 0x89A80000).
	struct Slot { ULONG_PTR base; SIZE_T size; };
	static const Slot kHeaps[] = {
		{ 0x000001AD89A80000ull, 0x200000 },
		{ 0x0000028765410000ull, 0x200000 },
		{ 0x0000000089A80000ull, 0x200000 },
	};
	for(const Slot &s : kHeaps) {
		char buf[192];
		MEMORY_BASIC_INFORMATION mbi{};
		VirtualQuery((LPCVOID)s.base, &mbi, sizeof(mbi));
		void *p = nullptr;
		DWORD old = 0;
		SIZE_T rsz = mbi.RegionSize ? mbi.RegionSize : 0x1000;
		if(mbi.State == MEM_COMMIT) {
			if(VirtualProtect(mbi.BaseAddress, rsz, PAGE_READWRITE, &old))
				p = (void *)s.base;
			// Never UnmapViewOfFile / Free foreign maps — breaks loader (0xc0000142).
		} else {
			p = VirtualAlloc((LPVOID)s.base, s.size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
			if(!p)
				p = VirtualAlloc((LPVOID)s.base, s.size, MEM_COMMIT, PAGE_READWRITE);
		}
		if(p) {
			sprintf_s(buf, "map stale %p OK (was state=%lx prot=%lx type=%lx rsz=%zx)",
				(void *)s.base, mbi.State, mbi.Protect, mbi.Type, (size_t)mbi.RegionSize);
		} else {
			VirtualQuery((LPCVOID)s.base, &mbi, sizeof(mbi));
			sprintf_s(buf, "map stale %p FAIL err=%lu state=%lx prot=%lx type=%lx",
				(void *)s.base, GetLastError(), mbi.State, mbi.Protect, mbi.Type);
		}
		Log(buf);
	}
}

static void LoadBeside(HINSTANCE self, const wchar_t *dll, const char *tag) {
	wchar_t path[MAX_PATH];
	DWORD n = GetModuleFileNameW(self, path, MAX_PATH);
	if(!n || n >= MAX_PATH) { Log("FAIL GetModuleFileName"); return; }
	wchar_t *slash = wcsrchr(path, L'\\');
	if(!slash) return;
	wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), dll);
	if(!LoadLibraryW(path)) {
		char buf[192];
		sprintf_s(buf, "FAIL LoadLibrary %s err=%lu", tag, GetLastError());
		Log(buf);
		return;
	}
	char buf[96];
	sprintf_s(buf, "loaded %s", tag);
	Log(buf);
}

static void LoadSlotPrep(HINSTANCE self) {
	if(!kLoadSlotPrep) { Log("SlotPrep load skipped"); return; }
	LoadBeside(self, L"SlotPrep.dll", "SlotPrep.dll");
	LoadBeside(self, L"HearthDB.dll", "HearthDB.dll");
	LoadBeside(self, L"SuperWoW.dll", "SuperWoW.dll");
}

static FARPROC Real(const char *name) {
	if(!g_real) {
		g_real = LoadLibraryExW(L"C:\\Windows\\System32\\version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
		if(!g_real) g_real = LoadLibraryW(L"C:\\Windows\\System32\\version.dll");
		if(!g_real) { Log("FAIL load System32\\version.dll"); return nullptr; }
	}
	return GetProcAddress(g_real, name);
}

extern "C" {

BOOL WINAPI Proxy_GetFileVersionInfoA(LPCSTR a, DWORD b, DWORD c, LPVOID d) {
	auto f = (BOOL(WINAPI *)(LPCSTR, DWORD, DWORD, LPVOID))Real("GetFileVersionInfoA");
	return f ? f(a, b, c, d) : FALSE;
}
BOOL WINAPI Proxy_GetFileVersionInfoW(LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
	auto f = (BOOL(WINAPI *)(LPCWSTR, DWORD, DWORD, LPVOID))Real("GetFileVersionInfoW");
	return f ? f(a, b, c, d) : FALSE;
}
BOOL WINAPI Proxy_GetFileVersionInfoExA(DWORD fl, LPCSTR a, DWORD b, DWORD c, LPVOID d) {
	auto f = (BOOL(WINAPI *)(DWORD, LPCSTR, DWORD, DWORD, LPVOID))Real("GetFileVersionInfoExA");
	return f ? f(fl, a, b, c, d) : FALSE;
}
BOOL WINAPI Proxy_GetFileVersionInfoExW(DWORD fl, LPCWSTR a, DWORD b, DWORD c, LPVOID d) {
	auto f = (BOOL(WINAPI *)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID))Real("GetFileVersionInfoExW");
	return f ? f(fl, a, b, c, d) : FALSE;
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeA(LPCSTR a, LPDWORD b) {
	auto f = (DWORD(WINAPI *)(LPCSTR, LPDWORD))Real("GetFileVersionInfoSizeA");
	return f ? f(a, b) : 0;
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeW(LPCWSTR a, LPDWORD b) {
	auto f = (DWORD(WINAPI *)(LPCWSTR, LPDWORD))Real("GetFileVersionInfoSizeW");
	return f ? f(a, b) : 0;
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeExA(DWORD fl, LPCSTR a, LPDWORD b) {
	auto f = (DWORD(WINAPI *)(DWORD, LPCSTR, LPDWORD))Real("GetFileVersionInfoSizeExA");
	return f ? f(fl, a, b) : 0;
}
DWORD WINAPI Proxy_GetFileVersionInfoSizeExW(DWORD fl, LPCWSTR a, LPDWORD b) {
	auto f = (DWORD(WINAPI *)(DWORD, LPCWSTR, LPDWORD))Real("GetFileVersionInfoSizeExW");
	return f ? f(fl, a, b) : 0;
}
BOOL WINAPI Proxy_GetFileVersionInfoByHandle(DWORD fl, HANDLE file, LPVOID buf, DWORD len) {
	auto f = (BOOL(WINAPI *)(DWORD, HANDLE, LPVOID, DWORD))Real("GetFileVersionInfoByHandle");
	return f ? f(fl, file, buf, len) : FALSE;
}
BOOL WINAPI Proxy_VerQueryValueA(LPCVOID a, LPCSTR b, LPVOID *c, PUINT d) {
	auto f = (BOOL(WINAPI *)(LPCVOID, LPCSTR, LPVOID *, PUINT))Real("VerQueryValueA");
	return f ? f(a, b, c, d) : FALSE;
}
BOOL WINAPI Proxy_VerQueryValueW(LPCVOID a, LPCWSTR b, LPVOID *c, PUINT d) {
	auto f = (BOOL(WINAPI *)(LPCVOID, LPCWSTR, LPVOID *, PUINT))Real("VerQueryValueW");
	return f ? f(a, b, c, d) : FALSE;
}
DWORD WINAPI Proxy_VerLanguageNameA(DWORD a, LPSTR b, DWORD c) {
	auto f = (DWORD(WINAPI *)(DWORD, LPSTR, DWORD))Real("VerLanguageNameA");
	return f ? f(a, b, c) : 0;
}
DWORD WINAPI Proxy_VerLanguageNameW(DWORD a, LPWSTR b, DWORD c) {
	auto f = (DWORD(WINAPI *)(DWORD, LPWSTR, DWORD))Real("VerLanguageNameW");
	return f ? f(a, b, c) : 0;
}
DWORD WINAPI Proxy_VerFindFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPSTR e, PUINT f, LPSTR g, PUINT h) {
	auto fn = (DWORD(WINAPI *)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT))Real("VerFindFileA");
	return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI Proxy_VerFindFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPWSTR e, PUINT f, LPWSTR g, PUINT h) {
	auto fn = (DWORD(WINAPI *)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT))Real("VerFindFileW");
	return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI Proxy_VerInstallFileA(DWORD a, LPCSTR b, LPCSTR c, LPCSTR d, LPCSTR e, LPCSTR f, LPSTR g, PUINT h) {
	auto fn = (DWORD(WINAPI *)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT))Real("VerInstallFileA");
	return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}
DWORD WINAPI Proxy_VerInstallFileW(DWORD a, LPCWSTR b, LPCWSTR c, LPCWSTR d, LPCWSTR e, LPCWSTR f, LPWSTR g, PUINT h) {
	auto fn = (DWORD(WINAPI *)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT))Real("VerInstallFileW");
	return fn ? fn(a, b, c, d, e, f, g, h) : 0;
}

} // extern C

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
	if(reason == DLL_PROCESS_ATTACH) {
		DisableThreadLibraryCalls(h);
		Log("VERSION.dll proxy attach");
		InstallPackerVeh();
		MapStaleDumpHeaps();
		Real("GetFileVersionInfoW");
		LoadSlotPrep(h);
	} else if(reason == DLL_PROCESS_DETACH) {
		if(g_real) { FreeLibrary(g_real); g_real = nullptr; }
	}
	return TRUE;
}
