// Copyright (c) 2020 Google Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
#include "llfio/v2.0/directory_handle.hpp"
#include <boost/nowide/convert.hpp>
#include <boost/timer/timer.hpp>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <vector>

namespace llfio = LLFIO_V2_NAMESPACE;

std::string convert_string(
    llfio::path_view::byte const* s, std::size_t length) {
#ifdef _WIN32
  return boost::nowide::narrow(reinterpret_cast<wchar_t const*>(s), length);
#else
  return std::string(
      reinterpret_cast<char const*>(s),
      reinterpret_cast<char const*>(s) + length);
#endif
}

#ifdef _WIN32
using native_string = std::wstring;
#else
using native_string = std::string;
#endif

int main() {
  boost::timer::auto_cpu_timer t;
  struct File {
    std::size_t parent;
    std::time_t created;
    std::time_t accessed;
    std::time_t modified;
    std::time_t updated;
    uint64_t size;
    std::string name;
    bool directory;
  };
  std::vector<File> files;

  struct DirectoryNode {
    std::size_t id;
    std::size_t path_index;
    native_string name;
  };
  std::vector<DirectoryNode> directory_stack;

  File root_file;
  root_file.parent = 0;
  root_file.name = ".";
  root_file.name.push_back(llfio::path_view::preferred_separator);
  files.push_back(root_file);

  native_string current_path;
  current_path.assign(root_file.name.begin(), root_file.name.end());

  std::vector<llfio::directory_entry> entry_buffer;
  llfio::directory_handle::buffers_type handle_buffer;
  // Good enough for the test.
  entry_buffer.resize(64 * 1024);
  std::size_t current = 0;
  std::size_t total_size = 0;
  std::size_t last_trace_size = 0;
  while(true) {
    llfio::result<llfio::directory_handle> result =
        llfio::directory({}, current_path);
    if(result.has_value()) {
      llfio::directory_handle d = std::move(result).value();
      if(d.is_symlink()) {
        continue;
      }

      handle_buffer = entry_buffer;
      llfio::result<llfio::directory_handle::buffers_type> listing =
          d.read(std::move(handle_buffer));
      if(listing.has_value()) {
        handle_buffer = std::move(listing).value();
        if(!handle_buffer.done()) {
          entry_buffer.resize(entry_buffer.size() + entry_buffer.size() / 2);
          continue;
        }

        for(llfio::directory_entry& e : handle_buffer) {
          if(visit(e.leafname, [](auto sv) { return sv[0] == '.'; })) {
            continue;
          }
          if(e.stat.st_type == std::filesystem::file_type::directory) {
            auto id = files.size();
            // directory_handle::read() only fills .metadata(), so to be
            // portable fetch any missing
            if(!(handle_buffer.metadata() & llfio::stat_t::want::mtim)) {
              // Fetch missing metadata
              auto h = llfio::file_handle::file(
                           d, e.leafname, llfio::file_handle::mode::attr_read)
                           .value();
              e.stat.fill(h, llfio::stat_t::want::mtim).value();
            }
            File f;
            f.parent = current;
            f.name = convert_string(
                e.leafname._raw_data(), e.leafname.native_size());
            f.directory = true;
            f.modified = std::chrono::system_clock::to_time_t(e.stat.st_mtim);
            files.push_back(f);
            directory_stack.push_back(
                {id, current_path.size(),
                 native_string(
                     (native_string::value_type const*)e.leafname._raw_data(),
                     e.leafname.native_size())});
          }
          else if(e.stat.st_type == std::filesystem::file_type::regular) {
            // directory_handle::read() only fills .metadata(), so to be
            // portable fetch any missing
            if(!(handle_buffer.metadata() &
                 (llfio::stat_t::want::mtim | llfio::stat_t::want::size))) {
              // Fetch missing metadata
              auto h = llfio::directory_handle::directory(
                           d, e.leafname, llfio::file_handle::mode::attr_read)
                           .value();
              e.stat
                  .fill(
                      h, llfio::stat_t::want::mtim | llfio::stat_t::want::size)
                  .value();
            }
            File f;
            f.parent = current;
            f.name = convert_string(
                e.leafname._raw_data(), e.leafname.native_size());
            f.size = e.stat.st_size;
            total_size += f.size;
            f.modified = std::chrono::system_clock::to_time_t(e.stat.st_mtim);
            f.directory = false;
            files.push_back(f);
          }
        }
      }
    }

    if(directory_stack.empty()) {
      break;
    }

    DirectoryNode& n = directory_stack.back();
    current_path.resize(n.path_index);
    current_path += n.name;
    current_path.push_back(llfio::path_view::preferred_separator);
    current = n.id;
    directory_stack.pop_back();
  }

  std::cout << "test-llfio found " << files.size() << " files totalling "
            << total_size / 1024 << " KiB." << std::endl;
  return 0;
}
