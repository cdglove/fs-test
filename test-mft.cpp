/*

*/
#include "MftParser.hpp"
#include <algorithm>
#include <assert.h>
#include <boost/timer/timer.hpp>
#include <ctime>
#include <iostream>
#include <iterator>
#include <numeric>
#include <stdint.h>
#include <stdio.h>
#include <vector>
#include <windows.h>

#include <boost/winapi/handles.hpp>
#include <string>

/* BOOT_BLOCK

*/
#pragma pack(push, 1)

typedef struct {
  UCHAR Jump[3];
  UCHAR Format[8];
  USHORT BytesPerSector;
  UCHAR SectorsPerCluster;
  USHORT BootSectors;
  UCHAR Mbz1;
  USHORT Mbz2;
  USHORT Reserved1;
  UCHAR MediaType;
  USHORT Mbz3;
  USHORT SectorsPerTrack;
  USHORT NumberOfHeads;
  ULONG PartitionOffset;
  ULONG Rserved2[2];
  ULONGLONG TotalSectors;
  ULONGLONG MftStartLcn;
  ULONGLONG Mft2StartLcn;
  ULONG ClustersPerFileRecord;
  ULONG ClustersPerIndexBlock;
  ULONGLONG VolumeSerialNumber;
  UCHAR Code[0x1AE];
  USHORT BootSignature;
} BOOT_BLOCK, *PBOOT_BLOCK;

#pragma pack(pop)

/* NTFS_RECORD_HEADER
    type - 'FILE' 'INDX' 'BAAD' 'HOLE' *CHKD'

*/
typedef struct {
  ULONG Type;
  USHORT UsaOffset;
  USHORT UsaCount;
  USN Usn;
} NTFS_RECORD_HEADER, *PNTFS_RECORD_HEADER;

/* FILE_RECORD_HEADER

*/
typedef struct {
  NTFS_RECORD_HEADER Ntfs;
  USHORT SequenceNumber;
  USHORT LinkCount;
  USHORT AttributesOffset;
  USHORT Flags; // 0x0001 InUse; 0x0002 Directory
  ULONG BytesInUse;
  ULONG BytesAllocated;
  ULARGE_INTEGER BaseFileRecord;
  USHORT NextAttributeNumber;
  USHORT unused;
  ULONG RecordNumber;
} FILE_RECORD_HEADER, *PFILE_RECORD_HEADER;

/* ATTRIBUTE_TYPE enumeration

*/

typedef enum {
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
  LoggedUtilityStream = 0x100
} ATTRIBUTE_TYPE,
    *PATTRIBUTE_TYPE;

/* ATTRIBUTE Structure

*/
typedef struct {
  ATTRIBUTE_TYPE AttributeType;
  ULONG Length;
  BOOLEAN Nonresident;
  UCHAR NameLength;
  USHORT NameOffset; // Starts form the Attribute Offset
  USHORT Flags;      // 0x001 = Compressed
  USHORT AttributeNumber;
} ATTRIBUTE, *PATTRIBUTE;

/* ATTRIBUTE resident

*/
typedef struct {
  ATTRIBUTE Attribute;
  ULONG ValueLength;
  USHORT ValueOffset; // Starts from the Attribute
  USHORT Flags;       // 0x0001 Indexed
} RESIDENT_ATTRIBUTE, *PRESIDENT_ATTRIBUTE;

/* ATTRIBUTE nonresident

*/
typedef struct {
  ATTRIBUTE Attribute;
  ULONGLONG LowVcn;
  ULONGLONG HighVcn;
  USHORT RunArrayOffset;
  UCHAR CompressionUnit;
  UCHAR AligmentOrReserved[5];
  ULONGLONG AllocatedSize;
  ULONGLONG DataSize;
  ULONGLONG InitializedSize;
  ULONGLONG CompressedSize; // Only when compressed
} NONRESIDENT_ATTRIBUTE, *PNONRESIDENT_ATTRIBUTE;

/*
    VolumeName - just a Unicode String
    Data = just data
    SecurityDescriptor - rarely found
    Bitmap - array of bits, which indicate the use of entries
*/

