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

#include <boost/assert.hpp>
#include <boost/core/ignore_unused.hpp>
#include <boost/nowide/convert.hpp>
#include <boost/throw_exception.hpp>
#include <boost/winapi/access_rights.hpp>
#include <boost/winapi/config.hpp>
#include <boost/winapi/file_management.hpp>
#include <cstddef>
#include <exception>
#include <stdexcept>
#include <vector>

extern "C" {
BOOST_WINAPI_IMPORT boost::winapi::BOOL_ BOOST_WINAPI_WINAPI_CC SetFilePointerEx(
    boost::winapi::HANDLE_ hFile,
    boost::winapi::LARGE_INTEGER_ liDistanceToMove,
    boost::winapi::PLARGE_INTEGER_ lpNewFilePointer,
    boost::winapi::DWORD_ dwMoveMethod);
}

namespace boost { namespace winapi {
using ::SetFilePointerEx;
}} // namespace boost::winapi

namespace fsdb {

template <typename Enum>
class EnumFlags {
 public:
  using StorageType = std::underlying_type_t<Enum>;

  static_assert(std::is_enum_v<Enum>, "Enum must be an enum type");
  explicit EnumFlags(Enum e)
      : bits_(static_cast<StorageType>(e)) {
  }

  EnumFlags& operator=(EnumFlags e) {
    bits_ = static_cast<StorageType>(e);
    return *this;
  }

  EnumFlags& operator&=(EnumFlags e) {
    bits_ &= e.bits_;
    return *this;
  }

  EnumFlags& operator&=(Enum e) {
    bits_ &= static_cast<StorageType>(e);
    return *this;
  }

  EnumFlags& operator|=(EnumFlags e) {
    bits_ |= e.bits_;
    return *this;
  }

  EnumFlags& operator|=(Enum e) {
    bits_ |= static_cast<StorageType>(e);
    return *this;
  }

  operator bool() const {
    return bits_ != 0;
  }

  friend EnumFlags operator&(EnumFlags a, EnumFlags b) {
    return EnumFlags(a.bits_ & b.bits_, 0);
  }

  friend EnumFlags operator&(Enum a, EnumFlags b) {
    return EnumFlags(static_cast<StorageType>(a) & b.bits_, 0);
  }

  friend EnumFlags operator&(EnumFlags a, Enum b) {
    return EnumFlags(a.bits_ & static_cast<StorageType>(b), 0);
  }

  friend EnumFlags operator|(EnumFlags a, EnumFlags b) {
    return EnumFlags(a.bits_ | b.bits_, 0);
  }

  friend EnumFlags operator|(Enum a, EnumFlags b) {
    return EnumFlags(static_cast<StorageType>(a) | b.bits_, 0);
  }

  friend EnumFlags operator|(EnumFlags a, Enum b) {
    return EnumFlags(a.bits_ | static_cast<StorageType>(b), 0);
  }

 private:
  EnumFlags(StorageType bits, int)
      : bits_(bits) {
  }

  StorageType bits_;
};

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

// All ntfs information taken from
// https://flatcap.org/linux-ntfs/ntfs/

// https://flatcap.org/linux-ntfs/ntfs/concepts/file_record.html
struct NtfsFileRecord {
  enum class Flag : std::uint16_t { None = 0, InUse = 1, Directory = 2 };
  std::uint32_t Type; // Magic int 'FILE', or 'ELIF' in little endian.
  std::uint16_t UsaOffset;
  std::uint16_t UsaCount;
  std::uint64_t Usn;
  std::uint16_t SequenceNumber;
  std::uint16_t LinkCount;
  std::uint16_t AttributesOffset;
  EnumFlags<Flag> Flags;
  std::uint32_t BytesInUse;
  std::uint32_t BytesAllocated;
  std::uint64_t BaseFileRecord;
  std::uint16_t NextAttributeNumber;
  std::uint16_t unused;
  std::uint32_t RecordId;
};

// https://flatcap.org/linux-ntfs/ntfs/attributes/index.html
enum class NtfsAttributeType : std::uint32_t {
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
  Terminator = 0xFFFFFFFF,
  FirstAttribute = StandardInformation,
  LastAttribute = LoggedUtilityStream,
};

// https://flatcap.org/linux-ntfs/ntfs/concepts/attribute_header.html
struct NtfsAttributeHeader {
  enum class Flag : std::uint16_t {
    None = 0,
    Compressed = 1,
  };

