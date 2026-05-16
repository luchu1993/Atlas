#ifndef ATLAS_UE_CLIENT_CORE_LAYER1_EXPORT_H_
#define ATLAS_UE_CLIENT_CORE_LAYER1_EXPORT_H_

// Cross-DLL export decoration for Layer 1 classes consumed by UE modules other
// than AtlasUE itself (e.g. the game-side UEClient module deriving from
// atlas::ClientEntity / instantiating atlas::AvatarFilter). Outside any UE
// build the macro vanishes so Layer 1 stays drop-in usable from a standalone
// test harness or non-UE C++ host.
#if defined(ATLASUE_API)
#define ATLAS_LAYER1_API ATLASUE_API
#else
#define ATLAS_LAYER1_API
#endif

#endif  // ATLAS_UE_CLIENT_CORE_LAYER1_EXPORT_H_