/* STANDARD_INFORMATION
    FILE_ATTRIBUTES_* like in windows.h
    and is always resident
*/
typedef struct {
  FILETIME CreationTime;
  FILETIME ChangeTime;
  FILETIME LastWriteTime;
  FILETIME LastAccessTime;
  ULONG FileAttributes;
  ULONG AligmentOrReservedOrUnknown[3];
  ULONG QuotaId;         // NTFS 3.0 or higher
  ULONG SecurityID;      // NTFS 3.0 or higher
  ULONGLONG QuotaCharge; // NTFS 3.0 or higher
  USN Usn;               // NTFS 3.0 or higher
} STANDARD_INFORMATION, *PSTANDARD_INFORMATION;

/* ATTRIBUTE_LIST
    is always nonresident and consists of an array of ATTRIBUTE_LIST
*/
typedef struct {
  ATTRIBUTE_TYPE Attribute;
  USHORT Length;
  UCHAR NameLength;
  USHORT NameOffset; // starts at structure begin
  ULONGLONG LowVcn;
  ULONGLONG FileReferenceNumber;
  USHORT AttributeNumber;
  USHORT AligmentOrReserved[3];
} ATTRIBUTE_LIST, *PATTRIBUTE_LIST;

/* FILENAME_ATTRIBUTE
    is always resident
    ULONGLONG informations only updated, if name changes
*/
typedef struct {
  ULONGLONG DirectoryFileReferenceNumber; // points to a MFT Index of a
                                          // directory
  FILETIME CreationTime; // saved on creation, changed when filename changes
  FILETIME ChangeTime;
  FILETIME LastWriteTime;
  FILETIME LastAccessTime;
  ULONGLONG AllocatedSize;
  ULONGLONG DataSize;
  ULONG FileAttributes; // ditto
  ULONG AligmentOrReserved;
  UCHAR NameLength;
  UCHAR NameType; // 0x01 Long 0x02 Short 0x00 Posix?
  WCHAR Name[1];
} FILENAME_ATTRIBUTE, *PFILENAME_ATTRIBUTE;

#define POSIX_NAME 0
#define WIN32_NAME 1
#define DOS_NAME 2
#define WIN32DOS_NAME 3

/* MYSTRUCTS


*/
#define NTFSDISK 1

// not supported
#define FAT32DISK 2
#define FATDISK 4
#define EXT2 8

#define UNKNOWN 0xff99ff99

typedef struct {
  LPCWSTR FileName;
  USHORT FileNameLength;
  USHORT Flags;
  ULARGE_INTEGER ParentId;
} SEARCHFILEINFO, *PSEARCHFILEINFO;

typedef struct {
  LPCWSTR FileName;
  USHORT FileNameLength;
  USHORT Flags;
  ULARGE_INTEGER ParentId;
  ULARGE_INTEGER FileSize;
  LPARAM UserData;
  PVOID ExtraData;
} SHORTFILEINFO, *PSHORTFILEINFO;

typedef struct {
  LPCWSTR FileName;
  USHORT FileNameLength;
  USHORT Flags;
  ULARGE_INTEGER ParentId;
  ULARGE_INTEGER FileSize;
  LPARAM UserData;
  PVOID ExtraData;

  FILETIME CreationTime;
  FILETIME AccessTime;
  FILETIME WriteTime;
  FILETIME ChangeTime;
  ULARGE_INTEGER AllocatedFileSize;
  DWORD FileAttributes;
  DWORD Attributes;
} LONGFILEINFO, *PLONGFILEINFO;

struct File {
  uint64_t parent;
  FILETIME created;
  FILETIME accessed;
  FILETIME modified;
  FILETIME updated;
  uint64_t size = 0;
  std::wstring name;
  bool directory;
  bool in_use = false;
};

