/*
** Civilization V Isolated Lua / LuaJIT / Native C Accelerator Benchmark & Equivalence Harness
**
** Reproduces the exact binary host behavior of CivilizationV_DX11.exe observed via REA:
**   - Custom lua_Alloc callback at 0x6ce440 (HeapAlloc + HeapSize + memcpy + HeapFree)
**   - Custom 0-based _G.ipairs C closure at 0x48da50 / 0x74c920
**   - Custom table.fill C closure at 0x66df70
**   - Custom table.count C closure at 0x4aac00
**   - Custom math.clamp C closure at 0x5221f0
**   - Deterministic Map.Rand LCG & Mock Map/Plot/GameInfo host APIs
**   - Real unmodified Civ 5 scripts loaded from the Steam installation directory
*/

#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Standard Lua 5.1 C API types & constants */
typedef struct lua_State lua_State;
typedef double lua_Number;
typedef ptrdiff_t lua_Integer;
typedef int (*lua_CFunction)(lua_State *L);
typedef void *(*lua_Alloc)(void *ud, void *ptr, size_t osize, size_t nsize);

#define LUA_REGISTRYINDEX (-10000)
#define LUA_ENVIRONINDEX  (-10001)
#define LUA_GLOBALSINDEX  (-10002)
#define lua_upvalueindex(i) (LUA_GLOBALSINDEX - (i))

#define LUA_TNIL          0
#define LUA_TBOOLEAN      1
#define LUA_TLIGHTUSERDATA 2
#define LUA_TNUMBER       3
#define LUA_TSTRING       4
#define LUA_TTABLE        5
#define LUA_TFUNCTION     6
#define LUA_TUSERDATA     7
#define LUA_TTHREAD       8

/* Function pointer table dynamically resolved from target lua51_Win32.dll */
typedef struct LuaAPI {
  lua_State *(*lua_newstate)(lua_Alloc f, void *ud);
  void (*lua_close)(lua_State *L);
  lua_State *(*lua_newthread)(lua_State *L);
  void (*luaL_openlibs)(lua_State *L);
  int (*luaL_loadbuffer)(lua_State *L, const char *buff, size_t sz, const char *name);
  int (*luaL_loadfile)(lua_State *L, const char *filename);
  int (*lua_pcall)(lua_State *L, int nargs, int nresults, int errfunc);
  void (*lua_call)(lua_State *L, int nargs, int nresults);
  int (*lua_gettop)(lua_State *L);
  void (*lua_settop)(lua_State *L, int idx);
  void (*lua_pushvalue)(lua_State *L, int idx);
  void (*lua_remove)(lua_State *L, int idx);
  void (*lua_insert)(lua_State *L, int idx);
  void (*lua_replace)(lua_State *L, int idx);
  int (*lua_type)(lua_State *L, int idx);
  lua_Number (*lua_tonumber)(lua_State *L, int idx);
  lua_Integer (*lua_tointeger)(lua_State *L, int idx);
  int (*lua_toboolean)(lua_State *L, int idx);
  const char *(*lua_tolstring)(lua_State *L, int idx, size_t *len);
  void (*lua_pushnil)(lua_State *L);
  void (*lua_pushnumber)(lua_State *L, lua_Number n);
  void (*lua_pushinteger)(lua_State *L, lua_Integer n);
  void (*lua_pushlstring)(lua_State *L, const char *s, size_t l);
  void (*lua_pushstring)(lua_State *L, const char *s);
  void (*lua_pushboolean)(lua_State *L, int b);
  void (*lua_pushcclosure)(lua_State *L, lua_CFunction fn, int n);
  void (*lua_gettable)(lua_State *L, int idx);
  void (*lua_getfield)(lua_State *L, int idx, const char *k);
  void (*lua_rawget)(lua_State *L, int idx);
  void (*lua_rawgeti)(lua_State *L, int idx, int n);
  void (*lua_createtable)(lua_State *L, int narr, int nrec);
  void (*lua_settable)(lua_State *L, int idx);
  void (*lua_setfield)(lua_State *L, int idx, const char *k);
  void (*lua_rawset)(lua_State *L, int idx);
  void (*lua_rawseti)(lua_State *L, int idx, int n);
  int (*lua_setmetatable)(lua_State *L, int objindex);
  int (*lua_setfenv)(lua_State *L, int idx);
  int (*lua_next)(lua_State *L, int idx);
  size_t (*lua_objlen)(lua_State *L, int idx);
  void (*luaL_checktype)(lua_State *L, int narg, int t);
  lua_Integer (*luaL_checkinteger)(lua_State *L, int narg);
  lua_Number (*luaL_checknumber)(lua_State *L, int narg);
  int (*lua_gc)(lua_State *L, int what, int data);
} LuaAPI;

static LuaAPI G;

#define lua_pop(L, n) G.lua_settop((L), -(n)-1)
#define lua_pushcfunction(L, f) G.lua_pushcclosure((L), (f), 0)
#define lua_newtable(L) G.lua_createtable((L), 0, 0)

