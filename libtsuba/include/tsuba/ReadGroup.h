#ifndef KATANA_LIBTSUBA_TSUBA_READGROUP_H_
#define KATANA_LIBTSUBA_TSUBA_READGROUP_H_

#include <future>
#include <list>
#include <memory>

#include "katana/Result.h"
#include "tsuba/AsyncOpGroup.h"

namespace tsuba {

/// Track multiple, outstanding async writes and provide a mechanism to ensure
/// that they have all completed
class ReadGroup {
public:
  static katana::Result<std::unique_ptr<ReadGroup>> Make();

  /// Wait until all operations this descriptor knows about have completed
  katana::Result<void> Finish();

  /// Add future to the list of futures this ReadGroup will wait for, note
  /// the file name for debugging. `on_complete` is guaranteed to be called
  /// in FIFO order
  void AddOp(
      std::future<katana::Result<void>> future, std::string file,
      const std::function<katana::Result<void>()>& on_complete);

  /// same as AddOp, but the future may return a data type which can then be
  /// consumed by on_complete
  template <typename RetType>
  void AddReturnsOp(
      std::future<katana::Result<RetType>> future, const std::string& file,
      const std::function<katana::Result<void>(RetType)>& on_complete) {
    // n.b., make shared instead of unique because move capture below prevents
    // passing generic_complete_fn as a std::function
    auto ret_val = std::make_shared<RetType>();
    auto new_future = std::async(
        std::launch::deferred,
        [future = std::move(future),
         &ret_val_storage = *ret_val]() mutable -> katana::Result<void> {
          auto res = future.get();
          if (!res) {
            return res.error();
          }
          ret_val_storage = res.value();
          return katana::ResultSuccess();
        });

    std::function<katana::Result<void>()> generic_complete_fn =
        [ret_val, on_complete]() -> katana::Result<void> {
      return on_complete(std::move(*ret_val));
    };
    AddOp(std::move(new_future), file, generic_complete_fn);
  }

private:
  AsyncOpGroup async_op_group_;
};

}  // namespace tsuba

#endif
