// provider_local.c - `cbase serve`'s provider (cbase/provider.h): SQLite by
// default (CBASE_STORE=dynamo for the DynamoDB Local lane), the in-process
// MQTT hub at /mqtt, bot drives on the bot pool, no edge to purge.
#include "cbase/provider.h"

#include "app/bots.h"
#include "config.h"
#include "realtime/rt_local.h"
#include "store/store.h"

const cb_provider cb_provider_local = {
    .name = "local",
    .mode = "serve",
    .flags = CB_PROV_HELD_SOCKETS | CB_PROV_TEST_CLOCK,
    .store = "sqlite",
    .rt_open = cb_rt_local_new,
    .routes = cb_rt_local_routes,
    .kick = cb_bots_kick_local,
};
