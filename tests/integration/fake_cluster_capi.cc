#include "fake_cluster_capi.h"

#include <chrono>
#include <thread>

#include "fake_cluster.h"

struct AtlasFakeCluster {
  atlas::test::FakeCluster impl;
};

ATLAS_TEST_API AtlasFakeCluster* AtlasFakeClusterCreate() {
  return new AtlasFakeCluster{};
}

ATLAS_TEST_API void AtlasFakeClusterDestroy(AtlasFakeCluster* cluster) {
  delete cluster;
}

ATLAS_TEST_API int AtlasFakeClusterStart(AtlasFakeCluster* cluster) {
  return cluster && cluster->impl.Start() ? 1 : 0;
}

ATLAS_TEST_API uint16_t AtlasFakeClusterLoginAppPort(AtlasFakeCluster* cluster) {
  return cluster ? cluster->impl.LoginAppPort() : 0;
}

ATLAS_TEST_API void AtlasFakeClusterSetLoginPolicy(AtlasFakeCluster* cluster, uint8_t policy) {
  if (cluster) cluster->impl.SetLoginPolicy(static_cast<atlas::test::LoginPolicy>(policy));
}

ATLAS_TEST_API void AtlasFakeClusterSetAuthPolicy(AtlasFakeCluster* cluster, uint8_t policy) {
  if (cluster) cluster->impl.SetAuthPolicy(static_cast<atlas::test::AuthPolicy>(policy));
}

ATLAS_TEST_API void AtlasFakeClusterPump(AtlasFakeCluster* cluster, int budget_ms) {
  if (!cluster) return;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(budget_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    cluster->impl.LoginAppDisp().ProcessOnce();
    cluster->impl.BaseAppDisp().ProcessOnce();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

ATLAS_TEST_API int AtlasFakeClusterLoginRequestSeen(AtlasFakeCluster* cluster) {
  return cluster && cluster->impl.LoginRequestSeen() ? 1 : 0;
}

ATLAS_TEST_API int AtlasFakeClusterAuthenticateRequestSeen(AtlasFakeCluster* cluster) {
  return cluster && cluster->impl.AuthenticateRequestSeen() ? 1 : 0;
}

ATLAS_TEST_API int AtlasFakeClusterRpcReceived(AtlasFakeCluster* cluster) {
  return cluster && cluster->impl.RpcReceived() ? 1 : 0;
}

ATLAS_TEST_API uint32_t AtlasFakeClusterLastRpcId(AtlasFakeCluster* cluster) {
  return cluster ? cluster->impl.LastRpcId() : 0;
}
