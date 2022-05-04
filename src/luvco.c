#include <luvco.h>
#include <luvco/base.h>
#include <luvco/object.h>
#include <luvco/scheduler.h>
#include <luvco/chan.h>
#include <luvco/ringbuf.h>

#include <lua/lua.h>
#include <lua/lauxlib.h>
#include <lua/lualib.h>
#include <uv.h>

#include <stdlib.h>
#include <assert.h>
#include <string.h>

static const char* LSTATE_FIELD = "luvco.local_state";
static const char* MAINSTATE_TAG_FILED = "luvco.main_state_tag";

static int RESUME_QUEUE_SIZE = 8;
static int RESUME_QUEUE_BUF_SIZE = 8;

static void local_state_init (luvco_lstate* lstate, luvco_gstate* gstate) {
    log_trace("local state init %p", lstate);
    lstate->gstate = gstate;
    lstate->toresume = (luvco_ringbuf2*)malloc(luvco_ringbuf2_sizeof(RESUME_QUEUE_SIZE));
    luvco_ringbuf2_init(lstate->toresume, RESUME_QUEUE_SIZE, RESUME_QUEUE_BUF_SIZE);
    lstate->coro_count = 0;
}

static void local_state_delete (luvco_lstate* lstate) {
    log_trace("local state delete %p", lstate);
    luvco_ringbuf2_delete(lstate->toresume);
    free(lstate->toresume);
}

static void print_all_handle (uv_handle_t* h, void* args) {
    printf("--%p\t%d:%s\n", h, h->type, uv_handle_type_name(h->type));
}

// top of stack is coroutine thread. pop it and register
// prevent gc collect coroutine
static void register_coro (luvco_lstate* lstate, lua_State* L) {
    luaL_checktype(L, -1, LUA_TTHREAD);
    lua_pushlightuserdata(L, (void*)L);
    lua_rotate(L, -2, 1);
    lua_settable(L, LUA_REGISTRYINDEX); // K: light userdata pointer, V: thread

    lstate->coro_count++;
}

// return 1 if all coro end
static int unregister_coro (luvco_lstate* lstate, lua_State* L) {
    log_trace("unregister coro %p", L);
    lua_pushlightuserdata(L, (void*)L);
    lua_pushnil(L);
    lua_settable(L, LUA_REGISTRYINDEX);

    if (--lstate->coro_count == 0) {
        return 1;
    }
    return 0;
}

// lua_state has only one shared luvco_gstate
static void luvco_init_luastate (lua_State* L, luvco_gstate* gstate) {
    luaL_requiref(L, "luvco", luvco_open_base, 1);
    lua_pop(L, 1);

    luvco_lstate* lstate = (luvco_lstate*)malloc(sizeof(luvco_lstate));
    lua_pushlightuserdata(L, lstate);
    lua_setfield(L, LUA_REGISTRYINDEX, LSTATE_FIELD);
    local_state_init(lstate, gstate);
    lstate->coro_count = 1;
}

luvco_lstate* luvco_get_state (lua_State* L) {
    lua_getfield(L, LUA_REGISTRYINDEX, LSTATE_FIELD);
    luvco_lstate* state = (luvco_lstate*)lua_touserdata(L, -1);
    lua_pop(L, 1);
    return state;
}

void luvco_yield (lua_State *L, lua_KContext k_ctx, lua_KFunction k) {
    // no use the yield result
    lua_yieldk(L, 0, k_ctx, k);
}

enum luvco_special_yield_tag {
    LUVCO_YIELD_THREAD = 1086,
};

void luvco_yield_thread (lua_State *L, lua_KContext k_ctx, lua_KFunction k, luvco_cb_f f, void* ud) {
    lua_pushlightuserdata(L, f);
    lua_pushlightuserdata(L, ud);
    lua_pushlightuserdata(L, (void*)LUVCO_YIELD_THREAD);
    lua_yieldk(L, 0, k_ctx, k);
}

void luvco_toresume (luvco_lstate* lstate, lua_State* L, int nargs) {
    assert(nargs >= 0 && nargs <= 3 && "args should between 0 and 3");
    assert(((intptr_t)L & 3) == 0 && "lua state not align, can't use low bit as flag");

    L = (lua_State*)((intptr_t)L | nargs);
    luvco_ringbuf2_spinpush(lstate->toresume, L);
}

enum luvco_resume_return luvco_resume (lua_State_flag* Lb) {
    int nargs = (intptr_t)Lb & 3;
    lua_State* L = (lua_State*)((intptr_t)Lb & (~3));
    luvco_lstate* lstate = luvco_get_state(L);

    int res;
    int ret = lua_resume(L, NULL, nargs, &res);
    if (res != 0) {
        log_error("unexpected lua resume return");
        luvco_dump_lua_stack(L);
        return LUVCO_RESUME_ERROR;
    }

