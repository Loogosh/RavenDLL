#pragma once
#include <string>
#include <vector>
#include <mutex>
#include <cstdint>
#include <optional>

struct HdbRow {
	std::vector<std::string> cols; // for QueryRaw positional; Query uses names
	std::vector<std::optional<std::string>> vals;
};

struct HdbResult {
	bool ok = false;
	std::string err;
	int rowsAffected = 0;
	std::vector<std::string> colNames;
	std::vector<HdbRow> rows; // Query: cols=names mirrored; QueryRaw: use colNames + vals
	enum Kind { None, Exec, Query, QueryRaw } kind = None;
};

bool HdbEnsureCustomData();
bool HdbValidFilename(const std::string &name);
int HdbOpen(const std::string &filename, std::string &err);
int HdbOpenAddon(const std::string &addon, const std::string &rel, std::string &err);
bool HdbClose(int handle); // false = invalid/already-closed
bool HdbExecute(int handle, const std::string &sql, std::string &err, int *affected = nullptr);
bool HdbQuery(int handle, const std::string &sql, HdbResult &out);
bool HdbQueryRaw(int handle, const std::string &sql, HdbResult &out);
void HdbResetAll();

// Async
int HdbExecuteAsync(int handle, const std::string &sql);
int HdbQueryAsync(int handle, const std::string &sql, bool raw);
bool HdbGetResult(int ticket, HdbResult &out); // false = still pending
void HdbClearPoison(int handle);
void HdbPumpAsync();
void HdbShutdownWorker();
int HdbVersionMaj();
int HdbVersionMin();
int HdbVersionPat();
