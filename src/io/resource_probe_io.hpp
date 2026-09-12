#pragma once
#include <filesystem>
#include <string>
namespace cs::io {
[[nodiscard]] std::string run_resource_request(const std::filesystem::path& path);
}
