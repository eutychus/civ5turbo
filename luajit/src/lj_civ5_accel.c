/*
** Civilization V Native C Accelerator & LuaJIT Bridge
**
** 1. Restores missing vanilla Lua 5.1 export `lua_setlevel` for 100% export parity
**    with Firaxis's `lua51_Win32.dll` (123/123 symbols).
** 2. Redirects `lua_newstate` away from Civ 5's slow `HeapAlloc+HeapSize+memcpy+HeapFree`
**    allocator (`0x6ce440`) to LuaJIT's internal `lj_alloc_f` slab allocator.
** 3. Intercepts Civ 5's JIT-aborting C closures registered in `0x7a5b30`:
**    - `_G.ipairs` (`0x48da50` / `0x74c920`) -> pure-Lua 0-based `rawget` closure (JIT-compiled!)
**    - `table.fill` (`0x66df70`) -> direct C `GCtab` array block fill
**    - `table.count` (`0x4aac00`) -> direct C `GCtab` array + hash node scan
**    - `math.clamp` (`0x5221f0`) -> pure-Lua JIT-inlined closure
** 4. Caches compiled `GCproto*` prototypes in `LUA_REGISTRYINDEX` across Civ 5's
**    263+ `lua_newthread` UI/gameplay contexts to eliminate redundant parsing.
** 5. Replaces hot $O(N^2)$ and hex-ring Lua bottlenecks in `MapmakerUtilities.lua`
**    (`GetShuffledCopyOfTable`, `TestMembership`, `IdentifyTableIndex`) and
**    `AssignStartingPlots.lua` (`PlaceResourceImpact`, `PlaceImpactAndRipples`)
**    with 100% RNG- and output-equivalent native C implementations.
*/

#define lj_civ5_accel_c
#define LUA_CORE

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "lua.h"
#include "lauxlib.h"
#include "luajit.h"

#include "lj_obj.h"
#include "lj_gc.h"
#include "lj_err.h"
#include "lj_str.h"
#include "lj_tab.h"
#include "lj_func.h"
#include "lj_state.h"
#include "lj_lib.h"
#include "lj_frame.h"
#include "lj_debug.h"
#include "lj_civ5_accel.h"

#if defined(_MSC_VER) && _MSC_VER < 1900
#define civ5_snprintf _snprintf
#else
#define civ5_snprintf snprintf
#endif

/* -- 1. Missing Lua 5.1.4 export in vanilla LuaJIT 2.1 ------------------- */

LUA_API void lua_setlevel(lua_State *from, lua_State *to)
{
  UNUSED(from);
  UNUSED(to);
}

/* -- 2. Runtime enable/disable check (CIV5_NATIVE_ACCEL) ----------------- */

static int s_civ5_accel_enabled = 1;
static int s_civ5_accel_checked = 0;

int lj_civ5_accel_check_newstate(void)
{
  char buf[16];
  DWORD n = GetEnvironmentVariableA("CIV5_NATIVE_ACCEL", buf, sizeof(buf));
  if (n > 0 && buf[0] == '0') {
    s_civ5_accel_enabled = 0;
  } else {
    s_civ5_accel_enabled = 1;
  }
  s_civ5_accel_checked = 1;
  return s_civ5_accel_enabled;
}

int lj_civ5_accel_enabled(void)
{
  if (!s_civ5_accel_checked)
    return lj_civ5_accel_check_newstate();
  return s_civ5_accel_enabled;
}

/* -- 3. Fast C implementations of Civ 5 C helpers ------------------------ */

static int lj_civ5_table_fill(lua_State *L)
{
  int nargs = (int)(L->top - L->base);
  if (nargs >= 3 && tvistab(L->base)) {
    /* 3-arg form: table.fill(t, val, count) */
    GCtab *t = tabV(L->base);
    cTValue *val = L->base + 1;
    int32_t count = lj_lib_checkint(L, 3);
    if (count > 0) {
      TValue v;
      int32_t i;
      copyTV(L, &v, val);
      if ((MSize)count >= t->asize)
        lj_tab_reasize(L, t, (uint32_t)count);
      for (i = 1; i <= count; i++) {
        TValue *slot = lj_tab_setint(L, t, i);
        copyTV(L, slot, &v);
      }
      if (tvisgcv(&v))
        lj_gc_anybarriert(L, t);
    }
    settabV(L, L->top++, t);
    return 1;
  } else {
    /* Civ 5 standard 2-arg form (0x66df70): table.fill(val, count) */
    int32_t count = lj_lib_checkint(L, 2);
    cTValue *val = L->base;
    uint32_t asize = count > 0 ? (uint32_t)count : 0;
    GCtab *t = lj_tab_new_ah(L, asize, 0);
    if (count > 0) {
      TValue *array = tvref(t->array);
      TValue v;
      int32_t i;
      copyTV(L, &v, val);
      for (i = 1; i <= count; i++) {
        copyTV(L, &array[i], &v);
      }
      if (tvisgcv(&v))
        lj_gc_anybarriert(L, t);
    }
    settabV(L, L->top++, t);
    lj_gc_check(L);
    return 1;
  }
}

static int lj_civ5_table_count(lua_State *L)
{
  GCtab *t = lj_lib_checktab(L, 1);
  TValue *array = tvref(t->array);
  MSize i, asize = t->asize, hmask = t->hmask;
  int32_t count = 0;
  for (i = 0; i < asize; i++) {
    if (!tvisnil(&array[i]))
      count++;
  }
  if (hmask > 0 || !tvisnil(&noderef(t->node)[0].val)) {
    Node *node = noderef(t->node);
    for (i = 0; i <= hmask; i++) {
      if (!tvisnil(&node[i].val))
        count++;
    }
  }
  setintV(L->top++, count);
  return 1;
}

/* -- 4. Native C accelerators for MapmakerUtilities.lua ------------------ */

static int32_t civ5_tv_to_int(cTValue *tv)
{
  if (!tv || tvisnil(tv)) return 0;
  if (tvisint(tv)) return intV(tv);
  if (tvisnum(tv)) return (int32_t)numV(tv);
  return 0;
}

static double civ5_tv_to_num(cTValue *tv)
{
  if (!tv || tvisnil(tv)) return 0.0;
  if (tvisint(tv)) return (double)intV(tv);
  if (tvisnum(tv)) return numV(tv);
  return 0.0;
}

static int32_t civ5_pos_mod(int32_t a, int32_t m)
{
  int32_t r;
  if (m <= 0) return 0;
  r = a % m;
  if (r < 0) r += m;
  return r;
}

static int32_t civ5_tab_maxn(GCtab *t)
{
  TValue *array = tvref(t->array);
  Node *node;
  lua_Number m = 0;
  ptrdiff_t i;
  for (i = (ptrdiff_t)t->asize - 1; i >= 0; i--) {
    if (!tvisnil(&array[i])) {
      m = (lua_Number)(int32_t)i;
      break;
    }
  }
  node = noderef(t->node);
  for (i = (ptrdiff_t)t->hmask; i >= 0; i--) {
    if (!tvisnil(&node[i].val) && tvisnumber(&node[i].key)) {
      lua_Number n = numberVnum(&node[i].key);
      if (n > m) m = n;
    }
  }
  return (int32_t)m;
}

static int lj_civ5_TestMembership(lua_State *L)
{
  GCtab *t = lj_lib_checktab(L, 1);
  cTValue *val = L->base + 1;
  if (val < L->top && !tvisnil(val)) {
    TValue *array = tvref(t->array);
    MSize i, asize = t->asize;
    if (!tviszero(val) && !(tvisnum(val) && tvisnan(val))) {
      uint64_t target = val->u64;
      for (i = 0; i < asize; i++) {
        if (array[i].u64 == target) {
          setboolV(L->top++, 1);
          return 1;
        }
      }
    } else {
      for (i = 0; i < asize; i++) {
        if (!tvisnil(&array[i]) && lj_obj_equal(&array[i], val)) {
          setboolV(L->top++, 1);
          return 1;
        }
      }
    }
    if (t->hmask > 0 || !tvisnil(&noderef(t->node)[0].val)) {
      Node *node = noderef(t->node);
      MSize hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
        if (!tvisnil(&node[i].val) && lj_obj_equal(&node[i].val, val)) {
          setboolV(L->top++, 1);
          return 1;
        }
      }
    }
  }
  setboolV(L->top++, 0);
  return 1;
}

static int lj_civ5_IdentifyTableIndex(lua_State *L)
{
  GCtab *t = lj_lib_checktab(L, 1);
  cTValue *val = L->base + 1;
  int found = 0;
  int32_t count = 0;
  GCtab *indices = lj_tab_new(L, 0, 0);
  settabV(L, L->top++, indices); /* Root on stack */
  if (val < L->top - 1 && !tvisnil(val)) {
    TValue *array = tvref(t->array);
    MSize i, asize = t->asize;
    for (i = 0; i < asize; i++) {
      if (!tvisnil(&array[i]) && lj_obj_equal(&array[i], val)) {
        TValue *dst;
        found = 1;
        count++;
        dst = lj_tab_setint(L, indices, count);
        setintV(dst, (int32_t)i);
      }
    }
    if (t->hmask > 0 || !tvisnil(&noderef(t->node)[0].val)) {
      Node *node = noderef(t->node);
      MSize hmask = t->hmask;
      for (i = 0; i <= hmask; i++) {
        if (!tvisnil(&node[i].val) && lj_obj_equal(&node[i].val, val)) {
          TValue *dst;
          found = 1;
          count++;
          dst = lj_tab_setint(L, indices, count);
          copyTV(L, dst, &node[i].key);
          if (tvisgcv(&node[i].key))
            lj_gc_barriert(L, indices, dst);
        }
      }
    }
  }
  /* Return bFoundValue, iNumTimesFoundValue, table_of_indices */
  setboolV(L->top - 1, found);
  setintV(L->top++, count);
  settabV(L, L->top++, indices);
  lj_gc_check(L);
  return 3;
}

static void civ5_push_env_global(lua_State *L, const char *name)
{
  /* 1. Fast path: check LUA_GLOBALSINDEX (L->env) */
  lua_getfield(L, LUA_GLOBALSINDEX, name);
  if (!lua_isnil(L, -1))
    return;
  lua_pop(L, 1);
  /* 2. Check Lua caller function environment (stack level 1) */
  {
    lua_Debug ar;
    if (lua_getstack(L, 1, &ar)) {
      lua_getinfo(L, "f", &ar);
      if (lua_isfunction(L, -1)) {
        lua_getfenv(L, -1);
        lua_remove(L, -2);
        if (lua_istable(L, -1)) {
          lua_getfield(L, -1, name);
          if (!lua_isnil(L, -1)) {
            lua_remove(L, -2);
            return;
          }
          lua_pop(L, 1);
        }
        lua_pop(L, 1);
      } else {
        lua_pop(L, 1);
      }
    }
  }
  /* 3. Check mainthread _G environment */
  {
    GCtab *g = tabref(mainthread(G(L))->env);
    if (g && g != tabref(L->env)) {
      settabV(L, L->top++, g);
      lua_getfield(L, -1, name);
      if (!lua_isnil(L, -1)) {
        lua_remove(L, -2);
        return;
      }
      lua_pop(L, 2);
    }
  }
  lua_pushnil(L);
}

