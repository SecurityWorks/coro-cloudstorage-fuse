#include "filesystem_context.h"

#include <coro/cloudstorage/cloud_exception.h>
#include <coro/http/http_server.h>

namespace coro::cloudstorage::internal {

namespace {

using ::coro::util::TypeList;

template <typename T>
concept HasUsageData = requires(T v) {
  { v.space_used }
  ->stdx::convertible_to<int64_t>;
  { v.space_total }
  ->stdx::convertible_to<int64_t>;
};

template <typename F>
auto AtScopeExit(F func) {
  struct Guard {
    explicit Guard(F func) : func(std::move(func)) {}
    ~Guard() {
      if (func) {
        (*func)();
      }
    }
    Guard(const Guard&) = delete;
    Guard(Guard&& other) noexcept {
      func = std::move(other.func);
      other.func = std::nullopt;
    }
    Guard& operator=(const Guard&) = delete;
    Guard& operator=(Guard&& other) noexcept {
      func = std::move(other.func);
      other.func = std::nullopt;
      return *this;
    }

    std::optional<F> func;
  };
  return Guard(std::move(func));
}

template <typename T, typename C>
std::vector<T> SplitString(const T& string, C delim) {
  std::vector<T> result;
  T current;
  for (auto c : string) {
    if (c == delim) {
      if (!current.empty()) {
        result.emplace_back(std::move(current));
      }
      current.clear();
    } else {
      current += c;
    }
  }
  if (!current.empty()) {
    result.emplace_back(std::move(current));
  }
  return result;
}

template <typename FileContext, typename ItemT, typename Directory>
Generator<std::vector<FileContext>> GetDirectory(ItemT d, Directory directory,
                                                 stdx::stop_token stop_token) {
  FOR_CO_AWAIT(auto& page_data,
               d.provider()->ListDirectory(std::move(directory),
                                           std::move(stop_token))) {
    std::vector<FileContext> page;
    for (auto& item : page_data.items) {
      page.emplace_back(FileContext{.item = ItemT(d.account, std::move(item))});
    }
    co_yield std::move(page);
  }
}

}  // namespace

template <typename... T>
void FileSystemContext<TypeList<T...>>::AccountListener::OnCreate(
    CloudProviderAccount* d) {
  context->accounts_.insert(
      std::shared_ptr<CloudProviderAccount>(d, [](auto) {}));
  std::cerr << "CREATED " << d->id << "\n";
}

template <typename... T>
void FileSystemContext<TypeList<T...>>::AccountListener::OnDestroy(
    CloudProviderAccount* d) {
  context->accounts_.erase(
      std::shared_ptr<CloudProviderAccount>(d, [](auto) {}));
  std::cerr << "REMOVED " << d->id << "\n";
}

template <typename... T>
FileSystemContext<TypeList<T...>>::FileSystemContext(event_base* event_base)
    : event_base_(event_base), event_loop_(event_base_) {
  Invoke(Main());
}

template <typename... T>
FileSystemContext<TypeList<T...>>::~FileSystemContext() {
  if (stop_source_.get_token().stop_possible()) {
    Quit();
  }
}

template <typename... T>
void FileSystemContext<TypeList<T...>>::Cancel() {
  stop_source_.request_stop();
}

template <typename... T>
void FileSystemContext<TypeList<T...>>::Quit() {
  if (!quit_called_) {
    quit_called_ = true;
    Cancel();
    quit_.SetValue();
  }
}

template <typename... T>
auto FileSystemContext<TypeList<T...>>::GetGenericItem(const FileContext& ctx)
    -> GenericItem {
  if (!ctx.item) {
    return GenericItem{.name = "root", .is_directory = true};
  }
  return std::visit([](const auto& d) { return d.GetGenericItem(); },
                    *ctx.item);
}

template <typename... T>
auto FileSystemContext<TypeList<T...>>::GetFileContext(std::string path) const
    -> Task<FileContext> {
  auto components = SplitString(path, '/');
  if (components.empty()) {
    co_return FileContext{};
  }
  auto account = GetAccount(*components.begin());
  std::string provider_path;
  for (size_t i = 1; i < components.size(); i++) {
    provider_path += "/" + components[i];
  }
  FileContext result = {};
  result.item = co_await std::visit(
      [&](auto& d) -> Task<std::variant<ItemT<T>...>> {
        using CloudProvider = std::remove_cvref_t<decltype(d)>;
        co_return Item<CloudProvider>(
            account,
            co_await d.GetItemByPath(provider_path, stop_source_.get_token()));
      },
      account->provider);
  co_return std::move(result);
}

template <typename... T>
template <typename D>
auto FileSystemContext<TypeList<T...>>::GetDirectoryGenerator::operator()(
    const D& d) const -> Generator<std::vector<FileContext>> {
  return std::visit(
      [d, stop_token = std::move(stop_token)](
          const auto& directory) -> Generator<std::vector<FileContext>> {
        using CloudProviderT = typename D::CloudProviderT;
        if constexpr (IsDirectory<decltype(directory), CloudProviderT>) {
          return GetDirectory<FileContext>(d, directory, std::move(stop_token));
        } else {
          throw std::invalid_argument("not a directory");
        }
      },
      d.item);
}

template <typename... T>
auto FileSystemContext<TypeList<T...>>::ReadDirectory(
    const FileContext& context) const -> Generator<std::vector<FileContext>> {
  if (!context.item) {
    std::vector<FileContext> result;
    for (auto account : accounts_) {
      auto root = co_await std::visit(
          [&](auto& provider) -> Task<FileContext> {
            using CloudProviderT = std::remove_cvref_t<decltype(provider)>;
            FileContext context = {};
            auto directory = co_await provider.GetRoot();
            directory.name = account->id;
            context.item = Item<CloudProviderT>(account, std::move(directory));
            co_return context;
          },
          account->provider);
      result.emplace_back(std::move(root));
    }
    co_yield std::move(result);
    co_return;
  }

  FOR_CO_AWAIT(std::vector<FileContext> & page_data,
               std::visit(GetDirectoryGenerator{stop_source_.get_token()},
                          *context.item)) {
    co_yield std::move(page_data);
  }
}

template <typename... T>
auto FileSystemContext<TypeList<T...>>::GetVolumeData() const
    -> Task<VolumeData> {
  std::vector<Task<VolumeData>> tasks;
  for (const auto& account : accounts_) {
    tasks.emplace_back(std::visit(
        [](auto& provider) -> Task<VolumeData> {
          auto data = co_await provider.GetGeneralData();
          if constexpr (HasUsageData<decltype(data)>) {
            co_return VolumeData{.space_used = data.space_used,
                                 .space_total = data.space_total};
          } else {
            co_return VolumeData{.space_used = 0, .space_total = 0};
          }
        },
        account->provider));
  }
  VolumeData total = {.space_used = 0, .space_total = 0};
  for (auto& d : tasks) {
    auto data = co_await std::move(d);
    total.space_used += data.space_used;
    if (data.space_total && total.space_total) {
      *total.space_total += *data.space_total;
    } else {
      total.space_total = std::nullopt;
      break;
    }
  }
  co_return total;
}

template <typename... T>
Task<std::string> FileSystemContext<TypeList<T...>>::Read(
    const FileContext& context, int64_t offset, int64_t size) const {
  using QueuedRead = typename FileContext::QueuedRead;

  if (!context.item) {
    throw CloudException("not a file");
  }
  auto generic_item = GetGenericItem(context);
  if (!generic_item.size) {
    throw CloudException("size unknown");
  }
  size = std::min<int64_t>(size, *generic_item.size - offset);
  auto& current_read = context.current_read;
  if (current_read &&
      (current_read->pending || offset > current_read->current_offset)) {
    if (!current_read->pending) {
      if (offset - current_read->current_offset <= 4 * size) {
        const int max_wait = 128;
        int current_delay = 0;
        while (!current_read->pending && current_delay < max_wait) {
          co_await event_loop_.Wait(current_delay, stop_source_.get_token());
          current_delay = current_delay * 2 + 1;
        }
      }
    }
    if (current_read->pending) {
      struct AwaiterGuard {
        void operator()() {
          context->queued_reads.erase(std::find(context->queued_reads.begin(),
                                                context->queued_reads.end(),
                                                queued_read));
        }
        const FileContext* context;
        const QueuedRead* queued_read;
      };
      QueuedRead queued_read{.offset = offset, .size = size};
      auto awaiter_guard = AtScopeExit(
          AwaiterGuard{.context = &context, .queued_read = &queued_read});
      context.queued_reads.emplace_back(&queued_read);
      co_await queued_read.awaiter;
    }
  }
  try {
    if (!current_read || current_read->current_offset != offset) {
      std::cerr << "STARTING READ " << generic_item.name << " " << offset
                << "\n";
      using CurrentRead = typename FileContext::CurrentRead;
      auto generator = std::visit(
          [&](const auto& d) {
            using CloudProviderT =
                typename std::remove_cvref_t<decltype(d)>::CloudProviderT;
            return std::visit(
                [&](const auto& item) -> Generator<std::string> {
                  if constexpr (IsFile<decltype(item), CloudProviderT>) {
                    return d.provider()->GetFileContent(
                        item, http::Range{.start = offset},
                        stop_source_.get_token());
                  } else {
                    throw std::invalid_argument("not a file");
                  }
                },
                d.item);
          },
          *context.item);
      current_read = CurrentRead{.generator = std::move(generator),
                                 .current_offset = offset,
                                 .pending = true};
      current_read->it = co_await current_read->generator.begin();
    } else {
      current_read->pending = true;
    }
    std::string chunk = std::exchange(current_read->chunk, "");
    while (static_cast<int64_t>(chunk.size()) < size &&
           *current_read->it != std::end(current_read->generator)) {
      chunk += std::move(**current_read->it);
      co_await ++*current_read->it;
    }
    current_read->current_offset = offset + size;
    if (static_cast<int64_t>(chunk.size()) > size) {
      current_read->chunk = std::string(chunk.begin() + size, chunk.end());
      chunk.resize(size);
    }
    current_read->pending = false;

    bool woken_up_someone = false;
    for (QueuedRead* read : context.queued_reads) {
      if (context.current_read->current_offset == read->offset) {
        read->awaiter.SetValue();
        woken_up_someone = true;
        break;
      }
    }
    if (!woken_up_someone && !context.queued_reads.empty()) {
      (*context.queued_reads.begin())->awaiter.SetValue();
    }

    co_return std::move(chunk);
  } catch (const std::exception& e) {
    auto current_offset =
        current_read ? current_read->current_offset : offset + size;
    current_read = std::nullopt;

    bool woken_up_someone = false;
    for (QueuedRead* read : context.queued_reads) {
      if (current_offset == read->offset) {
        read->awaiter.SetException(e);
        woken_up_someone = true;
        break;
      }
    }
    if (!woken_up_someone && !context.queued_reads.empty()) {
      (*context.queued_reads.begin())->awaiter.SetValue();
    }

    throw;
  }
}

template <typename... T>
auto FileSystemContext<TypeList<T...>>::GetAccount(std::string_view name) const
    -> std::shared_ptr<CloudProviderAccount> {
  auto it = std::find_if(accounts_.begin(), accounts_.end(),
                         [name](auto d) { return d->id == name; });
  if (it == accounts_.end()) {
    throw CloudException(CloudException::Type::kNotFound);
  } else {
    return *it;
  }
}

template <typename... T>
Task<> FileSystemContext<TypeList<T...>>::Main() {
  Http http{http::CurlHttp(event_base_)};
  CloudFactory cloud_factory(event_loop_, http);
  http::HttpServer http_server(
      event_base_, {.address = "0.0.0.0", .port = 12345},
      AccountManagerHandlerT(cloud_factory, AccountListener{this}));
  co_await quit_;
  co_await http_server.Quit();
}

template class FileSystemContext<CloudProviders>;

}  // namespace coro::cloudstorage::internal