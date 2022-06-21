#ifndef CORO_CLOUDSTORAGE_FUSE_FILESYSTEM_CONTEXT_H
#define CORO_CLOUDSTORAGE_FUSE_FILESYSTEM_CONTEXT_H

#include "coro/cloudstorage/cloud_factory.h"
#include "coro/cloudstorage/fuse/filesystem_provider.h"
#include "coro/cloudstorage/util/abstract_cloud_provider_impl.h"
#include "coro/cloudstorage/util/account_manager_handler.h"
#include "coro/cloudstorage/util/cloud_factory_context.h"
#include "coro/cloudstorage/util/merged_cloud_provider.h"
#include "coro/cloudstorage/util/timing_out_cloud_provider.h"
#include "coro/http/http_server.h"
#include "coro/util/event_loop.h"

namespace coro::cloudstorage::fuse {

class FileSystemContext {
 public:
  struct Config {
    int timeout_ms;
    std::optional<std::string> config_path;
    FileSystemProviderConfig fs_config;
  };

  explicit FileSystemContext(const coro::util::EventLoop* event_loop,
                             Config = {.timeout_ms = 10000});

  const auto& fs() const { return fs_; }
  auto& fs() { return fs_; }

  Task<> Quit() { return http_server_.Quit(); }

 private:
  util::CloudFactoryContext context_;
  util::MergedCloudProvider merged_provider_;
  decltype(util::CreateAbstractCloudProviderImpl(&merged_provider_)) provider_;
  util::TimingOutCloudProvider timing_out_provider_;
  FileSystemProvider fs_;
  coro::http::HttpServer<util::AccountManagerHandler> http_server_;
};

}  // namespace coro::cloudstorage::fuse

#endif  // CORO_CLOUDSTORAGE_FUSE_FILESYSTEM_CONTEXT_H
