#define UNICODE
#define NOMINMAX 1
#define BOOST_THREAD_VERSION 5
#include <Windows.h>

#include <boost/thread/barrier.hpp>
#include <boost/thread/executors/basic_thread_pool.hpp>
#include <boost/thread/sync_queue.hpp>
#include <boost/timer/timer.hpp>
#include <boost/scope_exit.hpp>
#include <iostream>
#include <numeric>
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
      (WCHAR*)(((BYTE*)record) + record->FileNameOffset + record->FileNameLength);

  printf("FileName: %.*ls\n", filenameend - filename, filename);

  //   buffer =
  //       VirtualAlloc(NULL, BUFFER_SIZE, MEM_RESERVE | MEM_COMMIT,
  //       PAGE_READWRITE);

  //   if(buffer == NULL) {
  //     printf("VirtualAlloc: %u\n", GetLastError());
  //     return;
  //   }

  //   mft_enum_data.StartFileReferenceNumber =
  //   record->ParentFileReferenceNumber; mft_enum_data.LowUsn = 0;
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

  //   if(parent_record->FileReferenceNumber !=
  //   record->ParentFileReferenceNumber)
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
      (WCHAR*)(((BYTE*)record) + record->FileNameOffset + record->FileNameLength);

  if(filenameend - filename != 9)
    return;

  if(wcsncmp(filename, L"magic.fff", 9) != 0)
    return;

  show_record(record);
}

class UsnDirectoryCollector {
 public:
  UsnDirectoryCollector(std::wstring volume)
      : volume_(std::move(volume)) {
  }

  int read_all() {
    HANDLE drive = CreateFile(
        volume_.c_str(), GENERIC_READ,
        FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
        OPEN_ALWAYS, 0, NULL);
    DWORD bytes_read = 0;
    std::vector<char> buffer;
    buffer.resize(1024 * 1024);
    if(!DeviceIoControl(
           drive, FSCTL_QUERY_USN_JOURNAL, NULL, 0, buffer.data(),
           buffer.size(), &bytes_read, NULL)) {
      printf("FSCTL_QUERY_USN_JOURNAL: %u\n", GetLastError());
      CloseHandle(drive);
      return -1;
    }

    CloseHandle(drive);

    USN_JOURNAL_DATA* journal =
        reinterpret_cast<USN_JOURNAL_DATA*>(buffer.data());
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

    std::atomic<uint64_t> outstanding{0};
    std::condition_variable done;
    outstanding++;
    usn_queue_.push(0);

    boost::executors::basic_thread_pool pool;
    boost::barrier wait(boost::thread::hardware_concurrency() + 1);
    std::vector<int> file_counts(boost::thread::hardware_concurrency(), 0);
    for(std::size_t i = 0; i < boost::thread::hardware_concurrency(); ++i) {
      pool.submit([&file_count = file_counts[i], &wait, volume = this->volume_,
                   &usn_queue = this->usn_queue_, &outstanding, &done,
                   mft_enum_data]() mutable {
        try {
          HANDLE drive = CreateFile(
              volume.c_str(), GENERIC_READ,
              FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
              OPEN_ALWAYS, 0, NULL);
          BOOST_SCOPE_EXIT(&drive) {
            CloseHandle(drive);
          }
          BOOST_SCOPE_EXIT_END
         
          std::vector<char> buffer;
          buffer.resize(4 * 1024 * 1024);
          DWORD bytes_read = 0;
          wait.wait();
          while(true) {
            USN current_id;
            try {
              auto st = usn_queue.wait_pull(current_id);
              if(st == boost::concurrent::queue_op_status::closed) {
                return;
              }

              mft_enum_data.StartFileReferenceNumber = current_id;
              if(!DeviceIoControl(
                     drive, FSCTL_ENUM_USN_DATA, &mft_enum_data,
                     sizeof(mft_enum_data), buffer.data(), buffer.size(),
                     &bytes_read, NULL)) {
                printf("===================================================\n");
                printf("FSCTL_ENUM_USN_DATA: %u\n", GetLastError());
                printf("Final ID: %lu\n", current_id);
                if(--outstanding == 0) {
                  done.notify_all();
                }

                continue;
              }

              char* cursor = buffer.data();
              USN next_id = *reinterpret_cast<DWORDLONG*>(buffer.data());
              ++outstanding;
              usn_queue.push(next_id);
              cursor += sizeof(USN);

              char* buffer_end = buffer.data() + bytes_read;

              while(cursor < buffer_end) {
                USN_RECORD* record = reinterpret_cast<USN_RECORD*>(cursor);
                ++file_count;
                check_record(record);
                cursor += record->RecordLength;
              }

              if(--outstanding == 0) {
                done.notify_all();
              }
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
      });
    }

    wait.wait();
    std::mutex m;
    std::unique_lock<std::mutex> lk(m);
    done.wait(lk, [&outstanding] { return outstanding == 0; });
    usn_queue_.close();
    pool.close();
    pool.join();
    return std::accumulate(file_counts.begin(), file_counts.end(), 0);
  }

 private:
  boost::concurrent::sync_queue<USN> usn_queue_;
  std::wstring volume_;
};

int main(int argc, char** argv) {
  boost::timer::auto_cpu_timer t;
  UsnDirectoryCollector collector(L"\\\\?\\c:");
  auto count = collector.read_all();
  std::cout << "Count -- " << count << std::endl;
  return 0;
}