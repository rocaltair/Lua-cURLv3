#include "lcurl.h"
#include "lceasy.h"
#include "lcerror.h"
#include "lcutils.h"
#include "lchttppost.h"

static const char *LCURL_ERROR_TAG = "LCURL_ERROR_TAG";

#define LCURL_EASY_NAME LCURL_PREFIX" Easy"
static const char *LCURL_EASY = LCURL_EASY_NAME;

typedef struct lcurl_callback_tag{
  int cb_ref;
  int ud_ref;
}lcurl_callback_t;

typedef struct lcurl_read_buffer_tag{
  int ref;
  int off;
}lcurl_read_buffer_t;


#define LCURL_LST_INDEX(N) LCURL_##N##_LIST,
#define LCURL_STR_INDEX(N)
#define LCURL_LNG_INDEX(N)
#define OPT_ENTRY(L, N, T, S) LCURL_##T##_INDEX(N)

enum {
  LCURL_LISY_DUMMY = -1,

  #include"lcopteasy.h"

  LCURL_LIST_COUNT,
};

#undef LCURL_LST_INDEX
#undef LCURL_STR_INDEX
#undef LCURL_LNG_INDEX
#undef OPT_ENTRY

typedef struct lcurl_easy_tag{
  lua_State *L;
  CURL *curl;
  int storage;
  int lists[LCURL_LIST_COUNT];
  int err_mode;
  lcurl_callback_t wr;
  lcurl_callback_t rd;
  lcurl_read_buffer_t rbuffer;
  
}lcurl_easy_t;

//{

int lcurl_easy_create(lua_State *L, int error_mode){
  lcurl_easy_t *p = lutil_newudatap(L, lcurl_easy_t, LCURL_EASY);
  int i;
  p->L = L;
  p->curl = curl_easy_init();
  if(!p->curl) return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, CURLE_FAILED_INIT);
  p->storage     = lcurl_storage_init(L);
  p->err_mode    = error_mode;
  p->wr.cb_ref   = p->wr.ud_ref = LUA_NOREF;
  p->rd.cb_ref   = p->rd.ud_ref = LUA_NOREF;
  p->rbuffer.ref = LUA_NOREF;
  for(i = 0; i < LCURL_LIST_COUNT; ++i){
    p->lists[i] = LUA_NOREF;
  }
  return 1;
}

static lcurl_easy_t *lcurl_geteasy_at(lua_State *L, int i){
  lcurl_easy_t *p = (lcurl_easy_t *)lutil_checkudatap (L, i, LCURL_EASY);
  luaL_argcheck (L, p != NULL, 1, LCURL_PREFIX"HTTPPost object expected");
  return p;
}

#define lcurl_geteasy(L) lcurl_geteasy_at((L),1)

static int lcurl_easy_cleanup(lua_State *L){
  lcurl_easy_t *p = lcurl_geteasy(L);
  if(p->curl){
    curl_easy_cleanup(p->curl);
    p->curl = NULL;
  }

  if(p->storage != LUA_NOREF){
    lcurl_storage_free(L, p->storage);
    p->storage = LUA_NOREF;
  }

  return 0;
}

static int lcurl_easy_perform(lua_State *L){
  lcurl_easy_t *p = lcurl_geteasy(L);
  CURLcode code;
  int top = 1;
  lua_settop(L, top);

  assert(p->rbuffer.ref == LUA_NOREF);

  code = curl_easy_perform(p->curl);

  if(p->rbuffer.ref != LUA_NOREF){
    luaL_unref(L, LCURL_LUA_REGISTRY, p->rbuffer.ref);
    p->rbuffer.ref = LUA_NOREF;
  }

  if(code == CURLE_OK){
    lua_settop(L, 1);
    return 1;
  }

  if((lua_gettop(L) > top)&&(lua_touserdata(L, top + 1) == LCURL_ERROR_TAG)){
    return lua_error(L);
  }

  if(code == CURLE_WRITE_ERROR){
    if(lua_gettop(L) > top){
      return lua_gettop(L) - top;
    }
  }

  if(code == CURLE_ABORTED_BY_CALLBACK){
    if(lua_gettop(L) > top){
      return lua_gettop(L) - top;
    }
  }

  return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
}

//{ OPTIONS