typedef struct {
  HANDLE fileHandle;
  DWORD type;
  DWORD IsLong;
  DWORD filesSize;
  DWORD realFiles;
  WCHAR DosDevice;
  union {
    struct {
      BOOT_BLOCK bootSector;
      DWORD BytesPerFileRecord;
      DWORD BytesPerCluster;
      BOOL complete;
      DWORD sizeMFT;
      DWORD entryCount;
      ULARGE_INTEGER MFTLocation;
      UCHAR* MFT;
      UCHAR* Bitmap;
    } NTFS;
    struct {
      DWORD FAT;
    } FAT;

    union {
      LONGFILEINFO* lFiles;
      SHORTFILEINFO* sFiles;
      SEARCHFILEINFO* fFiles;
    };
  };
  std::vector<File> Files;
} DISKHANDLE, *PDISKHANDLE;

typedef struct {
  HWND hWnd;
  DWORD Value;
} STATUSINFO, *PSTATUSINFO;

/* MY FUNCTIONS

*/

#define LONGINFO 1
#define SHORTINFO 2
#define SEARCHINFO 3
#define EXTRALONGINFO 4

typedef DWORD(__cdecl* FETCHPROC)(PDISKHANDLE, PFILE_RECORD_HEADER, PUCHAR);

// LinkedList
struct LINKITEM {
  unsigned int data;
  unsigned int entry;
  LINKITEM* next;
};

LINKITEM* fixlist = NULL;
LINKITEM* curfix = NULL;

void AddToFixList(int entry, int data) {
  //    curfix->entry = entry;
  //    curfix->data = data;
  //    curfix->next = new LINKITEM;
  //    curfix = curfix->next;
  //    curfix->next = NULL;
}

void CreateFixList() {
  fixlist = new LINKITEM;
  fixlist->next = NULL;
  curfix = fixlist;
}

void ProcessFixList(PDISKHANDLE disk) {
  SEARCHFILEINFO *info, *src;
  while(fixlist->next != NULL) {
    info = &disk->fFiles[fixlist->entry];
    src = &disk->fFiles[fixlist->data];
    info->FileName = src->FileName;
    info->FileNameLength = src->FileNameLength;

    info->ParentId = src->ParentId;
    // hide all that we used for cleanup
    src->ParentId.QuadPart = 0;
    LINKITEM* item;
    item = fixlist;
    fixlist = fixlist->next;
    delete item;
  }
  delete fixlist;
  fixlist = NULL;
  curfix = NULL;
}

// NONRESIDENT_ATTRIBUTE ERROR_ATTRIBUTE = {1,2,3,4,5};
#define CLUSTERSPERREAD 1024

PDISKHANDLE OpenDisk(wchar_t const* disk) {
  PDISKHANDLE tmpDisk;
  DWORD read;
  tmpDisk = new DISKHANDLE;
  memset(tmpDisk, 0, sizeof(DISKHANDLE));
  tmpDisk->fileHandle = CreateFileW(
      disk, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
      OPEN_EXISTING, FILE_FLAG_NO_BUFFERING, NULL);
  if(tmpDisk->fileHandle != INVALID_HANDLE_VALUE) {
    ReadFile(
        tmpDisk->fileHandle, &tmpDisk->NTFS.bootSector, sizeof(BOOT_BLOCK),
        &read, NULL);
    if(read == sizeof(BOOT_BLOCK)) {
      if(strncmp("NTFS", (const char*)&tmpDisk->NTFS.bootSector.Format, 4) ==
         0) {
        tmpDisk->type = NTFSDISK;
        tmpDisk->NTFS.BytesPerCluster =
            tmpDisk->NTFS.bootSector.BytesPerSector *
            tmpDisk->NTFS.bootSector.SectorsPerCluster;
        tmpDisk->NTFS.BytesPerFileRecord =
            tmpDisk->NTFS.bootSector.ClustersPerFileRecord < 0x80
                ? tmpDisk->NTFS.bootSector.ClustersPerFileRecord *
                      tmpDisk->NTFS.BytesPerCluster
                : 1 << (0x100 - tmpDisk->NTFS.bootSector.ClustersPerFileRecord);

        tmpDisk->NTFS.complete = FALSE;
        tmpDisk->NTFS.MFTLocation.QuadPart =
            tmpDisk->NTFS.bootSector.MftStartLcn *
            tmpDisk->NTFS.BytesPerCluster;
        tmpDisk->NTFS.MFT = NULL;
        tmpDisk->IsLong = FALSE;
        tmpDisk->NTFS.sizeMFT = 0;
      }
      else {
        tmpDisk->type = UNKNOWN;
        tmpDisk->lFiles = NULL;
      }
    }
    return tmpDisk;
  }

  delete tmpDisk;
  return NULL;
};

