#ifndef ATLAS_UE_CLIENT_CORE_EXPORT_H_
#define ATLAS_UE_CLIENT_CORE_EXPORT_H_

// Cross-DLL export decoration for Atlas core classes consumed from UE modules
// other than AtlasUE (e.g. the game-side UEClient module deriving from
// atlas::ClientEntity / instantiating atlas::AvatarFilter). Outside any UE
// build the macro vanishes so the core stays drop-in usable from a standalone
// test harness or non-UE C++ host.
#if defined(ATLASUE_API)
#define ATLAS_CORE_API ATLASUE_API
#else
#define ATLAS_CORE_API
#endif

#endif  // ATLAS_UE_CLIENT_CORE_EXPORT_H_