static int lj_civ5_GetShuffledCopyOfTable(lua_State *L)
{
  GCtab *in_tab = lj_lib_checktab(L, 1);
  int32_t len = civ5_tab_maxn(in_tab);
  GCtab *out_tab;
  TValue stack_buf[256];
  int32_t stack_tree[257];
  TValue *buf = stack_buf;
  int32_t *tree = stack_tree;
  int32_t loop, left_to_do = 0, top_bit = 1;
  int has_gcv = 0;
  int func_idx, msg_idx, out_idx;

  if (len <= 0) {
    out_tab = lj_tab_new(L, 0, 0);
    settabV(L, L->top++, out_tab);
    lj_gc_check(L);
    return 1;
  }

  if (len > 256) {
    buf = (TValue *)malloc((size_t)len * sizeof(TValue));
    tree = (int32_t *)malloc((size_t)(len + 1) * sizeof(int32_t));
    if (!buf || !tree) {
      if (buf && buf != stack_buf) free(buf);
      if (tree && tree != stack_tree) free(tree);
      lj_err_mem(L);
    }
  }

  while ((top_bit << 1) <= len)
    top_bit <<= 1;

  for (loop = 1; loop <= len; loop++) {
    cTValue *src = lj_tab_getint(in_tab, loop);
    tree[loop] = loop & (-loop);
    if (src && !tvisnil(src)) {
      copyTV(L, &buf[loop - 1], src);
      left_to_do = loop;
      if (tvisgcv(src)) has_gcv = 1;
    } else {
      setnilV(&buf[loop - 1]);
    }
  }

  out_tab = lj_tab_new_ah(L, (uint32_t)len, 0);
  settabV(L, L->top++, out_tab);
  out_idx = lua_gettop(L);

  /* Resolve Map.Rand and cache the message string on the stack */
  civ5_push_env_global(L, "Map");
  if (!lua_istable(L, -1) && !lua_isuserdata(L, -1)) {
    if (buf != stack_buf) free(buf);
    if (tree != stack_tree) free(tree);
    luaL_error(L, "Map table not found in GetShuffledCopyOfTable");
  }
  lua_getfield(L, -1, "Rand");
  lua_remove(L, -2); /* remove Map */
  func_idx = lua_gettop(L);

  lua_pushliteral(L, "Shuffling table entry - Lua");
  msg_idx = lua_gettop(L);

  for (loop = 1; loop <= len; loop++) {
    int32_t rand_val, random_index;
    TValue *dst;
    cTValue *fn_tv = L->base + func_idx - 1;

    if (tvisfunc(fn_tv) && iscfunc(funcV(fn_tv)) && funcV(fn_tv)->c.nupvalues == 0) {
      lua_CFunction cfn = funcV(fn_tv)->c.f;
      ptrdiff_t base_ofs = (char *)L->base - (char *)tvref(L->stack);
      ptrdiff_t call_ofs = (char *)L->top - (char *)tvref(L->stack);
      TValue *call_base = L->top;
      setintV(call_base, left_to_do);
      copyTV(L, call_base + 1, L->base + msg_idx - 1);
      L->base = call_base;
      L->top = call_base + 2;
      cfn(L);
      rand_val = civ5_tv_to_int(L->top - 1);
      L->base = (TValue *)((char *)tvref(L->stack) + base_ofs);
      L->top = (TValue *)((char *)tvref(L->stack) + call_ofs);
    } else {
      lua_pushvalue(L, func_idx);
      lua_pushinteger(L, left_to_do);
      lua_pushvalue(L, msg_idx);
      lua_call(L, 2, 1);
      rand_val = (int32_t)lua_tointeger(L, -1);
      lua_pop(L, 1);
    }

    random_index = 1 + rand_val;
    out_tab = tabV(L->base + out_idx - 1);
    dst = lj_tab_setint(L, out_tab, loop);
    if (random_index >= 1 && random_index <= left_to_do) {
      /* O(log N) Fenwick tree select + remove (exact match to table.remove) */
      int32_t idx = 0;
      int32_t bit = top_bit;
      int32_t rem = random_index;
      for (; bit > 0; bit >>= 1) {
        int32_t next = idx + bit;
        if (next <= len && tree[next] < rem) {
          idx = next;
          rem -= tree[next];
        }
      }
      idx++;
      for (bit = idx; bit <= len; bit += bit & (-bit)) {
        tree[bit]--;
      }
      copyTV(L, dst, &buf[idx - 1]);
    } else {
      setnilV(dst);
    }
    left_to_do--;
  }

  if (has_gcv) {
    out_tab = tabV(L->base + out_idx - 1);
    lj_gc_anybarriert(L, out_tab);
  }
  if (buf != stack_buf) free(buf);
  if (tree != stack_tree) free(tree);

  lua_settop(L, out_idx);
  lj_gc_check(L);
  return 1;
}

/* -- 5. Native C accelerators for AssignStartingPlots.lua ---------------- */

static GCtab *civ5_get_subtab(lua_State *L, GCtab *self_tab, const char *field)
{
  GCstr *s = lj_str_newz(L, field);
  cTValue *tv = lj_tab_getstr(self_tab, s);
  if (tv && tvistab(tv))
    return tabV(tv);
  return NULL;
}

static void civ5_read_hex_rings(lua_State *L, GCtab *self_tab,
                                int32_t odd_dx[6], int32_t odd_dy[6],
                                int32_t even_dx[6], int32_t even_dy[6])
{
  static const int32_t def_odd_dx[6]  = { 1,  1,  1,  0, -1,  0};
  static const int32_t def_odd_dy[6]  = { 1,  0, -1, -1,  0,  1};
  static const int32_t def_even_dx[6] = { 0,  1,  0, -1, -1, -1};
  static const int32_t def_even_dy[6] = { 1,  0, -1, -1,  0,  1};
  int i;
  GCtab *odd_tab = civ5_get_subtab(L, self_tab, "firstRingYIsOdd");
  GCtab *even_tab = civ5_get_subtab(L, self_tab, "firstRingYIsEven");

  for (i = 0; i < 6; i++) {
    odd_dx[i] = def_odd_dx[i];
    odd_dy[i] = def_odd_dy[i];
    even_dx[i] = def_even_dx[i];
    even_dy[i] = def_even_dy[i];
    if (odd_tab) {
      cTValue *pair_tv = lj_tab_getint(odd_tab, i + 1);
      if (pair_tv && tvistab(pair_tv)) {
        GCtab *pt = tabV(pair_tv);
        odd_dx[i] = civ5_tv_to_int(lj_tab_getint(pt, 1));
        odd_dy[i] = civ5_tv_to_int(lj_tab_getint(pt, 2));
      }
    }
    if (even_tab) {
      cTValue *pair_tv = lj_tab_getint(even_tab, i + 1);
      if (pair_tv && tvistab(pair_tv)) {
        GCtab *pt = tabV(pair_tv);
        even_dx[i] = civ5_tv_to_int(lj_tab_getint(pt, 1));
        even_dy[i] = civ5_tv_to_int(lj_tab_getint(pt, 2));
      }
    }
  }
}

static void civ5_get_map_info(lua_State *L, int32_t *iW, int32_t *iH,
                              int *wrapX, int *wrapY)
{
  int top = lua_gettop(L);
  civ5_push_env_global(L, "Map");
  if (lua_istable(L, -1) || lua_isuserdata(L, -1)) {
    int map_idx = lua_gettop(L);
    lua_getfield(L, map_idx, "GetGridSize");
    lua_call(L, 0, 2);
    *iW = (int32_t)lua_tointeger(L, -2);
    *iH = (int32_t)lua_tointeger(L, -1);
    lua_pop(L, 2);

    lua_getfield(L, map_idx, "IsWrapX");
    lua_pushvalue(L, map_idx);
    lua_call(L, 1, 1);
    *wrapX = lua_toboolean(L, -1);
    lua_pop(L, 1);

    lua_getfield(L, map_idx, "IsWrapY");
    lua_pushvalue(L, map_idx);
    lua_call(L, 1, 1);
    *wrapY = lua_toboolean(L, -1);
    lua_pop(L, 1);
  } else {
    *iW = 80;
    *iH = 52;
    *wrapX = 1;
    *wrapY = 0;
  }
  lua_settop(L, top);
}

static void civ5_place_resource_impact_core(
    lua_State *L, GCtab *self_tab,
    int32_t x, int32_t y, int32_t impact_table_number, int32_t radius,
    int32_t iW, int32_t iH, int wrapX, int wrapY,
    const int32_t odd_dx[6], const int32_t odd_dy[6],
    const int32_t even_dx[6], const int32_t even_dy[6])
{
  GCtab *target_tab = NULL;
  int32_t impact_value = 99;
  int32_t impactPlotIndex = y * iW + x + 1;

  switch (impact_table_number) {
  case 1: target_tab = civ5_get_subtab(L, self_tab, "strategicData"); break;
  case 2: target_tab = civ5_get_subtab(L, self_tab, "luxuryData"); break;
  case 3: target_tab = civ5_get_subtab(L, self_tab, "bonusData"); break;
  case 4:
    target_tab = civ5_get_subtab(L, self_tab, "fishData");
    impact_value = 1;
    break;
  case 5: target_tab = civ5_get_subtab(L, self_tab, "cityStateData"); break;
  case 6: target_tab = civ5_get_subtab(L, self_tab, "naturalWondersData"); break;
  case 7:
    target_tab = civ5_get_subtab(L, self_tab, "marbleData");
    impact_value = 1;
    break;
  default:
    break;
  }

  if (target_tab) {
    TValue *slot = lj_tab_setint(L, target_tab, impactPlotIndex);
    setintV(slot, impact_value);
  }

  if (radius == 0 || !target_tab)
    return;

  if (radius > 0 && (double)radius < (double)iH / 2.0) {
    int32_t ripple_radius;
    for (ripple_radius = 1; ripple_radius <= radius; ripple_radius++) {
      int32_t ripple_value = radius - ripple_radius + 1;
      int32_t currentX = x - ripple_radius;
      int32_t currentY = y;
      int dir;
      for (dir = 0; dir < 6; dir++) {
        int32_t step;
        for (step = 1; step <= ripple_radius; step++) {
          int is_odd = (currentY & 1) != 0;
          int32_t nextX = currentX + (is_odd ? odd_dx[dir] : even_dx[dir]);
          int32_t nextY = currentY + (is_odd ? odd_dy[dir] : even_dy[dir]);

          if (!wrapX && (nextX < 0 || nextX >= iW)) {
            /* out of bounds X */
          } else if (!wrapY && (nextY < 0 || nextY >= iH)) {
            /* out of bounds Y */
          } else {
            int32_t realX = nextX;
            int32_t realY = nextY;
            int32_t ringPlotIndex;
            TValue *slot;
            if (wrapX) {
              realX %= iW;
              if (realX < 0) realX += iW;
            }
            if (wrapY) {
              realY %= iH;
              if (realY < 0) realY += iH;
            }
            ringPlotIndex = realY * iW + realX + 1;
            slot = lj_tab_setint(L, target_tab, ringPlotIndex);

            if (impact_table_number == 1 || impact_table_number == 2 ||
                impact_table_number == 3 || impact_table_number == 6) {
              int32_t cur = civ5_tv_to_int(slot);
              if (cur > 0) {
                int32_t stronger = cur > ripple_value ? cur : ripple_value;
                int32_t overlap = stronger + 2;
                if (overlap > 50) overlap = 50;
                setintV(slot, overlap);
              } else {
                setintV(slot, ripple_value);
              }
            } else if (impact_table_number == 4) {
              int32_t cur = civ5_tv_to_int(slot);
              if (cur > 0) {
                int32_t stronger = cur > ripple_value ? cur : ripple_value;
                int32_t overlap = stronger + 1;
                if (overlap > 10) overlap = 10;
                setintV(slot, overlap);
              } else {
                setintV(slot, ripple_value);
              }
            } else if (impact_table_number == 5 || impact_table_number == 7) {
              setintV(slot, 1);
            }
          }
          currentX = nextX;
          currentY = nextY;
        }
      }
    }
  }
}

static int lj_civ5_PlaceResourceImpact(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t x = lj_lib_checkint(L, 2);
  int32_t y = lj_lib_checkint(L, 3);
  int32_t impact_table_number = lj_lib_checkint(L, 4);
  int32_t radius = lj_lib_checkint(L, 5);
  int32_t iW, iH;
  int wrapX, wrapY;
  int32_t odd_dx[6], odd_dy[6], even_dx[6], even_dy[6];

  civ5_get_map_info(L, &iW, &iH, &wrapX, &wrapY);
  civ5_read_hex_rings(L, self_tab, odd_dx, odd_dy, even_dx, even_dy);
  civ5_place_resource_impact_core(L, self_tab, x, y, impact_table_number, radius,
                                  iW, iH, wrapX, wrapY,
                                  odd_dx, odd_dy, even_dx, even_dy);
  return 0;
}

