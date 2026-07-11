#pragma once

#include <stdbool.h>

#include "app_settings.h"

bool syncRtcFromNtpConfig(const AppSettings *ntpCfg);
bool startPeriodicNtpSync(const AppSettings *ntpCfg);