  NtfsAttributeType Type;
  std::uint32_t Length;
  std::uint8_t Nonresident;
  std::uint8_t NameLength;
  std::uint16_t NameOffset;
  EnumFlags<Flag> Flags;
  std::uint16_t AttributeNumber;
};

struct NtfsResidentAttributeHeader : NtfsAttributeHeader {
  enum class Flag : std::uint16_t {
    None = 0,
    Indexed = 1,
  };

  std::uint32_t ValueLength;
  std::uint16_t ValueOffset;
  EnumFlags<Flag> Flags;
};

struct NtfsNonResidentAttributeHeader : NtfsAttributeHeader {
  std::uint64_t FirstVcn;
  std::uint64_t LastVcn;
  std::uint16_t RunArrayOffset;
  std::uint16_t CompressionUnit;
  std::uint8_t Pad_[4];
  std::uint64_t AllocatedSize;
  std::uint64_t DataSize;
  std::uint64_t InitializedSize;
  std::uint64_t CompressedSize;
};

// https://flatcap.org/linux-ntfs/ntfs/attributes/file_name.html
struct NtfsFilenameAttribute {
  enum class Flag : std::uint32_t {
    ReadOnly = 0x1,
    Hidden = 0x2,
    System = 0x4,
    Archive = 0x20,
    Device = 0x40,
    Normal = 0x80,
    Temporary = 0x100,
    Sparse = 0x200,
    Reparse = 0x400,
    Compressed = 0x800,
    Offline = 0x1000,
    NotContentIndexed = 0x2000,
    Encrypted = 0x4000,
    Directory = 0x10000000,
    IndexView = 0x20000000,
  };

  enum class NameType : std::uint8_t {
    Posix = 0,
    Win32 = 1,
    DOS = 2,
  };

  std::uint64_t DirectoryRecordId; // points to a MFT Index of a directory
  std::int64_t CreationTime; // saved on creation, changed when filename changes
  std::int64_t ChangeTime;
  std::int64_t LastWriteTime;
  std::int64_t LastAccessTime;
  std::uint64_t AllocatedSize;
  std::uint64_t DataSize;
  EnumFlags<Flag> Flags;
  std::uint32_t AligmentOrReserved;
  std::uint8_t NameLength;
  EnumFlags<NameType> NameTypes;
  boost::winapi::WCHAR_ Name[1];
};

namespace {

using DWORD = boost::winapi::DWORD_;

DWORD constexpr GENERIC_READ = boost::winapi::GENERIC_READ_;
DWORD constexpr FILE_SHARE_READ = boost::winapi::FILE_SHARE_READ_;
DWORD constexpr FILE_SHARE_WRITE = boost::winapi::FILE_SHARE_WRITE_;
DWORD constexpr OPEN_EXISTING = boost::winapi::OPEN_EXISTING_;
DWORD constexpr FILE_FLAG_NO_BUFFERING = boost::winapi::FILE_FLAG_NO_BUFFERING_;
DWORD constexpr FILE_BEGIN = boost::winapi::FILE_BEGIN_;
const boost::winapi::HANDLE_ INVALID_HANDLE_VALUE = boost::winapi::INVALID_HANDLE_VALUE_;

template <typename Destination, typename Source>
Destination const* offset_cast(Source const* s, std::size_t offset) {
  return reinterpret_cast<Destination const*>(
      reinterpret_cast<std::byte const*>(s) + offset);
}

template <typename Destination, typename Source>
Destination* offset_cast(Source* s, std::size_t offset) {
  return reinterpret_cast<Destination*>(reinterpret_cast<std::byte*>(s) + offset);
}

NtfsResidentAttributeHeader const* to_resident(NtfsAttributeHeader const* attr) {
  if(attr->Nonresident) {
    return nullptr;
  }
  return reinterpret_cast<NtfsResidentAttributeHeader const*>(attr);
}

NtfsNonResidentAttributeHeader const* to_nonresident(NtfsAttributeHeader const* attr) {
  if(!attr->Nonresident) {
    return nullptr;
  }
  return reinterpret_cast<NtfsNonResidentAttributeHeader const*>(attr);
}

template <typename T>
T const* attribute_cast(NtfsResidentAttributeHeader const* attr) {
  if(attr->ValueLength < sizeof(T)) {
    return nullptr;
  }

  return offset_cast<T>(attr, attr->ValueOffset);
}

class AttributeList {
 public:
  AttributeList(NtfsFileRecord const* file)
      : record_(file)
      , current_(offset_cast<NtfsAttributeHeader>(file, file->AttributesOffset)) {
  }

