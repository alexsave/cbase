/* page.c - the page: calls GET /api/hello through cbase's C client SDK on its cdom adapter, and shows
 * the answer. An anonymous client, so the call is plain HTTP to the page's own origin. */
#include "cdom.h"
#include "cbc_cdom.h"

static cbc_cdom client;   /* static and zeroed: the adapter's handle lives for the page */
static cd_id answer;      /* the text node the answer goes into */

static void on_event(void *ctx, cbc *c, const cbc_event *e, const uint8_t *bytes) {
  (void)ctx; (void)c;
  if (e->kind != CBC_EV_DONE) return;
  if (e->status == 200 && bytes) cd_set_text_node(answer, cd_tmp((const char *)bytes + e->off, e->len));
  else cd_set_text_node(answer, CD_LIT("the server did not answer"));
}

void cd_main(void) {
  cd_at(CD_MOUNT, 0);
  cd_open(CD_LIT("p")); cd_text(CD_LIT("asking...")); cd_name_text(answer = cd_id_new()); cd_close();

  cbc_cfg cfg;
  cbc_cfg_default(&cfg);
  cfg.anonymous = 1;   /* no accounts: every call is plain HTTP */
  if (cbc_cdom_start(&client, &cfg, NULL, on_event, NULL, NULL) != 0) return;
  /* Idempotent (safe to send again) and fail fast (a dead server is reported at once). */
  cbc_call(cbc_cdom_client(&client), CBC_GET, CBC_C_IDEMPOTENT | CBC_C_FAIL_FAST, "/api/hello", 10, NULL, 0,
           cbc_cdom_now());
  cbc_cdom_pump(&client);   /* after every call into the client */
}