static int lj_civ5_PlaceImpactAndRipples(lua_State *L)
{
  static const int32_t ripple_values[8] = {97, 95, 92, 89, 69, 57, 24, 15};
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t x = lj_lib_checkint(L, 2);
  int32_t y = lj_lib_checkint(L, 3);
  int32_t iW, iH;
  int wrapX, wrapY;
  int32_t odd_dx[6], odd_dy[6], even_dx[6], even_dy[6];
  GCtab *dist_tab, *coll_tab, *cs_tab;
  int32_t impactPlotIndex, ripple_radius;

  civ5_get_map_info(L, &iW, &iH, &wrapX, &wrapY);
  civ5_read_hex_rings(L, self_tab, odd_dx, odd_dy, even_dx, even_dy);

  /* 5 resource layer impacts */
  civ5_place_resource_impact_core(L, self_tab, x, y, 1, 0, iW, iH, wrapX, wrapY,
                                  odd_dx, odd_dy, even_dx, even_dy);
  civ5_place_resource_impact_core(L, self_tab, x, y, 2, 3, iW, iH, wrapX, wrapY,
                                  odd_dx, odd_dy, even_dx, even_dy);
  civ5_place_resource_impact_core(L, self_tab, x, y, 3, 3, iW, iH, wrapX, wrapY,
                                  odd_dx, odd_dy, even_dx, even_dy);
  civ5_place_resource_impact_core(L, self_tab, x, y, 4, 3, iW, iH, wrapX, wrapY,
                                  odd_dx, odd_dy, even_dx, even_dy);
  civ5_place_resource_impact_core(L, self_tab, x, y, 6, 4, iW, iH, wrapX, wrapY,
                                  odd_dx, odd_dy, even_dx, even_dy);

  dist_tab = civ5_get_subtab(L, self_tab, "distanceData");
  coll_tab = civ5_get_subtab(L, self_tab, "playerCollisionData");
  cs_tab   = civ5_get_subtab(L, self_tab, "cityStateData");

  impactPlotIndex = y * iW + x + 1;
  if (dist_tab) setintV(lj_tab_setint(L, dist_tab, impactPlotIndex), 99);
  if (coll_tab) setboolV(lj_tab_setint(L, coll_tab, impactPlotIndex), 1);
  if (cs_tab)   setintV(lj_tab_setint(L, cs_tab, impactPlotIndex), 1);

  for (ripple_radius = 1; ripple_radius <= 8; ripple_radius++) {
    int32_t ripple_value = ripple_values[ripple_radius - 1];
    int32_t currentX = x - ripple_radius;
    int32_t currentY = y;
    int dir;
    for (dir = 0; dir < 6; dir++) {
      int32_t step;
      for (step = 1; step <= ripple_radius; step++) {
        int is_odd = (currentY & 1) != 0;
        int32_t nextX = currentX + (is_odd ? odd_dx[dir] : even_dx[dir]);
        int32_t nextY = currentY + (is_odd ? odd_dy[dir] : even_dy[dir]);

        if (!wrapX && (nextX < 0 || nextX >= iW)) {
          /* out of bounds X */
        } else if (!wrapY && (nextY < 0 || nextY >= iH)) {
          /* out of bounds Y */
        } else {
          int32_t realX = nextX;
          int32_t realY = nextY;
          int32_t ringPlotIndex;
          if (wrapX) {
            realX %= iW;
            if (realX < 0) realX += iW;
          }
          if (wrapY) {
            realY %= iH;
            if (realY < 0) realY += iH;
          }
          ringPlotIndex = realY * iW + realX + 1;
          if (dist_tab) {
            TValue *slot = lj_tab_setint(L, dist_tab, ringPlotIndex);
            int32_t cur = civ5_tv_to_int(slot);
            if (cur > 0) {
              int32_t stronger = cur > ripple_value ? cur : ripple_value;
              int32_t overlap = (int32_t)floor((double)stronger * 1.2);
              if (overlap > 97) overlap = 97;
              setintV(slot, overlap);
            } else {
              setintV(slot, ripple_value);
            }
          }
          if (ripple_radius <= 6 && cs_tab) {
            setintV(lj_tab_setint(L, cs_tab, ringPlotIndex), 1);
          }
        }
        currentX = nextX;
        currentY = nextY;
      }
    }
  }
  return 0;
}

static int lj_civ5_ApplyHexAdjustment(lua_State *L)
{
  /* AssignStartingPlots:ApplyHexAdjustment(self, x, y, plot_adjustments) */
  int32_t x = lj_lib_checkint(L, 2);
  int32_t y = lj_lib_checkint(L, 3);
  GCtab *adj = lj_lib_checktab(L, 4);
  int32_t dx = civ5_tv_to_int(lj_tab_getint(adj, 1));
  int32_t dy = civ5_tv_to_int(lj_tab_getint(adj, 2));
  int32_t iW, iH, adj_x, adj_y;
  int wrapX, wrapY;

  civ5_get_map_info(L, &iW, &iH, &wrapX, &wrapY);
  adj_x = wrapX ? civ5_pos_mod(x + dx, iW) : (x + dx);
  adj_y = wrapY ? civ5_pos_mod(y + dy, iH) : (y + dy);
  setintV(L->top++, adj_x);
  setintV(L->top++, adj_y);
  return 2;
}

static GCtab *civ5_remove_dead_rows_core(
    lua_State *L, GCtab *fert_tab,
    int32_t iWestX, int32_t iSouthY, int32_t iWidth, int32_t iHeight,
    int32_t iW, int32_t iH,
    int32_t *out_wx, int32_t *out_sy, int32_t *out_w, int32_t *out_h)
{
  int32_t adjustSouth = 0, adjustNorth = 0, adjustWest = 0, adjustEast = 0;
  int32_t x, y;

  for (y = 0; y < iHeight; y++) {
    int keep = 0;
    for (x = 0; x < iWidth; x++) {
      if (civ5_tv_to_int(lj_tab_getint(fert_tab, y * iWidth + x + 1)) != 0) {
        keep = 1;
        break;
      }
    }
    if (keep) break;
    adjustSouth++;
  }

  for (y = iHeight - 1; y >= 0; y--) {
    int keep = 0;
    for (x = 0; x < iWidth; x++) {
      if (civ5_tv_to_int(lj_tab_getint(fert_tab, y * iWidth + x + 1)) != 0) {
        keep = 1;
        break;
      }
    }
    if (keep) break;
    adjustNorth++;
  }

  for (x = 0; x < iWidth; x++) {
    int keep = 0;
    for (y = 0; y < iHeight; y++) {
      if (civ5_tv_to_int(lj_tab_getint(fert_tab, y * iWidth + x + 1)) != 0) {
        keep = 1;
        break;
      }
    }
    if (keep) break;
    adjustWest++;
  }

  for (x = iWidth - 1; x >= 0; x--) {
    int keep = 0;
    for (y = 0; y < iHeight; y++) {
      if (civ5_tv_to_int(lj_tab_getint(fert_tab, y * iWidth + x + 1)) != 0) {
        keep = 1;
        break;
      }
    }
    if (keep) break;
    adjustEast++;
  }

  if (adjustSouth > 0 || adjustNorth > 0 || adjustWest > 0 || adjustEast > 0) {
    int32_t adj_w = (iWidth - adjustWest) - adjustEast;
    int32_t adj_h = (iHeight - adjustSouth) - adjustNorth;
    uint32_t total = (adj_w > 0 && adj_h > 0) ? (uint32_t)(adj_w * adj_h) : 0;
    GCtab *adj_tab = lj_tab_new_ah(L, total, 0);
    TValue *dst_arr = tvref(adj_tab->array);
    int32_t idx = 1;
    *out_wx = civ5_pos_mod(iWestX + adjustWest, iW);
    *out_sy = civ5_pos_mod(iSouthY + adjustSouth, iH);
    *out_w = adj_w;
    *out_h = adj_h;
    for (y = 0; y < adj_h; y++) {
      for (x = 0; x < adj_w; x++) {
        int32_t src_i = (y + adjustSouth) * iWidth + (x + adjustWest) + 1;
        int32_t val = civ5_tv_to_int(lj_tab_getint(fert_tab, src_i));
        setintV(&dst_arr[idx++], val);
      }
    }
    return adj_tab;
  } else {
    *out_wx = iWestX;
    *out_sy = iSouthY;
    *out_w = iWidth;
    *out_h = iHeight;
    return fert_tab;
  }
}

static int lj_civ5_RemoveDeadRows(lua_State *L)
{
  /* AssignStartingPlots:RemoveDeadRows(self, fertility_table, iWestX, iSouthY, iWidth, iHeight) */
  GCtab *fert_tab = lj_lib_checktab(L, 2);
  int32_t iWestX = lj_lib_checkint(L, 3);
  int32_t iSouthY = lj_lib_checkint(L, 4);
  int32_t iWidth = lj_lib_checkint(L, 5);
  int32_t iHeight = lj_lib_checkint(L, 6);
  int32_t iW, iH, wrapX, wrapY;
  int32_t out_wx, out_sy, out_w, out_h;
  GCtab *res_tab;

  civ5_get_map_info(L, &iW, &iH, &wrapX, &wrapY);
  res_tab = civ5_remove_dead_rows_core(L, fert_tab, iWestX, iSouthY, iWidth, iHeight,
                                       iW, iH, &out_wx, &out_sy, &out_w, &out_h);
  settabV(L, L->top++, res_tab);
  setintV(L->top++, out_wx);
  setintV(L->top++, out_sy);
  setintV(L->top++, out_w);
  setintV(L->top++, out_h);
  lj_gc_check(L);
  return 5;
}

static GCtab *civ5_make_rect_data(lua_State *L, int32_t wx, int32_t sy,
                                  int32_t w, int32_t h, int32_t area_id,
                                  int32_t fert, int32_t plots)
{
  GCtab *t = lj_tab_new_ah(L, 7, 0);
  TValue *arr = tvref(t->array);
  setintV(&arr[1], wx);
  setintV(&arr[2], sy);
  setintV(&arr[3], w);
  setintV(&arr[4], h);
  setintV(&arr[5], area_id);
  setintV(&arr[6], fert);
  setintV(&arr[7], plots);
  return t;
}

static void civ5_chop_into_two_core(
    lua_State *L, GCtab *fert_tab, GCtab *rect_tab,
    int bTaller, double chopPercent, int32_t iW, int32_t iH,
    GCtab **out_f1, GCtab **out_d1, GCtab **out_f2, GCtab **out_d2)
{
  int32_t iWestX = civ5_tv_to_int(lj_tab_getint(rect_tab, 1));
  int32_t iSouthY = civ5_tv_to_int(lj_tab_getint(rect_tab, 2));
  int32_t iRectWidth = civ5_tv_to_int(lj_tab_getint(rect_tab, 3));
  int32_t iRectHeight = civ5_tv_to_int(lj_tab_getint(rect_tab, 4));
  int32_t iAreaID = civ5_tv_to_int(lj_tab_getint(rect_tab, 5));
  double iTargetFertility = civ5_tv_to_num(lj_tab_getint(rect_tab, 6)) * chopPercent / 100.0;

  int32_t firstRegionWestX = iWestX;
  int32_t firstRegionSouthY = iSouthY;
  int32_t firstRegionWidth = 0, firstRegionHeight = 0;
  int32_t secondRegionWestX = 0, secondRegionSouthY = 0;
  int32_t secondRegionWidth = 0, secondRegionHeight = 0;
  int32_t iFirstRegionFertility = 0, iSecondRegionFertility = 0;
  GCtab *r1_raw = NULL, *r2_raw = NULL;
  int32_t fr_wx, fr_sy, fr_w, fr_h;
  int32_t sr_wx, sr_sy, sr_w, sr_h;
  int top = lua_gettop(L);

  if (bTaller) {
    int32_t rectY = 0, rectX, idx;
    firstRegionWidth = iRectWidth;
    secondRegionWestX = iWestX;
    secondRegionWidth = iRectWidth;
    while (rectY < iRectHeight) {
      for (rectX = 0; rectX < iRectWidth; rectX++) {
        int32_t pf = civ5_tv_to_int(lj_tab_getint(fert_tab, rectY * iRectWidth + rectX + 1));
        iFirstRegionFertility += pf;
      }
      if ((double)iFirstRegionFertility >= iTargetFertility || rectY == iRectHeight - 1) {
        firstRegionHeight = rectY + 1;
        secondRegionSouthY = civ5_pos_mod(iSouthY + rectY + 1, iH);
        secondRegionHeight = iRectHeight - firstRegionHeight;
        break;
      }
      rectY++;
    }
    r1_raw = lj_tab_new_ah(L, (uint32_t)(firstRegionWidth * firstRegionHeight), 0);
    settabV(L, L->top++, r1_raw);
    idx = 1;
    for (rectY = 0; rectY < firstRegionHeight; rectY++) {
      for (rectX = 0; rectX < iRectWidth; rectX++) {
        int32_t pf = civ5_tv_to_int(lj_tab_getint(fert_tab, rectY * iRectWidth + rectX + 1));
        setintV(&tvref(r1_raw->array)[idx++], pf);
      }
    }
    r2_raw = lj_tab_new_ah(L, (uint32_t)(secondRegionWidth * (secondRegionHeight > 0 ? secondRegionHeight : 0)), 0);
    settabV(L, L->top++, r2_raw);
    idx = 1;
    for (rectY = firstRegionHeight; rectY < iRectHeight; rectY++) {
      for (rectX = 0; rectX < iRectWidth; rectX++) {
        int32_t pf = civ5_tv_to_int(lj_tab_getint(fert_tab, rectY * iRectWidth + rectX + 1));
        iSecondRegionFertility += pf;
        setintV(&tvref(r2_raw->array)[idx++], pf);
      }
    }
  } else {
    int32_t rectX = 0, rectY, idx;
    firstRegionHeight = iRectHeight;
    secondRegionSouthY = iSouthY;
    secondRegionHeight = iRectHeight;
    while (rectX < iRectWidth) {
      for (rectY = 0; rectY < iRectHeight; rectY++) {
        int32_t pf = civ5_tv_to_int(lj_tab_getint(fert_tab, rectY * iRectWidth + rectX + 1));
        iFirstRegionFertility += pf;
      }
      if ((double)iFirstRegionFertility >= iTargetFertility || rectX == iRectWidth - 1) {
        firstRegionWidth = rectX + 1;
        secondRegionWestX = civ5_pos_mod(iWestX + rectX + 1, iW);
        secondRegionWidth = iRectWidth - firstRegionWidth;
        break;
      }
      rectX++;
    }
    r2_raw = lj_tab_new_ah(L, (uint32_t)((secondRegionWidth > 0 ? secondRegionWidth : 0) * secondRegionHeight), 0);
    settabV(L, L->top++, r2_raw);
    idx = 1;
    for (rectY = 0; rectY < iRectHeight; rectY++) {
      for (rectX = firstRegionWidth; rectX < iRectWidth; rectX++) {
        int32_t pf = civ5_tv_to_int(lj_tab_getint(fert_tab, rectY * iRectWidth + rectX + 1));
        iSecondRegionFertility += pf;
        setintV(&tvref(r2_raw->array)[idx++], pf);
      }
    }
    r1_raw = lj_tab_new_ah(L, (uint32_t)(firstRegionWidth * firstRegionHeight), 0);
    settabV(L, L->top++, r1_raw);
    idx = 1;
    for (rectY = 0; rectY < iRectHeight; rectY++) {
      for (rectX = 0; rectX < firstRegionWidth; rectX++) {
        int32_t pf = civ5_tv_to_int(lj_tab_getint(fert_tab, rectY * iRectWidth + rectX + 1));
        setintV(&tvref(r1_raw->array)[idx++], pf);
      }
    }
  }

  *out_f1 = civ5_remove_dead_rows_core(L, r1_raw, firstRegionWestX, firstRegionSouthY,
                                       firstRegionWidth, firstRegionHeight,
                                       iW, iH, &fr_wx, &fr_sy, &fr_w, &fr_h);
  settabV(L, L->top++, *out_f1);
  *out_f2 = civ5_remove_dead_rows_core(L, r2_raw, secondRegionWestX, secondRegionSouthY,
                                       secondRegionWidth, secondRegionHeight,
                                       iW, iH, &sr_wx, &sr_sy, &sr_w, &sr_h);
  settabV(L, L->top++, *out_f2);
  *out_d1 = civ5_make_rect_data(L, fr_wx, fr_sy, fr_w, fr_h, iAreaID,
                                iFirstRegionFertility, fr_w * fr_h);
  settabV(L, L->top++, *out_d1);
  *out_d2 = civ5_make_rect_data(L, sr_wx, sr_sy, sr_w, sr_h, iAreaID,
                                iSecondRegionFertility, sr_w * sr_h);
  lua_settop(L, top);
}

