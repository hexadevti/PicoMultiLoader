// shell.h - the terminal's commands (F5). The UI runs the ones that change screens (run, apps, home,
// files, ctl, usb, wifi on/off, clear, history, exit) itself; the rest are here.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define SHELL_ARGS 8

/** Where a command's output goes: one line, without the newline. `error` lines are shown in red. */
typedef void (*shell_out_t)(const char *text, bool error);

/** The shell's folder: a FatFS path, "" for the root. */
const char *shell_cwd(void);

/** Split `line` into words in place (double quotes keep spaces). Returns how many. */
int shell_split(char *line, char **argv, int max);

/** Turn `arg` into a FatFS path ("" = root) from the shell's folder; "." and ".." work. */
bool shell_resolve(char *out, size_t len, const char *arg);

/** Run a command. False if there is no such command (nothing is printed then). */
bool shell_run(int argc, char **argv, shell_out_t out);

/** Complete the last word of `line` (a command, else a file or folder); lists the choices if several. */
void shell_complete(char *line, size_t cap, shell_out_t out);
