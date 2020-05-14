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
#include <array>
#include <boost/timer/timer.hpp>
#include <ctime>
#include <err.h>
#include <fts.h>
#include <iostream>
#include <sys/stat.h>
#include <sys/types.h>
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
  root_file.name = "./";
  files.push_back(root_file);
  std::vector<std::size_t> directory_stack;
  directory_stack.push_back(0);
  std::size_t total_size = 0;

  int fts_options = FTS_PHYSICAL | FTS_NOCHDIR;
  std::array<char*, 2> roots = {root_file.name.data()};
  FTS* ftsp = nullptr;
  if((ftsp = fts_open(roots.data(), fts_options, nullptr)) == nullptr) {
    abort();
  }

  FTSENT* p = nullptr;
  while((p = fts_read(ftsp)) != nullptr) {
    auto depth = p->fts_level;
    directory_stack.resize(depth + 1);
    if(p->fts_info == FTS_D) {
      auto parent = directory_stack.back();
      File file;
      file.directory = true;
      file.name.assign(p->fts_name, p->fts_name + p->fts_namelen);
      file.parent = parent;
      file.modified = p->fts_statp->st_mtim.tv_sec;
      directory_stack.push_back(files.size());
      files.push_back(file);
    }
    else if(p->fts_info == FTS_F) {
      auto parent = directory_stack.back();
      File file;
      file.directory = false;
      file.name.assign(p->fts_name, p->fts_name + p->fts_namelen);
      file.parent = parent;
      file.size = file.modified = p->fts_statp->st_size;
      total_size += file.size;
      file.modified = p->fts_statp->st_mtim.tv_sec;
      directory_stack.push_back(files.size());
      files.push_back(file);
    }
  }

  fts_close(ftsp);

  std::cout << "test-fts found " << files.size() << " files totalling "
            << total_size / 1024 << " KiB." << std::endl;
  return 0;
}