static int lj_civ5_ChopIntoTwoRegions(lua_State *L)
{
  /* AssignStartingPlots:ChopIntoTwoRegions(self, fertility_table, rectangle_data_table, bTaller, chopPercent) */
  GCtab *fert_tab = lj_lib_checktab(L, 2);
  GCtab *rect_tab = lj_lib_checktab(L, 3);
  int bTaller = lua_toboolean(L, 4);
  double chopPercent = luaL_checknumber(L, 5);
  int32_t iW, iH, wrapX, wrapY;
  GCtab *f1, *d1, *f2, *d2, *outcome;

  civ5_get_map_info(L, &iW, &iH, &wrapX, &wrapY);
  civ5_chop_into_two_core(L, fert_tab, rect_tab, bTaller, chopPercent, iW, iH,
                          &f1, &d1, &f2, &d2);
  outcome = lj_tab_new_ah(L, 4, 0);
  settabV(L, &tvref(outcome->array)[1], f1);
  settabV(L, &tvref(outcome->array)[2], d1);
  settabV(L, &tvref(outcome->array)[3], f2);
  settabV(L, &tvref(outcome->array)[4], d2);
  lj_gc_anybarriert(L, outcome);
  settabV(L, L->top++, outcome);
  lj_gc_check(L);
  return 1;
}

static int lj_civ5_ChopIntoThreeRegions(lua_State *L)
{
  /* AssignStartingPlots:ChopIntoThreeRegions(self, fertility_table, rectangle_data_table, bTaller, chopPercent) */
  GCtab *fert_tab = lj_lib_checktab(L, 2);
  GCtab *rect_tab = lj_lib_checktab(L, 3);
  int bTaller = lua_toboolean(L, 4);
  int32_t iW, iH, wrapX, wrapY;
  GCtab *f1, *d1, *rem_f, *rem_d;
  GCtab *f2, *d2, *f3, *d3, *results;
  int bTallerRem;

  civ5_get_map_info(L, &iW, &iH, &wrapX, &wrapY);
  civ5_chop_into_two_core(L, fert_tab, rect_tab, bTaller, 33.0, iW, iH,
                          &f1, &d1, &rem_f, &rem_d);
  /* Root intermediate tables on stack across second chop */
  settabV(L, L->top++, f1);
  settabV(L, L->top++, d1);
  settabV(L, L->top++, rem_f);
  settabV(L, L->top++, rem_d);

  bTallerRem = civ5_tv_to_int(lj_tab_getint(rem_d, 4)) >
               civ5_tv_to_int(lj_tab_getint(rem_d, 3));
  civ5_chop_into_two_core(L, rem_f, rem_d, bTallerRem, 48.5, iW, iH,
                          &f2, &d2, &f3, &d3);
  L->top -= 4;

  results = lj_tab_new_ah(L, 6, 0);
  settabV(L, &tvref(results->array)[1], f1);
  settabV(L, &tvref(results->array)[2], d1);
  settabV(L, &tvref(results->array)[3], f2);
  settabV(L, &tvref(results->array)[4], d2);
  settabV(L, &tvref(results->array)[5], f3);
  settabV(L, &tvref(results->array)[6], d3);
  lj_gc_anybarriert(L, results);
  settabV(L, L->top++, results);
  lj_gc_check(L);
  return 1;
}

/* -- 6. Native C accelerators for FractalWorld & MultilayeredFractal ----- */

static int32_t civ5_get_plot_ocean(lua_State *L)
{
  int32_t ocean = 3; /* Default PlotTypes.PLOT_OCEAN in Civ 5 */
  int top = lua_gettop(L);
  civ5_push_env_global(L, "PlotTypes");
  if (lua_istable(L, -1)) {
    lua_getfield(L, -1, "PLOT_OCEAN");
    if (lua_isnumber(L, -1))
      ocean = (int32_t)lua_tointeger(L, -1);
  }
  lua_settop(L, top);
  return ocean;
}

static int32_t civ5_determine_x_shift_core(GCtab *plot_tab, int32_t W, int32_t H,
                                           int32_t group_radius, int32_t plot_ocean)
{
  int32_t stack_totals[512];
  int32_t *land_totals = stack_totals;
  int32_t x, y, col_idx, cur_col;
  int32_t best_value, best_group = 1;

  if (W <= 0 || H <= 0 || !plot_tab) return 0;
  if (W > 512) {
    land_totals = (int32_t *)malloc((size_t)W * sizeof(int32_t));
    if (!land_totals) return 0;
  }

  for (x = 0; x < W; x++) {
    int32_t cur_col_land = 0;
    for (y = 0; y < H; y++) {
      cTValue *tv = lj_tab_getint(plot_tab, y * W + x + 1);
      if (!tv || tvisnil(tv) || civ5_tv_to_int(tv) != plot_ocean)
        cur_col_land++;
    }
    land_totals[x] = cur_col_land;
  }

  best_value = H * (2 * group_radius + 1);
  for (col_idx = 1; col_idx <= W; col_idx++) {
    int32_t cur_group_total = 0;
    for (cur_col = col_idx - group_radius; cur_col <= col_idx + group_radius; cur_col++) {
      int32_t cur_idx = civ5_pos_mod(cur_col, W);
      if (cur_idx == 0) cur_idx = W;
      cur_group_total += land_totals[cur_idx - 1];
    }
    if (cur_group_total < best_value) {
      best_value = cur_group_total;
      best_group = col_idx;
    }
  }

  if (land_totals != stack_totals)
    free(land_totals);
  return best_group - 1;
}

static int32_t civ5_determine_y_shift_core(GCtab *plot_tab, int32_t W, int32_t H,
                                           int32_t group_radius, int32_t plot_ocean)
{
  int32_t stack_totals[512];
  int32_t *land_totals = stack_totals;
  int32_t x, y, row_idx, cur_row;
  int32_t best_value, best_group = 1;

  if (W <= 0 || H <= 0 || !plot_tab) return 0;
  if (H > 512) {
    land_totals = (int32_t *)malloc((size_t)H * sizeof(int32_t));
    if (!land_totals) return 0;
  }

  for (y = 0; y < H; y++) {
    int32_t cur_row_land = 0;
    for (x = 0; x < W; x++) {
      cTValue *tv = lj_tab_getint(plot_tab, y * W + x + 1);
      if (!tv || tvisnil(tv) || civ5_tv_to_int(tv) != plot_ocean)
        cur_row_land++;
    }
    land_totals[y] = cur_row_land;
  }

  best_value = W * (2 * group_radius + 1);
  for (row_idx = 1; row_idx <= H; row_idx++) {
    int32_t cur_group_total = 0;
    for (cur_row = row_idx - group_radius; cur_row <= row_idx + group_radius; cur_row++) {
      int32_t cur_idx = civ5_pos_mod(cur_row, H);
      if (cur_idx == 0) cur_idx = H;
      cur_group_total += land_totals[cur_idx - 1];
    }
    if (cur_group_total < best_value) {
      best_value = cur_group_total;
      best_group = row_idx;
    }
  }

  if (land_totals != stack_totals)
    free(land_totals);
  return best_group - 1;
}

static void civ5_shift_plots_by_core(lua_State *L, GCtab *plot_tab,
                                     int32_t xshift, int32_t yshift,
                                     int32_t W, int32_t H)
{
  if ((xshift > 0 || yshift > 0) && W > 0 && H > 0 && plot_tab) {
    int32_t iWH = W * H;
    int32_t i, iDestY, iDestX;
    TValue *buf = (TValue *)malloc((size_t)(iWH + 2) * sizeof(TValue));
    if (!buf) lj_err_mem(L);
    setnilV(&buf[0]);
    for (i = 1; i <= iWH + 1; i++) {
      cTValue *src = lj_tab_getint(plot_tab, i);
      if (src) copyTV(L, &buf[i], src);
      else setnilV(&buf[i]);
    }
    for (iDestY = 0; iDestY <= H; iDestY++) {
      for (iDestX = 0; iDestX <= W; iDestX++) {
        int32_t iDestI = W * iDestY + iDestX;
        int32_t iSourceX = civ5_pos_mod(iDestX + xshift, W);
        int32_t iSourceY = civ5_pos_mod(iDestY + yshift, H);
        int32_t iSourceI = W * iSourceY + iSourceX;
        TValue *dst = lj_tab_setint(L, plot_tab, iDestI);
        if (iSourceI >= 1 && iSourceI <= iWH + 1)
          copyTV(L, dst, &buf[iSourceI]);
        else
          setnilV(dst);
      }
    }
    free(buf);
  }
}

static int lj_civ5_FW_DetermineXShift(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t W = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsX")));
  int32_t H = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsY")));
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  int32_t ocean = civ5_get_plot_ocean(L);
  int32_t shift = civ5_determine_x_shift_core(plot_tab, W, H, W / 10, ocean);
  setintV(L->top++, shift);
  return 1;
}

static int lj_civ5_FW_DetermineYShift(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t W = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsX")));
  int32_t H = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsY")));
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  int32_t ocean = civ5_get_plot_ocean(L);
  int32_t shift = civ5_determine_y_shift_core(plot_tab, W, H, H / 15, ocean);
  setintV(L->top++, shift);
  return 1;
}

static int lj_civ5_FW_ShiftPlotTypesBy(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t xshift = lj_lib_checkint(L, 2);
  int32_t yshift = lj_lib_checkint(L, 3);
  int32_t W = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsX")));
  int32_t H = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsY")));
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  civ5_shift_plots_by_core(L, plot_tab, xshift, yshift, W, H);
  return 0;
}

static int lj_civ5_FW_ShiftPlotTypes(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t W = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsX")));
  int32_t H = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "iNumPlotsY")));
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  int32_t ocean = civ5_get_plot_ocean(L);
  int32_t shift_x = civ5_determine_x_shift_core(plot_tab, W, H, W / 10, ocean);
  int32_t shift_y = civ5_determine_y_shift_core(plot_tab, W, H, H / 15, ocean);
  civ5_shift_plots_by_core(L, plot_tab, shift_x, shift_y, W, H);
  return 0;
}

static int lj_civ5_MLF_DetermineXShift(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t W = lj_lib_checkint(L, 2);
  int32_t H = lj_lib_checkint(L, 3);
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  int32_t ocean = civ5_get_plot_ocean(L);
  int32_t rad = W / 10;
  if (rad < 1) rad = 1;
  setintV(L->top++, civ5_determine_x_shift_core(plot_tab, W, H, rad, ocean));
  return 1;
}