PDISKHANDLE OpenDisk(WCHAR DosDevice) {
  WCHAR path[9];
  path[0] = L'\\';
  path[1] = L'\\';
  path[2] = L'.';
  path[3] = L'\\';
  path[4] = DosDevice;
  path[5] = L':';
  path[6] = L'\\';
  path[7] = L'\0';
  PDISKHANDLE disk;
  disk = OpenDisk(path);
  if(disk != NULL) {
    disk->DosDevice = DosDevice;
    return disk;
  }
  return NULL;
}

BOOL CloseDisk(PDISKHANDLE disk) {
  if(disk != NULL) {
    if(disk->fileHandle > INVALID_HANDLE_VALUE)
      CloseHandle(disk->fileHandle);
    if(disk->type == NTFSDISK) {
      if(disk->NTFS.MFT != NULL)
        delete disk->NTFS.MFT;
      disk->NTFS.MFT = NULL;
      if(disk->NTFS.Bitmap != NULL)
        delete disk->NTFS.Bitmap;
      disk->NTFS.Bitmap = NULL;
    }
    if(disk->IsLong) {
      if(disk->lFiles != NULL)
        delete disk->lFiles;
      disk->lFiles = NULL;
    }
    else {
      if(disk->sFiles != NULL)
        delete disk->sFiles;
      disk->sFiles = NULL;
    }
    delete disk;
    return TRUE;
  }
  return FALSE;
};

BOOL FixFileRecord(PFILE_RECORD_HEADER file) {
  // int sec = 2048;
  PUSHORT usa = PUSHORT(PUCHAR(file) + file->Ntfs.UsaOffset);
  PUSHORT sector = PUSHORT(file);

  if(file->Ntfs.UsaCount > 4)
    return FALSE;
  for(ULONG i = 1; i < file->Ntfs.UsaCount; i++) {
    sector[255] = usa[i];
    sector += 256;
  }

  return TRUE;
}

ULONGLONG LoadMFT(PDISKHANDLE disk, BOOL complete) {
  DWORD read;
  ULARGE_INTEGER offset;
  UCHAR* buf;
  PFILE_RECORD_HEADER file;
  PNONRESIDENT_ATTRIBUTE nattr, nattr2;

  if(disk == NULL)
    return 0;

  if(disk->type == NTFSDISK) {
    offset = disk->NTFS.MFTLocation;

    SetFilePointer(
        disk->fileHandle, offset.LowPart, (PLONG)&offset.HighPart, FILE_BEGIN);
    buf = new UCHAR[disk->NTFS.BytesPerCluster];
    ReadFile(disk->fileHandle, buf, disk->NTFS.BytesPerCluster, &read, NULL);

    file = (PFILE_RECORD_HEADER)(buf);

    FixFileRecord(file);

    if(file->Ntfs.Type == 'ELIF') {
      PLONGFILEINFO data = (PLONGFILEINFO)buf;
      PATTRIBUTE attr = (PATTRIBUTE)((PUCHAR)(file) + file->AttributesOffset);
      int stop = std::min<USHORT>(8, file->NextAttributeNumber);

      data->Flags = file->Flags;

      for(int i = 0; i < stop; i++) {
        if(attr->AttributeType < 0 || attr->AttributeType > 0x100)
          break;

        switch(attr->AttributeType) {
        case AttributeList:
          // now it gets tricky
          // we have to rebuild the data attribute

          // wake down the list to find all runarrays
          // use ReadAttribute to get the list
          // I think, the right order is important

          // find out how to walk down the list !!!!

          // the only solution for now
          return 3;
          break;
        case Data:
          nattr = (PNONRESIDENT_ATTRIBUTE)attr;
          break;
        case Bitmap:
          nattr2 = (PNONRESIDENT_ATTRIBUTE)attr;
        default:
          break;
        };

        if(attr->Length > 0 && attr->Length < file->BytesInUse)
          attr = PATTRIBUTE(PUCHAR(attr) + attr->Length);
        else if(attr->Nonresident == TRUE)
          attr = PATTRIBUTE(PUCHAR(attr) + sizeof(NONRESIDENT_ATTRIBUTE));
      }
      if(nattr == NULL)
        return 0;
      if(nattr2 == NULL)
        return 0;

      disk->NTFS.sizeMFT = (DWORD)nattr->DataSize;
      disk->NTFS.MFT = buf;

      disk->NTFS.entryCount =
          disk->NTFS.sizeMFT / disk->NTFS.BytesPerFileRecord;
      return nattr->DataSize;
    }
  }
  return 0;
};