static int lcurl_opt_set_long_(lua_State *L, int opt){
  lcurl_easy_t *p = lcurl_geteasy(L);
  long val; CURLcode code;

  if(lua_isboolean(L, 2)) val = lua_toboolean(L, 2);
  else val = luaL_checklong(L, 2);
  
  code = curl_easy_setopt(p->curl, opt, val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }
  lua_settop(L, 1);
  return 1;
}

static int lcurl_opt_set_string_(lua_State *L, int opt, int store){
  lcurl_easy_t *p = lcurl_geteasy(L);
  const char *val = luaL_checkstring(L, 2);
  CURLcode code = curl_easy_setopt(p->curl, opt, val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }
  if(store)lcurl_storage_preserve_value(L, p->storage, 2);
  lua_settop(L, 1);
  return 1;
}

static int lcurl_opt_set_slist_(lua_State *L, int opt, int list_no){
  lcurl_easy_t *p = lcurl_geteasy(L);
  struct curl_slist *list = lcurl_util_to_slist(L, 2);
  CURLcode code;
  int ref = p->lists[list_no];

  luaL_argcheck(L, list, 2, "array expected");

  if(ref != LUA_NOREF){
    struct curl_slist *tmp = lcurl_storage_remove_slist(L, p->storage, ref);
    curl_slist_free_all(tmp);
    p->lists[list_no] = LUA_NOREF;
  }

  code = curl_easy_setopt(p->curl, opt, list);

  if(code != CURLE_OK){
    curl_slist_free_all(list);
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }

  p->lists[list_no] = lcurl_storage_preserve_slist(L, p->storage, list);
  lua_settop(L, 1);
  return 1;
}

#define LCURL_STR_OPT(N, S) static int lcurl_easy_set_##N(lua_State *L){\
  return lcurl_opt_set_string_(L, CURLOPT_##N, (S)); \
}

#define LCURL_LST_OPT(N, S) static int lcurl_easy_set_##N(lua_State *L){\
  return lcurl_opt_set_slist_(L, CURLOPT_##N, LCURL_##N##_LIST);\
}

#define LCURL_LNG_OPT(N, S) static int lcurl_easy_set_##N(lua_State *L){\
  return lcurl_opt_set_long_(L, CURLOPT_##N);\
}

#define OPT_ENTRY(L, N, T, S) LCURL_##T##_OPT(N, S)

#include "lcopteasy.h"

#undef OPT_ENTRY

static int lcurl_easy_set_POSTFIELDS(lua_State *L){
  lcurl_easy_t *p = lcurl_geteasy(L);
  size_t len; const char *val = luaL_checklstring(L, 2, &len);
  CURLcode code;
  if(lua_isnumber(L, 3)){
    size_t n = (size_t)lua_tonumber(L, 3);
    luaL_argcheck(L, len <= n, 3, "data length too big");
    len = n;
  }
  code = curl_easy_setopt(p->curl, CURLOPT_POSTFIELDS, val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }
  lcurl_storage_preserve_value(L, p->storage, 2);
  code = curl_easy_setopt(p->curl, CURLOPT_POSTFIELDSIZE, (long)len);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }
  lua_settop(L, 1);
  return 1;
}

#undef LCURL_STR_OPT
#undef LCURL_LST_OPT
#undef LCURL_LNG_OPT

static int lcurl_easy_set_HTTPPOST(lua_State *L){
  lcurl_easy_t *p = lcurl_geteasy(L);
  lcurl_hpost_t *post = lcurl_gethpost_at(L, 2);
  CURLcode code = curl_easy_setopt(p->curl, CURLOPT_HTTPPOST, post->post);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }

  lcurl_storage_preserve_value(L, p->storage, 2);

  lua_settop(L, 1);
  return 1;
}

//}

//{ info

static int lcurl_info_get_long_(lua_State *L, int opt){
  lcurl_easy_t *p = lcurl_geteasy(L);
  long val; CURLcode code;

  code = curl_easy_getinfo(p->curl, opt, &val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }

  lua_pushnumber(L, val);
  return 1;
}

static int lcurl_info_get_double_(lua_State *L, int opt){
  lcurl_easy_t *p = lcurl_geteasy(L);
  double val; CURLcode code;

  code = curl_easy_getinfo(p->curl, opt, &val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }

  lua_pushnumber(L, val);
  return 1;
}

static int lcurl_info_get_string_(lua_State *L, int opt){
  lcurl_easy_t *p = lcurl_geteasy(L);
  char *val; CURLcode code;
  
  code = curl_easy_getinfo(p->curl, opt, &val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }

  lua_pushstring(L, val);
  return 1;
}

