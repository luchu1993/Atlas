#ifndef ATLAS_UE_CLIENT_CORE_EXPORT_H_
#define ATLAS_UE_CLIENT_CORE_EXPORT_H_

// Cross-DLL decoration so non-AtlasUE consumers (game module, standalone test
// harness) resolve core symbols. Vanishes outside UE.
#if defined(ATLASUE_API)
#define ATLAS_CORE_API ATLASUE_API
#else
#define ATLAS_CORE_API
#endif

#endif  // ATLAS_UE_CLIENT_CORE_EXPORT_H_
