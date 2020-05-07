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
#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

#include <boost/nowide/convert.hpp>
#include <boost/timer/timer.hpp>
#include <vector>
#include <iostream>

int main() {
  boost::timer::auto_cpu_timer t;
  struct File {
    uint64_t parent;
    FILETIME created;
    FILETIME accessed;
    FILETIME modified;
    FILETIME updated;
    uint64_t size;
    std::string name;
    bool directory;
  };
  std::vector<File> files;

  struct DirectoryNode {
    std::size_t id;
    std::size_t path_index;
    std::wstring name;
  };
  std::vector<DirectoryNode> directory_stack;

  File root_file;
  root_file.parent = 0;
  root_file.name = "C:\\";
  files.push_back(root_file);

  std::wstring current_path(L"C:\\*");
  WIN32_FIND_DATAW wfd;
  HANDLE find_handle = FindFirstFileW(current_path.c_str(), &wfd);
  DWORD err = 0;

  if(find_handle == INVALID_HANDLE_VALUE) {
    abort();
  }

  std::size_t current = 0;
  while(true) {
    if((wcsncmp(L".", wfd.cFileName, 1) != 0) &&
       (wcsncmp(L"..", wfd.cFileName, 2) != 0)) {
      if(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        File f;
        f.parent = current;
        f.name = boost::nowide::narrow(wfd.cFileName);
        f.size = (static_cast<std::uint64_t>(wfd.nFileSizeHigh) << 32) |
                 wfd.nFileSizeLow;
        f.modified = wfd.ftLastWriteTime;
        f.directory = true;
        auto id = files.size();
        files.push_back(f);
        directory_stack.push_back({id, current_path.size() - 1, wfd.cFileName});
      }
      else {
        File f;
        f.parent = current;
        f.name = boost::nowide::narrow(wfd.cFileName);
        f.size = (static_cast<std::uint64_t>(wfd.nFileSizeHigh) << 32) |
                 wfd.nFileSizeLow;
        f.modified = wfd.ftLastWriteTime;
        f.directory = false;
        files.push_back(f);
      }
    }

    if(FindNextFileW(find_handle, &wfd)) {
      continue;
    }

    if((err = GetLastError()) != ERROR_NO_MORE_FILES) {
      break;
    }

    if(directory_stack.empty()) {
      break;
    }

    while(!directory_stack.empty()) {
      DirectoryNode& n = directory_stack.back();
      current_path.resize(n.path_index);
      current_path += n.name;
      current_path += L"\\*";
      current = n.id;
      directory_stack.pop_back();
      find_handle = FindFirstFileW(current_path.c_str(), &wfd);
      if(find_handle != INVALID_HANDLE_VALUE) {
        break;
      }
    }

    if(find_handle == INVALID_HANDLE_VALUE) {
      break;
    }
  }

  if(err != ERROR_NO_MORE_FILES) {
    abort();
  }

  if(find_handle != INVALID_HANDLE_VALUE) {
    if(FindClose(find_handle) == FALSE) {
      abort();
    }
  }

  std::cout << "test-win32 found " << files.size() << " files." << std::endl;
  return 0;
}