PATTRIBUTE FindAttribute(PFILE_RECORD_HEADER file, ATTRIBUTE_TYPE type) {
  PATTRIBUTE attr = (PATTRIBUTE)((PUCHAR)(file) + file->AttributesOffset);

  for(int i = 1; i < file->NextAttributeNumber; i++) {
    if(attr->AttributeType == type)
      return attr;

    if(attr->AttributeType < 1 || attr->AttributeType > 0x100)
      break;
    if(attr->Length > 0 && attr->Length < file->BytesInUse)
      attr = PATTRIBUTE(PUCHAR(attr) + attr->Length);
    else if(attr->Nonresident == TRUE)
      attr = PATTRIBUTE(PUCHAR(attr) + sizeof(NONRESIDENT_ATTRIBUTE));
  }
  return NULL;
}

ULONG RunLength(PUCHAR run) {
  // i guess it must be this way
  return (*run & 0xf) + ((*run >> 4) & 0xf) + 1;
}

LONGLONG RunLCN(PUCHAR run) {
  UCHAR n1 = *run & 0xf;
  UCHAR n2 = (*run >> 4) & 0xf;
  LONGLONG lcn = n2 == 0 ? 0 : CHAR(run[n1 + n2]);

  for(LONG i = n1 + n2 - 1; i > n1; i--)
    lcn = (lcn << 8) + run[i];
  return lcn;
}

ULONGLONG RunCount(PUCHAR run) {
  // count the runs we have to process
  UCHAR k = *run & 0xf;
  ULONGLONG count = 0;

  for(ULONG i = k; i > 0; i--)
    count = (count << 8) + run[i];

  return count;
}

BOOL FindRun(
    PNONRESIDENT_ATTRIBUTE attr,
    ULONGLONG vcn,
    PULONGLONG lcn,
    PULONGLONG count) {
  if(vcn < attr->LowVcn || vcn > attr->HighVcn)
    return FALSE;
  *lcn = 0;

  ULONGLONG base = attr->LowVcn;

  for(PUCHAR run = PUCHAR(PUCHAR(attr) + attr->RunArrayOffset); *run != 0;
      run += RunLength(run)) {
    *lcn += RunLCN(run);
    *count = RunCount(run);
    if(base <= vcn && vcn < base + *count) {
      *lcn = RunLCN(run) == 0 ? 0 : *lcn + vcn - base;
      *count -= ULONG(vcn - base);
      return TRUE;
    }
    else {
      base += *count;
    }
  }

  return FALSE;
}

