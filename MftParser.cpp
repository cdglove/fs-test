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
#include "MftParser.hpp"
#include <boost/throw_exception.hpp>
#include <exception>
#include <stdexcept>
#include <vector>

#include <boost/core/ignore_unused.hpp>
#include <boost/scope_exit.hpp>
#include <boost/winapi/access_rights.hpp>
#include <boost/winapi/config.hpp>
#include <boost/winapi/file_management.hpp>

extern "C" {
BOOST_WINAPI_IMPORT boost::winapi::BOOL_ BOOST_WINAPI_WINAPI_CC SetFilePointerEx(
    boost::winapi::HANDLE_ hFile,
    boost::winapi::LARGE_INTEGER_ liDistanceToMove,
    boost::winapi::PLARGE_INTEGER_ lpNewFilePointer,
    boost::winapi::DWORD_ dwMoveMethod);
}

namespace boost { namespace winapi {
using ::SetFilePointerEx;
using USN_ = boost::winapi::LONGLONG_;
}} // namespace boost::winapi

namespace fsdb {

namespace {

// So we're compatible with window.h without inlcuding it.
using UCHAR = boost::winapi::UCHAR_;
using DWORD = boost::winapi::DWORD_;
using USHORT = boost::winapi::USHORT_;
using ULONG = boost::winapi::ULONG_;
using ULONGLONG = boost::winapi::ULONGLONG_;
using LARGE_INTEGER = boost::winapi::LARGE_INTEGER_;
using USN = boost::winapi::USN_;
using BOOLEAN = boost::winapi::BOOLEAN_;

decltype(auto) GENERIC_READ = boost::winapi::GENERIC_READ_;
decltype(auto) FILE_SHARE_READ = boost::winapi::FILE_SHARE_READ_;
decltype(auto) FILE_SHARE_WRITE = boost::winapi::FILE_SHARE_WRITE_;
decltype(auto) OPEN_EXISTING = boost::winapi::OPEN_EXISTING_;
decltype(auto) FILE_FLAG_NO_BUFFERING = boost::winapi::FILE_FLAG_NO_BUFFERING_;
decltype(auto) FILE_BEGIN = boost::winapi::FILE_BEGIN_;
decltype(auto) INVALID_HANDLE_VALUE = boost::winapi::INVALID_HANDLE_VALUE_;

#pragma pack(push, 1)
struct BootBlock {
  std::uint8_t Jump[3];
  std::uint8_t Format[8];
  std::uint16_t BytesPerSector;
  std::uint8_t SectorsPerCluster;
  std::uint16_t BootSectors;
  std::uint8_t Mbz1;
  std::uint16_t Mbz2;
  std::uint16_t Reserved1;
  std::uint8_t MediaType;
  std::uint16_t Mbz3;
  std::uint16_t SectorsPerTrack;
  std::uint16_t NumberOfHeads;
  std::uint32_t PartitionOffset;
  std::uint32_t Rserved2[2];
  std::uint64_t TotalSectors;
  std::uint64_t MftStartLcn;
  std::uint64_t Mft2StartLcn;
  std::uint32_t ClustersPerFileRecord;
  std::uint32_t ClustersPerIndexBlock;
  std::uint64_t VolumeSerialNumber;
  std::uint8_t Code[0x1AE];
  std::uint16_t BootSignature;
};

#pragma pack(pop)

struct NftsRecordHeader {
  ULONG Type;
  USHORT UsaOffset;
  USHORT UsaCount;
  USN Usn;
};

namespace NftsFileRecordFlag {
USHORT constexpr None = 0;
USHORT constexpr InUse = 1;
USHORT constexpr Directory = 2;
} // namespace NftsFileRecordFlag

struct NtfsFileRecordHeader {
  NftsRecordHeader Ntfs;
  USHORT SequenceNumber;
  USHORT LinkCount;
  USHORT AttributesOffset;
  USHORT Flags; // NftsFileRecordFlag
  ULONG BytesInUse;
  ULONG BytesAllocated;
  LARGE_INTEGER BaseFileRecord;
  USHORT NextAttributeNumber;
  USHORT unused;
  ULONG RecordNumber;
};

enum class NtfsAttributeType {
  StandardInformation = 0x10,
  AttributeList = 0x20,
  FileName = 0x30,
  ObjectId = 0x40,
  SecurityDescripter = 0x50,
  VolumeName = 0x60,
  VolumeInformation = 0x70,
  Data = 0x80,
  IndexRoot = 0x90,
  IndexAllocation = 0xA0,
  Bitmap = 0xB0,
  ReparsePoint = 0xC0,
  EAInformation = 0xD0,
  EA = 0xE0,
  PropertySet = 0xF0,
  LoggedUtilityStream = 0x100,
  FirstAttribute = StandardInformation,
  LastAttribute = LoggedUtilityStream,
};

namespace NtfsAttributeFlag {
USHORT constexpr None = 0;
USHORT constexpr Compressed = 1;
} // namespace NtfsAttributeFlag

struct NtfsAttribute {
  NtfsAttributeType Type;
  ULONG Length;
  BOOLEAN Nonresident;
  UCHAR NameLength;
  USHORT NameOffset; // Starts form the Attribute Offset
  USHORT Flags;      // 0x001 = Compressed
  USHORT AttributeNumber;
};

} // namespace

// Sadly needs to be in global scope for the ptr in the header.
struct NtfsNonResidentAttribute {
  NtfsAttribute Attribute;
  ULONGLONG FirstVcn;
  ULONGLONG LastVcn;
  USHORT RunArrayOffset;
  USHORT CompressionUnit;
  UCHAR AligmentOrReserved[4];
  ULONGLONG AllocatedSize;
  ULONGLONG DataSize;
  ULONGLONG InitializedSize;
  ULONGLONG CompressedSize; // Only when compressed
};

namespace {

template <typename Destination, typename Source>
Destination const* offset_cast(Source const* s, std::size_t offset) {
  return reinterpret_cast<Destination const*>(
      reinterpret_cast<std::byte const*>(s) + offset);
}

template <typename Destination, typename Source>
Destination* offset_cast(Source* s, std::size_t offset) {
  return reinterpret_cast<Destination*>(reinterpret_cast<std::byte*>(s) + offset);
}

class DataRun {
 public:
  DataRun(NtfsNonResidentAttribute const* mft, std::uint64_t virtual_cluster)
      : logical_cluster_(0)
      , count_(0)
      , mft_(mft) {
    std::uint64_t base = mft->FirstVcn;
    // cglover-todo: This could be re-entrant and more optimal.
    for(auto run = offset_cast<std::uint8_t>(mft, mft->RunArrayOffset);
        *run != 0; run += run_length(run)) {
      auto run_lcn = get_logical_cluster_number(run);
      logical_cluster_ += run_lcn;
      count_ = run_count(run);
      if(base <= virtual_cluster && virtual_cluster < base + count_) {
        logical_cluster_ = run_lcn == 0
                               ? 0
                               : logical_cluster_ + virtual_cluster - base;
        count_ -= std::uint64_t(virtual_cluster - base);
        return;
      }
      else {
        base += count_;
      }
    }

    logical_cluster_ = 0;
    count_ = 0;
  }

