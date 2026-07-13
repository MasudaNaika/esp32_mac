#pragma once

#include <stddef.h>

// Copy the currently displayed 640x480 Mac bitmap and save it asynchronously.
// The caller runs on the console/Core 0 path; the copy is deliberately short
// and the SD write happens in a separate Core 0 task.
bool screenshotRequest(char *response, size_t responseSize);