  NtfsAttributeHeader const* next() {
    if(current_->Length > 0 && current_->Length < record_->BytesInUse) {
      current_ = offset_cast<NtfsAttributeHeader>(current_, current_->Length);
    }
    else if(current_->Nonresident) {
      current_ = offset_cast<NtfsAttributeHeader>(
          current_, sizeof(NtfsNonResidentAttributeHeader));
    }

    if(current_->Type == NtfsAttributeType::Terminator) {
      current_ = nullptr;
    }

    return current();
  }

  NtfsAttributeHeader const* current() const {
    return current_;
  }

 private:
  NtfsFileRecord const* record_;
  NtfsAttributeHeader const* current_;
};

bool fix_file_record(NtfsFileRecord* file) {
  std::uint16_t* usa = reinterpret_cast<std::uint16_t*>(
      reinterpret_cast<std::byte*>(file) + file->UsaOffset);
  std::uint16_t* sector = reinterpret_cast<std::uint16_t*>(file);

  if(file->UsaCount > 4) {
    return false;
  }
  for(std::uint32_t i = 1; i < file->UsaCount; i++) {
    sector[255] = usa[i];
    sector += 256;
  }

  return true;
}

std::time_t to_time_t(std::int64_t t) {
#if !defined(BOOST_MSVC) || BOOST_MSVC > 1300 // > VC++ 7.0
  if(t == 0) {
    return {};
  }
  t -= 116444736000000000LL;
#else
  t -= 116444736000000000;
#endif
  t /= 10000000;
  if(t < 0) {
    t = 0;
  }
  return static_cast<std::time_t>(t);
}

NtfsAttributeHeader const* find_attribute(
    NtfsFileRecord const* file, NtfsAttributeType type) {
  auto attribute = offset_cast<NtfsAttributeHeader>(file, file->AttributesOffset);
  int stop = std::min<int>(8, file->NextAttributeNumber);
  for(int i = 0; i < stop; ++i) {
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
      attribute = offset_cast<NtfsAttributeHeader>(attribute, attribute->Length);
    }
    else if(attribute->Nonresident) {
      attribute = offset_cast<NtfsAttributeHeader>(
          attribute, sizeof(NtfsNonResidentAttributeHeader));
    }
  }
  return nullptr;
}

NtfsFileRecord const* file_record_from_buffer(std::byte* data) {
  auto file = reinterpret_cast<NtfsFileRecord*>(data);
  fix_file_record(file);
  return file;
}

} // namespace
} // namespace fsdb