static int lj_civ5_MLF_DetermineYShift(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t W = lj_lib_checkint(L, 2);
  int32_t H = lj_lib_checkint(L, 3);
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  int32_t ocean = civ5_get_plot_ocean(L);
  int32_t rad = H / 10;
  if (rad < 1) rad = 1;
  setintV(L->top++, civ5_determine_y_shift_core(plot_tab, W, H, rad, ocean));
  return 1;
}

static int lj_civ5_MLF_ShiftRegionPlotsBy(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t xshift = lj_lib_checkint(L, 2);
  int32_t yshift = lj_lib_checkint(L, 3);
  int32_t W = lj_lib_checkint(L, 4);
  int32_t H = lj_lib_checkint(L, 5);
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  civ5_shift_plots_by_core(L, plot_tab, xshift, yshift, W, H);
  return 0;
}

static int lj_civ5_MLF_ShiftRegionPlots(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t W = lj_lib_checkint(L, 2);
  int32_t H = lj_lib_checkint(L, 3);
  GCtab *plot_tab = civ5_get_subtab(L, self_tab, "plotTypes");
  int32_t ocean = civ5_get_plot_ocean(L);
  int32_t rx = W / 10 < 1 ? 1 : W / 10;
  int32_t ry = H / 10 < 1 ? 1 : H / 10;
  int32_t shift_x = civ5_determine_x_shift_core(plot_tab, W, H, rx, ocean);
  int32_t shift_y = civ5_determine_y_shift_core(plot_tab, W, H, ry, ocean);
  civ5_shift_plots_by_core(L, plot_tab, shift_x, shift_y, W, H);
  return 0;
}

/* -- 7. Native C accelerators for InstanceManager.lua -------------------- */

static void civ5_call_root_sethide(lua_State *L, cTValue *inst_tv,
                                   GCstr *root_name, int bHide)
{
  cTValue *ctrl_tv;
  int top;
  if (!inst_tv || !tvistab(inst_tv) || !root_name) return;
  ctrl_tv = lj_tab_getstr(tabV(inst_tv), root_name);
  if (!ctrl_tv || tvisnil(ctrl_tv)) return;
  top = lua_gettop(L);
  copyTV(L, L->top++, ctrl_tv);
  lua_getfield(L, -1, "SetHide");
  if (!lua_isnil(L, -1)) {
    lua_insert(L, -2);
    lua_pushboolean(L, bHide);
    lua_call(L, 2, 0);
  } else {
    lua_settop(L, top);
  }
}

static GCstr *civ5_get_root_name(lua_State *L, GCtab *self_tab)
{
  cTValue *tv = lj_tab_getstr(self_tab, lj_str_newlit(L, "m_RootControlName"));
  if (tv && tvisstr(tv))
    return strV(tv);
  return NULL;
}

static int lj_civ5_IM_GetInstance(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  GCtab *avail_tab = civ5_get_subtab(L, self_tab, "m_AvailableInstances");
  GCtab *alloc_tab;
  int32_t avail_len, alloc_len, i_avail, i_alloc;
  cTValue *inst_tv;
  TValue inst_copy;
  TValue *dst;

  if (!avail_tab) return 0;
  avail_len = (int32_t)lj_tab_len(avail_tab);
  if (avail_len == 0) {
    lua_getfield(L, 1, "BuildInstance");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 0);
    self_tab = tabV(L->base);
    avail_tab = civ5_get_subtab(L, self_tab, "m_AvailableInstances");
    if (!avail_tab) return 0;
    avail_len = (int32_t)lj_tab_len(avail_tab);
    if (avail_len == 0) return 0;
  }

  inst_tv = lj_tab_getint(avail_tab, avail_len);
  if (!inst_tv || tvisnil(inst_tv)) return 0;
  copyTV(L, &inst_copy, inst_tv);
  setnilV(lj_tab_setint(L, avail_tab, avail_len));
  lj_gc_anybarriert(L, avail_tab);

  /* Root inst_copy on stack across SetHide call */
  copyTV(L, L->top++, &inst_copy);
  civ5_call_root_sethide(L, &inst_copy, civ5_get_root_name(L, self_tab), 0);
  L->top--;

  self_tab = tabV(L->base);
  alloc_tab = civ5_get_subtab(L, self_tab, "m_AllocatedInstances");
  if (alloc_tab) {
    alloc_len = (int32_t)lj_tab_len(alloc_tab);
    dst = lj_tab_setint(L, alloc_tab, alloc_len + 1);
    copyTV(L, dst, &inst_copy);
    if (tvisgcv(&inst_copy))
      lj_gc_barriert(L, alloc_tab, dst);
  }

  i_avail = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "m_iAvailableInstances")));
  i_alloc = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "m_iAllocatedInstances")));
  setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_iAvailableInstances")), i_avail - 1);
  setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_iAllocatedInstances")), i_alloc + 1);
  lj_gc_anybarriert(L, self_tab);

  copyTV(L, L->top++, &inst_copy);
  return 1;
}

static int lj_civ5_IM_ReleaseInstance(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  cTValue *inst_arg = L->base + 1;
  GCtab *alloc_tab, *avail_tab;
  TValue inst_copy;
  uint64_t target_u64;
  MSize i, asize;

  if (inst_arg >= L->top || tvisnil(inst_arg))
    return 0;
  copyTV(L, &inst_copy, inst_arg);
  target_u64 = inst_copy.u64;

  alloc_tab = civ5_get_subtab(L, self_tab, "m_AllocatedInstances");
  if (!alloc_tab) return 0;

  asize = alloc_tab->asize;
  for (i = 1; i < asize; i++) {
    TValue *arr = tvref(alloc_tab->array);
    if (arr[i].u64 == target_u64) {
      int32_t alloc_len = (int32_t)lj_tab_len(alloc_tab);
      int32_t avail_len, i_avail, i_alloc;
      TValue *dst;

      copyTV(L, L->top++, &inst_copy);
      civ5_call_root_sethide(L, &inst_copy, civ5_get_root_name(L, self_tab), 1);
      L->top--;

      self_tab = tabV(L->base);
      alloc_tab = civ5_get_subtab(L, self_tab, "m_AllocatedInstances");
      avail_tab = civ5_get_subtab(L, self_tab, "m_AvailableInstances");
      if (alloc_tab && (int32_t)i <= alloc_len && (MSize)alloc_len < alloc_tab->asize) {
        arr = tvref(alloc_tab->array);
        if ((int32_t)i < alloc_len) {
          memmove(&arr[i], &arr[i + 1], (size_t)(alloc_len - (int32_t)i) * sizeof(TValue));
        }
        setnilV(&arr[alloc_len]);
        lj_gc_anybarriert(L, alloc_tab);
      }

      if (avail_tab) {
        avail_len = (int32_t)lj_tab_len(avail_tab);
        dst = lj_tab_setint(L, avail_tab, avail_len + 1);
        copyTV(L, dst, &inst_copy);
        if (tvisgcv(&inst_copy))
          lj_gc_barriert(L, avail_tab, dst);
      }

      i_avail = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "m_iAvailableInstances")));
      i_alloc = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "m_iAllocatedInstances")));
      setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_iAvailableInstances")), i_avail + 1);
      setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_iAllocatedInstances")), i_alloc - 1);
      lj_gc_anybarriert(L, self_tab);
      break;
    }
  }
  return 0;
}

static int lj_civ5_IM_ResetInstances(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  GCtab *alloc_tab = civ5_get_subtab(L, self_tab, "m_AllocatedInstances");
  GCtab *avail_tab = civ5_get_subtab(L, self_tab, "m_AvailableInstances");
  int32_t alloc_len, avail_len, k, i_count;

  if (alloc_tab && avail_tab) {
    alloc_len = (int32_t)lj_tab_len(alloc_tab);
    avail_len = (int32_t)lj_tab_len(avail_tab);
    for (k = alloc_len; k >= 1; k--) {
      self_tab = tabV(L->base);
      alloc_tab = civ5_get_subtab(L, self_tab, "m_AllocatedInstances");
      if (!alloc_tab) break;
      {
        cTValue *tv = lj_tab_getint(alloc_tab, k);
        if (tv && !tvisnil(tv)) {
          TValue iter_copy;
          TValue *dst;
          copyTV(L, &iter_copy, tv);
          setnilV(lj_tab_setint(L, alloc_tab, k));
          lj_gc_anybarriert(L, alloc_tab);
          copyTV(L, L->top++, &iter_copy);
          civ5_call_root_sethide(L, &iter_copy, civ5_get_root_name(L, self_tab), 1);
          L->top--;
          self_tab = tabV(L->base);
          avail_tab = civ5_get_subtab(L, self_tab, "m_AvailableInstances");
          if (avail_tab) {
            avail_len++;
            dst = lj_tab_setint(L, avail_tab, avail_len);
            copyTV(L, dst, &iter_copy);
            if (tvisgcv(&iter_copy))
              lj_gc_barriert(L, avail_tab, dst);
          }
        }
      }
    }
  }
  self_tab = tabV(L->base);
  i_count = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "m_iCount")));
  setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_iAvailableInstances")), i_count);
  setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_iAllocatedInstances")), 0);
  lj_gc_anybarriert(L, self_tab);
  return 0;
}

static int lj_civ5_GIM_GetInstance(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  int32_t next_idx = civ5_tv_to_int(lj_tab_getstr(self_tab, lj_str_newlit(L, "m_NextInstanceIndex")));
  GCtab *inst_tab = civ5_get_subtab(L, self_tab, "m_Instances");
  cTValue *inst_tv;
  TValue inst_copy;

  if (!inst_tab) return 0;
  if (next_idx > (int32_t)lj_tab_len(inst_tab)) {
    lua_getfield(L, 1, "BuildInstance");
    lua_pushvalue(L, 1);
    lua_call(L, 1, 0);
    self_tab = tabV(L->base);
    inst_tab = civ5_get_subtab(L, self_tab, "m_Instances");
    if (!inst_tab) return 0;
  }
  inst_tv = lj_tab_getint(inst_tab, next_idx);
  if (!inst_tv || tvisnil(inst_tv)) return 0;
  copyTV(L, &inst_copy, inst_tv);
  copyTV(L, L->top++, &inst_copy);
  civ5_call_root_sethide(L, &inst_copy, civ5_get_root_name(L, self_tab), 0);
  L->top--;
  self_tab = tabV(L->base);
  setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_NextInstanceIndex")), next_idx + 1);
  lj_gc_anybarriert(L, self_tab);
  copyTV(L, L->top++, &inst_copy);
  return 1;
}

static int lj_civ5_GIM_ResetInstances(lua_State *L)
{
  GCtab *self_tab = lj_lib_checktab(L, 1);
  GCtab *inst_tab = civ5_get_subtab(L, self_tab, "m_Instances");
  if (inst_tab) {
    int32_t i, len = (int32_t)lj_tab_len(inst_tab);
    for (i = 1; i <= len; i++) {
      self_tab = tabV(L->base);
      inst_tab = civ5_get_subtab(L, self_tab, "m_Instances");
      if (!inst_tab) break;
      {
        cTValue *tv = lj_tab_getint(inst_tab, i);
        if (tv && !tvisnil(tv)) {
          TValue tv_copy;
          copyTV(L, &tv_copy, tv);
          copyTV(L, L->top++, &tv_copy);
          civ5_call_root_sethide(L, &tv_copy, civ5_get_root_name(L, self_tab), 1);
          L->top--;
        }
      }
    }
  }
  self_tab = tabV(L->base);
  setintV(lj_tab_setstr(L, self_tab, lj_str_newlit(L, "m_NextInstanceIndex")), 1);
  lj_gc_anybarriert(L, self_tab);
  return 0;
}

/* -- 8. Native C + JIT-Sunk accelerators for FLuaVector & IconSupport ---- */

