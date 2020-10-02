#include "tsuba/HttpNameServerClient.h"

#include <cassert>
#include <future>
#include <regex>

#include "GlobalState.h"
#include "galois/Http.h"
#include "galois/Logging.h"
#include "galois/Result.h"
#include "tsuba/Errors.h"

using json = nlohmann::json;

namespace {

struct HealthResponse {
  std::string status;
};

void
from_json(const json& j, HealthResponse& resp) {
  j.at("status").get_to(resp.status);
}

}  // namespace

namespace tsuba {

galois::Result<std::string>
HttpNameServerClient::BuildUrl(const galois::Uri& rdg_name) {
  return prefix_ + "rdgs/" + rdg_name.Encode();
}

galois::Result<void>
HttpNameServerClient::CheckHealth() {
  auto health_res =
      galois::HttpGetJson<HealthResponse>(prefix_ + "health-status");
  if (!health_res) {
    return health_res.error();
  }
  HealthResponse health = std::move(health_res.value());
  if (health.status != "ok") {
    GALOIS_LOG_ERROR("name server reports status {}", health.status);
    return ErrorCode::TODO;
  }
  return galois::ResultSuccess();
}

galois::Result<RDGMeta>
HttpNameServerClient::Get(const galois::Uri& rdg_name) {
  auto uri_res = BuildUrl(rdg_name);
  if (!uri_res) {
    return uri_res.error();
  }
  auto meta_res = galois::HttpGetJson<RDGMeta>(uri_res.value());
  if (meta_res) {
    meta_res.value().dir_ = rdg_name;
  }
  return meta_res;
}

galois::Result<void>
HttpNameServerClient::Create(const galois::Uri& rdg_name, const RDGMeta& meta) {
  // TODO(thunt) we check ID here because MemoryNameServer needs to be able to
  // store separate copies on all hosts for testing (fix it)
  galois::Result<void> res = galois::ResultSuccess();
  if (Comm()->ID == 0) {
    auto uri_res = BuildUrl(rdg_name);
    if (uri_res) {
      res = galois::HttpPostJson(uri_res.value(), meta);
    } else {
      res = uri_res.error();
    }
  }
  if (!res) {
    Comm()->NotifyFailure();
  }
  return galois::ResultSuccess();
}

galois::Result<void>
HttpNameServerClient::Update(
    const galois::Uri& rdg_name, uint64_t old_version, const RDGMeta& meta) {
  // TODO(thunt) we check ID here because MemoryNameServer needs to be able to
  // store separate copies on all hosts for testing (fix it)
  galois::Result<void> res = galois::ResultSuccess();
  if (Comm()->ID == 0) {
    auto uri_res = BuildUrl(rdg_name);
    if (uri_res) {
      auto query_string = fmt::format("?expected-version={}", old_version);
      res = galois::HttpPutJson(uri_res.value() + query_string, meta);
    } else {
      res = uri_res.error();
    }
  }
  if (!res) {
    Comm()->NotifyFailure();
  }
  return res;
}

galois::Result<std::unique_ptr<NameServerClient>>
HttpNameServerClient::Make(std::string_view host, int port) {
  // HttpInit is idempotent
  if (auto res = galois::HttpInit(); !res) {
    return res.error();
  }
  return std::unique_ptr<NameServerClient>(
      new HttpNameServerClient(host, port));
}

}  // namespace tsuba