static int lcurl_info_get_slist_(lua_State *L, int opt){
  lcurl_easy_t *p = lcurl_geteasy(L);
  struct curl_slist *val; CURLcode code;
  
  code = curl_easy_getinfo(p->curl, opt, &val);
  if(code != CURLE_OK){
    return lcurl_fail_ex(L, p->err_mode, LCURL_ERROR_EASY, code);
  }

  lcurl_util_slist_to_table(L, val);
  curl_slist_free_all(val);

  return 1;
}

#define LCURL_STR_INFO(N, S) static int lcurl_easy_get_##N(lua_State *L){\
  return lcurl_info_get_string_(L, CURLINFO_##N); \
}

#define LCURL_LST_INFO(N, S) static int lcurl_easy_get_##N(lua_State *L){\
  return lcurl_info_get_slist_(L, CURLINFO_##N);\
}

#define LCURL_LNG_INFO(N, S) static int lcurl_easy_get_##N(lua_State *L){\
  return lcurl_info_get_long_(L, CURLINFO_##N);\
}

#define LCURL_DBL_INFO(N, S) static int lcurl_easy_get_##N(lua_State *L){\
  return lcurl_info_get_double_(L, CURLINFO_##N);\
}

#define OPT_ENTRY(L, N, T, S) LCURL_##T##_INFO(N, S)

#include "lcinfoeasy.h"

#undef OPT_ENTRY

#undef LCURL_STR_INFO
#undef LCURL_LST_INFO
#undef LCURL_LNG_INFO
#undef LCURL_DBL_INFO

//}

//{ CallBack

static int lcurl_util_push_cb(lua_State *L, lcurl_callback_t *c){
  assert(c->cb_ref != LUA_NOREF);
  lua_rawgeti(L, LCURL_LUA_REGISTRY, c->cb_ref);
  if(c->ud_ref != LUA_NOREF){
    lua_rawgeti(L, LCURL_LUA_REGISTRY, c->ud_ref);
    return 2;
  }
  return 1;
}

static int lcurl_easy_set_callback(lua_State *L, 
  lcurl_easy_t *p, lcurl_callback_t *c,
  int OPT_CB, int OPT_UD,
  const char *method, void *func
)
{
  if(c->ud_ref != LUA_NOREF){
    luaL_unref(L, LCURL_LUA_REGISTRY, c->ud_ref);
    c->ud_ref = LUA_NOREF;
  }

  if(c->cb_ref != LUA_NOREF){
    luaL_unref(L, LCURL_LUA_REGISTRY, c->cb_ref);
    c->cb_ref = LUA_NOREF;
  }

  if(lua_gettop(L) >= 3){// function + context
    lua_settop(L, 3);
    luaL_argcheck(L, !lua_isnil(L, 2), 2, "no function present");
    c->ud_ref = luaL_ref(L, LCURL_LUA_REGISTRY);
    c->cb_ref = luaL_ref(L, LCURL_LUA_REGISTRY);
    assert(1 == lua_gettop(L));
    return 1;
  }

  lua_settop(L, 2);

  if(lua_isnoneornil(L, 2)){
    lua_pop(L, 1);
    assert(1 == lua_gettop(L));

    curl_easy_setopt(p->curl, OPT_UD, 0);
    curl_easy_setopt(p->curl, OPT_CB, 0);

    return 1;
  }

  if(lua_isfunction(L, 2)){
    c->cb_ref = luaL_ref(L, LCURL_LUA_REGISTRY);
    assert(1 == lua_gettop(L));

    curl_easy_setopt(p->curl, OPT_UD, p);
    curl_easy_setopt(p->curl, OPT_CB, func);
    return 1;
  }

  if(lua_isuserdata(L, 2) || lua_istable(L, 2)){
    lua_getfield(L, 2, method);
    luaL_argcheck(L, lua_isfunction(L, -1), 2, "method not found in object");
    c->cb_ref = luaL_ref(L, LCURL_LUA_REGISTRY);
    c->ud_ref = luaL_ref(L, LCURL_LUA_REGISTRY);
    curl_easy_setopt(p->curl, OPT_UD, p);
    curl_easy_setopt(p->curl, OPT_CB, func);
    assert(1 == lua_gettop(L));
    return 1;
  }

  lua_pushliteral(L, "invalid object type");
  return lua_error(L);
}


//{ Writer