static int load_lua_api(HMODULE hMod)
{
#define LOAD_SYM(name) \
  do { \
    *(void **)&G.name = (void *)GetProcAddress(hMod, #name); \
    if (!G.name) { \
      fprintf(stderr, "Missing export: %s\n", #name); \
      return 0; \
    } \
  } while (0)

  LOAD_SYM(lua_newstate);
  LOAD_SYM(lua_close);
  LOAD_SYM(lua_newthread);
  LOAD_SYM(luaL_openlibs);
  LOAD_SYM(luaL_loadbuffer);
  LOAD_SYM(luaL_loadfile);
  LOAD_SYM(lua_pcall);
  LOAD_SYM(lua_call);
  LOAD_SYM(lua_gettop);
  LOAD_SYM(lua_settop);
  LOAD_SYM(lua_pushvalue);
  LOAD_SYM(lua_remove);
  LOAD_SYM(lua_insert);
  LOAD_SYM(lua_replace);
  LOAD_SYM(lua_type);
  LOAD_SYM(lua_tonumber);
  LOAD_SYM(lua_tointeger);
  LOAD_SYM(lua_toboolean);
  LOAD_SYM(lua_tolstring);
  LOAD_SYM(lua_pushnil);
  LOAD_SYM(lua_pushnumber);
  LOAD_SYM(lua_pushinteger);
  LOAD_SYM(lua_pushlstring);
  LOAD_SYM(lua_pushstring);
  LOAD_SYM(lua_pushboolean);
  LOAD_SYM(lua_pushcclosure);
  LOAD_SYM(lua_gettable);
  LOAD_SYM(lua_getfield);
  LOAD_SYM(lua_rawget);
  LOAD_SYM(lua_rawgeti);
  LOAD_SYM(lua_createtable);
  LOAD_SYM(lua_settable);
  LOAD_SYM(lua_setfield);
  LOAD_SYM(lua_rawset);
  LOAD_SYM(lua_rawseti);
  LOAD_SYM(lua_setmetatable);
  LOAD_SYM(lua_setfenv);
  LOAD_SYM(lua_next);
  LOAD_SYM(lua_objlen);
  LOAD_SYM(luaL_checktype);
  LOAD_SYM(luaL_checkinteger);
  LOAD_SYM(luaL_checknumber);
  LOAD_SYM(lua_gc);
#undef LOAD_SYM
  return 1;
}

/* =========================================================================
 * Exact Reproduction of CivilizationV_DX11.exe Binary Functions
 * ========================================================================= */

static HANDLE s_civ5_heap = NULL;
static uint64_t s_alloc_calls = 0;

/* 0x6ce440: Civ 5's custom lua_Alloc callback (never uses HeapReAlloc!) */
static void *civ5_host_alloc(void *ud, void *ptr, size_t osize, size_t nsize)
{
  (void)ud;
  (void)osize;
  s_alloc_calls++;
  if (nsize == 0) {
    if (ptr)
      HeapFree(s_civ5_heap, 0, ptr);
    return NULL;
  } else {
    void *new_ptr = HeapAlloc(s_civ5_heap, 0, nsize);
    if (ptr && new_ptr) {
      size_t old_actual = HeapSize(s_civ5_heap, 0, ptr);
      size_t copy_sz = old_actual < nsize ? old_actual : nsize;
      memcpy(new_ptr, ptr, copy_sz);
      HeapFree(s_civ5_heap, 0, ptr);
    }
    return new_ptr;
  }
}

/* 0x74c920: Civ 5's custom ipairs iterator step C function */
static int civ5_host_ipairs_aux(lua_State *L)
{
  lua_Integer i;
  G.luaL_checktype(L, 1, LUA_TTABLE);
  i = G.luaL_checkinteger(L, 2) + 1;
  G.lua_pushinteger(L, i);
  G.lua_rawgeti(L, 1, (int)i);
  return (G.lua_type(L, -1) == LUA_TNIL) ? 0 : 2;
}

/* 0x48da50: Civ 5's custom 0-based ipairs factory C closure */
static int civ5_host_ipairs(lua_State *L)
{
  int start_idx;
  G.luaL_checktype(L, 1, LUA_TTABLE);
  G.lua_pushvalue(L, lua_upvalueindex(1));
  G.lua_pushvalue(L, 1);
  G.lua_rawgeti(L, 1, 0);
  start_idx = (G.lua_type(L, -1) != LUA_TNIL) ? -1 : 0;
  G.lua_settop(L, -2);
  G.lua_pushinteger(L, start_idx);
  return 3;
}

/* 0x66df70: Civ 5's custom table.fill(value, count) C function */
static int civ5_host_table_fill(lua_State *L)
{
  int count = (int)G.luaL_checkinteger(L, 2);
  int i;
  G.lua_createtable(L, count, 0);
  for (i = 1; i <= count; i++) {
    G.lua_pushvalue(L, 1);
    G.lua_rawseti(L, -2, i);
  }
  return 1;
}

/* 0x4aac00: Civ 5's custom table.count(t) C function */
static int civ5_host_table_count(lua_State *L)
{
  int count = 0;
  G.luaL_checktype(L, 1, LUA_TTABLE);
  G.lua_pushnil(L);
  while (G.lua_next(L, 1) != 0) {
    count++;
    G.lua_settop(L, -2);
  }
  G.lua_pushinteger(L, count);
  return 1;
}

/* 0x5221f0: Civ 5's custom math.clamp(v, lo, hi) C function */
static int civ5_host_math_clamp(lua_State *L)
{
  double v  = G.luaL_checknumber(L, 1);
  double lo = G.luaL_checknumber(L, 2);
  double hi = G.luaL_checknumber(L, 3);
  if (v < lo) v = lo;
  else if (v > hi) v = hi;
  G.lua_pushnumber(L, v);
  return 1;
}

/* 0x7a5b30: Civ 5's state registration sequence */
static void civ5_register_host_extensions(lua_State *L)
{
  G.luaL_openlibs(L);

  /* Overwrite _G.ipairs with 0x48da50 + upvalue 0x74c920 */
  G.lua_pushcclosure(L, civ5_host_ipairs_aux, 0);
  G.lua_pushcclosure(L, civ5_host_ipairs, 1);
  G.lua_setfield(L, LUA_GLOBALSINDEX, "ipairs");

  /* Add table.fill and table.count */
  G.lua_getfield(L, LUA_GLOBALSINDEX, "table");
  G.lua_pushcclosure(L, civ5_host_table_fill, 0);
  G.lua_setfield(L, -2, "fill");
  G.lua_pushcclosure(L, civ5_host_table_count, 0);
  G.lua_setfield(L, -2, "count");
  lua_pop(L, 1);

  /* Add math.clamp */
  G.lua_getfield(L, LUA_GLOBALSINDEX, "math");
  G.lua_pushcclosure(L, civ5_host_math_clamp, 0);
  G.lua_setfield(L, -2, "clamp");
  lua_pop(L, 1);
}

/* =========================================================================
 * Deterministic Mock Map / Plot / Game Host Environment for Civ 5 Scripts
 * ========================================================================= */

static uint64_t s_rng_state = 0xC1055EED12345678ULL;
static uint64_t s_rng_calls = 0;
static uint64_t s_suite_rng_calls = 0;
static int s_map_w = 128;
static int s_map_h = 80;
static int s_wrap_x = 1;
static int s_wrap_y = 0;

static void reset_rng(uint64_t seed)
{
  s_rng_state = seed;
  s_rng_calls = 0;
}

static int mock_Map_Rand(lua_State *L)
{
  lua_Integer limit = G.luaL_checkinteger(L, 1);
  s_rng_calls++;
  s_suite_rng_calls++;
  s_rng_state = s_rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
  if (limit <= 0) {
    G.lua_pushinteger(L, 0);
  } else {
    uint32_t r = (uint32_t)(s_rng_state >> 32);
    G.lua_pushinteger(L, (lua_Integer)(r % (uint32_t)limit));
  }
  return 1;
}

static int mock_Map_GetGridSize(lua_State *L)
{
  G.lua_pushinteger(L, s_map_w);
  G.lua_pushinteger(L, s_map_h);
  return 2;
}

static int mock_Map_IsWrapX(lua_State *L)
{
  G.lua_pushboolean(L, s_wrap_x);
  return 1;
}

static int mock_Map_IsWrapY(lua_State *L)
{
  G.lua_pushboolean(L, s_wrap_y);
  return 1;
}

static int mock_noop_print(lua_State *L)
{
  (void)L;
  return 0;
}

static void register_civ5_mock_host(lua_State *L)
{
  /* Suppress script debug print() spam during benchmarking */
  lua_pushcfunction(L, mock_noop_print);
  G.lua_setfield(L, LUA_GLOBALSINDEX, "print");

  /* Map table */
  lua_newtable(L);
  lua_pushcfunction(L, mock_Map_Rand);
  G.lua_setfield(L, -2, "Rand");
  lua_pushcfunction(L, mock_Map_GetGridSize);
  G.lua_setfield(L, -2, "GetGridSize");
  lua_pushcfunction(L, mock_Map_IsWrapX);
  G.lua_setfield(L, -2, "IsWrapX");
  lua_pushcfunction(L, mock_Map_IsWrapY);
  G.lua_setfield(L, -2, "IsWrapY");
  G.lua_setfield(L, LUA_GLOBALSINDEX, "Map");

  /* PlotTypes, TerrainTypes, FeatureTypes, DirectionTypes, ResourceUsageTypes, GameDefines */
  {
    const char * helper_enums =
      "PlotTypes = { PLOT_MOUNTAIN = 0, PLOT_HILLS = 1, PLOT_LAND = 2, PLOT_OCEAN = 3, NUM_PLOT_TYPES = 4 }\n"
      "TerrainTypes = { TERRAIN_GRASS = 0, TERRAIN_PLAINS = 1, TERRAIN_DESERT = 2, TERRAIN_TUNDRA = 3, TERRAIN_SNOW = 4, TERRAIN_COAST = 5, TERRAIN_OCEAN = 6 }\n"
      "FeatureTypes = { NO_FEATURE = -1, FEATURE_ICE = 0, FEATURE_JUNGLE = 1, FEATURE_MARSH = 2, FEATURE_OASIS = 3, FEATURE_FLOOD_PLAINS = 4, FEATURE_FOREST = 5, FEATURE_FALLOUT = 6 }\n"
      "DirectionTypes = { DIRECTION_NORTHEAST = 0, DIRECTION_EAST = 1, DIRECTION_SOUTHEAST = 2, DIRECTION_SOUTHWEST = 3, DIRECTION_WEST = 4, DIRECTION_NORTHWEST = 5, NUM_DIRECTION_TYPES = 6 }\n"
      "ResourceUsageTypes = { RESOURCEUSAGE_STRATEGIC = 0, RESOURCEUSAGE_LUXURY = 1, RESOURCEUSAGE_BONUS = 2 }\n"
      "GameDefines = { MAX_MAJOR_CIVS = 22, MAX_CIV_PLAYERS = 63 }\n"
      "Map.GetFractalFlags = function() return {} end\n"
      "Map.GetWorldSize = function() return 5 end\n"
      "GameInfo = {\n"
      "    Worlds = function()\n"
      "      local i = 0\n"
      "      local rows = {\n"
      "        { Type = 'WORLDSIZE_DUEL', ID = 0 },\n"
      "        { Type = 'WORLDSIZE_TINY', ID = 1 },\n"
      "        { Type = 'WORLDSIZE_SMALL', ID = 2 },\n"
      "        { Type = 'WORLDSIZE_STANDARD', ID = 3 },\n"
      "        { Type = 'WORLDSIZE_LARGE', ID = 4 },\n"
      "        { Type = 'WORLDSIZE_HUGE', ID = 5 },\n"
      "      }\n"
      "      return function() i = i + 1; return rows[i] end\n"
      "    end,\n"
      "    IconTextureAtlases = setmetatable({}, {\n"
      "      __call = function()\n"
      "        local i = 0\n"
      "        local raw_rows = {\n"
      "          { Atlas = 'CIV_COLOR_ATLAS', IconSize = 64, Filename = 'CivSymbolsColor64.dds', IconsPerRow = 8, IconsPerColumn = 8 },\n"
      "          { Atlas = 'CIV_COLOR_ATLAS', IconSize = 45, Filename = 'CivSymbolsColor45.dds', IconsPerRow = 8, IconsPerColumn = 8 },\n"
      "          { Atlas = 'CIV_COLOR_ATLAS', IconSize = 32, Filename = 'CivSymbolsColor32.dds', IconsPerRow = 8, IconsPerColumn = 8 },\n"
      "          { Atlas = 'UNIT_FLAG_ATLAS', IconSize = 32, Filename = 'UnitFlagAtlasWhite.dds', IconsPerRow = 16, IconsPerColumn = 8 },\n"
      "          { Atlas = 'TECH_ATLAS_1',    IconSize = 64, Filename = 'TechnologyAtlas64.dds', IconsPerRow = 8, IconsPerColumn = 8 },\n"
      "        }\n"
      "        return function()\n"
      "          i = i + 1\n"
      "          local inner = raw_rows[i]\n"
      "          if not inner then return nil end\n"
      "          -- Reproduces CvGameDatabase PushDatabaseRow (0x1000b570): outer table has boolean true, __index has values\n"
      "          local outer = { Atlas = true, IconSize = true, Filename = true, IconsPerRow = true, IconsPerColumn = true }\n"
      "          return setmetatable(outer, { __index = function(_, k) return inner[k] end })\n"
      "        end\n"
      "      end\n"
      "    }),\n"
      "  }\n"
      "Events = {\n"
      "  AfterModsDeactivate = { Add = function() end },\n"
      "  AfterModsActivate   = { Add = function() end },\n"
      "}\n"
      "ContextPtr = {\n"
      "  BuildInstance = function(self, instName, ctrlTab)\n"
      "    ctrlTab.Anchor = {\n"
      "      hidden = false,\n"
      "      SetHide = function(c, b) c.hidden = b end\n"
      "    }\n"
      "  end,\n"
      "  BuildInstanceForControl = function(self, instName, ctrlTab, parent)\n"
      "    ctrlTab.Anchor = {\n"
      "      hidden = false,\n"
      "      SetHide = function(c, b) c.hidden = b end\n"
      "    }\n"
      "  end,\n"
      "  DestroyChild = function() end,\n"
      "}\n"
      "Fractal = {\n"
      "  Create = function(w, h, grain, flags, fracXExp, fracYExp)\n"
      "    local t = { w = w, h = h, grain = grain or 2 }\n"
      "    function t:GetHeight(a, b)\n"
      "      if b == nil then return a end\n"
      "      return ((a * 73 + b * 151 + self.grain * 997) % 100)\n"
      "    end\n"
      "    function t:BuildRidges() end\n"
      "    return t\n"
      "  end\n"
      "}\n"
      "function include(name) end\n";
    if (G.luaL_loadbuffer(L, helper_enums, strlen(helper_enums), "=[enums]") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "Failed to load helper enums: %s\n", G.lua_tolstring(L, -1, NULL));
    }
  }
}

/* =========================================================================
 * File Loader & FNV-1a 64-bit Checksum Helpers
 * ========================================================================= */

static char *read_file_all(const char *path, size_t *out_sz)
{
  FILE *fp = fopen(path, "rb");
  char *buf;
  long sz;
  if (!fp) return NULL;
  fseek(fp, 0, SEEK_END);
  sz = ftell(fp);
  fseek(fp, 0, SEEK_SET);
  buf = (char *)malloc((size_t)sz + 1);
  if (!buf) {
    fclose(fp);
    return NULL;
  }
  fread(buf, 1, (size_t)sz, fp);
  buf[sz] = '\0';
  fclose(fp);
  *out_sz = (size_t)sz;
  return buf;
}

static uint64_t fnv1a_init(void)
{
  return 14695981039346656037ULL;
}

static uint64_t fnv1a_u64(uint64_t h, uint64_t v)
{
  int i;
  for (i = 0; i < 8; i++) {
    h ^= (v & 0xff);
    h *= 1099511628211ULL;
    v >>= 8;
  }
  return h;
}

static uint64_t hash_lua_array(lua_State *L, int idx, int len, uint64_t h)
{
  int i;
  for (i = 1; i <= len; i++) {
    int t;
    G.lua_rawgeti(L, idx, i);
    t = G.lua_type(L, -1);
    if (t == LUA_TNUMBER) {
      int64_t iv = (int64_t)G.lua_tointeger(L, -1);
      h = fnv1a_u64(h, (uint64_t)iv);
    } else if (t == LUA_TBOOLEAN) {
      h = fnv1a_u64(h, (uint64_t)G.lua_toboolean(L, -1));
    } else {
      h = fnv1a_u64(h, 0xdeadbeefULL);
    }
    lua_pop(L, 1);
  }
  return h;
}

/* =========================================================================
 * Naive lua2c C-API Transpilation Function (for Feasibility Comparison)
 * ========================================================================= */

/*
 * Transpiled version of:
 *   function sum_clamped(t, n)
 *     local s = 0
 *     for i = 1, n do
 *       s = s + math.clamp(t[i], 10, 90)
 *     end
 *     return s
 *   end
 * using cross-DLL Lua C API calls on each iteration (like lua2c).
 */
static int naive_lua2c_sum_clamped(lua_State *L)
{
  lua_Integer n = G.luaL_checkinteger(L, 2);
  double s = 0.0;
  lua_Integer i;
  for (i = 1; i <= n; i++) {
    G.lua_getfield(L, LUA_GLOBALSINDEX, "math");
    G.lua_getfield(L, -1, "clamp");
    G.lua_rawgeti(L, 1, (int)i);
    G.lua_pushinteger(L, 10);
    G.lua_pushinteger(L, 90);
    G.lua_call(L, 3, 1);
    s += G.lua_tonumber(L, -1);
    lua_pop(L, 2); /* pop result and math table */
  }
  G.lua_pushnumber(L, s);
  return 1;
}

/* =========================================================================
 * Benchmark & Equivalence Suite
 * ========================================================================= */

#define NUM_WORKLOADS 10

typedef struct BenchResult {
  double ms[NUM_WORKLOADS];
  uint64_t hash[NUM_WORKLOADS];
  uint64_t rng_calls;
  uint64_t suite_rng_calls;
  uint64_t alloc_calls;
  double lua2c_ms;
  double luajit_trace_ms;
} BenchResult;

static double qpc_ms(LARGE_INTEGER t0, LARGE_INTEGER t1, LARGE_INTEGER freq)
{
  return (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)freq.QuadPart;
}

static int format_path(char *dst, size_t capacity, const char *dir, const char *suffix)
{
  int n = snprintf(dst, capacity, "%s%s", dir, suffix);
  return n >= 0 && (size_t)n < capacity;
}

/* Return -1 on I/O failure, 1 for identical files, and 0 otherwise. */
static int files_equal(const char *a, const char *b)
{
  FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
  unsigned char ba[4096], bb[4096];
  size_t na, nb;
  int result = 1;
  if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return -1; }
  do {
    na = fread(ba, 1, sizeof(ba), fa);
    nb = fread(bb, 1, sizeof(bb), fb);
    if (na != nb || memcmp(ba, bb, na) != 0) { result = 0; break; }
  } while (na == sizeof(ba));
  if (ferror(fa) || ferror(fb)) result = -1;
  fclose(fa); fclose(fb);
  return result;
}

