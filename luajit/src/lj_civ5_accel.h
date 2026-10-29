/*
** Civilization V Native C Accelerator & LuaJIT Bridge
*/

#ifndef _LJ_CIV5_ACCEL_H
#define _LJ_CIV5_ACCEL_H

#include "lua.h"
#include "lj_obj.h"

int lj_civ5_accel_check_newstate(void);
int lj_civ5_accel_enabled(void);
void lj_civ5_prof_flush(void);
int64_t lj_civ5_entry_begin(lua_State *L, int nargs, void **key);
void lj_civ5_entry_end(void *key, int64_t t0);
void lj_civ5_intercept_setfield(lua_State *L, int idx, const char *k);
int lj_civ5_loadbuffer_hook(lua_State *L, const char *buf, size_t size,
                            const char *name, const char *mode, int *handled);

#endif
