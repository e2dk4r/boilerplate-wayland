#pragma once

#if IS_BUILD_DEBUG
#define debug_assert(x)                                                                                                \
  if (!(x)) {                                                                                                          \
    __builtin_debugtrap();                                                                                             \
  }
#else
#define debug_assert(x)
#endif

#define runtime_assert(x)                                                                                              \
  if (!(x)) {                                                                                                          \
    __builtin_trap();                                                                                                  \
  }