static int lj_civ5_PopulateIconAtlases(lua_State *L)
{
  int top = lua_gettop(L);
  GCtab *caller_env = NULL;
  GCtab *l_env;
  GCtab *g;
  GCtab *atlases = NULL;

  /* Mode 1: _c_pop(false) invalidates the registry cache on mod activate/deactivate */
  if (top >= 1 && lua_isboolean(L, 1)) {
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "_CIV5_LAST_ATLAS_FN");
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, "_CIV5_CACHED_ATLASES");
    return 0;
  }

  /* If _orig_pop (Lua closure) was passed as arg 1, get its environment table */
  if (top >= 1 && lua_isfunction(L, 1)) {
    lua_getfenv(L, 1);
    if (lua_istable(L, -1))
      caller_env = tabV(L->top - 1);
    lua_pop(L, 1);
  }

  /* Look up GameInfo (from caller_env, L->env, or mainthread _G) */
  if (caller_env) {
    settabV(L, L->top++, caller_env);
    lua_getfield(L, -1, "GameInfo");
    lua_remove(L, -2);
    if (lua_isnil(L, -1)) {
      lua_pop(L, 1);
      civ5_push_env_global(L, "GameInfo");
    }
  } else {
    civ5_push_env_global(L, "GameInfo");
  }

  if (lua_istable(L, -1) || lua_isuserdata(L, -1)) {
    lua_getfield(L, -1, "IconTextureAtlases");
    /* Accept ANY non-nil callable: function (benchmark) OR userdata/table with __call (real Civ 5 CvGameDatabase) */
    if (!lua_isnil(L, -1)) {
      int fn_idx = lua_gettop(L);
      /* Check if LUA_REGISTRYINDEX already has a cached table for this exact IconTextureAtlases object */
      lua_getfield(L, LUA_REGISTRYINDEX, "_CIV5_LAST_ATLAS_FN");
      if (lua_rawequal(L, -1, fn_idx)) {
        lua_pop(L, 1);
        lua_getfield(L, LUA_REGISTRYINDEX, "_CIV5_CACHED_ATLASES");
        if (lua_istable(L, -1)) {
          atlases = tabV(L->top - 1);
        } else {
          lua_pop(L, 1);
        }
      } else {
        lua_pop(L, 1);
      }

      if (!atlases) {
        /* Cache miss! First, if _orig_pop was passed as arg 1, run _orig_pop() in LuaJIT! */
        if (top >= 1 && lua_isfunction(L, 1)) {
          lua_pushvalue(L, 1);
          if (lua_pcall(L, 0, 0, 0) == 0) {
            lua_getfenv(L, 1);
            if (lua_istable(L, -1)) {
              caller_env = tabV(L->top - 1);
              lua_getfield(L, -1, "g_IconAtlases");
              if (lua_istable(L, -1)) {
                atlases = tabV(L->top - 1);
                /* Save in registry for all subsequent UI contexts! */
                lua_pushvalue(L, -1);
                lua_setfield(L, LUA_REGISTRYINDEX, "_CIV5_CACHED_ATLASES");
                lua_pushvalue(L, fn_idx);
                lua_setfield(L, LUA_REGISTRYINDEX, "_CIV5_LAST_ATLAS_FN");
              }
              lua_pop(L, 1);
            }
            lua_pop(L, 1);
          } else {
            lua_pop(L, 1); /* pop error message */
          }
        }
      }
    }
  }

  if (!atlases) {
    atlases = lj_tab_new_ah(L, 0, 2);
  }
  settabV(L, L->top++, atlases); /* Root atlases on stack */

  l_env = tabref(L->env);
  g = tabref(mainthread(G(L))->env);

  /* Store in caller_env, L->env, and main globals */
  if (caller_env) {
    TValue *slot = lj_tab_setstr(L, caller_env, lj_str_newlit(L, "g_IconAtlases"));
    settabV(L, slot, atlases);
    lj_gc_barriert(L, caller_env, slot);
  }
  if (l_env && l_env != caller_env) {
    TValue *slot = lj_tab_setstr(L, l_env, lj_str_newlit(L, "g_IconAtlases"));
    settabV(L, slot, atlases);
    lj_gc_barriert(L, l_env, slot);
  }
  if (g && g != caller_env && g != l_env) {
    TValue *slot = lj_tab_setstr(L, g, lj_str_newlit(L, "g_IconAtlases"));
    settabV(L, slot, atlases);
    lj_gc_barriert(L, g, slot);
  }

  lua_settop(L, top);
  settabV(L, L->top++, atlases);
  return 1;
}

static const char k_civ5_vec_lua[] =
  "\nfunction Vector2(i, j) return { x = i, y = j } end\n"
  "function Vector3(i, j, k) return { x = i, y = j, z = k } end\n"
  "function Vector4(i, j, k, l) return { x = i, y = j, z = k, w = l } end\n"
  "function Color(r, g, b, a) return { x = r, y = g, z = b, w = a } end\n"
  "function VecAdd(v1, v2)\n"
  "  return {\n"
  "    x = (v1.x ~= nil and v2.x ~= nil) and (v1.x + v2.x) or nil,\n"
  "    y = (v1.y ~= nil and v2.y ~= nil) and (v1.y + v2.y) or nil,\n"
  "    z = (v1.z ~= nil and v2.z ~= nil) and (v1.z + v2.z) or nil,\n"
  "    w = (v1.w ~= nil and v2.w ~= nil) and (v1.w + v2.w) or nil\n"
  "  }\n"
  "end\n"
  "function VecSubtract(v1, v2)\n"
  "  return {\n"
  "    x = (v1.x ~= nil and v2.x ~= nil) and (v1.x - v2.x) or nil,\n"
  "    y = (v1.y ~= nil and v2.y ~= nil) and (v1.y - v2.y) or nil,\n"
  "    z = (v1.z ~= nil and v2.z ~= nil) and (v1.z - v2.z) or nil,\n"
  "    w = (v1.w ~= nil and v2.w ~= nil) and (v1.w - v2.w) or nil\n"
  "  }\n"
  "end\n";

static const char k_civ5_icon_suffix_lua[] =
  "\nlocal _orig_pop = PopulateIconAtlases\n"
  "local _c_pop = __civ5_pop_atlases or (math and math.__civ5_pop_atlases)\n"
  "local _floor = math.floor\n"
  "function PopulateIconAtlases()\n"
  "  if _c_pop then\n"
  "    local a = _c_pop(_orig_pop)\n"
  "    g_IconAtlases = a\n"
  "    return a\n"
  "  end\n"
  "  _orig_pop()\n"
  "  return g_IconAtlases\n"
  "end\n"
  "function IconLookup(offset, iconSize, atlas)\n"
  "  if offset < 0 then return nil, nil end\n"
  "  local ga = g_IconAtlases\n"
  "  if ga == nil then\n"
  "    if _c_pop then\n"
  "      ga = _c_pop(_orig_pop)\n"
  "      g_IconAtlases = ga\n"
  "    else\n"
  "      _orig_pop()\n"
  "      ga = g_IconAtlases\n"
  "    end\n"
  "  end\n"
  "  local a = ga and ga[atlas]\n"
  "  if a ~= nil then\n"
  "    local e = a[iconSize]\n"
  "    if e ~= nil then\n"
  "      local fn, nc, nr = e[1], e[2], e[3]\n"
  "      if fn == nil or offset > (nr * nc) - 1 then return end\n"
  "      return { x = (offset % nc) * iconSize, y = _floor(offset / nc) * iconSize }, fn\n"
  "    end\n"
  "  end\n"
  "end\n"
  "function IconHookup(offset, iconSize, atlas, imageControl)\n"
  "  if offset < 0 or imageControl == nil then return nil end\n"
  "  local ga = g_IconAtlases\n"
  "  if ga == nil then\n"
  "    if _c_pop then\n"
  "      ga = _c_pop(_orig_pop)\n"
  "      g_IconAtlases = ga\n"
  "    else\n"
  "      _orig_pop()\n"
  "      ga = g_IconAtlases\n"
  "    end\n"
  "  end\n"
  "  local a = ga and ga[atlas]\n"
  "  if a ~= nil then\n"
  "    local e = a[iconSize]\n"
  "    if e ~= nil then\n"
  "      local fn, nc, nr = e[1], e[2], e[3]\n"
  "      if fn == nil or offset > (nr * nc) - 1 then return end\n"
  "      imageControl:SetTexture(fn)\n"
  "      imageControl:SetTextureOffsetVal((offset % nc) * iconSize, _floor(offset / nc) * iconSize)\n"
  "    end\n"
  "  end\n"
  "  return true\n"
  "end\n"
  "if Events and Events.AfterModsDeactivate and Events.AfterModsDeactivate.Add then\n"
  "  Events.AfterModsDeactivate.Add(function()\n"
  "    if _c_pop then _c_pop(false) end\n"
  "  end)\n"
  "end\n"
  "if Events and Events.AfterModsActivate and Events.AfterModsActivate.Add then\n"
  "  Events.AfterModsActivate.Add(function()\n"
  "    if _c_pop then _c_pop(false) end\n"
  "  end)\n"
  "end\n";

/* -- 9. Patch callbacks invoked at the end of target script loads -------- */

static void civ5_bind_env_cfunc(lua_State *L, int env_idx,
                                lua_CFunction fn, const char *name)
{
  lua_pushcfunction(L, fn);
  if (env_idx != 0) {
    lua_pushvalue(L, env_idx < 0 ? (env_idx - 1) : env_idx);
    lua_setfenv(L, -2);
    lua_setfield(L, env_idx < 0 ? (env_idx - 1) : env_idx, name);
  }
  lua_pushcfunction(L, fn);
  lua_setfield(L, LUA_GLOBALSINDEX, name);
}

static int lj_civ5_patch_mmu(lua_State *L)
{
  lua_Debug ar;
  if (lua_getstack(L, 1, &ar) && lua_getinfo(L, "f", &ar)) {
    lua_getfenv(L, -1);
    if (lua_istable(L, -1)) {
      civ5_bind_env_cfunc(L, -1, lj_civ5_GetShuffledCopyOfTable, "GetShuffledCopyOfTable");
      civ5_bind_env_cfunc(L, -1, lj_civ5_TestMembership, "TestMembership");
      civ5_bind_env_cfunc(L, -1, lj_civ5_IdentifyTableIndex, "IdentifyTableIndex");
    }
    lua_pop(L, 2);
  } else {
    civ5_bind_env_cfunc(L, 0, lj_civ5_GetShuffledCopyOfTable, "GetShuffledCopyOfTable");
    civ5_bind_env_cfunc(L, 0, lj_civ5_TestMembership, "TestMembership");
    civ5_bind_env_cfunc(L, 0, lj_civ5_IdentifyTableIndex, "IdentifyTableIndex");
  }
  return 0;
}

static int lj_civ5_patch_asp(lua_State *L)
{
  if (lua_istable(L, 1)) {
    lua_pushcfunction(L, lj_civ5_PlaceResourceImpact);
    lua_setfield(L, 1, "PlaceResourceImpact");
    lua_pushcfunction(L, lj_civ5_PlaceImpactAndRipples);
    lua_setfield(L, 1, "PlaceImpactAndRipples");
    lua_pushcfunction(L, lj_civ5_ApplyHexAdjustment);
    lua_setfield(L, 1, "ApplyHexAdjustment");
    lua_pushcfunction(L, lj_civ5_RemoveDeadRows);
    lua_setfield(L, 1, "RemoveDeadRows");
    lua_pushcfunction(L, lj_civ5_ChopIntoTwoRegions);
    lua_setfield(L, 1, "ChopIntoTwoRegions");
    lua_pushcfunction(L, lj_civ5_ChopIntoThreeRegions);
    lua_setfield(L, 1, "ChopIntoThreeRegions");
  }
  return 0;
}

static int lj_civ5_patch_fw(lua_State *L)
{
  if (lua_istable(L, 1)) {
    lua_pushcfunction(L, lj_civ5_FW_DetermineXShift);
    lua_setfield(L, 1, "DetermineXShift");
    lua_pushcfunction(L, lj_civ5_FW_DetermineYShift);
    lua_setfield(L, 1, "DetermineYShift");
    lua_pushcfunction(L, lj_civ5_FW_ShiftPlotTypesBy);
    lua_setfield(L, 1, "ShiftPlotTypesBy");
    lua_pushcfunction(L, lj_civ5_FW_ShiftPlotTypes);
    lua_setfield(L, 1, "ShiftPlotTypes");
  }
  return 0;
}

static int lj_civ5_patch_mlf(lua_State *L)
{
  if (lua_istable(L, 1)) {
    lua_pushcfunction(L, lj_civ5_MLF_DetermineXShift);
    lua_setfield(L, 1, "DetermineXShift");
    lua_pushcfunction(L, lj_civ5_MLF_DetermineYShift);
    lua_setfield(L, 1, "DetermineYShift");
    lua_pushcfunction(L, lj_civ5_MLF_ShiftRegionPlotsBy);
    lua_setfield(L, 1, "ShiftRegionPlotsBy");
    lua_pushcfunction(L, lj_civ5_MLF_ShiftRegionPlots);
    lua_setfield(L, 1, "ShiftRegionPlots");
  }
  return 0;
}

static int lj_civ5_patch_im(lua_State *L)
{
  if (lua_istable(L, 1)) {
    lua_pushcfunction(L, lj_civ5_IM_GetInstance);
    lua_setfield(L, 1, "GetInstance");
    lua_pushcfunction(L, lj_civ5_IM_ReleaseInstance);
    lua_setfield(L, 1, "ReleaseInstance");
    lua_pushcfunction(L, lj_civ5_IM_ResetInstances);
    lua_setfield(L, 1, "ResetInstances");
  }
  if (lua_istable(L, 2)) {
    lua_pushcfunction(L, lj_civ5_GIM_GetInstance);
    lua_setfield(L, 2, "GetInstance");
    lua_pushcfunction(L, lj_civ5_GIM_ResetInstances);
    lua_setfield(L, 2, "ResetInstances");
  }
  return 0;
}

