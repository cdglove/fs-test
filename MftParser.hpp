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
  std::uint64_t id = 0;
  std::uint64_t parent = 0;
  std::time_t created = 0;
  std::time_t accessed = 0;
  std::time_t modified = 0;
  std::uint64_t size = 0;
  std::string name;
  bool directory = false;
};

class MftParser {
 public:
  MftParser();
  ~MftParser();

  void open(std::wstring volume);
  void close();
  std::uint64_t count() const;
  std::vector<MftFile> read_all() const;

 private:
  friend class MftReader;
  void load_boot_sector();
  void load_mft();
  void read_data_run(
      std::uint64_t cluster, std::uint64_t count, std::vector<MftFile>& dest) const;
  void process_mft_read_buffer(
      std::vector<std::byte>& buffer, std::vector<MftFile>& dest) const;

  std::wstring volume_name_;
  boost::winapi::HANDLE_ volume_ = nullptr;
  std::uint32_t bytes_per_cluster_ = 0;
  std::uint64_t bytes_per_file_record_ = 0;
  std::uint32_t records_per_cluster_ = 0;
  std::vector<std::byte> mft_buffer_;
  struct NtfsNonResidentAttributeHeader const* mft_;
  std::uint64_t mft_location_ = 0;
  std::uint64_t mft_size_ = 0;
  std::uint64_t mft_record_count_ = 0;
};

class MftReader {
 public:
  MftReader(MftParser const& parser);
  bool read(std::vector<MftFile>& dest);

 private:
  class DataRun {
   public:
    DataRun(struct NtfsNonResidentAttributeHeader const* mft);
    std::uint64_t size() const;
    std::uint64_t logical_cluster() const;
    DataRun next() const;

   private:
    DataRun(std::uint8_t const* run, std::uint64_t offset);
    std::uint64_t length_ = 0;
    std::int64_t offset_ = 0;
    std::uint8_t const* run_ = nullptr;
  };

  MftParser const* parser_;
  DataRun run_;
  std::uint64_t remaining_;
};

} // namespace fsdb

#endif // FSDB_MFTPARSER_HPP