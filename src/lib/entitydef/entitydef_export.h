#ifndef ATLAS_LIB_ENTITYDEF_ENTITYDEF_EXPORT_H_
#define ATLAS_LIB_ENTITYDEF_ENTITYDEF_EXPORT_H_

// ATLAS_EDR_DLL is defined by the SHARED build target and its public
// consumers. STATIC consumers see no decoration.
#if defined(_WIN32) && defined(ATLAS_EDR_DLL)
#ifdef ATLAS_EDR_EXPORTS
#define ATLAS_EDR_API __declspec(dllexport)
#else
#define ATLAS_EDR_API __declspec(dllimport)
#endif
#elif !defined(_WIN32) && defined(ATLAS_EDR_DLL)
#define ATLAS_EDR_API __attribute__((visibility("default")))
#else
#define ATLAS_EDR_API
#endif

#define ATLAS_EDR_CALL extern "C" ATLAS_EDR_API

#endif  // ATLAS_LIB_ENTITYDEF_ENTITYDEF_EXPORT_H_
