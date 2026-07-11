#pragma once

#include <stddef.h>

typedef const char *(*ConsoleShellRomSourceGetter)(void);

void consoleShellStart(ConsoleShellRomSourceGetter romSourceGetter);
bool consoleShellRunCommand(const char *command, char *response, size_t responseSize);
