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
#define BOOST_THREAD_VERSION 5

#include <windows.h>

#include <boost/nowide/convert.hpp>
#include <boost/timer/timer.hpp>

#include <boost/thread/barrier.hpp>
#include <boost/thread/executors/basic_thread_pool.hpp>
#include <boost/thread/sync_queue.hpp>

#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <numeric>
#include <vector>

struct File {
  uint64_t parent;
  uint64_t id;
  FILETIME created;
  FILETIME accessed;
  FILETIME modified;
  FILETIME updated;
  uint64_t size;
  std::string name;
};

// struct DirectoryNode {
//   std::size_t id;
//   std::size_t path_index;
//   std::wstring name;
// };

class Win32DirectoryCollector {
 public:
  struct ChildDirectory {
    std::wstring name;
    uint64_t id;
  };

  struct Directory {
    std::wstring path;
    std::vector<ChildDirectory> children;
  };

  struct SharedData {
    std::atomic<uint64_t> file_id{0};
    std::atomic<uint64_t> directory_id{1};
    std::atomic<uint64_t> outstanding{0};
    std::condition_variable done;
    boost::concurrent::sync_queue<Directory> work_queue;
  };

  explicit Win32DirectoryCollector(SharedData& shared)
      : shared_(&shared) {
  }

  void process_directory(std::wstring path) {
    process_directory(std::move(path), shared_->directory_id++);
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    shared_->done.wait(lk, [this] { return shared_->outstanding == 0; });
  }

  void process_queue() {
    try {
      while(true) {
        Directory dir;
        try {
          boost::concurrent::queue_op_status st =
              shared_->work_queue.wait_pull(dir);
          if(st == boost::concurrent::queue_op_status::closed) {
            return;
          }
          process_directories(std::move(dir.path), std::move(dir.children));
        }
        catch(boost::thread_interrupted&) {
          return;
        }
      }
    }
    catch(...) {
      std::terminate();
      return;
    }
  };

  std::vector<File> const& files() const {
    return files_;
  }

  std::vector<File> const& directories() const {
    return directories_;
  }

 private:
  void process_directories(
      std::wstring path, std::vector<ChildDirectory> children) {
    for(auto&& c : children) {
      auto child_path = path + c.name + L"\\";
      process_directory(child_path, c.id);
    }

    if(--shared_->outstanding == 0) {
      shared_->done.notify_all();
    }
  }

  void process_directory(std::wstring path, uint64_t parent_id) {
    WIN32_FIND_DATAW wfd;
    path += L"*";
    HANDLE find_handle = FindFirstFileExW(
        path.c_str(), FindExInfoBasic, &wfd, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_CASE_SENSITIVE | FIND_FIRST_EX_LARGE_FETCH |
            FIND_FIRST_EX_ON_DISK_ENTRIES_ONLY);
    if(find_handle == INVALID_HANDLE_VALUE) {
      return;
    }

    std::vector<ChildDirectory> children;
    while(true) {
      if((wcsncmp(L".", wfd.cFileName, 1) != 0) &&
         (wcsncmp(L"..", wfd.cFileName, 2) != 0)) {
        if(wfd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
          File f;
          f.parent = parent_id;
          f.id = shared_->directory_id++;
          f.name = boost::nowide::narrow(wfd.cFileName);
          f.size = (static_cast<std::uint64_t>(wfd.nFileSizeHigh) << 32) |
                   wfd.nFileSizeLow;
          f.modified = wfd.ftLastWriteTime;
          directories_.push_back(f);
          children.push_back({wfd.cFileName, f.id});
        }
        else {
          File f;
          f.parent = parent_id;
          f.id = shared_->file_id++;
          f.name = boost::nowide::narrow(wfd.cFileName);
          f.size = (static_cast<std::uint64_t>(wfd.nFileSizeHigh) << 32) |
                   wfd.nFileSizeLow;
          f.modified = wfd.ftLastWriteTime;
          files_.push_back(f);
        }
      }

      if(!FindNextFileW(find_handle, &wfd)) {
        break;
      }
    }

    DWORD err = err = GetLastError();
    if(err != ERROR_NO_MORE_FILES) {
      abort();
    }

    if(find_handle != INVALID_HANDLE_VALUE) {
      if(FindClose(find_handle) == FALSE) {
        abort();
      }
    }

    path.pop_back();
    shared_->outstanding++;
    shared_->work_queue.push({std::move(path), std::move(children)});
  }

  SharedData* shared_;
  std::vector<File> files_;
  std::vector<File> directories_;
  std::wstring current_path_;
};

int main() {
  boost::timer::auto_cpu_timer t;

  // std::vector<DirectoryNode> directory_stack;
  // boost::sync_queue<std::wstring> work;

  // // auto process_directory
  std::vector<File> directories;
  directories.push_back({});
  File root_file;
  root_file.parent = 0;
  root_file.id = 1;
  root_file.name = "\\\\?\\C:\\";
  directories.push_back(root_file);

  Win32DirectoryCollector::SharedData shared;
  Win32DirectoryCollector thread_collector(shared);
  std::vector<Win32DirectoryCollector> collector_refs(
      boost::thread::hardware_concurrency(), Win32DirectoryCollector(shared));
  boost::executors::basic_thread_pool pool;
  boost::barrier wait(boost::thread::hardware_concurrency() + 1);
  for(std::size_t i = 0; i < boost::thread::hardware_concurrency(); ++i) {
    pool.submit([&collector_refs, i, &wait] {
      wait.wait();
      collector_refs[i].process_queue();
    });
  }

  wait.wait();
  thread_collector.process_directory(L"\\\\?\\C:\\");
  shared.work_queue.close();
  pool.close();
  pool.join();

  directories.insert(directories.end(), thread_collector.directories().begin(), thread_collector.directories().end());
  std::vector<File> files = thread_collector.files();
  auto total_files = std::accumulate(
      collector_refs.begin(), collector_refs.end(), files.size(),
      [](std::size_t val, Win32DirectoryCollector const& tc) {
        return tc.files().size() + val;
      });

  auto total_directories = std::accumulate(
      collector_refs.begin(), collector_refs.end(), directories.size(),
      [](std::size_t val, Win32DirectoryCollector const& tc) {
        return tc.directories().size() + val;
      });

  files.resize(total_files);
  directories.resize(total_directories);

  std::size_t total_size = 0;
  for(auto&& tc : collector_refs) {
    for(auto&& f : tc.files()) {
      total_size += f.size;
      files[f.id] = std::move(f);
    }

    for(auto&& d : tc.directories()) {
      directories[d.id] = std::move(d);
    }
  }

  std::cout << "test-win32-threaded found " << files.size()
            << " files totalling " << total_size / 1024 << " KiB." << std::endl;
  return 0;
}
