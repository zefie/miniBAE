#pragma once

#define NOMINMAX
// Do not define WIN32_LEAN_AND_MEAN here — pfc/timers.h needs timeGetTime from mmsystem.h.

#include <helpers/foobar2000+atl.h>
#include <helpers/helpers.h>
#include <helpers/atl-misc.h>
#include <helpers/DarkMode.h>

#include <mmsystem.h>

#include <SDK/cfg_var.h>
#include <SDK/preferences_page.h>
#include <SDK/input_impl.h>
#include <SDK/input_file_type.h>
#include <SDK/coreDarkMode.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
