#ifndef ATLAS_LIB_PLATFORM_PLATFORM_CONFIG_H_
#define ATLAS_LIB_PLATFORM_PLATFORM_CONFIG_H_

#include <bit>
#include <cstddef>

#if defined(_MSC_VER)
#define ATLAS_COMPILER_MSVC 1
#define ATLAS_COMPILER_CLANG 0
#define ATLAS_COMPILER_GCC 0
#elif defined(__clang__)
#define ATLAS_COMPILER_MSVC 0
#define ATLAS_COMPILER_CLANG 1
#define ATLAS_COMPILER_GCC 0
#elif defined(__GNUC__)
#define ATLAS_COMPILER_MSVC 0
#define ATLAS_COMPILER_CLANG 0
#define ATLAS_COMPILER_GCC 1
#else
#define ATLAS_COMPILER_MSVC 0
#define ATLAS_COMPILER_CLANG 0
#define ATLAS_COMPILER_GCC 0
#endif

#ifndef ATLAS_PLATFORM_WINDOWS
#define ATLAS_PLATFORM_WINDOWS 0
#endif

#ifndef ATLAS_PLATFORM_LINUX
#define ATLAS_PLATFORM_LINUX 0
#endif

#ifndef ATLAS_PLATFORM_MACOS
#define ATLAS_PLATFORM_MACOS 0
#endif

#ifndef ATLAS_ARCH_X64
#define ATLAS_ARCH_X64 0
#endif

#ifndef ATLAS_DEBUG
#define ATLAS_DEBUG 0
#endif

namespace atlas::platform {

inline constexpr bool kIsWindows = static_cast<bool>(ATLAS_PLATFORM_WINDOWS);
inline constexpr bool kIsLinux = static_cast<bool>(ATLAS_PLATFORM_LINUX);
inline constexpr bool kIsMacos = static_cast<bool>(ATLAS_PLATFORM_MACOS);
inline constexpr bool kIsX64 = static_cast<bool>(ATLAS_ARCH_X64);
inline constexpr bool kIsDebug = static_cast<bool>(ATLAS_DEBUG);

inline constexpr bool kIsMsvc = static_cast<bool>(ATLAS_COMPILER_MSVC);
inline constexpr bool kIsClang = static_cast<bool>(ATLAS_COMPILER_CLANG);
inline constexpr bool kIsGcc = static_cast<bool>(ATLAS_COMPILER_GCC);

inline constexpr bool kIsLittleEndian = (std::endian::native == std::endian::little);
inline constexpr std::size_t kCacheLineSize = 64;

}  // namespace atlas::platform

#if ATLAS_COMPILER_MSVC
#define ATLAS_FORCE_INLINE __forceinline
#define ATLAS_NO_INLINE __declspec(noinline)
#elif ATLAS_COMPILER_CLANG || ATLAS_COMPILER_GCC
#define ATLAS_FORCE_INLINE __attribute__((always_inline)) inline
#define ATLAS_NO_INLINE __attribute__((noinline))
#else
#define ATLAS_FORCE_INLINE inline
#define ATLAS_NO_INLINE
#endif

#if ATLAS_COMPILER_MSVC
#define ATLAS_DEBUG_BREAK() __debugbreak()
#elif ATLAS_COMPILER_CLANG || ATLAS_COMPILER_GCC
#define ATLAS_DEBUG_BREAK() __builtin_trap()
#else
#define ATLAS_DEBUG_BREAK() ((void)0)
#endif

#endif  // ATLAS_LIB_PLATFORM_PLATFORM_CONFIG_H_
