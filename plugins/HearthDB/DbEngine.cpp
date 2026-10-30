#include "DbEngine.h"
#include <charconv>
#include <queue>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <array>
#include <memory>
#include <chrono>
#include "../../third_party/sqlite/sqlite3.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

static constexpr int kMaxDb = 32;
static constexpr int kVerMaj = 0, kVerMin = 4, kVerPat = 1;

struct DbSlot {
	sqlite3 *db = nullptr;
	bool readonly = false;
	bool poison = false;
};

static std::mutex g_mu;
static DbSlot g_db[kMaxDb];

struct AsyncJob {
	int ticket = 0;
	int handle = 0;
	std::string sql;
	enum { Exec, Query, QueryRaw } kind = Exec;
};
struct AsyncDone {
	int ticket = 0;
	HdbResult result;
};

static std::mutex g_aq;
static std::condition_variable g_aqCv;
static std::queue<AsyncJob> g_jobs;
static std::mutex g_doneMu;
static std::vector<AsyncDone> g_done;
static std::atomic<int> g_nextTicket{1};
static std::atomic<bool> g_workerRun{false};
static std::thread g_worker;

int HdbVersionMaj() { return kVerMaj; }
int HdbVersionMin() { return kVerMin; }
int HdbVersionPat() { return kVerPat; }

