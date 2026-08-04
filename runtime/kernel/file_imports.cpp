// The Nt* file-I/O imports, over kernel/vfs.h.
//
// Separated from imports.cpp because the file layer has its own kernel object (the
// open handle) and its own out-parameter contract, and because phase 2 will grow it
// substantially — the `.big` archive work and the seek-order oracle both land here.
//
// WHAT A1 SHOWS THIS TITLE DOING
// ------------------------------
// Every boot-era open has the same shape:
//
//   NtCreateFile(handleOut, 80100080, objAttrs("game:\data\preload4.big"), iosb,
//                allocSize=0, attributes=00000080, shareAccess=1, disposition=1,
//                options=00000068)
//
//   80100080 = GENERIC_READ | SYNCHRONIZE     0x80 = FILE_ATTRIBUTE_NORMAL
//   share 1  = FILE_SHARE_READ                disposition 1 = FILE_OPEN (must exist)
//   options 0x68 = FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE |
//                  FILE_NO_INTERMEDIATE_BUFFERING
//
// Read-only, synchronous, existing files. 312 opens across the boot. There is no
// write path in the boot era at all — the save path (finding 12) is phase 8.
//
// THE OUT-PARAMETER RULE APPLIES HARDEST HERE
// -------------------------------------------
// Asura's Wrath's finding 14: an error return only protects you against a guest
// that checks the return. Every function below writes its handle and its
// IO_STATUS_BLOCK on every path, including failures — a caller that ignores the
// status and reads the buffer gets a coherent "0 bytes, failed", not stack garbage
// it will treat as a file length.
//
// NtReadFile is `kHighFrequency` in Xenia, so it is INVISIBLE in A1 and every other
// level-3 capture (finding 2). A5 is the read oracle. Do not conclude from A1 that
// this title does not read files.
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>

#include "guestcall.h"
#include "heap.h"
#include "klog.h"
#include "kobject.h"
#include "memory.h"
#include "vfs.h"

namespace fs = std::filesystem;

// Signalled by imports.cpp; NtReadFile/NtWriteFile take an optional event handle
// that NT signals on completion. Ours complete synchronously but still owe the
// signal, or an IO thread parked on that event never wakes and the file it wanted
// looks like a hang rather than a missing feature.
void SignalGuestEvent(uint32_t handle);

namespace {

// NTSTATUS values this layer traffics in beyond the shared set in kobject.h.
constexpr uint32_t STATUS_NO_SUCH_FILE       = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE        = 0xC0000011;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_NOT_A_DIRECTORY    = 0xC0000103;

struct FileHandle final : KernelObject
{
    FILE* fp{};
    std::string guestPath;
    std::string hostPath;
    bool isDirectory = false;
    uint64_t size = 0;

