#pragma once

#include "integrations/kafka/consumer.hpp"

#include <experimental/optional>
#include <mutex>
#include <unordered_map>

#include "storage/kvstore.hpp"

namespace integrations {
namespace kafka {

class Streams final {
 public:
  Streams(const std::string &streams_directory,
          std::function<void(const std::vector<std::string> &)> stream_writer);

  void Recover();

  void Create(const StreamInfo &info, bool download_transform_script = true);

  void Drop(const std::string &stream_name);

  void Start(const std::string &stream_name,
             std::experimental::optional<int64_t> batch_limit =
                 std::experimental::nullopt);

  void Stop(const std::string &stream_name);

  void StartAll();

  void StopAll();

  std::vector<StreamInfo> Show();

  std::vector<std::string> Test(const std::string &stream_name,
                                std::experimental::optional<int64_t>
                                    batch_limit = std::experimental::nullopt);

 private:
  std::string streams_directory_;
  std::function<void(const std::vector<std::string> &)> stream_writer_;

  storage::KVStore metadata_store_;

  std::mutex mutex_;
  std::unordered_map<std::string, Consumer> consumers_;

  std::string GetTransformScriptDir();
  std::string GetTransformScriptPath(const std::string &stream_name);
};

}  // namespace kafka
}  // namespace integrations