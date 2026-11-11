/* vm_remote.c - the scripts SSM runs on a VM (vm_remote.h). The deploy is tools/cbase.d/vm_remote.sh
 * with the app's values in a header and its two units spliced in; the undeploy and the data list
 * are literal scripts. The text is the shell's, byte for byte: a port must not change what runs on
 * the instance, and vm_remote.sh itself stays bash, read from its file at run time.
 */
#include "vm_remote.h"

#include "cli.h"
#include "conf.h"
#include "vm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* splice_units: vm_remote.sh from its second line on, with @CBASE_SERVICE@ and @CBASE_SOCKET@
 * replaced by the lines of the unit and the socket (awk's splice in vm.sh's vm_remote_deploy). */
static void add_lines(cbc_sbuf *out, const char *text) {
  const char *p = text;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    cbc_sb_add(out, p, len);
    cbc_sb_str(out, "\n");
    p = nl ? nl + 1 : p + len;
  }
}

static char *splice_units(const char *unit, const char *socket) {
  char *text = cbc_vm_read(cbc_vm_file("vm_remote.sh"));
  cbc_sbuf out = { 0 };
  const char *p = strchr(text, '\n');   /* NR == 1 is the shebang, dropped */
  p = p ? p + 1 : text + strlen(text);
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len == 15 && memcmp(p, "@CBASE_SERVICE@", 15) == 0) add_lines(&out, unit);
    else if (len == 14 && memcmp(p, "@CBASE_SOCKET@", 14) == 0) add_lines(&out, socket);
    else {
      cbc_sb_add(&out, p, len);
      cbc_sb_str(&out, "\n");
    }
    p = nl ? nl + 1 : p + len;
  }
  if (!out.d) cbc_sb_str(&out, "");
  return out.d;
}

char *cbc_vm_remote_deploy(const char *port, const char *bucket, const char *key, const char *sha, const char *region,
                          const char *state_bucket) {
  const char *app = cbc_conf.app;
  char *data = cbc_vm_data_dir();
  char *settings = cbc_vm_settings();
  cbc_sbuf h = { 0 };
  char *line;
  /* The header: the heredoc of vm.sh's vm_remote_deploy, with its values in as they are (every one
   * is a name, digits, hex, a path or a bucket and region name this tool checked). */
  cbc_asprintf(&line,
               "#!/bin/bash\n"
               "# The deploy of cbase app %s on its VM, rendered by ./cbase deploy (tools/cbase.d/vm.sh) from\n"
               "# vm_remote.sh, and run as root by SSM.\n"
               "app=%s\n"
               "port=%s\n"
               "user=cbase-%s\n"
               "unit=cbase-%s.service\n"
               "sock=cbase-%s.socket\n"
               "home=\"${CBASE_VM_ROOT:-}/srv/cbase/%s\"\n"
               "data=%s\n"
               "bucket=%s\n"
               "key=%s\n"
               "sha=%s\n"
               "region=%s\n"
               "settings=%s\n"
               "replaces=%s\n",
               app, app, port, app, app, app, app, data, bucket, key, sha, region, settings, cbc_conf.vm_replaces);
  cbc_sb_str(&h, line);
  char *unit = cbc_vm_unit(port, region, cbc_vm_backup_bucket(state_bucket));
  char *socket = cbc_vm_socket(port);
  char *body = splice_units(unit, socket);
  cbc_sb_str(&h, body);
  return h.d;
}

char *cbc_vm_remote_undeploy(bool delete_data) {
  const char *app = cbc_conf.app;
  cbc_sbuf s = { 0 };
  char *head;
  /* The first heredoc is vm.sh's unquoted one: $app is its value, and \$ is a literal $. */
  cbc_asprintf(&head,
               "#!/bin/bash\n"
               "# The undeploy of cbase app %s on its VM, rendered by ./cbase undeploy (tools/cbase.d/vm.sh).\n"
               "set -euo pipefail\n"
               "R=\"${CBASE_VM_ROOT:-}\"\n"
               "app=%s\n"
               "unit=cbase-%s.service\n"
               "sock=cbase-%s.socket\n"
               "home=\"$R/srv/cbase/%s\"\n"
               "say() { echo \"CBASE: $*\"; }\n"
               "if [ -f \"$R/etc/systemd/system/$unit\" ]; then\n"
               "  # The socket first: with it listening, a connection would start the service again.\n"
               "  systemctl disable --now --quiet \"$sock\" 2> /dev/null || true\n"
               "  systemctl disable --now --quiet \"$unit\" || true\n"
               "  rm -f \"$R/etc/systemd/system/$unit\" \"$R/etc/systemd/system/$sock\"\n"
               "  systemctl daemon-reload\n"
               "  say \"$unit and $sock stopped, disabled and removed\"\n"
               "else\n"
               "  say \"$unit is not installed\"\n"
               "fi\n"
               "rm -f \"$home/serve\" \"$home/serve.new\" \"$R/etc/cbase/$app.env\"\n",
               app, app, app, app, app);
  cbc_sb_str(&s, head);
  if (delete_data) {
    cbc_sb_str(&s,
               "rm -rf \"$home\"\n"
               "user=\"cbase-$app\"\n"
               "if id -u \"$user\" > /dev/null 2>&1; then userdel \"$user\" || true; fi\n"
               "say \"deleted /srv/cbase/$app and its data (--delete-data)\"\n");
  } else {
    cbc_sb_str(&s,
               "if [ -d \"$home/data\" ]; then say \"kept /srv/cbase/$app/data (./cbase undeploy --delete-data deletes it)\"; fi\n");
  }
  return s.d;
}

char *cbc_vm_remote_data(void) {
  return cbc_dup("#!/bin/bash\n"
                 "set -euo pipefail\n"
                 "R=\"${CBASE_VM_ROOT:-}\"\n"
                 "for d in \"$R\"/srv/cbase/*/data; do\n"
                 "  [ -d \"$d\" ] || continue\n"
                 "  a=\"${d%/data}\"\n"
                 "  echo \"CBASE: data ${a##*/}\"\n"
                 "done\n");
}
