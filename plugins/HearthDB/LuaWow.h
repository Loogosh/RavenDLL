#pragma once
// WowClassic 1.14.2.42597 embedded Lua helpers (dumpimage-verified RVAs).
#include <cstdint>
#include <cstring>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using lua_State = void;

inline uint8_t *WowBase() {
	return (uint8_t *)GetModuleHandleW(nullptr);
}
template <typename T>
inline T WowFn(uint32_t rva) {
	return (T)(WowBase() + rva);
}

static constexpr uint32_t RVA_LUA_GETTOP = 0x58F200;
static constexpr uint32_t RVA_LUA_CREATETABLE = 0x58EB10;
static constexpr uint32_t RVA_LUA_PUSHNIL = 0x58FBD0;
static constexpr uint32_t RVA_LUA_PUSHNUMBER = 0x58FBF0; // (L, double in xmm1)
static constexpr uint32_t RVA_LUA_PUSHINTEGER = 0x58FB00; // (L, int edx)
static constexpr uint32_t RVA_LUA_PUSHSTRING = 0x58FC10;
static constexpr uint32_t RVA_LUA_PUSHBOOLEAN = 0x58F940;
static constexpr uint32_t RVA_LUA_TONUMBER = 0x58F500;
static constexpr uint32_t RVA_LUA_TOSTRING = 0x590920; // (L, idx, size_t* opt)
static constexpr uint32_t RVA_LUA_SETTABLE = 0x58FFB0; // (L, idx) key+val on stack
static constexpr uint32_t RVA_LUA_RAWSETI = 0x5900A0;  // (L, idx, n)

inline int lua_gettop(lua_State *L) {
	return WowFn<int (*)(lua_State *)>(RVA_LUA_GETTOP)(L);
}
inline void lua_createtable(lua_State *L, int narr, int nrec) {
	WowFn<void (*)(lua_State *, int, int)>(RVA_LUA_CREATETABLE)(L, narr, nrec);
}
inline void lua_pushnil(lua_State *L) {
	WowFn<void (*)(lua_State *)>(RVA_LUA_PUSHNIL)(L);
}
inline void lua_pushnumber(lua_State *L, double n) {
	WowFn<void (*)(lua_State *, double)>(RVA_LUA_PUSHNUMBER)(L, n);
}
inline void lua_pushinteger(lua_State *L, int n) {
	WowFn<void (*)(lua_State *, int)>(RVA_LUA_PUSHINTEGER)(L, n);
}
inline void lua_pushstring(lua_State *L, const char *s) {
	WowFn<void (*)(lua_State *, const char *)>(RVA_LUA_PUSHSTRING)(L, s ? s : "");
}
inline void lua_pushboolean(lua_State *L, int b) {
	WowFn<void (*)(lua_State *, int)>(RVA_LUA_PUSHBOOLEAN)(L, b);
}
inline double lua_tonumber(lua_State *L, int idx) {
	return WowFn<double (*)(lua_State *, int)>(RVA_LUA_TONUMBER)(L, idx);
}
inline const char *lua_tostring(lua_State *L, int idx) {
	return WowFn<const char *(*)(lua_State *, int, size_t *)>(RVA_LUA_TOSTRING)(L, idx, nullptr);
}
inline void lua_settable(lua_State *L, int idx) {
	WowFn<void (*)(lua_State *, int)>(RVA_LUA_SETTABLE)(L, idx);
}
inline void lua_rawseti(lua_State *L, int idx, int n) {
	WowFn<void (*)(lua_State *, int, int)>(RVA_LUA_RAWSETI)(L, idx, n);
}
inline bool lua_isstring(lua_State *L, int idx) {
	return lua_tostring(L, idx) != nullptr;
}
// Field set: pushstring(key); push value; lua_settable(L, idx);