static void civ5_rawset_cfunc(lua_State *L, GCtab *t, const char *name, lua_CFunction fn)
{
  GCfunc *cf;
  TValue *slot;
  if (!t) return;
  lua_pushcfunction(L, fn);
  cf = funcV(L->top - 1);
  slot = lj_tab_setstr(L, t, lj_str_newz(L, name));
  setfuncV(L, slot, cf);
  lj_gc_barriert(L, t, slot);
  L->top--;
}

static void civ5_ensure_helpers_installed(lua_State *L)
{
  GCtab *g = tabref(mainthread(G(L))->env);
  GCtab *l_env = tabref(L->env);
  cTValue *tv;
  if (g) {
    tv = lj_tab_getstr(g, lj_str_newlit(L, "__civ5_pop_atlases"));
    if (!tv || tvisnil(tv)) {
      civ5_rawset_cfunc(L, g, "__civ5_patch_mmu", lj_civ5_patch_mmu);
      civ5_rawset_cfunc(L, g, "__civ5_patch_asp", lj_civ5_patch_asp);
      civ5_rawset_cfunc(L, g, "__civ5_patch_fw", lj_civ5_patch_fw);
      civ5_rawset_cfunc(L, g, "__civ5_patch_mlf", lj_civ5_patch_mlf);
      civ5_rawset_cfunc(L, g, "__civ5_patch_im", lj_civ5_patch_im);
      civ5_rawset_cfunc(L, g, "__civ5_pop_atlases", lj_civ5_PopulateIconAtlases);
    }
    /* Also install inside _G.math, which Civ 5 copies into every UI thread environment! */
    tv = lj_tab_getstr(g, lj_str_newlit(L, "math"));
    if (tv && tvistab(tv)) {
      GCtab *mtab = tabV(tv);
      cTValue *mtv = lj_tab_getstr(mtab, lj_str_newlit(L, "__civ5_pop_atlases"));
      if (!mtv || tvisnil(mtv)) {
        civ5_rawset_cfunc(L, mtab, "__civ5_patch_mmu", lj_civ5_patch_mmu);
        civ5_rawset_cfunc(L, mtab, "__civ5_patch_asp", lj_civ5_patch_asp);
        civ5_rawset_cfunc(L, mtab, "__civ5_patch_fw", lj_civ5_patch_fw);
        civ5_rawset_cfunc(L, mtab, "__civ5_patch_mlf", lj_civ5_patch_mlf);
        civ5_rawset_cfunc(L, mtab, "__civ5_patch_im", lj_civ5_patch_im);
        civ5_rawset_cfunc(L, mtab, "__civ5_pop_atlases", lj_civ5_PopulateIconAtlases);
      }
    }
  }
  if (l_env && l_env != g) {
    tv = lj_tab_getstr(l_env, lj_str_newlit(L, "__civ5_pop_atlases"));
    if (!tv || tvisnil(tv)) {
      civ5_rawset_cfunc(L, l_env, "__civ5_patch_mmu", lj_civ5_patch_mmu);
      civ5_rawset_cfunc(L, l_env, "__civ5_patch_asp", lj_civ5_patch_asp);
      civ5_rawset_cfunc(L, l_env, "__civ5_patch_fw", lj_civ5_patch_fw);
      civ5_rawset_cfunc(L, l_env, "__civ5_patch_mlf", lj_civ5_patch_mlf);
      civ5_rawset_cfunc(L, l_env, "__civ5_patch_im", lj_civ5_patch_im);
      civ5_rawset_cfunc(L, l_env, "__civ5_pop_atlases", lj_civ5_PopulateIconAtlases);
    }
  }
}

/* -- 10. Hook for lua_setfield (intercepts 0x7a5b30 registrations) ------- */

static const char k_civ5_ipairs_lua[] =
  "local rawget = rawget\n"
  "local function civ5_ipairs_aux(t, i)\n"
  "  i = i + 1\n"
  "  local v = rawget(t, i)\n"
  "  if v ~= nil then return i, v end\n"
  "end\n"
  "return function(t)\n"
  "  if rawget(t, 0) ~= nil then\n"
  "    return civ5_ipairs_aux, t, -1\n"
  "  else\n"
  "    return civ5_ipairs_aux, t, 0\n"
  "  end\n"
  "end\n";

static const char k_civ5_clamp_lua[] =
  "return function(v, lo, hi)\n"
  "  if v < lo then return lo\n"
  "  elseif v > hi then return hi\n"
  "  else return v end\n"
  "end\n";

static int civ5_is_host_table(lua_State *L, int idx, const char *name)
{
  GCtab *g = tabref(mainthread(G(L))->env);
  cTValue *tv = lj_tab_getstr(g, lj_str_newz(L, name));
  return tv && tvistab(tv) && lua_topointer(L, idx) == (const void *)tabV(tv);
}

void lj_civ5_intercept_setfield(lua_State *L, int idx, const char *k)
{
  if (!lj_civ5_accel_enabled() || !k)
    return;
  if (L->top <= L->base || !tvisfunc(L->top - 1) || !iscfunc(funcV(L->top - 1)))
    return;

  if (idx == LUA_GLOBALSINDEX && strcmp(k, "ipairs") == 0) {
    /* Replace 0x48da50 C closure with JIT-compilable 0-based Lua closure */
    if (luaL_loadbuffer(L, k_civ5_ipairs_lua, sizeof(k_civ5_ipairs_lua) - 1,
                        "=[civ5_ipairs]") == 0 &&
        lua_pcall(L, 0, 1, 0) == 0) {
      copyTV(L, L->top - 2, L->top - 1);
      L->top--;
    } else {
      lua_pop(L, 1);
    }
    civ5_ensure_helpers_installed(L);
  } else if (strcmp(k, "fill") == 0 && civ5_is_host_table(L, idx, "table") && funcV(L->top - 1)->c.f != lj_civ5_table_fill) {
    L->top--;
    lua_pushcfunction(L, lj_civ5_table_fill);
  } else if (strcmp(k, "count") == 0 && civ5_is_host_table(L, idx, "table") && funcV(L->top - 1)->c.f != lj_civ5_table_count) {
    L->top--;
    lua_pushcfunction(L, lj_civ5_table_count);
  } else if (strcmp(k, "clamp") == 0 && civ5_is_host_table(L, idx, "math")) {
    if (luaL_loadbuffer(L, k_civ5_clamp_lua, sizeof(k_civ5_clamp_lua) - 1,
                        "=[civ5_clamp]") == 0 &&
        lua_pcall(L, 0, 1, 0) == 0) {
      copyTV(L, L->top - 2, L->top - 1);
      L->top--;
    } else {
      lua_pop(L, 1);
    }
    civ5_ensure_helpers_installed(L);
  }
}

/* ---- host->Lua entry timing (outermost calls only) ---- */
typedef struct Civ5EntEnt { void *id; uint32_t calls; double total_ms, max_ms; char name[120]; } Civ5EntEnt;
#define CIV5_ENT_SLOTS 4096
static Civ5EntEnt *s_ent_tab;
static int s_ent_depth;
static GCfunc *s_ent_fn;
static double s_ent_qpc_ms;
static LARGE_INTEGER s_ent_t0;
static float *s_ent_sec;  /* Lua ms per wall second */
#define CIV5_ENT_SECS 14400

