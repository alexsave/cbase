// env_file.h - CBASE_ENV_FILE: settings from a file of KEY=VALUE lines, read into the environment
// before the configuration is (config.h cb_config_load), so a developer's secrets (amgi's
// .env.local: GEMINI_API_KEY, GEMINI_API_BASE) live in one gitignored file and not in a shell's
// history. The app reads its own the way it reads any variable: getenv.
//
// THE FORMAT, strict on purpose: what is not one of these is refused, never guessed at.
//   - One setting per line, KEY=VALUE, with nothing before the key and no blank on either side of
//     the '=' (a blank there is refused, not trimmed: "KEY = v" is no setting).
//   - KEY is a letter or '_', then letters, digits and '_'.
//   - VALUE is everything after the '=', to the end of the line, taken as written: no shell
//     expansion ($HOME stays "$HOME"), no escapes. It may be empty. One wrapped in a pair of
//     single or double quotes loses the pair and nothing else ('a b' is a b, "a # b" is a # b); a
//     value that opens a quote and does not end with it is refused.
//   - In an UNQUOTED value, as Node's process.loadEnvFile reads one, a '#' after a space or a tab
//     starts a comment: the value is what comes before it, its trailing blanks trimmed
//     ("KEY=abc # my key" is abc). A '#' with no blank before it is the value's own (a#b). An
//     unquoted value with no comment that ends in a blank is refused (quote it if the blank is
//     meant).
//   - A line whose first character is '#' is a comment; a line of nothing or only blanks is
//     skipped. A "\r\n" line end is a "\n" one.
//   - A key may appear once.
// A file that cannot be read, holds a NUL byte, is larger than CB_ENV_FILE_MAX, or has a line
// that breaks a rule refuses the boot, with a sentence naming the file and the line's number;
// nothing from such a file is set.
//
// THE ENVIRONMENT WINS: a key already in the environment (set at all, even to "") keeps its
// value, and the file's line for it is skipped. So `GEMINI_API_KEY=other cbase serve` overrides
// the file, as the shell's variables override every default.
#ifndef CB_ENV_FILE_H
#define CB_ENV_FILE_H

#include <stddef.h>

#define CB_ENV_FILE_MAX (1 << 20)   // bytes

// Reads `path` (relative to the working directory) and sets each of its keys that the
// environment does not already hold. CB_OK, or CB_E_INVALID with a sentence in err (and nothing
// set).
int cb_env_file_load(const char *path, char *err, size_t cap);

#endif