DWORD inline FetchSearchInfo(
    PDISKHANDLE disk, PFILE_RECORD_HEADER file, PUCHAR buf) {
  PFILENAME_ATTRIBUTE fn;
  DWORD real_file = 0;
  PLONGFILEINFO data = (PLONGFILEINFO)buf;
  PATTRIBUTE attr = (PATTRIBUTE)((PUCHAR)(file) + file->AttributesOffset);
  int stop = std::min<USHORT>(8, file->NextAttributeNumber);

  if(file->Ntfs.Type == 'ELIF') {
    data->Flags = file->Flags;

    for(int i = 0; i < stop; i++) {
      if(attr->AttributeType < 0 || attr->AttributeType > 0x100)
        break;
      switch(attr->AttributeType) {
      case FileName:
        fn = PFILENAME_ATTRIBUTE(
            PUCHAR(attr) + PRESIDENT_ATTRIBUTE(attr)->ValueOffset);
        if(fn->NameType & WIN32_NAME || fn->NameType == 0) {
          if(file->Flags & 0x0001) {
            File& f = disk->Files[file->RecordNumber];
            f.in_use = true;
            f.name = {fn->Name, fn->NameLength};
            f.parent = fn->DirectoryFileReferenceNumber & 0x0000ffffffffffff;
            f.size = fn->DataSize;
            f.modified = fn->LastWriteTime;
            f.directory = file->Flags & 0x0002;
          }

          if(file->BaseFileRecord.LowPart != 0) {
            AddToFixList(file->BaseFileRecord.LowPart, file->RecordNumber);
          }

          real_file = 1;
        }
        break;
      case Data: {
        if(attr->Nonresident) {
          PNONRESIDENT_ATTRIBUTE nattr = PNONRESIDENT_ATTRIBUTE(attr);
          File& f = disk->Files[file->RecordNumber];
          f.size = nattr->DataSize;
          real_file = 1;
        }
      } break;
      default:
        break;
      };

      if(attr->Length > 0 && attr->Length < file->BytesInUse)
        attr = PATTRIBUTE(PUCHAR(attr) + attr->Length);
      else if(attr->Nonresident == TRUE)
        attr = PATTRIBUTE(PUCHAR(attr) + sizeof(NONRESIDENT_ATTRIBUTE));
    }
  }
  return sizeof(SEARCHFILEINFO) * real_file;
}

DWORD inline ProcessBuffer(
    PDISKHANDLE disk, PUCHAR buffer, DWORD size, FETCHPROC fetch) {
  PUCHAR end;
  PUCHAR data;
  DWORD count = 0;
  PFILE_RECORD_HEADER fh;
  end = PUCHAR(buffer) + size;
  data = PUCHAR(disk->fFiles);
  data += sizeof(SEARCHFILEINFO) * disk->filesSize;

  while(buffer < end) {
    fh = PFILE_RECORD_HEADER(buffer);
    FixFileRecord(fh);
    if(FetchSearchInfo(disk, fh, data) > 0)
      disk->realFiles++;
    buffer += disk->NTFS.BytesPerFileRecord;
    data += sizeof(SEARCHFILEINFO);
    disk->filesSize++;
  }
  return 0;
}

DWORD ReadMFTLCN(
    PDISKHANDLE disk,
    ULONGLONG lcn,
    ULONG count,
    PVOID buffer,
    FETCHPROC fetch,
    PSTATUSINFO info) {
  LARGE_INTEGER offset;
  DWORD read = 0;
  DWORD ret = 0;
  DWORD cnt = 0, c = 0, pos = 0;

  offset.QuadPart = lcn * disk->NTFS.BytesPerCluster;
  SetFilePointer(
      disk->fileHandle, offset.LowPart, &offset.HighPart, FILE_BEGIN);

  cnt = count / CLUSTERSPERREAD;

  for(DWORD i = 1; i <= cnt; i++) {
    ReadFile(
        disk->fileHandle, buffer, CLUSTERSPERREAD * disk->NTFS.BytesPerCluster,
        &read, NULL);
    c += CLUSTERSPERREAD;
    pos += read;

    ProcessBuffer(disk, (PUCHAR)buffer, read, fetch);
  }

  ReadFile(
      disk->fileHandle, buffer, (count - c) * disk->NTFS.BytesPerCluster, &read,
      NULL);
  ProcessBuffer(disk, (PUCHAR)buffer, read, fetch);
  pos += read;
  return pos;
}

