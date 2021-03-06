#define UNICODE
#define NOMINMAX 1
#include <Windows.h>

#include <boost/timer/timer.hpp>
#include <stdio.h>
#include <string>
#include <vector>

void show_record(USN_RECORD* record) {
  void* buffer;
  MFT_ENUM_DATA mft_enum_data;
  DWORD bytecount = 1;
  USN_RECORD* parent_record;

  WCHAR* filename;
  WCHAR* filenameend;

  printf("=================================================================\n");
  printf("RecordLength: %u\n", record->RecordLength);
  printf("MajorVersion: %u\n", (DWORD)record->MajorVersion);
  printf("MinorVersion: %u\n", (DWORD)record->MinorVersion);
  printf("FileReferenceNumber: %lu\n", record->FileReferenceNumber);
  printf("ParentFRN: %lu\n", record->ParentFileReferenceNumber);
  printf("USN: %lu\n", record->Usn);
  printf("Timestamp: %lu\n", record->TimeStamp);
  printf("Reason: %u\n", record->Reason);
  printf("SourceInfo: %u\n", record->SourceInfo);
  printf("SecurityId: %u\n", record->SecurityId);
  printf("FileAttributes: %x\n", record->FileAttributes);
  printf("FileNameLength: %u\n", (DWORD)record->FileNameLength);

  filename = (WCHAR*)(((BYTE*)record) + record->FileNameOffset);
  filenameend =
      (WCHAR*)(((BYTE*)record) + record->FileNameOffset +
      record->FileNameLength);

  printf("FileName: %.*ls\n", filenameend - filename, filename);

//   buffer =
//       VirtualAlloc(NULL, BUFFER_SIZE, MEM_RESERVE | MEM_COMMIT,
//       PAGE_READWRITE);

//   if(buffer == NULL) {
//     printf("VirtualAlloc: %u\n", GetLastError());
//     return;
//   }

//   mft_enum_data.StartFileReferenceNumber = record->ParentFileReferenceNumber;
//   mft_enum_data.LowUsn = 0;
//   mft_enum_data.HighUsn = maxusn;
//   mft_enum_data.MinMajorVersion = 2;
//   mft_enum_data.MaxMajorVersion = 2;

//   if(!DeviceIoControl(
//          drive, FSCTL_ENUM_USN_DATA, &mft_enum_data, sizeof(mft_enum_data),
//          buffer, BUFFER_SIZE, &bytecount, NULL)) {
//     printf("FSCTL_ENUM_USN_DATA (show_record): %u\n", GetLastError());
//     return;
//   }

//   parent_record = (USN_RECORD*)((USN*)buffer + 1);

//   if(parent_record->FileReferenceNumber != record->ParentFileReferenceNumber)
//   {
//     printf(
//         "=================================================================\n");
//     printf(
//         "Couldn't retrieve FileReferenceNumber %u\n",
//         record->ParentFileReferenceNumber);
//     return;
//   }

//   show_record(parent_record);
}

void check_record(USN_RECORD* record) {
  WCHAR* filename;
  WCHAR* filenameend;

  filename = (WCHAR*)(((BYTE*)record) + record->FileNameOffset);
  filenameend =
      (WCHAR*)(((BYTE*)record) + record->FileNameOffset +
      record->FileNameLength);

  if(filenameend - filename != 9)
    return;

  if(wcsncmp(filename, L"magic.fff", 9) != 0)
    return;

  show_record(record);
}

class UsnDirectoryCollector {
 public:
  UsnDirectoryCollector(std::wstring volume) {
    drive_ = CreateFile(
        volume.c_str(), GENERIC_READ,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, FILE_FLAG_NO_BUFFERING, NULL);

    buffer_.resize(4 * 1024 * 1024);
  }

  UsnDirectoryCollector() {
    if(drive_) {
      CloseHandle(drive_);
    }
  }

  void read_all() {
    DWORD bytes_read = 0;
    if(!DeviceIoControl(
           drive_, FSCTL_QUERY_USN_JOURNAL, NULL, 0, buffer_.data(),
           buffer_.size(), &bytes_read, NULL)) {
      printf("FSCTL_QUERY_USN_JOURNAL: %u\n", GetLastError());
      return;
    }

    USN_JOURNAL_DATA* journal =
        reinterpret_cast<USN_JOURNAL_DATA*>(buffer_.data());
    printf("UsnJournalID: %lu\n", journal->UsnJournalID);
    printf("FirstUsn: %lu\n", journal->FirstUsn);
    printf("NextUsn: %lu\n", journal->NextUsn);
    printf("LowestValidUsn: %lu\n", journal->LowestValidUsn);
    printf("MaxUsn: %lu\n", journal->MaxUsn);
    printf("MaximumSize: %lu\n", journal->MaximumSize);
    printf("AllocationDelta: %lu\n", journal->AllocationDelta);

    USN maxusn = journal->MaxUsn;
    MFT_ENUM_DATA mft_enum_data;
    mft_enum_data.StartFileReferenceNumber = 0;
    mft_enum_data.LowUsn = 0;
    mft_enum_data.HighUsn = maxusn;
    mft_enum_data.MinMajorVersion = 2;
    mft_enum_data.MaxMajorVersion = 2;

    int call_count = 0;
    int file_count = 0;
    DWORDLONG next_id = 0;
    for(;;) {
      //      printf("=================================================================\n");
      //      printf("Calling FSCTL_ENUM_USN_DATA\n");

      if(!DeviceIoControl(
             drive_, FSCTL_ENUM_USN_DATA, &mft_enum_data, sizeof(mft_enum_data),
             buffer_.data(), buffer_.size(), &bytes_read, NULL)) {
        printf(
            "================================================================="
            "\n");
        printf("FSCTL_ENUM_USN_DATA: %u\n", GetLastError());
        printf("Final ID: %lu\n", next_id);
        printf("File count: %lu\n", file_count);
        printf("Call count: %lu\n", call_count);
        return;
      }

      ++call_count;

      char* cursor = buffer_.data();
      next_id = *reinterpret_cast<DWORDLONG*>(buffer_.data());
      cursor += sizeof(USN);

      char* buffer_end = buffer_.data() + bytes_read;

      while(cursor < buffer_end) {
        USN_RECORD* record = reinterpret_cast<USN_RECORD*>(cursor);
        ++file_count;
        check_record(record);
        cursor += record->RecordLength;
      }

      mft_enum_data.StartFileReferenceNumber = next_id;
    }
  }

 private:
  HANDLE drive_;
  std::vector<char> buffer_;
};

int main(int argc, char** argv) {
  boost::timer::auto_cpu_timer t;
  UsnDirectoryCollector collector(L"\\\\?\\c:");
  collector.read_all();
}