    ~FileHandle() override
    {
        if (fp)
            fclose(fp);
    }
};

std::atomic<uint32_t> g_opens{ 0 };

bool FileTrace()
{
    static const bool on = getenv("CZ_FILE_TRACE") != nullptr;
    return on;
}

// Pull the path out of an OBJECT_ATTRIBUTES. Xenia prints these as
// `701BFD08(FFFFFFFD,game:\layout.bin,00000040)` — {RootDirectory, Name, Attributes}
// — and the 0xFFFFFFFD root is "no root directory", which is every boot-era open.
std::string ObjectPath(XOBJECT_ATTRIBUTES* attrs)
{
    if (!attrs || !attrs->Name)
        return {};
    XANSI_STRING* name = attrs->Name;
    if (!name->Buffer)
        return {};
    return std::string(name->Buffer.get(), name->Length);
}

FileHandle* Resolve(uint32_t handle)
{
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return nullptr;
    return dynamic_cast<FileHandle*>(GetKernelObject(handle));
}

// The one place a host errno becomes an NTSTATUS. Kept separate so the mapping is
// visible rather than scattered — RtlNtStatusToDosError translates these onward and
// this title branches on the DOS error.
uint32_t OpenStatusFor(const std::string& guestPath)
{
    return guestPath.find('\\') != std::string::npos ? STATUS_NO_SUCH_FILE
                                                     : STATUS_OBJECT_NAME_NOT_FOUND;
}

uint32_t NtCreateFile_x(be<uint32_t>* handleOut, uint32_t desiredAccess,
                        XOBJECT_ATTRIBUTES* attrs, XIO_STATUS_BLOCK* iosb,
                        be<uint64_t>* allocationSize, uint32_t fileAttributes,
                        uint32_t shareAccess, uint32_t createDisposition,
                        uint32_t createOptions)
{
    // Fill both out-parameters up front, so every early return below is already
    // honest about them (finding 14).
    if (handleOut)
        *handleOut = 0;
    if (iosb)
    {
        iosb->Status = STATUS_NO_SUCH_FILE;
        iosb->Information = 0;
    }

    const std::string guestPath = ObjectPath(attrs);
    if (guestPath.empty())
        return STATUS_INVALID_PARAMETER;

    const std::string hostPath = VfsResolveExisting(guestPath);
    std::error_code ec;
    if (hostPath.empty())
    {
        // FILE_OPEN (1) on a file that does not exist. This is a legitimate answer,
        // not a runtime failure: A1 shows the title probing for optional files
        // (game:\data\capcom.txt) and carrying on. Counted and logged sparsely so a
        // WRONG not-found (a path we should have resolved) is still visible.
        static std::atomic<int> misses{ 0 };
        const int n = misses.fetch_add(1);
        if (n < 32 || FileTrace())
            KLOG("NtCreateFile('%s') -> not found\n", guestPath.c_str());
        return OpenStatusFor(guestPath);
    }

    const bool directory = fs::is_directory(hostPath, ec);
    constexpr uint32_t FILE_DIRECTORY_FILE     = 0x00000001;
    constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
    if (directory && (createOptions & FILE_NON_DIRECTORY_FILE))
        return STATUS_NOT_A_DIRECTORY;
    if (!directory && (createOptions & FILE_DIRECTORY_FILE))
        return STATUS_OBJECT_PATH_NOT_FOUND;

    auto* file = CreateKernelObject<FileHandle>();
    if (!file)
        return STATUS_NO_MEMORY;
    file->guestPath = guestPath;
    file->hostPath = hostPath;
    file->isDirectory = directory;

    if (!directory)
    {
        // Read-only is the whole boot era (A1: desiredAccess 0x80100080 on every
        // open). A write path opens later for saves; when it does, it belongs here
        // with its own evidence rather than as a speculative "rb+" today.
        file->fp = fopen(hostPath.c_str(), "rb");
        if (!file->fp)
        {
            KLOG("NtCreateFile('%s'): resolved to %s but fopen failed: %s\n",
                 guestPath.c_str(), hostPath.c_str(), strerror(errno));
            DestroyKernelObject(GetKernelHandle(file));
            return STATUS_UNSUCCESSFUL;
        }
        file->size = fs::file_size(hostPath, ec);
    }

    const uint32_t handle = GetKernelHandle(file);
    if (handleOut)
        *handleOut = handle;
    if (iosb)
    {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = 1; // FILE_OPENED
    }
    const uint32_t n = g_opens.fetch_add(1);
    if (n < 64 || FileTrace())
        KLOG("NtCreateFile #%u '%s' -> handle %08X (%llu bytes%s)\n", n, guestPath.c_str(),
             handle, (unsigned long long)file->size, directory ? ", directory" : "");
    return STATUS_SUCCESS;
}

// NtOpenFile is NtCreateFile without the creation arguments; the boot never calls
// it, but the image imports it and routing it through the same code is strictly
// better than a stub that returns an error where a handle is expected.
uint32_t NtOpenFile_x(be<uint32_t>* handleOut, uint32_t desiredAccess,
                      XOBJECT_ATTRIBUTES* attrs, XIO_STATUS_BLOCK* iosb,
                      uint32_t shareAccess, uint32_t openOptions)
{
    return NtCreateFile_x(handleOut, desiredAccess, attrs, iosb, nullptr, 0x80, shareAccess,
                          1 /* FILE_OPEN */, openOptions);
}

// FILE_NETWORK_OPEN_INFORMATION: five 64-bit times/sizes then the attribute dword.
// A1: NtQueryFullAttributesFile("game:\data\serial.bin", 701BFC70) — the title
// stat()s a file before deciding whether to open it, so a wrong answer here changes
// which files it opens, not just how fast.
uint32_t NtQueryFullAttributesFile_x(XOBJECT_ATTRIBUTES* attrs, be<uint32_t>* info)
{
    if (info)
        memset(info, 0, 0x38);

    const std::string guestPath = ObjectPath(attrs);
    if (guestPath.empty())
        return STATUS_INVALID_PARAMETER;
    const std::string hostPath = VfsResolveExisting(guestPath);
    if (hostPath.empty())
    {
        if (FileTrace())
            KLOG("NtQueryFullAttributesFile('%s') -> not found\n", guestPath.c_str());
        return OpenStatusFor(guestPath);
    }
    if (!info)
        return STATUS_INVALID_PARAMETER;

    std::error_code ec;
    const bool directory = fs::is_directory(hostPath, ec);
    const uint64_t size = directory ? 0 : uint64_t(fs::file_size(hostPath, ec));

    // Offsets in dwords: [0..1] creation, [2..3] last access, [4..5] last write,
    // [6..7] change, [8..9] allocation size, [10..11] end of file, [12] attributes.
    auto put64 = [&](size_t dwordIndex, uint64_t v) {
        info[dwordIndex] = uint32_t(v >> 32);
        info[dwordIndex + 1] = uint32_t(v);
    };
    put64(8, (size + 0x7FF) & ~uint64_t(0x7FF)); // allocation: 2 KB sectors, as on disc
    put64(10, size);
    info[12] = directory ? 0x10 /* FILE_ATTRIBUTE_DIRECTORY */
                         : 0x20 /* FILE_ATTRIBUTE_ARCHIVE */;
    return STATUS_SUCCESS;
}

// (handle, event, apcRoutine, apcContext, iosb, buffer, length, byteOffset).
// kHighFrequency in Xenia — invisible in A1, visible in A5 (finding 2).
uint32_t NtReadFile_x(uint32_t handle, uint32_t event, uint32_t apcRoutine,
                      uint32_t apcContext, XIO_STATUS_BLOCK* iosb, uint8_t* buffer,
                      uint32_t length, be<uint64_t>* byteOffset)
{
    if (iosb)
    {
        iosb->Status = STATUS_UNSUCCESSFUL;
        iosb->Information = 0;
    }
    FileHandle* file = Resolve(handle);
    if (!file || !file->fp)
        return STATUS_INVALID_HANDLE;
    if (!buffer)
        return STATUS_INVALID_PARAMETER;

    // A null byteOffset means "continue from the file pointer"; NT also spells
    // "current position" as the sentinel 0xFFFFFFFFFFFFFFFE.
    if (byteOffset)
    {
        const uint64_t offset = *byteOffset;
        if (offset != 0xFFFFFFFFFFFFFFFEull)
            fseeko(file->fp, off_t(offset), SEEK_SET);
    }

    const size_t got = fread(buffer, 1, length, file->fp);
    if (iosb)
    {
        iosb->Status = got == 0 && length != 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS;
        iosb->Information = uint32_t(got);
    }
    if (FileTrace())
        KLOG("NtReadFile('%s', %u bytes @ %lld) -> %zu\n", file->guestPath.c_str(), length,
             byteOffset ? (long long)byteOffset->get() : -1LL, got);

    // Owed even though the read completed synchronously.
    SignalGuestEvent(event);
    return got == 0 && length != 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS;
}

// A1: NtQueryInformationFile(F80000E0, iosb, buffer, 0x38, 0x22) and
// (F80001D0, iosb, buffer, 0x08, 0x0E) — classes 0x22 and 0x0E, sizes 0x38 and 8.
// 0x0E is FilePositionInformation (one 64-bit offset); 0x22 is
// FileNetworkOpenInformation, the same 0x38-byte block NtQueryFullAttributesFile
// fills.
uint32_t NtQueryInformationFile_x(uint32_t handle, XIO_STATUS_BLOCK* iosb,
                                  be<uint32_t>* info, uint32_t length,
                                  uint32_t infoClass)
{
    if (iosb)
    {
        iosb->Status = STATUS_UNSUCCESSFUL;
        iosb->Information = 0;
    }
    FileHandle* file = Resolve(handle);
    if (!file)
        return STATUS_INVALID_HANDLE;
    if (!info || length == 0)
        return STATUS_INVALID_PARAMETER;
    memset(info, 0, length);

    auto put64 = [&](size_t dwordIndex, uint64_t v) {
        info[dwordIndex] = uint32_t(v >> 32);
        info[dwordIndex + 1] = uint32_t(v);
    };

    switch (infoClass)
    {
        case 0x0E: // FilePositionInformation
        {
            const uint64_t pos = file->fp ? uint64_t(ftello(file->fp)) : 0;
            put64(0, pos);
            break;
        }
        case 0x05: // FileStandardInformation: {alloc, eof, links, delete, directory}
            put64(0, (file->size + 0x7FF) & ~uint64_t(0x7FF));
            put64(2, file->size);
            info[4] = 1;
            reinterpret_cast<uint8_t*>(info)[20] = 0;
            reinterpret_cast<uint8_t*>(info)[21] = file->isDirectory ? 1 : 0;
            break;
        case 0x22: // FileNetworkOpenInformation — same block as the attributes query
            put64(8, (file->size + 0x7FF) & ~uint64_t(0x7FF));
            put64(10, file->size);
            info[12] = file->isDirectory ? 0x10 : 0x20;
            break;
        default:
            // Not guessed at. An unknown class gets a zeroed buffer and an error, so
            // a guest that ignores the status reads zeros rather than stack garbage,
            // and the log names the class so it can be implemented from evidence.
            KLOG("NtQueryInformationFile: unhandled class 0x%X on '%s' (len %u)\n",
                 infoClass, file->guestPath.c_str(), length);
            return STATUS_NOT_IMPLEMENTED;
    }

    if (iosb)
    {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = length;
    }
    return STATUS_SUCCESS;
}

uint32_t NtSetInformationFile_x(uint32_t handle, XIO_STATUS_BLOCK* iosb, be<uint32_t>* info,
                                uint32_t length, uint32_t infoClass)
{
    if (iosb)
    {
        iosb->Status = STATUS_UNSUCCESSFUL;
        iosb->Information = 0;
    }
    FileHandle* file = Resolve(handle);
    if (!file)
        return STATUS_INVALID_HANDLE;

    if (infoClass == 0x0E && info && length >= 8 && file->fp) // FilePositionInformation
    {
        const uint64_t pos = (uint64_t(info[0]) << 32) | info[1];
        fseeko(file->fp, off_t(pos), SEEK_SET);
        if (iosb)
        {
            iosb->Status = STATUS_SUCCESS;
            iosb->Information = length;
        }
        return STATUS_SUCCESS;
    }

    KLOG("NtSetInformationFile: unhandled class 0x%X on '%s' (len %u)\n", infoClass,
         file->guestPath.c_str(), length);
    return STATUS_NOT_IMPLEMENTED;
}

} // namespace

GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile_x)
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile_x)
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile_x)
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile_x)