    switch (ret) {
    case LUA_YIELD:
        if (lua_gettop(L) >= 1) {
            intptr_t tag = (intptr_t)lua_touserdata(L, -1);
            switch (tag) {
            case LUVCO_YIELD_THREAD:{
                luvco_cb_f afterf = lua_touserdata(L, -3);
                void* afterf_ud= lua_touserdata(L, -2);
                lua_pop(L, 3);
                if (afterf != NULL) {
                    afterf(afterf_ud);
                }
                return LUVCO_RESUME_YIELD_THREAD;
            } break;
            }
        }
        return LUVCO_RESUME_NORMAL;
    case LUA_OK:
        ret = unregister_coro(lstate, L);
        if (ret != 1) {
            return LUVCO_RESUME_NORMAL;
        }
        int ty = lua_getfield(L, LUA_REGISTRYINDEX, MAINSTATE_TAG_FILED);
        lua_pop(L, 1);
        if (ty == LUA_TNIL) {
            log_trace("lstate:%p, all coro end, close L:%p", lstate, L);
            lua_close(L);               // 1, close L
            local_state_delete(lstate); // 2, close lstate.  ORDER IS IMPORTANT!!!
            free(lstate);
        } else {
            log_trace("lstate:%p, all coro end, is main coro, gc", lstate);
            lua_gc(L, LUA_GCCOLLECT);
            local_state_delete(lstate);
            free(lstate);
        }
        return LUVCO_RESUME_LSTATE_END;
    default:
        log_error("luvco resume error");
        luvco_dump_lua_stack(L);
        return LUVCO_RESUME_ERROR;
    }
    assert(false && "error");
}

static int spawn_k (lua_State *L, int status, lua_KContext ctx) {
    return 0;
}

static int spawn (lua_State* L) {
    luaL_checktype(L, 1, LUA_TFUNCTION);
    luvco_lstate* lstate = luvco_get_state(L);

    lua_State* NL = lua_newthread(L);
    register_coro(lstate, L);

    lua_xmove(L, NL, 1);  // pop function from L to NL

    log_trace("spawn from L:%p, NL:%p", L, NL);

    luvco_toresume(lstate, NL, 0);
    luvco_toresume(lstate, L, 0);
    luvco_yield(L, (lua_KContext)NULL, spawn_k);
}

static int ispawn_k (lua_State *L, int status, lua_KContext ctx) {
    return (int)(intptr_t)ctx;
}

#define ispawn_base_code \
    size_t str_len; \
    const char* code = luaL_checklstring(L, 1, &str_len); \
    luvco_lstate* lstate = luvco_get_state(L); \
    luvco_gstate* gstate = lstate->gstate; \
    lua_State* NL = (gstate->newluaf)(gstate->newluaf_ud); \
    luaL_loadbuffer(NL, code, str_len, "ispawn_NL_code"); \
    luvco_init_luastate(NL, gstate); \
    luvco_lstate* nlstate = luvco_get_state(NL); \
    log_trace("ispawn from L:%p, lstate:%p, NL:%p, nlstate:%p", L, lstate, NL, nlstate);

static int ispawn (lua_State* L) {
    ispawn_base_code
    luvco_toresume(lstate, L, 0);
    luvco_toresume(nlstate, NL, 0);
    luvco_scheduler_addwork(gstate->scheduler, nlstate);
    luvco_yield(L, (lua_KContext)0, ispawn_k);
}

static int ispawn_r (lua_State* L) {
    ispawn_base_code
    luaL_requiref(NL, "luvco_chan", luvco_open_chan, 0);
    lua_pop(NL, 1);

    luvco_chan1_build(NL, L);
    lua_setglobal(NL, "send_parent");

    luvco_toresume(lstate, L, 1);
    luvco_toresume(nlstate, NL, 0);
    luvco_scheduler_addwork(gstate->scheduler, nlstate);
    luvco_yield(L, (lua_KContext)1, ispawn_k);
}

static int ispawn_s (lua_State* L) {
    ispawn_base_code
    luaL_requiref(NL, "luvco_chan", luvco_open_chan, 0);
    lua_pop(NL, 1);

    luvco_chan1_build(L, NL);
    lua_setglobal(NL, "recv_parent");

    luvco_toresume(lstate, L, 1);
    luvco_toresume(nlstate, NL, 0);
    luvco_scheduler_addwork(gstate->scheduler, nlstate);
    luvco_yield(L, (lua_KContext)1, ispawn_k);
}

static int ispawn_rs (lua_State* L) {
    ispawn_base_code
    luaL_requiref(NL, "luvco_chan", luvco_open_chan, 0);
    lua_pop(NL, 1);

    luvco_chan1_build(NL, L);
    lua_setglobal(NL, "send_parent");
    luvco_chan1_build(L, NL);
    lua_setglobal(NL, "recv_parent");

    luvco_toresume(lstate, L, 2);
    luvco_toresume(nlstate, NL, 0);
    luvco_scheduler_addwork(gstate->scheduler, nlstate);
    luvco_yield(L, (lua_KContext)2, ispawn_k);
}