  std::uint64_t logical_cluster() const {
    return logical_cluster_;
  }

  std::uint64_t count() const {
    return count_;
  }

  DataRun next() const {
    return DataRun(mft_, logical_cluster_ + count_);
  }

 private:
  std::uint64_t logical_cluster_ = 0;
  std::uint64_t count_ = 0;
  NtfsNonResidentAttribute const* mft_;

  static std::uint64_t get_logical_cluster_number(std::uint8_t const* run) {
    std::uint8_t n1 = *run & 0xf;
    std::uint8_t n2 = (*run >> 4) & 0xf;
    std::uint64_t lcn = n2 == 0 ? 0 : std::int8_t(run[n1 + n2]);
    for(int i = n1 + n2 - 1; i > n1; i--)
      lcn = (lcn << 8) + run[i];
    return lcn;
  }

  static std::uint64_t run_count(std::uint8_t const* run) {
    std::uint8_t k = *run & 0xf;
    std::uint64_t count = 0;
    for(int i = k; i > 0; i--) {
      count = (count << 8) + run[i];
    }

    return count;
  }

  static std::uint64_t run_length(std::uint8_t const* run) {
    return (*run & 0xf) + ((*run >> 4) & 0xf) + 1;
  }
};

bool fix_file_record(NtfsFileRecordHeader* file) {
  // int sec = 2048;
  USHORT* usa = reinterpret_cast<USHORT*>(
      reinterpret_cast<std::byte*>(file) + file->Ntfs.UsaOffset);
  USHORT* sector = reinterpret_cast<USHORT*>(file);

  if(file->Ntfs.UsaCount > 4)
    return false;
  for(ULONG i = 1; i < file->Ntfs.UsaCount; i++) {
    sector[255] = usa[i];
    sector += 256;
  }

  return true;
}

NtfsAttribute const* find_attribute(
    NtfsFileRecordHeader const* file, NtfsAttributeType type) {
  auto attribute = offset_cast<NtfsAttribute>(file, file->AttributesOffset);
  for(int i = 0; i < file->NextAttributeNumber; ++i) {
    if(attribute->Type == type) {
      return attribute;
    }

    if(attribute->Type < NtfsAttributeType::FirstAttribute) {
      break;
    }

    if(attribute->Type > NtfsAttributeType::LastAttribute) {
      break;
    }

    if(attribute->Length > 0 && attribute->Length < file->BytesInUse) {
      attribute = offset_cast<NtfsAttribute>(attribute, attribute->Length);
    }
    else if(attribute->Nonresident) {
      attribute = offset_cast<NtfsAttribute>(
          attribute, sizeof(NtfsNonResidentAttribute));
    }
  }
  return nullptr;
}

NtfsFileRecordHeader const* file_record_from_buffer(std::byte* data) {
  auto file = reinterpret_cast<NtfsFileRecordHeader*>(data);
  fix_file_record(file);
  return file;
}

} // namespace

MftParser::~MftParser() {
  close();
}

void MftParser::open(std::wstring volume) {
  using namespace boost::winapi;
  volume_ = boost::winapi::CreateFileW(
      volume.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
      OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, nullptr);

  if(volume_ == INVALID_HANDLE_VALUE) {
    BOOST_THROW_EXCEPTION(std::runtime_error("Failed to open volume"));
  }

  try {
    load_boot_sector();
    load_mft();
    volume_name_ = std::move(volume);
  }
  catch(...) {
    close();
    throw;
  }
}

void MftParser::close() {
  if(volume_) {
    boost::winapi::CloseHandle(volume_);
    volume_ = nullptr;
  }
}

void MftParser::read() {
  DataRun run(mft_data_attribute_, 0);
  auto count = mft_data_attribute_->LastVcn;

  for(auto remaining = count; remaining != 0;) {
    auto read_cluster_count = std::min(run.count(), remaining);
    if(run.logical_cluster() != 0) {
      read_data_run(run.logical_cluster(), read_cluster_count);
    }
    remaining -= read_cluster_count;
    run = run.next();
  }
}

void MftParser::load_boot_sector() {
  BootBlock boot_sector;
  DWORD read_size = 0;
  boost::winapi::ReadFile(
      volume_, &boot_sector, sizeof(boot_sector), &read_size, nullptr);
  if(read_size != sizeof(boot_sector)) {
    BOOST_THROW_EXCEPTION(std::runtime_error("Failed to read boot sector."));
  }

  if(std::memcmp("NTFS", boot_sector.Format, 4)) {
    BOOST_THROW_EXCEPTION(std::runtime_error("Volume is not an NTFS drive"));
  }

  bytes_per_cluster_ = boot_sector.BytesPerSector * boot_sector.SectorsPerCluster;

  if(boot_sector.ClustersPerFileRecord < 0x80) {
    bytes_per_file_record_ = boot_sector.ClustersPerFileRecord * bytes_per_cluster_;
  }
  else {
    bytes_per_file_record_ = std::uint64_t(1)
                             << (0x100 - boot_sector.ClustersPerFileRecord);
  }

  mft_location_ = boot_sector.MftStartLcn * bytes_per_cluster_;
}

void MftParser::load_mft() {
  LARGE_INTEGER offset;
  offset.QuadPart = mft_location_;
  boost::winapi::SetFilePointerEx(volume_, offset, nullptr, FILE_BEGIN);
  mft_buffer_.resize(bytes_per_cluster_);
  DWORD read_size = 0;
  if(!boost::winapi::ReadFile(
         volume_, mft_buffer_.data(), bytes_per_cluster_, &read_size, NULL)) {
    BOOST_THROW_EXCEPTION(std::runtime_error("Failed to read MFT"));
  }

  if(read_size < sizeof(NtfsFileRecordHeader)) {
    BOOST_THROW_EXCEPTION(
        std::runtime_error("Failed to read MFT file record header"));
  }

  auto file = file_record_from_buffer(mft_buffer_.data());
  if(file->Ntfs.Type != 'ELIF') {
    BOOST_THROW_EXCEPTION(
        std::runtime_error("Failed to read MFT as Ntfs file"));
  }

  auto data_attrib = find_attribute(file, NtfsAttributeType::Data);
  auto bitmap_attrib = find_attribute(file, NtfsAttributeType::Bitmap);
  if(!data_attrib || !bitmap_attrib) {
    BOOST_THROW_EXCEPTION(
        std::runtime_error("Failed to find applicable attributes."));
  }

  auto data_nr_attrib = reinterpret_cast<NtfsNonResidentAttribute const*>(
      data_attrib);
  mft_size_ = data_nr_attrib->DataSize;
  BOOST_ASSERT(mft_size_ % bytes_per_file_record_ == 0);
  mft_record_count_ = mft_size_ / bytes_per_file_record_;
  mft_data_attribute_ = data_nr_attrib;
  mft_current_record_ = 0;
}

void MftParser::read_data_run(std::uint64_t cluster, std::uint64_t count) {
  LARGE_INTEGER offset;
  offset.QuadPart = cluster * bytes_per_cluster_;
  boost::winapi::SetFilePointerEx(volume_, offset, nullptr, FILE_BEGIN);

  std::uint32_t clusters_per_read = 1024;
  auto read_count = count / clusters_per_read;
  auto bytes_per_read = clusters_per_read * bytes_per_cluster_;
  std::vector<std::byte> buffer(bytes_per_read);

  DWORD bytes_read = 0;
  for(int i = 0; i < read_count; ++i) {
    boost::winapi::ReadFile(
        volume_, buffer.data(), bytes_per_read, &bytes_read, nullptr);

    if(bytes_read != buffer.size()) {
      boost::throw_exception(
          std::runtime_error("Failed to read correct number of bytes."));
    }

    process_mft_read_buffer(buffer);
  }

  auto remaining_clusters = static_cast<DWORD>(count % clusters_per_read);
  buffer.resize(remaining_clusters * bytes_per_cluster_);
  boost::winapi::ReadFile(
      volume_, buffer.data(), remaining_clusters * bytes_per_cluster_,
      &bytes_read, nullptr);

  if(bytes_read != buffer.size()) {
    boost::throw_exception(
        std::runtime_error("Failed to read correct number of bytes."));
  }

  process_mft_read_buffer(buffer);
}

void MftParser::process_mft_read_buffer(std::vector<std::byte>& buffer) {
  for(auto i = buffer.begin(), e = buffer.end(); i != e;
      i += bytes_per_file_record_) {
    NtfsFileRecordHeader const* record = file_record_from_buffer(&*i);
    auto info = find_attribute(record, NtfsAttributeType::StandardInformation);
    auto name = find_attribute(record, NtfsAttributeType::FileName);
    auto data = find_attribute(record, NtfsAttributeType::Data);
  }
}

} // namespace fsdb