namespace fsdb {

MftParser::MftParser() = default;

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

std::uint64_t MftParser::count() const {
  return mft_record_count_ + 16; // +16 for special files.
}

std::vector<MftFile> MftParser::read_all() const {
  MftReader reader(*this);
  std::vector<MftFile> ret;
  ret.reserve(count());
  reader.read(ret);
  return ret;
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
  boost::winapi::LARGE_INTEGER_ offset;
  offset.QuadPart = mft_location_;
  boost::winapi::SetFilePointerEx(volume_, offset, nullptr, FILE_BEGIN);
  mft_buffer_.resize(bytes_per_cluster_);
  DWORD read_size = 0;
  if(!boost::winapi::ReadFile(
         volume_, mft_buffer_.data(), bytes_per_cluster_, &read_size, NULL)) {
    BOOST_THROW_EXCEPTION(std::runtime_error("Failed to read MFT"));
  }

  if(read_size < sizeof(NtfsFileRecord)) {
    BOOST_THROW_EXCEPTION(
        std::runtime_error("Failed to read MFT file record header"));
  }

  auto file = file_record_from_buffer(mft_buffer_.data());
  if(file->Type != 'ELIF') {
    BOOST_THROW_EXCEPTION(
        std::runtime_error("Failed to read MFT as Ntfs file"));
  }

  auto data_attrib = find_attribute(file, NtfsAttributeType::Data);
  auto bitmap_attrib = find_attribute(file, NtfsAttributeType::Bitmap);
  if(!data_attrib || !bitmap_attrib) {
    BOOST_THROW_EXCEPTION(
        std::runtime_error("Failed to find applicable attributes."));
  }

  mft_ = to_nonresident(data_attrib);
  mft_size_ = mft_->DataSize;
  BOOST_ASSERT(mft_size_ % bytes_per_file_record_ == 0);
  mft_record_count_ = mft_size_ / bytes_per_file_record_;
  BOOST_ASSERT(bytes_per_cluster_ % bytes_per_file_record_ == 0);
  records_per_cluster_ = bytes_per_cluster_ / bytes_per_file_record_;
}

void MftParser::read_data_run(
    std::uint64_t cluster, std::uint64_t count, std::vector<MftFile>& dest) const {
  boost::winapi::LARGE_INTEGER_ offset;
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

    process_mft_read_buffer(buffer, dest);
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

  process_mft_read_buffer(buffer, dest);
}

void MftParser::process_mft_read_buffer(
    std::vector<std::byte>& buffer, std::vector<MftFile>& dest) const {
  for(auto i = buffer.begin(), e = buffer.end(); i != e;
      i += bytes_per_file_record_) {
    NtfsFileRecord const* record = file_record_from_buffer(&*i);
    if(!(record->Flags & NtfsFileRecord::Flag::InUse)) {
      continue;
    }

    dest.emplace_back();
    MftFile& f = dest.back();

    AttributeList attributes(record);
    for(AttributeList attributes(record); attributes.current(); attributes.next()) {
      auto current = attributes.current();
      switch(current->Type) {
        case NtfsAttributeType::FileName: {
          auto name_attribute = attribute_cast<NtfsFilenameAttribute>(
              to_resident(current));
          if(!name_attribute) {
            continue;
          }

          f.id = record->RecordId;
          f.parent = name_attribute->DirectoryRecordId & 0x0000ffffffffffff;
          f.name = boost::nowide::narrow(
              name_attribute->Name, name_attribute->NameLength);
          f.size = name_attribute->DataSize;
          f.modified = to_time_t(name_attribute->LastWriteTime);
          f.created = to_time_t(name_attribute->CreationTime);
          f.accessed = to_time_t(name_attribute->LastAccessTime);
          f.directory = record->Flags & NtfsFileRecord::Flag::Directory;

        } break;
        case NtfsAttributeType::Data: {
          if(auto data_attribute = to_nonresident(current)) {
            f.size = data_attribute->DataSize;
          }
        } break;
        // Silence warning
        default:
          break;
      }
    }

    if(f.name.empty()) {
      dest.pop_back();
    }
  }
}

MftReader::MftReader(MftParser const& parser)
    : parser_(&parser)
    , run_(parser.mft_)
    , remaining_(parser.mft_->LastVcn + 1) {
}

bool MftReader::read(std::vector<MftFile>& dest) {
  for(; remaining_ != 0; run_ = run_.next()) {
    auto cluster_space_remaining = dest.capacity() / parser_->records_per_cluster_;
    auto read_cluster_count = std::min(run_.size(), remaining_);
    read_cluster_count = std::min(read_cluster_count, cluster_space_remaining);
    if(read_cluster_count == 0) {
      return false;
    }

    if(run_.logical_cluster() != 0) {
      parser_->read_data_run(run_.logical_cluster(), read_cluster_count, dest);
    }
    remaining_ -= read_cluster_count;
  }

  return true;
}

MftReader::DataRun::DataRun(NtfsNonResidentAttributeHeader const* mft)
    : DataRun(offset_cast<std::uint8_t>(mft, mft->RunArrayOffset), 0) {
}

std::uint64_t MftReader::DataRun::size() const {
  return length_;
}

std::uint64_t MftReader::DataRun::logical_cluster() const {
  return offset_;
}

MftReader::DataRun MftReader::DataRun::next() const {
  return DataRun(run_, offset_);
}

MftReader::DataRun::DataRun(std::uint8_t const* run, std::uint64_t offset)
    : run_(run) {
  std::uint8_t length_length = (*run_) & 0xf;
  std::uint8_t offset_length = ((*run_) >> 4) & 0xf;
  ++run_;

  for(int i = 0; i < length_length; ++i) {
    length_ |= std::uint64_t(*run_) << (8 * i);
    ++run_;
  }

  // Write one byte to extract the sign.
  offset_ = std::int8_t(run_[offset_length - 1]);
  offset_ <<= 8 * (offset_length - 1);
  // Can or in the remainder of the bites
  for(int i = 0; i < offset_length - 1; ++i) {
    offset_ |= std::uint64_t(*run_) << (8 * i);
    ++run_;
  }
  // And one more for the byte we extracted first.
  ++run_;

  offset_ += offset;
}

} // namespace fsdb