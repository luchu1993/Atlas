#ifndef ATLAS_LIB_NET_CLIENT_NET_CLIENT_EXPORT_H_
#define ATLAS_LIB_NET_CLIENT_NET_CLIENT_EXPORT_H_

// ATLAS_NET_CLIENT_DLL is defined by the SHARED build target (and its
// public consumers). STATIC consumers see no decoration.
#if defined(_WIN32) && defined(ATLAS_NET_CLIENT_DLL)
#  ifdef ATLAS_NET_CLIENT_EXPORTS
#    define ATLAS_NET_API __declspec(dllexport)
#  else
#    define ATLAS_NET_API __declspec(dllimport)
#  endif
#elif !defined(_WIN32) && defined(ATLAS_NET_CLIENT_DLL)
#  define ATLAS_NET_API __attribute__((visibility("default")))
#else
#  define ATLAS_NET_API
#endif

#define ATLAS_NET_CALL extern "C" ATLAS_NET_API

#endif  // ATLAS_LIB_NET_CLIENT_NET_CLIENT_EXPORT_H_
