#pragma once

// Portable compiler attributes that are not universally supported by both Clang and gcc.

#ifdef __has_cpp_attribute
#if __has_cpp_attribute(clang::nonblocking)
#define LS_NONBLOCKING [[clang::nonblocking]]
#endif
#endif

#ifndef LS_NONBLOCKING
#define LS_NONBLOCKING
#endif
