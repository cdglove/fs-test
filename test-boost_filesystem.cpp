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
#include <boost/filesystem.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/timer/timer.hpp>
#include <iostream>
#include <vector>

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

  File root_file;
  root_file.parent = 0;
  root_file.name = ".";
  files.push_back(root_file);
  std::vector<std::size_t> directory_stack;
  directory_stack.push_back(0);
  std::size_t total_size = 0;
  using namespace boost::filesystem;
  recursive_directory_iterator i(
      root_file.name, directory_options::skip_permission_denied);
  for(; i != recursive_directory_iterator(); ++i) {
    try {
      boost::system::error_code ec;
      auto stat = i->status(ec);
      if(ec) {
        continue;
      }

      auto depth = i.depth();
      directory_stack.resize(depth + 1);
      if(is_directory(stat)) {
        auto parent = directory_stack.back();
        auto lwt = last_write_time(i->path(), ec);
        if(ec) {
          continue;
        }
        File file;
        file.directory = true;
        file.name = i->path().filename().string();
        file.parent = parent;
        file.modified = lwt;
        directory_stack.push_back(files.size());
        files.push_back(file);
      }
      else if(is_regular_file(stat)) {
        auto parent = directory_stack.back();
        auto lwt = last_write_time(i->path(), ec);
        if(ec) {
          continue;
        }
        auto size = file_size(i->path(), ec);
        if(ec) {
          continue;
        }
        File file;
        file.directory = false;
        file.name = i->path().filename().string();
        file.parent = parent;
        file.size = size;
        total_size += file.size;
        file.modified = lwt;
        directory_stack.push_back(files.size());
        files.push_back(file);
      }
    }
    catch(...) {
    }
  }

  std::cout << "test-boost_filesystem found " << files.size()
            << " files totalling " << total_size / 1024 << " KiB." << std::endl;
  return 0;
}
