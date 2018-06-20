#include "telemetry/collectors.hpp"

#include <experimental/filesystem>
#include <string>
#include <utility>

#include <sys/types.h>
#include <unistd.h>

#include "utils/file.hpp"
#include "utils/string.hpp"

namespace telemetry {

const std::pair<const std::string, const double> GetCpuUsage(pid_t pid,
                                                             pid_t tid = 0) {
  std::string name;
  double cpu = 0;
  std::string path = "";
  if (tid == 0) {
    path = fmt::format("/proc/{}/stat", pid);
  } else {
    path = fmt::format("/proc/{}/task/{}/stat", pid, tid);
  }
  auto stat_data = utils::ReadLines(path);
  if (stat_data.size() >= 1) {
    auto split = utils::Split(stat_data[0]);
    if (split.size() >= 20) {
      int off = 0;
      for (int i = 1; i < split.size(); ++i) {
        if (utils::EndsWith(split[i], ")")) {
          off = i - 1;
          break;
        }
      }
      // These fields are: utime, stime, cutime, cstime.
      // Their description can be found in `man proc` under `/proc/[pid]/stat`.
      for (int i = 14; i <= 17; ++i) {
        cpu += std::stoull(split[i - 1 + off]);
      }
      name = utils::Trim(
          utils::Join(std::vector<std::string>(split.begin() + 1,
                                               split.begin() + 2 + off),
                      " "),
          "()");
    }
  }
  cpu /= sysconf(_SC_CLK_TCK);
  return {name, cpu};
}

const nlohmann::json GetResourceUsage() {
  // Get PID of entire process.
  pid_t pid = getpid();

  // Get CPU usage for each thread and total usage.
  nlohmann::json cpu = nlohmann::json::object();
  cpu["threads"] = nlohmann::json::array();

  // Find all threads.
  std::string task_file = fmt::format("/proc/{}/task", pid);
  if (!std::experimental::filesystem::exists(task_file)) {
    return nlohmann::json::object();
  }
  for (auto &file :
       std::experimental::filesystem::directory_iterator(task_file)) {
    auto split = utils::Split(file.path().string(), "/");
    if (split.size() < 1) continue;
    pid_t tid = std::stoi(split[split.size() - 1]);
    auto cpu_usage = GetCpuUsage(pid, tid);
    cpu["threads"].push_back(
        {{"name", cpu_usage.first}, {"usage", cpu_usage.second}});
  }
  auto cpu_total = GetCpuUsage(pid);
  cpu["usage"] = cpu_total.second;

  // Parse memory usage.
  uint64_t memory = 0;
  auto statm_data = utils::ReadLines(fmt::format("/proc/{}/statm", pid));
  if (statm_data.size() >= 1) {
    auto split = utils::Split(statm_data[0]);
    if (split.size() >= 2) {
      memory = std::stoull(split[1]) * sysconf(_SC_PAGESIZE);
    }
  }

  return {{"cpu", cpu}, {"memory", memory}};
}

}  // namespace telemetry