bool HdbEnsureCustomData() {
	return CreateDirectoryA("CustomData", nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool HdbValidFilename(const std::string &name) {
	if(name.empty()) return false;
	for(char c : name)
		if(strchr("<>:\"/\\|?*", c)) return false;
	return true;
}

static bool HdbValidPath(const std::string &path) {
	if(path.empty()) return false;
	size_t i = 0;
	while(i <= path.size()) {
		size_t j = path.find_first_of("/\\", i);
		std::string part = path.substr(i, j == std::string::npos ? std::string::npos : j - i);
		if(part.empty() || part == "." || part == ".." || !HdbValidFilename(part)) return false;
		if(j == std::string::npos) break;
		i = j + 1;
	}
	return true;
}

static void CancelJobsForHandle(int handle) {
	std::lock_guard<std::mutex> lk(g_aq);
	std::queue<AsyncJob> kept;
	while(!g_jobs.empty()) {
		AsyncJob j = std::move(g_jobs.front());
		g_jobs.pop();
		if(j.handle == handle) {
			HdbResult r;
			r.ok = false;
			r.err = "cancelled: handle closed";
			r.kind = HdbResult::Exec;
			std::lock_guard<std::mutex> d(g_doneMu);
			g_done.push_back({j.ticket, std::move(r)});
		} else {
			kept.push(std::move(j));
		}
	}
	g_jobs = std::move(kept);
}

static void ApplyPragmas(sqlite3 *db, bool writable) {
	sqlite3_exec(db, "PRAGMA temp_store=MEMORY;", nullptr, nullptr, nullptr);
	sqlite3_exec(db, "PRAGMA busy_timeout=5000;", nullptr, nullptr, nullptr);
	if(writable) {
		sqlite3_exec(db, "PRAGMA journal_mode=WAL;", nullptr, nullptr, nullptr);
		sqlite3_exec(db, "PRAGMA synchronous=NORMAL;", nullptr, nullptr, nullptr);
		sqlite3_exec(db, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr);
	}
}

static int AllocSlot(sqlite3 *db, bool ro) {
	std::lock_guard<std::mutex> lock(g_mu);
	for(int i = 0; i < kMaxDb; i++) {
		if(!g_db[i].db) {
			g_db[i] = {db, ro, false}; // clears stale poison (Turtle alloc_handle)
			return i + 1;
		}
	}
	return 0;
}

static DbSlot *GetSlot(int h) {
	if(h < 1 || h > kMaxDb) return nullptr;
	return g_db[h - 1].db ? &g_db[h - 1] : nullptr;
}

int HdbOpen(const std::string &filename, std::string &err) {
	if(!HdbValidFilename(filename)) { err = "invalid filename"; return 0; }
	if(!HdbEnsureCustomData()) { err = "CustomData mkdir failed"; return 0; }
	std::string path = "CustomData\\" + filename;
	sqlite3 *db = nullptr;
	if(sqlite3_open(path.c_str(), &db) != SQLITE_OK) {
		err = db ? sqlite3_errmsg(db) : "open failed";
		if(db) sqlite3_close(db);
		return 0;
	}
	ApplyPragmas(db, true);
	int h = AllocSlot(db, false);
	if(!h) { err = "too many open databases"; sqlite3_close(db); return 0; }
	return h;
}

int HdbOpenAddon(const std::string &addon, const std::string &rel, std::string &err) {
	if(!HdbValidFilename(addon)) { err = "invalid addon name"; return 0; }
	if(!HdbValidPath(rel)) { err = "invalid addon path"; return 0; }
	std::string norm = rel;
	for(char &c : norm) if(c == '/') c = '\\';
	std::string path = "Interface\\AddOns\\" + addon + "\\" + norm;
	sqlite3 *db = nullptr;
	if(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
		err = db ? sqlite3_errmsg(db) : "open addon failed";
		if(db) sqlite3_close(db);
		return 0;
	}
	ApplyPragmas(db, false);
	int h = AllocSlot(db, true);
	if(!h) { err = "too many open databases"; sqlite3_close(db); return 0; }
	return h;
}

bool HdbClose(int handle) {
	CancelJobsForHandle(handle);
	std::lock_guard<std::mutex> lock(g_mu);
	auto *s = GetSlot(handle);
	if(!s) return false;
	s->poison = true; // worker skips leftover jobs (Turtle cancel_handle)
	sqlite3_close(s->db);
	*s = {};
	return true;
}

bool HdbExecute(int handle, const std::string &sql, std::string &err, int *affected) {
	std::lock_guard<std::mutex> lock(g_mu);
	auto *s = GetSlot(handle);
	if(!s) { err = "invalid handle"; return false; }
	if(s->readonly) { err = "database is read-only"; return false; }
	char *errmsg = nullptr;
	int rc = sqlite3_exec(s->db, sql.c_str(), nullptr, nullptr, &errmsg);
	if(rc != SQLITE_OK) {
		err = errmsg ? errmsg : "execute failed";
		sqlite3_free(errmsg);
		return false;
	}
	if(affected) *affected = (int)sqlite3_changes(s->db);
	return true;
}

static bool QueryImpl(sqlite3 *db, const std::string &sql, HdbResult &out, bool named) {
	out = {};
	out.kind = named ? HdbResult::Query : HdbResult::QueryRaw;
	sqlite3_stmt *st = nullptr;
	if(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) != SQLITE_OK) {
		out.err = sqlite3_errmsg(db);
		return false;
	}
	int ncols = sqlite3_column_count(st);
	out.colNames.resize(ncols);
	for(int c = 0; c < ncols; c++) {
		const char *n = sqlite3_column_name(st, c);
		out.colNames[c] = n ? n : "";
	}
	while(sqlite3_step(st) == SQLITE_ROW) {
		HdbRow row;
		row.cols = out.colNames;
		row.vals.resize(ncols);
		for(int c = 0; c < ncols; c++) {
			int t = sqlite3_column_type(st, c);
			if(t == SQLITE_NULL) row.vals[c] = std::nullopt;
			else if(t == SQLITE_BLOB) row.vals[c] = std::string("<blob>");
			else if(t == SQLITE_FLOAT) {
				// Turtle (Rust) does f64::to_string(): 1.0 -> "1", 2.5 -> "2.5".
				// sqlite3_column_text would give "1.0" and break value comparisons.
				const double d = sqlite3_column_double(st, c);
				char nb[64];
				auto res = std::to_chars(nb, nb + sizeof(nb), d);
				row.vals[c] = res.ec == std::errc() ? std::string(nb, res.ptr)
				                                    : std::string(sqlite3_column_text(st, c) ? (const char *)sqlite3_column_text(st, c) : "");
			} else {
				const unsigned char *txt = sqlite3_column_text(st, c);
				row.vals[c] = txt ? (const char *)txt : "";
			}
		}
		out.rows.push_back(std::move(row));
	}
	sqlite3_finalize(st);
	out.ok = true;
	return true;
}

bool HdbQuery(int handle, const std::string &sql, HdbResult &out) {
	std::lock_guard<std::mutex> lock(g_mu);
	auto *s = GetSlot(handle);
	if(!s) { out.err = "invalid handle"; return false; }
	return QueryImpl(s->db, sql, out, true);
}

bool HdbQueryRaw(int handle, const std::string &sql, HdbResult &out) {
	std::lock_guard<std::mutex> lock(g_mu);
	auto *s = GetSlot(handle);
	if(!s) { out.err = "invalid handle"; return false; }
	return QueryImpl(s->db, sql, out, false);
}

void HdbResetAll() {
	{
		std::lock_guard<std::mutex> lock(g_mu);
		for(int i = 0; i < kMaxDb; i++) {
			if(g_db[i].db) { sqlite3_close(g_db[i].db); g_db[i] = {}; }
		}
	}
	std::lock_guard<std::mutex> d(g_doneMu);
	g_done.clear();
}

static void WorkerLoop() {
	while(g_workerRun) {
		AsyncJob job;
		{
			std::unique_lock<std::mutex> lk(g_aq);
			g_aqCv.wait_for(lk, std::chrono::milliseconds(50), [] {
				return !g_jobs.empty() || !g_workerRun;
			});
			if(!g_workerRun && g_jobs.empty()) break;
			if(g_jobs.empty()) continue;
			job = std::move(g_jobs.front());
			g_jobs.pop();
		}
		HdbResult r;
		r.kind = (job.kind == AsyncJob::Exec) ? HdbResult::Exec
			: (job.kind == AsyncJob::Query) ? HdbResult::Query : HdbResult::QueryRaw;
		bool poison = false;
		{
			std::lock_guard<std::mutex> lock(g_mu);
			auto *s = GetSlot(job.handle);
			if(!s) { r.err = "invalid handle"; }
			else if(s->poison) { r.err = "handle poisoned"; }
			else if(job.kind == AsyncJob::Exec) {
				if(s->readonly) { r.err = "database is read-only"; poison = true; }
				else {
					char *errmsg = nullptr;
					int rc = sqlite3_exec(s->db, job.sql.c_str(), nullptr, nullptr, &errmsg);
					if(rc != SQLITE_OK) {
						r.err = errmsg ? errmsg : "execute failed";
						sqlite3_free(errmsg);
						poison = true;
					} else {
						r.ok = true;
						r.rowsAffected = (int)sqlite3_changes(s->db);
					}
				}
			} else {
				if(!QueryImpl(s->db, job.sql, r, job.kind == AsyncJob::Query))
					poison = true;
			}
			if(poison && s) s->poison = true;
		}
		std::lock_guard<std::mutex> d(g_doneMu);
		g_done.push_back({job.ticket, std::move(r)});
	}
}

static void EnsureWorker() {
	bool expected = false;
	if(g_workerRun.compare_exchange_strong(expected, true)) {
		g_worker = std::thread(WorkerLoop);
	}
}

static int Enqueue(AsyncJob job) {
	EnsureWorker();
	{
		std::lock_guard<std::mutex> lock(g_mu);
		auto *s = GetSlot(job.handle);
		if(!s) return 0;
		if(s->poison) return -1; // poisoned — caller raises Lua error
	}
	job.ticket = g_nextTicket.fetch_add(1);
	{
		std::lock_guard<std::mutex> lk(g_aq);
		g_jobs.push(std::move(job));
	}
	g_aqCv.notify_one();
	return job.ticket;
}

int HdbExecuteAsync(int handle, const std::string &sql) {
	return Enqueue({0, handle, sql, AsyncJob::Exec});
}
int HdbQueryAsync(int handle, const std::string &sql, bool raw) {
	return Enqueue({0, handle, sql, raw ? AsyncJob::QueryRaw : AsyncJob::Query});
}

bool HdbGetResult(int ticket, HdbResult &out) {
	std::lock_guard<std::mutex> d(g_doneMu);
	for(auto it = g_done.begin(); it != g_done.end(); ++it) {
		if(it->ticket == ticket) {
			out = std::move(it->result);
			g_done.erase(it);
			return true;
		}
	}
	return false;
}

void HdbClearPoison(int handle) {
	std::lock_guard<std::mutex> lock(g_mu);
	if(auto *s = GetSlot(handle)) s->poison = false;
}

void HdbPumpAsync() {}

void HdbShutdownWorker() {
	g_workerRun = false;
	g_aqCv.notify_all();
	if(g_worker.joinable()) g_worker.join();
}