int64_t lj_civ5_entry_begin(lua_State *L, int nargs, void **key)
{
  LARGE_INTEGER t;
  TValue *f;
  if (!s_ent_tab) return 0;
  if (L->base != tvref(L->stack) + 1 || s_ent_depth != 0) return 0;
  f = L->top - nargs - 1;
  if (f < L->base || !tvisfunc(f)) return 0;
  *key = isluafunc(funcV(f)) ? (void *)funcproto(funcV(f)) : (void *)funcV(f)->c.f;
  s_ent_depth = 1;
  s_ent_fn = funcV(f);
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

void lj_civ5_entry_end(void *key, int64_t t0)
{
  LARGE_INTEGER t;
  double ms;
  uint32_t i, probe;
  Civ5EntEnt *e = NULL;
  if (!t0) return;
  QueryPerformanceCounter(&t);
  s_ent_depth = 0;
  if (!s_ent_tab) return;
  ms = (double)(t.QuadPart - t0) * s_ent_qpc_ms;
  i = ((uint32_t)(uintptr_t)key >> 3) % CIV5_ENT_SLOTS;
  for (probe = 0; probe < 128; probe++, i = (i + 1) % CIV5_ENT_SLOTS) {
    if (s_ent_tab[i].id == key) { e = &s_ent_tab[i]; break; }
    if (!s_ent_tab[i].id) {
      e = &s_ent_tab[i]; e->id = key;
      if (s_ent_fn && isluafunc(s_ent_fn)) {
        GCproto *pt = funcproto(s_ent_fn);
        const char *cn = strdata(proto_chunkname(pt));
        const char *a = strstr(cn, "Assets");
        civ5_snprintf(e->name, sizeof(e->name), "%.100s:%d", a ? a : cn, (int)pt->firstline);
      } else {
        civ5_snprintf(e->name, sizeof(e->name), "@%p", key);
      }
      break;
    }
  }
  if (!e) return;
  e->calls++; e->total_ms += ms; if (ms > e->max_ms) e->max_ms = ms;
  if (s_ent_sec) {
    double wall_s = (double)(t.QuadPart - s_ent_t0.QuadPart) * s_ent_qpc_ms / 1000.0;
    if (wall_s >= 0 && wall_s < CIV5_ENT_SECS) s_ent_sec[(int)wall_s] += (float)ms;
  }
}


/* -- 10b. Opt-in sampling profiler (enabled by civ5_profile.enable) ------ */

#define CIV5_PROF_SLOTS 16384
#define CIV5_PROF_KEYLEN 400
typedef struct Civ5ProfEnt {
  uint32_t hash;
  uint32_t count;
  char vm;
  char key[CIV5_PROF_KEYLEN];
} Civ5ProfEnt;

static Civ5ProfEnt *s_prof_tab;
static int s_prof_started;
static uint32_t s_prof_total, s_prof_overflow;
static uint32_t s_prof_win[8];  /* window counts by vm state: N I C G J other */
static DWORD s_prof_last_dump, s_prof_last_tick;
static char s_prof_path[MAX_PATH], s_prof_tl_path[MAX_PATH];

static void civ5_prof_dump(void)
{
  FILE *fp = fopen(s_prof_path, "wb");
  uint32_t i;
  if (!fp) return;
  fprintf(fp, "# total=%u overflow=%u (1 sample ~ 1ms)\n", s_prof_total, s_prof_overflow);
  for (i = 0; i < CIV5_PROF_SLOTS; i++)
    if (s_prof_tab[i].count)
      fprintf(fp, "%u\t%c\t%s\n", s_prof_tab[i].count, s_prof_tab[i].vm, s_prof_tab[i].key);
  fclose(fp);
  {
    char p2[MAX_PATH]; uint32_t k; LARGE_INTEGER now; double wall_ms, lua_ms = 0;
    strcpy(p2, s_prof_path); strcpy(p2 + strlen(p2) - 4, "_entries.tsv");
    fp = fopen(p2, "wb");
    if (fp && s_ent_tab) {
      QueryPerformanceCounter(&now);
      wall_ms = (double)(now.QuadPart - s_ent_t0.QuadPart) * s_ent_qpc_ms;
      for (k = 0; k < CIV5_ENT_SLOTS; k++) if (s_ent_tab[k].id) lua_ms += s_ent_tab[k].total_ms;
      fprintf(fp, "# wall_ms=%.0f lua_entry_ms=%.0f depth=%d\n", wall_ms, lua_ms, s_ent_depth);
      for (k = 0; k < CIV5_ENT_SLOTS; k++) if (s_ent_tab[k].id)
        fprintf(fp, "%u\t%.2f\t%.3f\t%s\n", s_ent_tab[k].calls, s_ent_tab[k].total_ms, s_ent_tab[k].max_ms, s_ent_tab[k].name);
      fclose(fp);
    } else if (fp) fclose(fp);
    strcpy(p2, s_prof_path); strcpy(p2 + strlen(p2) - 4, "_seconds.csv");
    fp = fopen(p2, "wb");
    if (fp && s_ent_sec) {
      int last = 0, q;
      for (q = 0; q < CIV5_ENT_SECS; q++) if (s_ent_sec[q] > 0) last = q;
      for (q = 0; q <= last; q++) fprintf(fp, "%d,%.1f\n", q, s_ent_sec[q]);
      fclose(fp);
    } else if (fp) fclose(fp);
  }
}

static void civ5_walk_stack(lua_State *L, char *out, size_t cap)
{
  int level, size;
  size_t n = 0;
  out[0] = 0;
  for (level = 0; level < 6; level++) {
    cTValue *frame = lj_debug_frame(L, level, &size);
    GCfunc *fn;
    char one[160];
    int w;
    if (!frame) break;
    fn = frame_func(frame);
    if (isluafunc(fn)) {
      GCproto *pt = funcproto(fn);
      const char *cn = strdata(proto_chunkname(pt));
      const char *a = strstr(cn, "Assets");
      w = civ5_snprintf(one, sizeof(one), "%.110s:%d;", a ? a : cn, (int)pt->firstline);
    } else if (iscfunc(fn)) {
      w = civ5_snprintf(one, sizeof(one), "@%p;", (void *)fn->c.f);
    } else {
      w = civ5_snprintf(one, sizeof(one), "[ff%d];", (int)fn->c.ffid);
    }
    if (w < 0 || n + (size_t)w + 1 >= cap) break;
    memcpy(out + n, one, (size_t)w);
    n += (size_t)w;
    out[n] = 0;
  }
}

static void civ5_prof_cb(void *data, lua_State *L, int samples, int vmstate)
{
  size_t len = 0;
  const char *s;
  char key[CIV5_PROF_KEYLEN];
  uint32_t h = 2166136261u, idx;
  int probe, w;
  DWORD now;
  UNUSED(data);

  UNUSED(s);
  civ5_walk_stack(L, key, sizeof(key));
  len = strlen(key);
  if (len == 0) strcpy(key, "(no-lua-frame)");
  if (samples > 4) vmstate = 'X';  /* gap: native time elapsed before Lua resumed */

  h = (h ^ (uint8_t)vmstate) * 16777619u;
  for (idx = 0; key[idx]; idx++) h = (h ^ (uint8_t)key[idx]) * 16777619u;
  idx = h % CIV5_PROF_SLOTS;
  for (probe = 0; probe < 64; probe++, idx = (idx + 1) % CIV5_PROF_SLOTS) {
    Civ5ProfEnt *e = &s_prof_tab[idx];
    if (e->count == 0) {
      e->hash = h; e->vm = (char)vmstate; strcpy(e->key, key); e->count = samples;
      break;
    }
    if (e->hash == h && e->vm == (char)vmstate && strcmp(e->key, key) == 0) {
      e->count += samples; break;
    }
  }
  if (probe == 64) s_prof_overflow += samples;
  s_prof_total += samples;
  w = vmstate == 'N' ? 0 : vmstate == 'I' ? 1 : vmstate == 'C' ? 2 :
      vmstate == 'G' ? 3 : vmstate == 'J' ? 4 : 5;
  s_prof_win[w] += samples;

  now = GetTickCount();
  if (now - s_prof_last_tick >= 1000) {  /* timeline: one line per second */
    FILE *fp = fopen(s_prof_tl_path, "ab");
    if (fp) {
      fprintf(fp, "%u,%u,%u,%u,%u,%u,%u\n", now, s_prof_win[0], s_prof_win[1],
              s_prof_win[2], s_prof_win[3], s_prof_win[4], s_prof_win[5]);
      fclose(fp);
    }
    memset(s_prof_win, 0, sizeof(s_prof_win));
    s_prof_last_tick = now;
  }
  if (now - s_prof_last_dump >= 5000) {
    civ5_prof_dump();
    s_prof_last_dump = now;
  }
}

void lj_civ5_prof_flush(void)
{
  if (s_prof_tab) {
    civ5_prof_dump();
    free(s_prof_tab);
    s_prof_tab = NULL;
    free(s_ent_tab); s_ent_tab = NULL;
    free(s_ent_sec); s_ent_sec = NULL;
  }
  s_prof_started = 0;
}

static void civ5_prof_maybe_start(lua_State *L)
{
  char dir[MAX_PATH], marker[MAX_PATH];
  DWORD n;
  if (s_prof_started) return;
  s_prof_started = 1;
  n = GetEnvironmentVariableA("USERPROFILE", dir, sizeof(dir));
  if (!n || n > MAX_PATH - 100) return;
  strcat(dir, "\\Documents\\My Games\\Sid Meier's Civilization 5\\Logs\\");
  sprintf(marker, "%sciv5_profile.enable", dir);
  if (GetFileAttributesA(marker) == INVALID_FILE_ATTRIBUTES) return;
  sprintf(s_prof_path, "%sciv5_lua_profile.tsv", dir);
  sprintf(s_prof_tl_path, "%sciv5_lua_timeline.csv", dir);
  s_prof_tab = (Civ5ProfEnt *)calloc(CIV5_PROF_SLOTS, sizeof(Civ5ProfEnt));
  if (!s_prof_tab) return;
  { LARGE_INTEGER fq; QueryPerformanceFrequency(&fq); s_ent_qpc_ms = 1000.0 / (double)fq.QuadPart; QueryPerformanceCounter(&s_ent_t0); }
  s_ent_sec = (float *)calloc(CIV5_ENT_SECS, sizeof(float));
  s_ent_tab = (Civ5EntEnt *)calloc(CIV5_ENT_SLOTS, sizeof(Civ5EntEnt));
  DeleteFileA(s_prof_tl_path);
  s_prof_last_dump = s_prof_last_tick = GetTickCount();
  luaJIT_profile_start(L, "i1", civ5_prof_cb, NULL);
}


/* -- 11. Hook for luaL_loadbufferx (GCproto cache + native injection) ---- */

static uint64_t civ5_fnv1a64(const char *buf, size_t size, const char *name)
{
  uint64_t h = 14695981039346656037ULL;
  size_t i;
  for (i = 0; i < size; i++) {
    h ^= (uint8_t)buf[i];
    h *= 1099511628211ULL;
  }
  if (name) {
    while (*name) {
      h ^= (uint8_t)*name++;
      h *= 1099511628211ULL;
    }
  }
  return h;
}

static int civ5_memcontains(const char *haystack, size_t hlen,
                            const char *needle, size_t nlen)
{
  size_t i;
  if (nlen == 0 || hlen < nlen) return 0;
  for (i = 0; i <= hlen - nlen; i++) {
    if (haystack[i] == needle[0] && memcmp(haystack + i, needle, nlen) == 0)
      return 1;
  }
  return 0;
}

typedef struct Civ5BufReaderCtx {
  const char *str;
  size_t size;
} Civ5BufReaderCtx;

static const char *civ5_reader_string(lua_State *L, void *ud, size_t *size)
{
  Civ5BufReaderCtx *ctx = (Civ5BufReaderCtx *)ud;
  UNUSED(L);
  if (ctx->size == 0) return NULL;
  *size = ctx->size;
  ctx->size = 0;
  return ctx->str;
}

int lj_civ5_loadbuffer_hook(lua_State *L, const char *buf, size_t size,
                            const char *name, const char *mode, int *handled)
{
  uint64_t hash;
  char key_buf[32];
  GCstr *cache_key_str, *entry_key_str;
  cTValue *cache_tv, *entry_tv;
  GCtab *reg_tab, *cache_tab;
  const char *suffix = NULL;
  const char *load_buf = buf;
  size_t load_size = size;
  char *patched_buf = NULL;
  Civ5BufReaderCtx ctx;
  int status;

  *handled = 0;
  if (!lj_civ5_accel_enabled() || !buf || size == 0)
    return 0;
  /* Skip binary bytecode chunks */
  if (buf[0] == LUA_SIGNATURE[0] || (mode && strchr(mode, 'b') && !strchr(mode, 't')))
    return 0;
  /* Skip internal chunks created by lj_civ5_accel itself */
  if (name && strncmp(name, "=[civ5_", 7) == 0)
    return 0;

  /* Guarantee helpers exist in _G, _G.math, and L->env before proto cache hit or compile */
  civ5_ensure_helpers_installed(L);
  civ5_prof_maybe_start(L);

  hash = civ5_fnv1a64(buf, size, name);
  sprintf(key_buf, "%08x_%08x_%08x",
          (uint32_t)(hash >> 32), (uint32_t)hash, (uint32_t)size);

  reg_tab = tabV(registry(L));
  cache_key_str = lj_str_newlit(L, "__civ5_proto_cache");
  cache_tv = lj_tab_getstr(reg_tab, cache_key_str);
  if (!cache_tv || !tvistab(cache_tv)) {
    TValue *slot;
    cache_tab = lj_tab_new(L, 0, 6);
    slot = lj_tab_setstr(L, reg_tab, cache_key_str);
    settabV(L, slot, cache_tab);
    lj_gc_anybarriert(L, reg_tab);
  } else {
    cache_tab = tabV(cache_tv);
  }

  entry_key_str = lj_str_newz(L, key_buf);
  entry_tv = lj_tab_getstr(cache_tab, entry_key_str);
  if (entry_tv && tvisproto(entry_tv)) {
    /* Fast path: clone a fresh Lua closure from the cached GCproto* */
    GCproto *pt = protoV(entry_tv);
    GCfunc *fn = lj_func_newL_empty(L, pt, tabref(L->env));
    setfuncV(L, L->top++, fn);
    lj_gc_check(L);
    *handled = 1;
    return 0;
  }

  /* Detect target scripts by name or signature */
  if ((name && strstr(name, "MapmakerUtilities") != NULL) ||
      (size > 1000 && civ5_memcontains(buf, size, "function GetShuffledCopyOfTable(", 32))) {
    suffix = "\nlocal _p = __civ5_patch_mmu or (math and math.__civ5_patch_mmu); if _p then _p() end\n";
  } else if ((name && strstr(name, "AssignStartingPlots") != NULL) ||
             (size > 5000 && civ5_memcontains(buf, size, "function AssignStartingPlots:PlaceResourceImpact(", 49))) {
    suffix = "\nlocal _p = __civ5_patch_asp or (math and math.__civ5_patch_asp); if _p then _p(AssignStartingPlots) end\n";
  } else if ((name && strstr(name, "FractalWorld") != NULL) ||
             (size > 1000 && civ5_memcontains(buf, size, "function FractalWorld:ShiftPlotTypes(", 37))) {
    suffix = "\nlocal _p = __civ5_patch_fw or (math and math.__civ5_patch_fw); if _p then _p(FractalWorld) end\n";
  } else if ((name && strstr(name, "MultilayeredFractal") != NULL) ||
             (size > 1000 && civ5_memcontains(buf, size, "function MultilayeredFractal:ShiftRegionPlots(", 46))) {
    suffix = "\nlocal _p = __civ5_patch_mlf or (math and math.__civ5_patch_mlf); if _p then _p(MultilayeredFractal) end\n";
  } else if ((name && strstr(name, "InstanceManager") != NULL) ||
             (size > 500 && civ5_memcontains(buf, size, "GenerationalInstanceManager =", 29))) {
    suffix = "\nlocal _p = __civ5_patch_im or (math and math.__civ5_patch_im); if _p then _p(InstanceManager, GenerationalInstanceManager) end\n";
  } else if ((name && strstr(name, "FLuaVector") != NULL) ||
             (size > 200 && size < 4000 && civ5_memcontains(buf, size, "function Vector2(", 17) &&
              civ5_memcontains(buf, size, "function VecSubtract(", 21))) {
    suffix = k_civ5_vec_lua;
  } else if ((name && strstr(name, "IconSupport") != NULL) ||
             (size > 1000 && civ5_memcontains(buf, size, "function PopulateIconAtlases()", 30))) {
    suffix = k_civ5_icon_suffix_lua;
  }

  if (suffix) {
    size_t slen = strlen(suffix);
    patched_buf = (char *)malloc(size + slen + 1);
    if (patched_buf) {
      memcpy(patched_buf, buf, size);
      memcpy(patched_buf + size, suffix, slen + 1);
      load_buf = patched_buf;
      load_size = size + slen;
    }
  }

  ctx.str = load_buf;
  ctx.size = load_size;
  status = lua_loadx(L, civ5_reader_string, &ctx, name, mode);
  if (patched_buf)
    free(patched_buf);

  if (status == 0 && L->top > L->base &&
      tvisfunc(L->top - 1) && isluafunc(funcV(L->top - 1))) {
    /* Keep at most 256 prototypes; clear the cache when it fills. */
    TValue *slot;
    reg_tab = tabV(registry(L));
    cache_key_str = lj_str_newlit(L, "__civ5_proto_cache");
    cache_tv = lj_tab_getstr(reg_tab, cache_key_str);
    if (cache_tv && tvistab(cache_tv)) {
      cache_tab = tabV(cache_tv);
      cTValue *count_tv = lj_tab_getint(cache_tab, 1);
      int32_t count = count_tv && tvisnumber(count_tv) ? numberVint(count_tv) : 0;
      if (count >= 256) {
        lj_tab_clear(cache_tab);
        count = 0;
      }
      slot = lj_tab_setint(L, cache_tab, 1);
      setintV(slot, count + 1);
      entry_key_str = lj_str_newz(L, key_buf);
      slot = lj_tab_setstr(L, cache_tab, entry_key_str);
      setprotoV(L, slot, funcproto(funcV(L->top - 1)));
      lj_gc_barriert(L, cache_tab, slot);
    }
  }

  *handled = 1;
  return status;
}
