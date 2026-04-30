#include "machined/watcher_forwarder.h"

#include <algorithm>

#include "foundation/clock.h"
#include "foundation/log.h"

namespace atlas::machined {

WatcherForwarder::WatcherForwarder(const ProcessRegistry& registry,
                                   ChannelResolver requester_resolver)
    : registry_(registry), requester_resolver_(std::move(requester_resolver)) {}

void WatcherForwarder::HandleRequest(Channel* requester_channel, const WatcherRequest& req) {
  if (requester_channel == nullptr) return;

  // Find target by name if specified, otherwise use first of that type
  std::optional<ProcessEntry> target;
  if (!req.target_name.empty()) {
    target = registry_.FindByName(req.target_type, req.target_name);
  } else {
    auto candidates = registry_.FindByType(req.target_type);
    if (!candidates.empty()) target = candidates.front();
  }

  if (!target || target->channel == nullptr || !target->channel->IsConnected()) {
    // Target not found - reply immediately with not-found
    WatcherResponse resp;
    resp.request_id = req.request_id;
    resp.found = false;
    resp.source_name = req.target_name;
    resp.value = "";
    if (auto r = requester_channel->SendMessage(resp); !r) {
      ATLAS_LOG_WARNING("WatcherForwarder: failed to send not-found response: {}",
                        r.Error().Message());
    }
    return;
  }

  uint32_t fwd_id = next_forward_id_++;

  WatcherForward fwd;
  fwd.request_id = fwd_id;
  fwd.requester_name = "";  // machined itself is the proxy
  fwd.watcher_path = req.watcher_path;

  if (auto r = target->channel->SendMessage(fwd); !r) {
    ATLAS_LOG_WARNING("WatcherForwarder: failed to forward request to {}: {}", target->name,
                      r.Error().Message());
    // Send error response
    WatcherResponse resp;
    resp.request_id = req.request_id;
    resp.found = false;
    if (auto r2 = requester_channel->SendMessage(resp); !r2) {
      ATLAS_LOG_WARNING("WatcherForwarder: failed to send error response: {}",
                        r2.Error().Message());
    }
    return;
  }

  pending_.push_back(
      {fwd_id, req.request_id, requester_channel->RemoteAddress(), target->name, Clock::now()});
  ATLAS_LOG_DEBUG("WatcherForwarder: forwarded request {} → {} (fwd_id={})", req.request_id,
                  target->name, fwd_id);
}

void WatcherForwarder::HandleReply(Channel* source_channel, const WatcherReply& reply) {
  auto it = std::find_if(pending_.begin(), pending_.end(), [&](const PendingEntry& e) {
    return e.forwarded_request_id == reply.request_id;
  });

  if (it == pending_.end()) {
    ATLAS_LOG_DEBUG("WatcherForwarder: received reply for unknown request_id={}", reply.request_id);
    return;
  }

  // Find source name for the response
  std::string source_name = it->target_name;
  const uint32_t kRequesterRequestId = it->requester_request_id;
  const Address kRequesterAddr = it->requester_addr;
  pending_.erase(it);

  Channel* requester = requester_resolver_ ? requester_resolver_(kRequesterAddr) : nullptr;
  if (requester == nullptr || !requester->IsConnected()) {
    ATLAS_LOG_DEBUG("WatcherForwarder: requester gone for request_id={}", reply.request_id);
    return;
  }

  WatcherResponse resp;
  resp.request_id = kRequesterRequestId;
  resp.found = reply.found;
  resp.source_name = std::move(source_name);
  resp.value = reply.value;

  if (auto r = requester->SendMessage(resp); !r) {
    ATLAS_LOG_WARNING("WatcherForwarder: failed to send WatcherResponse: {}", r.Error().Message());
  }

  (void)source_channel;
}

void WatcherForwarder::CheckTimeouts() {
  auto now = Clock::now();
  auto before = pending_.size();

  std::erase_if(pending_, [&](const PendingEntry& e) {
    if (now - e.issued_at < kReplyTimeout) return false;

    ATLAS_LOG_WARNING("WatcherForwarder: request_id={} timed out (target={})",
                      e.forwarded_request_id, e.target_name);

    Channel* requester = requester_resolver_ ? requester_resolver_(e.requester_addr) : nullptr;
    if (requester != nullptr && requester->IsConnected()) {
      WatcherResponse resp;
      resp.request_id = e.requester_request_id;
      resp.found = false;
      resp.source_name = e.target_name;
      resp.value = "";
      (void)requester->SendMessage(resp);
    }
    return true;
  });

  auto expired = before - pending_.size();
  if (expired > 0) {
    ATLAS_LOG_DEBUG("WatcherForwarder: expired {} timed-out request(s)", expired);
  }
}

}  // namespace atlas::machined