static int lcurl_write_callback(char *ptr, size_t size, size_t nmemb, void *arg){
  lcurl_easy_t *p = arg;
  lua_State *L = p->L;

  size_t ret = size * nmemb;
  int    top = lua_gettop(L);
  int    n   = lcurl_util_push_cb(L, &p->wr);

  lua_pushlstring(L, ptr, ret);
  if(lua_pcall(L, n, LUA_MULTRET, 0)){
    assert(lua_gettop(L) >= top);
    lua_pushlightuserdata(L, LCURL_ERROR_TAG);
    lua_insert(L, top+1);
    return 0;
  }

  if(lua_gettop(L) > top){
    if(lua_isnil(L, top + 1)) return 0;
    if(lua_isboolean(L, top + 1)){
      if(!lua_toboolean(L, top + 1)) ret = 0;
    }
    else ret = (size_t)lua_tonumber(L, top + 1);
  }

  lua_settop(L, top);
  return ret;
}

static int lcurl_easy_set_WRITEFUNCTION(lua_State *L){
  lcurl_easy_t *p = lcurl_geteasy(L);
  return lcurl_easy_set_callback(L, p, &p->wr,
    CURLOPT_WRITEFUNCTION, CURLOPT_WRITEDATA,
    "write", lcurl_write_callback
  );
}

//}

//{ Reader

static int lcurl_read_callback(char *buffer, size_t size, size_t nitems, void *arg){
  lcurl_easy_t *p = arg;
  lua_State *L = p->L;

  const char *data; size_t data_size;

  size_t ret = size * nitems;
  int n, top = lua_gettop(L);

  if(p->rbuffer.ref != LUA_NOREF){
    lua_rawgeti(L, LCURL_LUA_REGISTRY, p->rbuffer.ref);
    data = luaL_checklstring(L, -1, &data_size);
    lua_pop(L, 1);

    data = data + p->rbuffer.off;
    data_size -= p->rbuffer.off;

    if(data_size > ret){
      data_size = ret;
      memcpy(buffer, data, data_size);
      p->rbuffer.off += data_size;
    }
    else{
      memcpy(buffer, data, data_size);
      luaL_unref(L, LCURL_LUA_REGISTRY, p->rbuffer.ref);
      p->rbuffer.ref = LUA_NOREF;
    }

    lua_settop(L, top);
    return data_size;
  }

  // buffer is clean
  assert(p->rbuffer.ref == LUA_NOREF);

  n = lcurl_util_push_cb(L, &p->rd);
  lua_pushnumber(L, ret);
  if(lua_pcall(L, n, LUA_MULTRET, 0)) return CURL_READFUNC_ABORT;

  if(lua_isnoneornil(L, top + 1)) return CURL_READFUNC_ABORT;
  data = lua_tolstring(L, -1, &data_size);
  if(!data) return CURL_READFUNC_ABORT;

  if(data_size > ret){
    data_size = ret;
    p->rbuffer.ref = luaL_ref(L, LCURL_LUA_REGISTRY);
    p->rbuffer.off = data_size;
  }
  memcpy(buffer, data, data_size);

  lua_settop(L, top);
  return data_size;
}

static int lcurl_easy_set_READFUNCTION(lua_State *L){
  lcurl_easy_t *p = lcurl_geteasy(L);
  return lcurl_easy_set_callback(L, p, &p->rd, 
    CURLOPT_READFUNCTION, CURLOPT_READDATA,
    "read", lcurl_read_callback
  );
}

//}

//}

//}


#define OPT_ENTRY(L, N, T, S) { "setopt_"#L, lcurl_easy_set_##N },
static const struct luaL_Reg lcurl_easy_methods[] = {

  #include "lcopteasy.h"

  OPT_ENTRY(httppost,      HTTPPOST,      TTT, 0)
  OPT_ENTRY(writefunction, WRITEFUNCTION, TTT, 0)
  OPT_ENTRY(readfunction,  READFUNCTION,  TTT, 0)

  {"getinfo_effective_url", lcurl_easy_get_EFFECTIVE_URL },

  {"perform",  lcurl_easy_perform        },
  {"close",    lcurl_easy_cleanup        },
  {"__gc",     lcurl_easy_cleanup        },

  {NULL,NULL}
};
#undef OPT_ENTRY

void lcurl_easy_initlib(lua_State *L, int nup){
  if(!lutil_createmetap(L, LCURL_EASY, lcurl_easy_methods, nup))
    lua_pop(L, nup);
  lua_pop(L, 1);
}