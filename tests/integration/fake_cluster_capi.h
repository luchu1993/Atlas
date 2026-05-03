#ifndef ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_CAPI_H_
#define ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_CAPI_H_

#include <cstdint>

#if defined(_WIN32)
#define ATLAS_TEST_API extern "C" __declspec(dllexport)
#else
#define ATLAS_TEST_API extern "C" __attribute__((visibility("default")))
#endif

struct AtlasFakeCluster;

ATLAS_TEST_API AtlasFakeCluster* AtlasFakeClusterCreate();
ATLAS_TEST_API void AtlasFakeClusterDestroy(AtlasFakeCluster* cluster);

// Returns 1 on success, 0 on bind failure.
ATLAS_TEST_API int AtlasFakeClusterStart(AtlasFakeCluster* cluster);

ATLAS_TEST_API uint16_t AtlasFakeClusterLoginAppPort(AtlasFakeCluster* cluster);

// Mirrors atlas::test::LoginPolicy / AuthPolicy enums.
ATLAS_TEST_API void AtlasFakeClusterSetLoginPolicy(AtlasFakeCluster* cluster, uint8_t policy);
ATLAS_TEST_API void AtlasFakeClusterSetAuthPolicy(AtlasFakeCluster* cluster, uint8_t policy);

// Drives both dispatchers for up to budget_ms; returns when budget expires.
// Caller polls predicates between calls.
ATLAS_TEST_API void AtlasFakeClusterPump(AtlasFakeCluster* cluster, int budget_ms);

ATLAS_TEST_API int AtlasFakeClusterLoginRequestSeen(AtlasFakeCluster* cluster);
ATLAS_TEST_API int AtlasFakeClusterAuthenticateRequestSeen(AtlasFakeCluster* cluster);
ATLAS_TEST_API int AtlasFakeClusterRpcReceived(AtlasFakeCluster* cluster);
ATLAS_TEST_API uint32_t AtlasFakeClusterLastRpcId(AtlasFakeCluster* cluster);

#endif  // ATLAS_TESTS_INTEGRATION_FAKE_CLUSTER_CAPI_H_
