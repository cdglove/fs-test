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
#include <boost/timer/timer.hpp>
#include <ctime>
#include <dirent.h>
#include <iostream>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

int main() {
  boost::timer::auto_cpu_timer t;
  struct File {
    std::uint64_t parent;
    std::time_t created;
    std::time_t accessed;
    std::time_t modified;
    std::time_t updated;
    std::uint64_t size;
    std::string name;
    bool directory;
  };
  std::vector<File> files;

  struct DirectoryNode {
    std::size_t id;
    std::size_t path_index;
    std::string name;
  };
  std::vector<DirectoryNode> directory_stack;

  File root_file;
  root_file.parent = 0;
  root_file.name = "./";
  files.push_back(root_file);

  std::string current_path;
  current_path.assign(root_file.name.begin(), root_file.name.end());

  DIR* dir = nullptr;
  if(!(dir = opendir(root_file.name.c_str()))) {
    abort();
  }

  std::size_t current = 0;
  std::size_t total_size = 0;
  while(true) {
    dirent* entry;
    while((entry = readdir(dir)) != NULL) {
      if(strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }

      if(entry->d_type == DT_DIR) {
        auto path_backup = current_path.size();
        current_path += entry->d_name;
        struct stat s;
        stat(current_path.c_str(), &s);
        current_path.resize(path_backup);
        File f;
        f.parent = current;
        f.name = entry->d_name;
        f.modified = s.st_mtim.tv_sec;
        f.directory = true;
        auto id = files.size();
        files.push_back(f);
        directory_stack.push_back({id, current_path.size(), entry->d_name});
      }
      else if(entry->d_type == DT_REG) {
        auto path_backup = current_path.size();
        current_path += entry->d_name;
        struct stat s;
        stat(current_path.c_str(), &s);
        current_path.resize(path_backup);
        File f;
        f.parent = current;
        f.name = entry->d_name;
        f.size = s.st_size;
        total_size += f.size;
        f.modified = s.st_mtim.tv_sec;
        f.directory = false;
        files.push_back(f);
      }
    }

    if(directory_stack.empty()) {
      break;
    }

    while(!directory_stack.empty()) {
      DirectoryNode& n = directory_stack.back();
      current_path.resize(n.path_index);
      current_path += n.name;
      current_path += "/";
      current = n.id;
      directory_stack.pop_back();
      if(dir) {
        if(closedir(dir) == -1) {
          abort();
        }
      }
      dir = opendir(current_path.c_str());
      if(dir) {
        break;
      }
    }

    if(!dir) {
      break;
    }
  }

  if(dir) {
    if(closedir(dir) == -1) {
      abort();
    }
  }

  std::cout << "test-win32 found " << files.size() << " files totalling "
            << total_size / 1024 << " KiB." << std::endl;
  return 0;
}