static int import_lib (lua_State *L) {
    const char* s = luaL_checkstring(L, 1);
    if (strcmp(s, "lualibs") == 0) {
        luaL_openlibs(L);
        return 0;
    } else if (strcmp(s, "net") == 0) {
        luaL_requiref(L, "luvco_net", luvco_open_net, 0);
    } else if (strcmp(s, "chan") == 0) {
        luaL_requiref(L, "luvco_chan", luvco_open_chan, 0);
    }
    return 1;
}

static const luaL_Reg base_lib [] = {
    { "spawn", spawn },
    { "ispawn", ispawn },
    { "import", import_lib },
    { NULL, NULL }
};

static const luaL_Reg base_add_chan_lib [] = {
    { "ispawn_r", ispawn_r },
    { "ispawn_s", ispawn_s },
    { "ispawn_rs", ispawn_rs },
    { NULL, NULL }
};

int luvco_open_base (lua_State* L) {
    luaL_newlib(L, base_lib);
    return 1;
}

void luvco_open_chan_withbase (lua_State* L) {
    int ty = lua_getglobal(L, "luvco");
    if (ty == LUA_TTABLE) {
        luaL_setfuncs(L, base_add_chan_lib, 0);
    }
    lua_pop(L, 1);
}

static lua_State* default_newluaf (void* ud) {
    return luaL_newstate();
}

luvco_gstate* luvco_init (lua_State* L, luvco_newluaf f, void* f_ud) {
    if (f == NULL) {
        f = default_newluaf;
    }

    log_trace("luvco init main_coro L:%p", L);

    luvco_gstate* state = malloc(sizeof(luvco_gstate));
    state->main_coro = L;
    state->newluaf = f;
    state->newluaf_ud = f_ud;
    state->uvworklist = (luvco_ringbuf2*)malloc(luvco_ringbuf2_sizeof(4));
    luvco_ringbuf2_init(state->uvworklist, 4, 32);

    uv_loop_init(&state->loop);

    luvco_init_luastate(L, state);
    lua_pushinteger(L, 0);
    lua_setfield(L, LUA_REGISTRYINDEX, MAINSTATE_TAG_FILED);

    return state;
}

#define NPROCESS 4

typedef struct luvco_deamon_data {
    uv_idle_t idle;
    luvco_ringbuf2* uvworklist;
    luvco_scheduler* s;
} luvco_deamon_data;

static void deamon_close_cb (uv_handle_t* handle) {
    log_debug("deamon close cb called");
}

static void luvco_deamon (uv_idle_t* handle) {
    luvco_deamon_data* deamon = container_of(handle, luvco_deamon_data, idle);
    luvco_uvwork* uvwork;
    while (luvco_ringbuf2_unlockpop(deamon->uvworklist, (void**)&uvwork) == 0) {
        uvwork->cb(uvwork);
    }

    if ( luvco_scheduler_totalwork(deamon->s) == 0 ) {
        log_debug("deamon: all work end, quit event loop");
        uv_idle_stop(handle);
        uv_close((uv_handle_t*)handle, deamon_close_cb);
    }
}

void luvco_run (luvco_gstate* state) {
    log_trace("luvco run, global state:%p", state);

    luvco_scheduler* s = (luvco_scheduler*)malloc(luvco_scheduler_sizeof(NPROCESS));
    luvco_scheduler_init(s, NPROCESS);
    state->scheduler = s;
    luvco_lstate* lstate = luvco_get_state(state->main_coro);
    luvco_toresume(lstate, state->main_coro, 0);
    luvco_scheduler_addwork(s, lstate);

    luvco_deamon_data deamon;
    deamon.s = s;
    deamon.uvworklist = state->uvworklist;
    uv_idle_init(&state->loop, &deamon.idle);
    uv_idle_start(&deamon.idle, luvco_deamon);
    uv_run(&state->loop, UV_RUN_DEFAULT);

    luvco_scheduler_stop(s);
    luvco_scheduler_delete(s);
    free(s);

    log_trace("luvco end, global state:%p", state);
}


void luvco_close (luvco_gstate* state) {
    int ret = uv_loop_close(&state->loop);
    if (ret == UV_EBUSY) {
        log_error("uv close return UV_EBUSY");
        uv_walk(&state->loop, print_all_handle, NULL);
    } else {
        log_debug("uv close");
    }

    // clean main lua state
    lua_State* L = state->main_coro;
    lua_pushnil(L);
    lua_setfield(L, LUA_REGISTRYINDEX, LSTATE_FIELD);

    lua_gc(L, LUA_GCCOLLECT);
    luvco_ringbuf2_delete(state->uvworklist);
    free(state->uvworklist);
    free(state);
}
