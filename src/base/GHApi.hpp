#pragma once

#if defined(_WIN32)
#if defined(GH_WINDOWS_EXPORT)
#define GH_API __declspec(dllexport)
#elif defined(GH_WINDOWS_IMPORT)
#define GH_API __declspec(dllimport)
#else
#define GH_API
#endif
#else
#define GH_API
#endif