static int run_suite(const char *dll_path, const char *civ5_dir,
                     int accel_mode, const char *label, BenchResult *out)
{
  HMODULE hMod;
  LARGE_INTEGER freq, t0, t1;
  char path_mmu[MAX_PATH], path_asp[MAX_PATH], path_fw[MAX_PATH], path_vec[MAX_PATH];
  char path_mlf[MAX_PATH], path_im[MAX_PATH], path_icon[MAX_PATH];
  size_t sz_mmu = 0, sz_asp = 0, sz_fw = 0, sz_vec = 0;
  size_t sz_mlf = 0, sz_im = 0, sz_icon = 0;
  char *buf_mmu, *buf_asp, *buf_fw, *buf_vec;
  char *buf_mlf, *buf_im, *buf_icon;
  lua_State *L;

  QueryPerformanceFrequency(&freq);
  SetEnvironmentVariableA("CIV5_NATIVE_ACCEL", accel_mode ? "1" : "0");

  hMod = LoadLibraryA(dll_path);
  if (!hMod) {
    fprintf(stderr, "Failed to load DLL: %s (err=%lu)\n", dll_path, GetLastError());
    return 0;
  }
  if (!load_lua_api(hMod)) {
    FreeLibrary(hMod);
    return 0;
  }

  if (!format_path(path_mmu, sizeof(path_mmu), civ5_dir, "\\Assets\\DLC\\Expansion2\\Gameplay\\Lua\\MapmakerUtilities.lua") ||
      !format_path(path_asp, sizeof(path_asp), civ5_dir, "\\Assets\\DLC\\Expansion2\\Gameplay\\Lua\\AssignStartingPlots.lua") ||
      !format_path(path_fw, sizeof(path_fw), civ5_dir, "\\Assets\\Gameplay\\Lua\\FractalWorld.lua") ||
      !format_path(path_vec, sizeof(path_vec), civ5_dir, "\\Assets\\Gameplay\\Lua\\FLuaVector.lua") ||
      !format_path(path_mlf, sizeof(path_mlf), civ5_dir, "\\Assets\\Gameplay\\Lua\\MultilayeredFractal.lua") ||
      !format_path(path_im, sizeof(path_im), civ5_dir, "\\Assets\\UI\\InstanceManager.lua") ||
      !format_path(path_icon, sizeof(path_icon), civ5_dir, "\\Assets\\UI\\IconSupport.lua")) {
    fprintf(stderr, "Civ 5 path too long\n");
    FreeLibrary(hMod);
    return 0;
  }

  buf_mmu  = read_file_all(path_mmu,  &sz_mmu);
  buf_asp  = read_file_all(path_asp,  &sz_asp);
  buf_fw   = read_file_all(path_fw,   &sz_fw);
  buf_vec  = read_file_all(path_vec,  &sz_vec);
  buf_mlf  = read_file_all(path_mlf,  &sz_mlf);
  buf_im   = read_file_all(path_im,   &sz_im);
  buf_icon = read_file_all(path_icon, &sz_icon);

  if (!buf_mmu || !buf_asp || !buf_fw || !buf_vec || !buf_mlf || !buf_im || !buf_icon) {
    fprintf(stderr, "Failed to read Civ 5 Lua scripts from %s\n", civ5_dir);
    return 0;
  }

  memset(out, 0, sizeof(*out));
  s_alloc_calls = 0;
  s_suite_rng_calls = 0;

  L = G.lua_newstate(civ5_host_alloc, NULL);
  civ5_register_host_extensions(L);
  register_civ5_mock_host(L);

  /* -----------------------------------------------------------------------
   * Workload 1: Civ 5 0-Based ipairs + math.clamp Hot Iteration
   * (Tests both 0-based tables like Players[] and 1-based plot tables)
   * ----------------------------------------------------------------------- */
  {
    const char *w1_code =
      "local t0 = { [0] = 42 }\n"
      "local t1 = {}\n"
      "for i = 1, 512 do\n"
      "  t0[i] = (i * 37) % 120\n"
      "  t1[i] = (i * 53) % 120\n"
      "end\n"
      "local clamp = math.clamp\n"
      "local total = 0\n"
      "for iter = 1, 2500 do\n"
      "  for i, v in ipairs(t0) do\n"
      "    total = total + clamp(v, 10, 100) + i\n"
      "  end\n"
      "  for i, v in ipairs(t1) do\n"
      "    total = total + clamp(v, 15, 95) + i\n"
      "  end\n"
      "end\n"
      "return total\n";

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w1_code, strlen(w1_code), "=[workload1]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W1 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[0] = qpc_ms(t0, t1, freq);
    out->hash[0] = fnv1a_u64(fnv1a_init(), (uint64_t)G.lua_tointeger(L, -1));
    lua_pop(L, 1);
  }

  /* -----------------------------------------------------------------------
   * Workload 2: Civ 5 table.fill + table.count on Huge Map Layers (10,240 plots)
   * ----------------------------------------------------------------------- */
  {
    const char *w2_code =
      "local fill = table.fill\n"
      "local count = table.count\n"
      "local acc = 0\n"
      "for iter = 1, 40 do\n"
      "  local d1 = fill(0, 10240)\n"
      "  local d2 = fill(false, 10240)\n"
      "  local d3 = fill(-1, 2560)\n"
      "  acc = acc + count(d1) + count(d2) + count(d3) + d1[10240] - d3[2560]\n"
      "end\n"
      "return acc\n";

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w2_code, strlen(w2_code), "=[workload2]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W2 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[1] = qpc_ms(t0, t1, freq);
    out->hash[1] = fnv1a_u64(fnv1a_init(), (uint64_t)G.lua_tointeger(L, -1));
    lua_pop(L, 1);
  }

  /* -----------------------------------------------------------------------
   * Workload 3: Real MapmakerUtilities.lua
   * (GetShuffledCopyOfTable, TestMembership, IdentifyTableIndex)
   * ----------------------------------------------------------------------- */
  {
    const char *w3_code =
      "local plots = {}\n"
      "for i = 1, 2400 do plots[i] = i * 3 end\n"
      "local last_shuffled\n"
      "local member_hits = 0\n"
      "for pass = 1, 20 do\n"
      "  last_shuffled = GetShuffledCopyOfTable(plots)\n"
      "  for q = 1, 200 do\n"
      "    if TestMembership(last_shuffled, q * 30) then\n"
      "      member_hits = member_hits + 1\n"
      "    end\n"
      "  end\n"
      "  local found, cnt, idx_tab = IdentifyTableIndex(last_shuffled, 1200)\n"
      "  if found then member_hits = member_hits + cnt + idx_tab[1] end\n"
      "end\n"
      "return last_shuffled, member_hits\n";
    uint64_t h;

    reset_rng(0x1122334455667788ULL);
    if (G.luaL_loadbuffer(L, buf_mmu, sz_mmu, "@MapmakerUtilities.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "MMU load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w3_code, strlen(w3_code), "=[workload3]") != 0 ||
        G.lua_pcall(L, 0, 2, 0) != 0) {
      fprintf(stderr, "W3 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[2] = qpc_ms(t0, t1, freq);

    h = fnv1a_init();
    h = fnv1a_u64(h, (uint64_t)G.lua_tointeger(L, -1));
    h = fnv1a_u64(h, s_rng_state);
    h = fnv1a_u64(h, s_rng_calls);
    lua_pop(L, 1); /* pop member_hits, leaving last_shuffled */
    h = hash_lua_array(L, G.lua_gettop(L), 2400, h);
    lua_pop(L, 1);
    out->hash[2] = h;
    out->rng_calls = s_rng_calls;
  }

  /* -----------------------------------------------------------------------
   * Workload 4: Real AssignStartingPlots.lua
   * (PlaceImpactAndRipples & PlaceResourceImpact across 10,240-plot Huge Map)
   * ----------------------------------------------------------------------- */
  {
    const char *w4_code =
      "local iW, iH = Map.GetGridSize()\n"
      "local nPlots = iW * iH\n"
      "local asp = {\n"
      "  PlaceImpactAndRipples = AssignStartingPlots.PlaceImpactAndRipples,\n"
      "  PlaceResourceImpact = AssignStartingPlots.PlaceResourceImpact,\n"
      "  firstRingYIsEven = {{0, 1}, {1, 0}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}},\n"
      "  firstRingYIsOdd = {{1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {0, 1}},\n"
      "  distanceData = table.fill(0, nPlots),\n"
      "  playerCollisionData = table.fill(false, nPlots),\n"
      "  cityStateData = table.fill(0, nPlots),\n"
      "  strategicData = table.fill(0, nPlots),\n"
      "  luxuryData = table.fill(0, nPlots),\n"
      "  bonusData = table.fill(0, nPlots),\n"
      "  fishData = table.fill(0, nPlots),\n"
      "  naturalWondersData = table.fill(0, nPlots),\n"
      "  marbleData = table.fill(0, nPlots),\n"
      "}\n"
      "for pass = 1, 25 do\n"
      "  for c = 1, 22 do\n"
      "    local sx = (c * 17 + pass * 3) % iW\n"
      "    local sy = (c * 11 + pass * 5) % iH\n"
      "    asp:PlaceImpactAndRipples(sx, sy)\n"
      "  end\n"
      "  for r = 1, 350 do\n"
      "    local rx = (r * 29 + pass * 7) % iW\n"
      "    local ry = (r * 19 + pass * 2) % iH\n"
      "    local layer = (r % 7) + 1\n"
      "    local rad = (r % 5)\n"
      "    asp:PlaceResourceImpact(rx, ry, layer, rad)\n"
      "  end\n"
      "end\n"
      "return asp\n";
    uint64_t h;
    const char *layers[] = {
      "distanceData", "playerCollisionData", "cityStateData",
      "strategicData", "luxuryData", "bonusData",
      "fishData", "naturalWondersData", "marbleData"
    };
    int li;

    if (G.luaL_loadbuffer(L, buf_asp, sz_asp, "@AssignStartingPlots.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "ASP load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w4_code, strlen(w4_code), "=[workload4]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W4 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[3] = qpc_ms(t0, t1, freq);

    h = fnv1a_init();
    for (li = 0; li < 9; li++) {
      G.lua_getfield(L, -1, layers[li]);
      h = hash_lua_array(L, G.lua_gettop(L), s_map_w * s_map_h, h);
      lua_pop(L, 1);
    }
    lua_pop(L, 1); /* pop asp */
    out->hash[3] = h;
  }

  /* -----------------------------------------------------------------------
   * Workload 5: Real FractalWorld.lua + FLuaVector.lua
   * (GeneratePlotTypes, ShiftPlotTypes, FindPlateValue, HexToWorld)
   * ----------------------------------------------------------------------- */
  {
    const char *w5_code =
      "local fw = FractalWorld.Create()\n"
      "fw:InitFractal{}\n"
      "local plots\n"
      "for iter = 1, 12 do\n"
      "  plots = fw:GeneratePlotTypes{ water_percent = 68, shift_plot_types = true }\n"
      "end\n"
      "local vsum = 0\n"
      "for i = 1, 10000 do\n"
      "  local v1 = Vector2(i, i * 2)\n"
      "  local v2 = Vector2(i % 128, i % 80)\n"
      "  local va = VecAdd(v1, v2)\n"
      "  local vs = VecSubtract(v1, v2)\n"
      "  vsum = vsum + va.x + va.y + vs.x + vs.y\n"
      "end\n"
      "return plots, vsum\n";
    uint64_t h;

    reset_rng(0x99AABBCCDDEEFF01ULL);
    if (G.luaL_loadbuffer(L, buf_fw, sz_fw, "@FractalWorld.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "FW load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    if (G.luaL_loadbuffer(L, buf_vec, sz_vec, "@FLuaVector.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "Vec load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w5_code, strlen(w5_code), "=[workload5]") != 0 ||
        G.lua_pcall(L, 0, 2, 0) != 0) {
      fprintf(stderr, "W5 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[4] = qpc_ms(t0, t1, freq);

    h = fnv1a_init();
    h = fnv1a_u64(h, (uint64_t)G.lua_tointeger(L, -1));
    lua_pop(L, 1);
    h = hash_lua_array(L, G.lua_gettop(L), s_map_w * s_map_h, h);
    lua_pop(L, 1);
    out->hash[4] = h;
  }

  /* -----------------------------------------------------------------------
   * Workload 6: Multi-Context lua_newthread + luaL_loadbuffer (150 UI Contexts)
   * Reproduces Civ 5 loading shared scripts into 150 separate UI/Gameplay threads
   * ----------------------------------------------------------------------- */
  {
    int ctx_idx;
    uint64_t h = fnv1a_init();

    QueryPerformanceCounter(&t0);
    for (ctx_idx = 0; ctx_idx < 150; ctx_idx++) {
      static const char *const k_whitelist[] = {
        "math", "table", "string", "ipairs", "pairs", "rawget", "rawset",
        "setmetatable", "getmetatable", "print", "type", "tostring", "tonumber",
        "GameInfo", "Events", "ContextPtr", "Map", "PlotTypes", "TerrainTypes",
        "FeatureTypes", "DirectionTypes", "ResourceUsageTypes", "GameDefines",
        "include", NULL
      };
      int wi;
      lua_State *co = G.lua_newthread(L);
      /* Reproduces Civ 5 _CreateThread (0x6ab1e0) + 0x470250:
       * Isolated environment table T with NO metatable (NO __index = _G!),
       * populated ONLY by shallow-copying whitelisted standard globals. */
      lua_newtable(co);
      for (wi = 0; k_whitelist[wi] != NULL; wi++) {
        G.lua_getfield(co, LUA_GLOBALSINDEX, k_whitelist[wi]);
        G.lua_setfield(co, 1, k_whitelist[wi]);
      }
      G.lua_pushvalue(co, 1);
      G.lua_replace(co, LUA_GLOBALSINDEX); /* set co->env = T */
      /* env table T is at index 1 on co's stack */

      /* Load FLuaVector.lua, IconSupport.lua, InstanceManager.lua, MapmakerUtilities.lua, AssignStartingPlots.lua */
      if (G.luaL_loadbuffer(co, buf_vec, sz_vec, "@FLuaVector.lua") != 0 ||
          (G.lua_pushvalue(co, 1), G.lua_setfenv(co, -2), G.lua_pcall(co, 0, 0, 0)) != 0) {
        fprintf(stderr, "W6 Vec err: %s\n", G.lua_tolstring(co, -1, NULL));
        return 0;
      }

      if (G.luaL_loadbuffer(co, buf_icon, sz_icon, "@IconSupport.lua") != 0 ||
          (G.lua_pushvalue(co, 1), G.lua_setfenv(co, -2), G.lua_pcall(co, 0, 0, 0)) != 0) {
        fprintf(stderr, "W6 IconSupport err: %s\n", G.lua_tolstring(co, -1, NULL));
        return 0;
      }

      if (G.luaL_loadbuffer(co, buf_im, sz_im, "@InstanceManager.lua") != 0 ||
          (G.lua_pushvalue(co, 1), G.lua_setfenv(co, -2), G.lua_pcall(co, 0, 0, 0)) != 0) {
        fprintf(stderr, "W6 InstanceManager err: %s\n", G.lua_tolstring(co, -1, NULL));
        return 0;
      }

      if (G.luaL_loadbuffer(co, buf_mmu, sz_mmu, "@MapmakerUtilities.lua") != 0 ||
          (G.lua_pushvalue(co, 1), G.lua_setfenv(co, -2), G.lua_pcall(co, 0, 0, 0)) != 0) {
        fprintf(stderr, "W6 MMU err: %s\n", G.lua_tolstring(co, -1, NULL));
        return 0;
      }

      if (G.luaL_loadbuffer(co, buf_asp, sz_asp, "@AssignStartingPlots.lua") != 0 ||
          (G.lua_pushvalue(co, 1), G.lua_setfenv(co, -2), G.lua_pcall(co, 0, 0, 0)) != 0) {
        fprintf(stderr, "W6 ASP err: %s\n", G.lua_tolstring(co, -1, NULL));
        return 0;
      }

      /* Execute IconLookup(23, 64, "CIV_COLOR_ATLAS") inside the isolated UI thread (like UniqueBonuses.lua:13) */
      G.lua_getfield(co, 1, "IconLookup");
      G.lua_pushinteger(co, 23);
      G.lua_pushinteger(co, 64);
      G.lua_pushstring(co, "CIV_COLOR_ATLAS");
      if (G.lua_pcall(co, 3, 2, 0) != 0) {
        fprintf(stderr, "W6 IconLookup runtime err: %s\n", G.lua_tolstring(co, -1, NULL));
        return 0;
      }
      h = fnv1a_u64(h, (uint64_t)G.lua_type(co, -2));
      h = fnv1a_u64(h, (uint64_t)G.lua_objlen(co, -1));
      lua_pop(co, 2);

      /* Verify AssignStartingPlots is defined in context env */
      G.lua_getfield(co, 1, "AssignStartingPlots");
      h = fnv1a_u64(h, (uint64_t)G.lua_type(co, -1));
      lua_pop(co, 2); /* pop AssignStartingPlots and env table */

      lua_pop(L, 1); /* pop thread from main stack */
    }
    QueryPerformanceCounter(&t1);
    out->ms[5] = qpc_ms(t0, t1, freq);
    out->hash[5] = h;
  }

  /* -----------------------------------------------------------------------
   * Workload 7: Real InstanceManager.lua (160 UI scripts)
   * Simulates YieldIconManager / UnitFlagManager allocating & releasing 600
   * instances across camera pans (where ReleaseInstance is O(N^2) in Lua)
   * ----------------------------------------------------------------------- */
  {
    const char *w7_code =
      "local im = InstanceManager:new('YieldImageInstance', 'Anchor', nil)\n"
      "local gim = GenerationalInstanceManager:new('GenInstance', 'Anchor', nil)\n"
      "local active = {}\n"
      "local checksum = 0\n"
      "for pass = 1, 12 do\n"
      "  for i = 1, 500 do\n"
      "    active[i] = im:GetInstance()\n"
      "  end\n"
      "  checksum = checksum + im.m_iAllocatedInstances * 1000 + im.m_iAvailableInstances\n"
      "  for i = 1, 500 do\n"
      "    im:ReleaseInstance(active[i])\n"
      "  end\n"
      "  checksum = checksum + im.m_iAllocatedInstances * 100 + im.m_iAvailableInstances\n"
      "  for i = 1, 200 do\n"
      "    active[i] = im:GetInstance()\n"
      "  end\n"
      "  im:ResetInstances()\n"
      "  checksum = checksum + im.m_iAvailableInstances + im.m_iAllocatedInstances\n"
      "  for i = 1, 200 do\n"
      "    local g = gim:GetInstance()\n"
      "    if not g.Anchor.hidden then checksum = checksum + 1 end\n"
      "  end\n"
      "  gim:ResetInstances()\n"
      "  checksum = checksum + gim.m_NextInstanceIndex\n"
      "end\n"
      "return checksum\n";

    if (G.luaL_loadbuffer(L, buf_im, sz_im, "@InstanceManager.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "IM load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w7_code, strlen(w7_code), "=[workload7]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W7 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[6] = qpc_ms(t0, t1, freq);
    out->hash[6] = fnv1a_u64(fnv1a_init(), (uint64_t)G.lua_tointeger(L, -1));
    lua_pop(L, 1);
  }

  /* -----------------------------------------------------------------------
   * Workload 8: Real IconSupport.lua + FLuaVector.lua (186 UI scripts)
   * Simulates UnitFlagManager + CityBannerManager + TopPanel calling
   * IconLookup, IconHookup, Vector2, Vector4, Color, VecAdd, VecSubtract
   * ----------------------------------------------------------------------- */
  {
    const char *w8_code =
      "for ui_ctx = 1, 186 do\n"
      "  g_IconAtlases = nil\n"
      "  PopulateIconAtlases()\n"
      "end\n"
      "local ctrl = {\n"
      "  tex_len = 0, ox = 0, oy = 0,\n"
      "  SetTexture = function(self, t) self.tex_len = #t end,\n"
      "  SetTextureOffsetVal = function(self, x, y) self.ox = x; self.oy = y end,\n"
      "}\n"
      "local atlases = { 'CIV_COLOR_ATLAS', 'UNIT_FLAG_ATLAS', 'TECH_ATLAS_1' }\n"
      "local sizes   = { 64, 32, 64 }\n"
      "local sum = 0\n"
      "for iter = 1, 2500 do\n"
      "  for k = 1, 3 do\n"
      "    local idx = (iter + k * 7) % 48\n"
      "    local uv, fname = IconLookup(idx, sizes[k], atlases[k])\n"
      "    if uv then sum = sum + uv.x + uv.y + #fname end\n"
      "    if IconHookup(idx, sizes[k], atlases[k], ctrl) then\n"
      "      sum = sum + ctrl.ox + ctrl.oy + ctrl.tex_len\n"
      "    end\n"
      "  end\n"
      "  local c1 = Color(iter % 255, (iter * 3) % 255, (iter * 7) % 255, 255)\n"
      "  local c2 = Vector4(10, 20, 30, 40)\n"
      "  local ca = VecAdd(c1, c2)\n"
      "  local cs = VecSubtract(ca, c2)\n"
      "  sum = sum + cs.x + cs.y + cs.z + cs.w\n"
      "end\n"
      "return sum\n";

    if (G.luaL_loadbuffer(L, buf_vec, sz_vec, "@FLuaVector.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "FLuaVector load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    if (G.luaL_loadbuffer(L, buf_icon, sz_icon, "@IconSupport.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "IconSupport load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w8_code, strlen(w8_code), "=[workload8]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W8 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[7] = qpc_ms(t0, t1, freq);
    out->hash[7] = fnv1a_u64(fnv1a_init(), (uint64_t)G.lua_tointeger(L, -1));
    lua_pop(L, 1);
  }

  /* -----------------------------------------------------------------------
   * Workload 9: Real AssignStartingPlots.lua Regional Division & Hex Math
   * (DivideIntoRegions, ChopIntoThreeRegions, ChopIntoTwoRegions, RemoveDeadRows,
   *  ApplyHexAdjustment across 22-civ Huge map regional subdivisions)
   * ----------------------------------------------------------------------- */
  {
    const char *w9_code =
      "local iW, iH = Map.GetGridSize()\n"
      "local base_fert = {}\n"
      "local total_fert = 0\n"
      "for y = 0, iH - 1 do\n"
      "  for x = 0, iW - 1 do\n"
      "    local f = 0\n"
      "    if x >= 4 and x < iW - 4 and y >= 3 and y < iH - 3 then\n"
      "      f = ((x * 13 + y * 29) % 7)\n"
      "    end\n"
      "    base_fert[y * iW + x + 1] = f\n"
      "    total_fert = total_fert + f\n"
      "  end\n"
      "end\n"
      "local asp = {\n"
      "  DivideIntoRegions = AssignStartingPlots.DivideIntoRegions,\n"
      "  ChopIntoThreeRegions = AssignStartingPlots.ChopIntoThreeRegions,\n"
      "  ChopIntoTwoRegions = AssignStartingPlots.ChopIntoTwoRegions,\n"
      "  RemoveDeadRows = AssignStartingPlots.RemoveDeadRows,\n"
      "  ApplyHexAdjustment = AssignStartingPlots.ApplyHexAdjustment,\n"
      "  firstRingYIsEven = {{0, 1}, {1, 0}, {0, -1}, {-1, -1}, {-1, 0}, {-1, 1}},\n"
      "  firstRingYIsOdd = {{1, 1}, {1, 0}, {1, -1}, {0, -1}, {-1, 0}, {0, 1}},\n"
      "  regionData = {},\n"
      "}\n"
      "local sum = 0\n"
      "for pass = 1, 20 do\n"
      "  asp.regionData = {}\n"
      "  local rect = { 0, 0, iW, iH, 1, total_fert, iW * iH }\n"
      "  asp:DivideIntoRegions(22, base_fert, rect)\n"
      "  for r_idx, r in ipairs(asp.regionData) do\n"
      "    sum = sum + r[1] * 3 + r[2] * 5 + r[3] * 7 + r[4] * 11 + r[6] + r[7]\n"
      "  end\n"
      "  for h = 1, 300 do\n"
      "    local adj = asp.firstRingYIsOdd[(h % 6) + 1]\n"
      "    local ax, ay = asp:ApplyHexAdjustment(h % iW, h % iH, adj)\n"
      "    sum = sum + ax + ay\n"
      "  end\n"
      "end\n"
      "return sum\n";

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w9_code, strlen(w9_code), "=[workload9]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W9 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[8] = qpc_ms(t0, t1, freq);
    out->hash[8] = fnv1a_u64(fnv1a_init(), (uint64_t)G.lua_tointeger(L, -1));
    lua_pop(L, 1);
  }

  /* -----------------------------------------------------------------------
   * Workload 10: Real MultilayeredFractal.lua Regional Shift & Layering
   * (Included by 36 map scripts: ShiftRegionPlots, DetermineXShift,
   *  DetermineYShift, ShiftRegionPlotsBy across 40 regional layers)
   * ----------------------------------------------------------------------- */
  {
    const char *w10_code =
      "local mlf = MultilayeredFractal.Create()\n"
      "local rw, rh = 96, 60\n"
      "for layer = 1, 30 do\n"
      "  mlf.plotTypes = table.fill(PlotTypes.PLOT_OCEAN, rw * rh)\n"
      "  for y = 0, rh - 1 do\n"
      "    for x = 0, rw - 1 do\n"
      "      if ((x + layer * 5) % 17) > 6 and ((y + layer * 3) % 13) > 4 then\n"
      "        mlf.plotTypes[y * rw + x + 1] = PlotTypes.PLOT_LAND\n"
      "      end\n"
      "    end\n"
      "  end\n"
      "  mlf:ShiftRegionPlots(rw, rh)\n"
      "  for x = 0, rw - 1 do\n"
      "    for y = 0, rh - 1 do\n"
      "      local i = y * rw + x + 1\n"
      "      if mlf.plotTypes[i] ~= PlotTypes.PLOT_OCEAN then\n"
      "        mlf.wholeworldPlotTypes[y * mlf.iW + x + 1] = mlf.plotTypes[i]\n"
      "      end\n"
      "    end\n"
      "  end\n"
      "end\n"
      "return mlf.wholeworldPlotTypes\n";
    uint64_t h;

    if (G.luaL_loadbuffer(L, buf_mlf, sz_mlf, "@MultilayeredFractal.lua") != 0 ||
        G.lua_pcall(L, 0, 0, 0) != 0) {
      fprintf(stderr, "MLF load error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }

    QueryPerformanceCounter(&t0);
    if (G.luaL_loadbuffer(L, w10_code, strlen(w10_code), "=[workload10]") != 0 ||
        G.lua_pcall(L, 0, 1, 0) != 0) {
      fprintf(stderr, "W10 error: %s\n", G.lua_tolstring(L, -1, NULL));
      return 0;
    }
    QueryPerformanceCounter(&t1);
    out->ms[9] = qpc_ms(t0, t1, freq);
    h = hash_lua_array(L, G.lua_gettop(L), s_map_w * s_map_h, fnv1a_init());
    lua_pop(L, 1);
    out->hash[9] = h;
  }

  /* -----------------------------------------------------------------------
   * Bonus Feasibility Microbenchmark: Naive lua2c C-API vs LuaJIT Trace
   * ----------------------------------------------------------------------- */
  {
    const char *setup_code =
      "bench_tab = {}\n"
      "for i = 1, 1000 do bench_tab[i] = (i * 19) % 100 end\n"
      "function luajit_sum_clamped(t, n)\n"
      "  local s = 0\n"
      "  local clamp = math.clamp\n"
      "  for iter = 1, 500 do\n"
      "    for i = 1, n do s = s + clamp(t[i], 10, 90) end\n"
      "  end\n"
      "  return s\n"
      "end\n"
      "function lua2c_driver(t, n)\n"
      "  local s = 0\n"
      "  local fn = naive_lua2c_sum_clamped\n"
      "  for iter = 1, 500 do\n"
      "    s = s + fn(t, n)\n"
      "  end\n"
      "  return s\n"
      "end\n";
    lua_pushcfunction(L, naive_lua2c_sum_clamped);
    G.lua_setfield(L, LUA_GLOBALSINDEX, "naive_lua2c_sum_clamped");
    G.luaL_loadbuffer(L, setup_code, strlen(setup_code), "=[lua2c_bench]");
    G.lua_pcall(L, 0, 0, 0);

    QueryPerformanceCounter(&t0);
    G.lua_getfield(L, LUA_GLOBALSINDEX, "luajit_sum_clamped");
    G.lua_getfield(L, LUA_GLOBALSINDEX, "bench_tab");
    G.lua_pushinteger(L, 1000);
    G.lua_pcall(L, 2, 1, 0);
    QueryPerformanceCounter(&t1);
    out->luajit_trace_ms = qpc_ms(t0, t1, freq);
    lua_pop(L, 1);

    QueryPerformanceCounter(&t0);
    G.lua_getfield(L, LUA_GLOBALSINDEX, "lua2c_driver");
    G.lua_getfield(L, LUA_GLOBALSINDEX, "bench_tab");
    G.lua_pushinteger(L, 1000);
    G.lua_pcall(L, 2, 1, 0);
    QueryPerformanceCounter(&t1);
    out->lua2c_ms = qpc_ms(t0, t1, freq);
    lua_pop(L, 1);
  }

  out->suite_rng_calls = s_suite_rng_calls;
  out->alloc_calls = s_alloc_calls;
  G.lua_close(L);
  FreeLibrary(hMod);
  free(buf_mmu);
  free(buf_asp);
  free(buf_fw);
  free(buf_vec);
  free(buf_mlf);
  free(buf_im);
  free(buf_icon);

  printf("=== %s ===\n", label);
  printf("  W1  (0-based ipairs + math.clamp) : %8.2f ms | hash=0x%016llx\n", out->ms[0], out->hash[0]);
  printf("  W2  (table.fill + table.count)    : %8.2f ms | hash=0x%016llx\n", out->ms[1], out->hash[1]);
  printf("  W3  (MapmakerUtilities shuffle)   : %8.2f ms | hash=0x%016llx (RNG calls=%llu)\n", out->ms[2], out->hash[2], out->rng_calls);
  printf("  W4  (AssignStartingPlots ripples) : %8.2f ms | hash=0x%016llx\n", out->ms[3], out->hash[3]);
  printf("  W5  (FractalWorld + ShiftPlots)   : %8.2f ms | hash=0x%016llx\n", out->ms[4], out->hash[4]);
  printf("  W6  (150 UI Context Script Loads) : %8.2f ms | hash=0x%016llx\n", out->ms[5], out->hash[5]);
  printf("  W7  (InstanceManager UI Pooling)  : %8.2f ms | hash=0x%016llx\n", out->ms[6], out->hash[6]);
  printf("  W8  (IconSupport + FLuaVector UI) : %8.2f ms | hash=0x%016llx\n", out->ms[7], out->hash[7]);
  printf("  W9  (ASP 22-Civ Region Division)  : %8.2f ms | hash=0x%016llx\n", out->ms[8], out->hash[8]);
  printf("  W10 (MultilayeredFractal Shift)   : %8.2f ms | hash=0x%016llx\n", out->ms[9], out->hash[9]);
  printf("  Host 0x6ce440 HeapAlloc calls     : %llu\n", out->alloc_calls);
  printf("  Lua/LuaJIT loop vs Naive lua2c    : %8.2f ms vs %8.2f ms (lua2c is %.1fx slower)\n\n",
         out->luajit_trace_ms, out->lua2c_ms,
         out->luajit_trace_ms > 0.0 ? out->lua2c_ms / out->luajit_trace_ms : 0.0);

  return 1;
}

int main(int argc, char **argv)
{
  char env_civ5_dir[MAX_PATH];
  char vanilla_bak1[MAX_PATH], vanilla_bak2[MAX_PATH], vanilla_main[MAX_PATH];
  char exe_dir_dll[MAX_PATH];
  const char *civ5_dir = "C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sid Meier's Civilization V";
  const char *vanilla_dll = NULL;
  const char *custom_dll  = "dist\\lua51_Win32.dll";
  BenchResult r_vanilla, r_luajit_dropin, r_hybrid_native;
  double total_vanilla = 0, total_dropin = 0, total_hybrid = 0;
  int all_match = 1;
  int i;
  DWORD path_len;
  const char *w_names[NUM_WORKLOADS] = {
    "W1:  0-Based ipairs + math.clamp",
    "W2:  table.fill + table.count (Huge)",
    "W3:  MapmakerUtilities Shuffle/Member",
    "W4:  AssignStartingPlots Hex Ripples",
    "W5:  FractalWorld + ShiftPlotTypes",
    "W6:  150 UI Thread Script Loads",
    "W7:  InstanceManager UI Pool (160 UI)",
    "W8:  IconSupport + FLuaVector (186 UI)",
    "W9:  ASP 22-Civ Region Division",
    "W10: MultilayeredFractal Shift (36 Map)"
  };

  if (argc >= 2 && argv[1][0] != '\0') {
    path_len = GetFullPathNameA(argv[1], MAX_PATH, exe_dir_dll, NULL);
    if (!path_len || path_len >= MAX_PATH) { fprintf(stderr, "Custom DLL path too long or invalid\n"); return 1; }
    custom_dll = exe_dir_dll;
  } else if (GetFileAttributesA("dist\\lua51_Win32.dll") != INVALID_FILE_ATTRIBUTES) {
    path_len = GetFullPathNameA("dist\\lua51_Win32.dll", MAX_PATH, exe_dir_dll, NULL);
    if (!path_len || path_len >= MAX_PATH) return 1;
    custom_dll = exe_dir_dll;
  } else if (GetFileAttributesA("..\\dist\\lua51_Win32.dll") != INVALID_FILE_ATTRIBUTES) {
    path_len = GetFullPathNameA("..\\dist\\lua51_Win32.dll", MAX_PATH, exe_dir_dll, NULL);
    if (!path_len || path_len >= MAX_PATH) return 1;
    custom_dll = exe_dir_dll;
  } else if ((path_len = GetModuleFileNameA(NULL, exe_dir_dll, MAX_PATH)) > 0 && path_len < MAX_PATH) {
    char candidate[MAX_PATH];
    char *slash = strrchr(exe_dir_dll, '\\');
    if (slash) {
      *slash = '\0';
      if (!format_path(candidate, sizeof(candidate), exe_dir_dll, "\\lua51_Win32.dll")) return 1;
      if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
        strcpy(exe_dir_dll, candidate);
        custom_dll = exe_dir_dll;
      } else {
        if (!format_path(candidate, sizeof(candidate), exe_dir_dll, "\\..\\dist\\lua51_Win32.dll")) return 1;
        if (GetFileAttributesA(candidate) != INVALID_FILE_ATTRIBUTES) {
          path_len = GetFullPathNameA(candidate, MAX_PATH, exe_dir_dll, NULL);
          if (!path_len || path_len >= MAX_PATH) return 1;
          custom_dll = exe_dir_dll;
        }
      }
    }
  }

  if (argc >= 3 && argv[2][0] != '\0') {
    civ5_dir = argv[2];
  } else {
    path_len = GetEnvironmentVariableA("CIV5_DIR", env_civ5_dir, MAX_PATH);
    if (path_len >= MAX_PATH) { fprintf(stderr, "CIV5_DIR is too long\n"); return 1; }
    if (path_len > 0) civ5_dir = env_civ5_dir;
  }

  if (!format_path(vanilla_bak1, sizeof(vanilla_bak1), civ5_dir, "\\lua51_Win32.dll.vanilla.bak") ||
      !format_path(vanilla_bak2, sizeof(vanilla_bak2), civ5_dir, "\\lua51_Win32.dll.vanilla_backup") ||
      !format_path(vanilla_main, sizeof(vanilla_main), civ5_dir, "\\lua51_Win32.dll")) {
    fprintf(stderr, "Civ 5 path too long\n");
    return 1;
  }
  if (GetFileAttributesA(vanilla_bak1) != INVALID_FILE_ATTRIBUTES)
    vanilla_dll = vanilla_bak1;
  else if (GetFileAttributesA(vanilla_bak2) != INVALID_FILE_ATTRIBUTES)
    vanilla_dll = vanilla_bak2;
  else
    vanilla_dll = vanilla_main;

  {
    int same = files_equal(vanilla_dll, custom_dll);
    if (same != 0) {
      fprintf(stderr, same > 0 ? "Baseline and custom DLL are identical; a vanilla baseline is required.\n" : "Could not compare baseline and custom DLL files.\n");
      return 1;
    }
  }

  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  s_civ5_heap = HeapCreate(0, 0, 0);

  printf("================================================================================\n");
  printf("Civ 5 Isolated Lua / LuaJIT / Native C Accelerator Benchmark & Equivalence Suite\n");
  printf("================================================================================\n\n");

  if (!run_suite(vanilla_dll, civ5_dir, 0,
                 "1. Vanilla Firaxis lua51_Win32.dll (Lua 5.1.4 Interpreter)",
                 &r_vanilla))
    return 1;

  if (!run_suite(custom_dll, civ5_dir, 0,
                 "2. Legacy Drop-In LuaJIT 2.1 (MPPatch-style, Unpatched Host Hooks)",
                 &r_luajit_dropin))
    return 1;

  if (!run_suite(custom_dll, civ5_dir, 1,
                 "3. Hybrid LuaJIT 2.1 + Civ 5 Native C Accelerator (dist\\lua51_Win32.dll)",
                 &r_hybrid_native))
    return 1;

  printf("================================================================================\n");
  printf("%-38s | %9s | %9s | %9s | %8s | %8s | %s\n",
         "Workload", "Vanilla", "LuaJIT", "Hybrid C", "vsVanil", "vsLuaJIT", "Equiv?");
  printf("--------------------------------------------------------------------------------\n");
  for (i = 0; i < NUM_WORKLOADS; i++) {
    int eq = (r_vanilla.hash[i] == r_luajit_dropin.hash[i]) &&
             (r_vanilla.hash[i] == r_hybrid_native.hash[i]);
    if (!eq) all_match = 0;
    total_vanilla += r_vanilla.ms[i];
    total_dropin  += r_luajit_dropin.ms[i];
    total_hybrid  += r_hybrid_native.ms[i];
    printf("%-38s | %7.2fms | %7.2fms | %7.2fms | %7.2fx | %7.2fx | %s\n",
           w_names[i],
           r_vanilla.ms[i],
           r_luajit_dropin.ms[i],
           r_hybrid_native.ms[i],
           r_vanilla.ms[i] / r_hybrid_native.ms[i],
           r_luajit_dropin.ms[i] / r_hybrid_native.ms[i],
           eq ? "EXACT MATCH" : "MISMATCH!");
  }
  if (r_vanilla.suite_rng_calls != r_luajit_dropin.suite_rng_calls ||
      r_vanilla.suite_rng_calls != r_hybrid_native.suite_rng_calls) {
    fprintf(stderr, "Suite Map.Rand call counts differ: %llu / %llu / %llu\n",
            r_vanilla.suite_rng_calls, r_luajit_dropin.suite_rng_calls, r_hybrid_native.suite_rng_calls);
    all_match = 0;
  }
  printf("--------------------------------------------------------------------------------\n");
  printf("%-38s | %7.2fms | %7.2fms | %7.2fms | %7.2fx | %7.2fx | %s\n",
         "TOTAL SUITE TIME (10 WORKLOADS)",
         total_vanilla, total_dropin, total_hybrid,
         total_vanilla / total_hybrid,
         total_dropin / total_hybrid,
         all_match ? "100% EXACT" : "FAILED!");
  printf("================================================================================\n");

  HeapDestroy(s_civ5_heap);
  return all_match ? 0 : 2;
}
