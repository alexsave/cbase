/* vm_boot.c - vm_boot.sh's user-data text (vm_boot.h). */
#include "vm_boot.h"

#include "cli.h"
#include "vm.h"

#include <stdbool.h>
#include <string.h>

/* A line is dropped when it is empty or when its first non-blank character is '#'. */
static bool dropped(const char *line) {
  if (*line == '\0') return true;
  const char *p = line;
  while (*p == ' ' || *p == '\t' || *p == '\f' || *p == '\v' || *p == '\r') p++;
  return *p == '#';
}

char *cbc_vm_boot_userdata(void) {
  char *text = cbc_vm_read(cbc_vm_file("vm_boot.sh"));
  cbc_buf out = { 0 };
  const char *p = text;
  int nr = 0;
  while (*p) {
    const char *nl = strchr(p, '\n');
    size_t len = nl ? (size_t)(nl - p) : strlen(p);
    nr++;
    char *line = cbc_dupn(p, len);
    if (nr == 1 || !dropped(line)) {
      cbc_buf_str(&out, line);
      cbc_buf_str(&out, "\n");
    }
    p = nl ? nl + 1 : p + len;
  }
  if (!out.data) cbc_buf_str(&out, "");
  return out.data;
}
