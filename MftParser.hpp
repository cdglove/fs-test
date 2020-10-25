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
#ifndef FSDB_MFTPARSER_HPP
#define FSDB_MFTPARSER_HPP

#include <boost/winapi/handles.hpp>
#include <cstddef>
#include <ctime>
#include <string>
#include <vector>

namespace fsdb {

struct MftFile {
  std::uint64_t parent = 0;
  std::time_t created = 0;
  std::time_t accessed = 0;
  std::time_t modified = 0;
  std::time_t updated = 0;
  std::uint64_t size = 0;
  std::wstring name;
  bool directory = false;
  bool in_use = false;
};

class MftParser {
 public:
  MftParser() = default;
  ~MftParser();

  void open(std::wstring volume);
  void close();
  void read();

  std::vector<MftFile>& TEMP_files() {
    return files_;
  }

 private:
  void load_boot_sector();
  void load_mft();
  void read_data_run(std::uint64_t cluster, std::uint64_t count);
  void process_mft_read_buffer(std::vector<std::byte>& buffer);

  std::wstring volume_name_;
  boost::winapi::HANDLE_ volume_ = nullptr;
  std::uint32_t bytes_per_cluster_ = 0;
  std::uint64_t bytes_per_file_record_ = 0;
  std::vector<std::byte> mft_buffer_;
  struct NtfsNonResidentAttributeHeader const* mft_data_attribute_ = nullptr;
  std::uint64_t mft_location_ = 0;
  std::uint64_t mft_size_ = 0;
  std::uint64_t mft_record_count_ = 0;
  std::uint64_t mft_current_record_ = 0;
  std::vector<MftFile> files_;
};

} // namespace fsdb

#endif // FSDB_MFTPARSER_HPP