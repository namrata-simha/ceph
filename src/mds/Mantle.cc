#include "mdstypes.h"
#include "MDSRank.h"
#include "Mantle.h"
#include "msg/Messenger.h"

#include <fstream>

#define dout_subsys ceph_subsys_mds_balancer
#undef DOUT_COND
#define DOUT_COND(cct, l) l<=cct->_conf->debug_mds || l <= cct->_conf->debug_mds_balancer
#undef dout_prefix
#define dout_prefix *_dout << "mds.mantle "

int dout_wrapper(lua_State *L)
{
  #undef dout_prefix
  #define dout_prefix *_dout << "lua.balancer "

  /* Lua indexes the stack from the bottom up */
  int bottom = -1 * lua_gettop(L);
  if (!lua_isinteger(L, bottom)) {
    dout(0) << "WARNING: BAL_LOG has no message" << dendl;
    return -EINVAL;
  }

  /* bottom of the stack is the log level */
  int level = lua_tointeger(L, bottom);

  /* rest of the stack is the message */
  string s = "";
  for (int i = bottom + 1; i < 0; i++)
    lua_isstring(L, i) ? s.append(lua_tostring(L, i)) : s.append("<empty>");

  dout(level) << s << dendl;
  return 0;
}

int Mantle::balance(vector < map<string, double> > metrics, map<mds_rank_t,double> &my_targets)
{
  /* build lua vm state */
  lua_State *L = luaL_newstate(); 
  if (!L) {
    dout(0) << "WARNING: mantle could not load Lua state" << dendl;
    return -ENOEXEC;
  }

  /* balancer policies can use basic Lua functions */
  luaopen_base(L);

  /* load script from localfs */
  ifstream t("/tmp/test.lua");
  string script((std::istreambuf_iterator<char>(t)),
                 std::istreambuf_iterator<char>()); 

  /* load the balancer */
  if (luaL_loadstring(L, script.c_str())) {
    dout(0) << "WARNING: mantle could not load balancer: "
            << lua_tostring(L, -1) << dendl;
    lua_close(L);
    return -EINVAL;
  }

  /* setup debugging */
  lua_register(L, "BAL_LOG", dout_wrapper);

  /* global mds table to hold all metrics */
  lua_newtable(L);

  /* push name of mds (i) and its table of metrics onto Lua stack */
  for (unsigned i=0; i < metrics.size(); i++) {
    lua_pushinteger(L, i);
    lua_newtable(L);

    /* push metrics into this mds's table; setfield assigns key/pops val */
    for (map<string, double>::iterator it = metrics[i].begin();
         it != metrics[i].end();
         it++) {
      lua_pushnumber(L, it->second);
      lua_setfield(L, -2, it->first.c_str());
    }

    /* in global mds table at stack[-3], set k=stack[-1] to v=stack[-2] */
    lua_rawset(L, -3);
  }

  /* set the name of the global mds table */
  lua_setglobal(L, "mds");

  /* compile/execute balancer */
  int ret = lua_pcall(L, 0, LUA_MULTRET, 0);

  if (ret) {
    dout(0) << "WARNING: mantle could not execute script: "
            << lua_tostring(L, -1) << dendl;
    lua_close(L);
    return -EINVAL;
  }

  if (lua_istable(L, -1) == 0 ||
      metrics.size() != lua_rawlen(L, -1)) {
    dout(0) << "WARNING: mantle script returned a malformed response" << dendl;
    lua_close(L);
    return -EINVAL;
  }

  /* parse response by iterating over Lua stack */
  mds_rank_t it = mds_rank_t(0);
  lua_pushnil(L);
  while (lua_next(L, -2) != 0) {
    my_targets[it] = (lua_tointeger(L, -1));
    lua_pop(L, 1);
    it++;
  }

  lua_close(L);

  return 0;
}
