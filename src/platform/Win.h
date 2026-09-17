// ---------------------------------------------------------------------------
// Win.h - the one place that includes <windows.h>.
//
// Every platform/core header includes this instead of <windows.h> directly so
// the lean-and-mean / NOMINMAX / STRICT defines are guaranteed to be set the
// same way in every translation unit.
// ---------------------------------------------------------------------------
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef STRICT
#define STRICT
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