DWORD ReadMFTParse(
    PDISKHANDLE disk,
    PNONRESIDENT_ATTRIBUTE attr,
    ULONGLONG vcn,
    ULONG count,
    PVOID buffer,
    FETCHPROC fetch,
    PSTATUSINFO info) {
  ULONGLONG lcn, runcount;
  ULONG readcount, left;
  DWORD ret = 0;
  PUCHAR bytes = PUCHAR(buffer);
  PUCHAR data;

  int x;
  x = (disk->NTFS.entryCount + 16) * sizeof(SEARCHFILEINFO);
  data = new UCHAR[x];
  memset(data, 0, x);
  disk->fFiles = (PSEARCHFILEINFO)data;

  for(left = count; left > 0; left -= readcount) {
    FindRun(attr, vcn, &lcn, &runcount);
    readcount = ULONG(std::min<ULONGLONG>(runcount, left));
    ULONG n = readcount * disk->NTFS.BytesPerCluster;
    if(lcn == 0) {
      // spares file?
      memset(bytes, 0, n);
    }
    else {
      ret += ReadMFTLCN(disk, lcn, readcount, buffer, fetch, info);
    }
    vcn += readcount;
    bytes += n;
  }
  return ret;
}

DWORD ParseMFT(PDISKHANDLE disk, UINT option, PSTATUSINFO info) {
  PFILE_RECORD_HEADER fh;
  // PRESIDENT_ATTRIBUTE attr;
  PNONRESIDENT_ATTRIBUTE nattr;
  // LONGFILEINFO* data;
  DWORD index = 0;

  // FETCHPROC fetch;
  // fetch = FetchFileInfo;

  if(disk == NULL)
    return 0;

  if(disk->type == NTFSDISK) {
    CreateFixList();
    fh = PFILE_RECORD_HEADER(disk->NTFS.MFT);
    FixFileRecord(fh);
    disk->IsLong = sizeof(SEARCHFILEINFO);
    nattr = (PNONRESIDENT_ATTRIBUTE)FindAttribute(fh, Data);
    DWORD ret = 0;
    if(nattr != NULL) {
      std::vector<UCHAR> buffer(CLUSTERSPERREAD * disk->NTFS.BytesPerCluster);
      ret = ReadMFTParse(
          disk, nattr, 0, ULONG(nattr->HighVcn) + 1, buffer.data(), NULL, info);
    }

    ProcessFixList(disk);
    return ret;
  }

  return 0;
}

static std::time_t to_time_t(const FILETIME& ft) {
  __int64 t =
      (static_cast<__int64>(ft.dwHighDateTime) << 32) + ft.dwLowDateTime;
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



int main() {
  boost::timer::auto_cpu_timer t;
  //auto disk = OpenDisk(L"\\\\?\\c:");
  fsdb::MftParser parser;
  parser.open(L"\\\\?\\c:");
  parser.read();
  parser.close();
  // if(!LoadMFT(disk, FALSE)) {
  //   return 1;
  // }

  // STATUSINFO status;
  // disk->Files.resize(disk->NTFS.entryCount + 16);
  // if(!ParseMFT(disk, SEARCHINFO, &status)) {
  //   return 1;
  // }

  // auto total_count = std::count_if(
  //     disk->Files.begin(), disk->Files.end(),
  //     [](auto&& f) { return f.in_use; });

  // auto total_size = std::accumulate(
  //     disk->Files.begin(), disk->Files.end(), std::size_t(0),
  //     [](std::size_t v, auto&& f) {
  //       if(f.name == L"$BadClus") {
  //         return v;
  //       }

  //       if(!f.in_use) {
  //         return v;
  //       }

  //       return v + f.size;
  //     });

  // std::cout << "test-mft found " << total_count << " files totalling "
  //           << total_size / 1024 << " KiB." << std::endl;

  // std::sort(disk->Files.begin(), disk->Files.end(), [](auto&& a, auto&& b) {
  //   if(a.in_use && b.in_use) {
  //     return a.size > b.size;
  //   }

  //   if(!a.in_use) {
  //     return false;
  //   }

  //   if(!b.in_use) {
  //     return true;
  //   }

  //   return false;
  // });

  // std::transform(
  //     disk->Files.begin(), disk->Files.begin() + 24,
  //     std::ostream_iterator<std::wstring, wchar_t>(std::wcout, L"\n"),
  //     [](auto&& f) { return f.name + L", " + std::to_wstring(f.size); });
  return 0;
}