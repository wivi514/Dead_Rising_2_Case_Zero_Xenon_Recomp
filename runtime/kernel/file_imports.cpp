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
// AND THE SAVE PATH IS A DIFFERENT SHAPE, WHICH IS WHY IT SILENTLY DID NOTHING
// ---------------------------------------------------------------------------
// A3 (the save round-trip capture) shows the whole save as ONE open and ONE write:
//
//   NtCreateFile(handleOut, 40100080, objAttrs("save:\DR2P000.DSF"), iosb, 0, 0,
//                share=0, disposition=00000005, options=00000064)
//   NtWriteFile(handle, event, apc=0, apcCtx, iosb, buffer, length=0004A000, offset=0)
//
//   40100080 = GENERIC_WRITE | SYNCHRONIZE | FILE_READ_ATTRIBUTES
//   disposition 5 = FILE_OVERWRITE_IF — create it, or truncate it if it is there
//
// Every earlier version of this file ignored `createDisposition` entirely and opened
// unconditionally with `"rb"`, and `NtWriteFile` was a generated honest-failure stub.
// So a save could not work in two independent ways at once, and NEITHER of them was
// visible: the open failed with STATUS_NO_SUCH_FILE through a not-found printer that
// stops after 32 lines, and the write failed through a stub whose only trace is the
// `[kcall]` log. The title's own report was "the save mounts and then nothing
// happens", which is exactly what those two produce together.
//
// The rule this is an instance of: an import list is not a feature list. `NtWriteFile`
// was in the import table from day one and had a stub from day one, and a stub that
// fails honestly is still a feature that does not exist (gotcha 67).
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
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "guestcall.h"
#include "heap.h"
#include "klog.h"
#include "kobject.h"
#include "memory.h"
#include "vfs.h"

namespace fs = std::filesystem;

// Both defined in imports.cpp, because the Event type and the alertable wait sites
// live there.
//
// NtReadFile/NtWriteFile signal completion through EITHER an event handle OR an APC
// routine, and this title uses the APC. Ours complete synchronously but still owe
// the notification, or the thread waiting on it never wakes and the file it wanted
// looks like a hang rather than a missing feature.
void SignalGuestEvent(uint32_t handle);
void QueueThreadApc(uint32_t routine, uint32_t context, uint32_t ioStatusBlock);

namespace {

// NTSTATUS values this layer traffics in beyond the shared set in kobject.h.
constexpr uint32_t STATUS_NO_SUCH_FILE       = 0xC000000F;
constexpr uint32_t STATUS_END_OF_FILE        = 0xC0000011;
constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND = 0xC000003A;
constexpr uint32_t STATUS_NOT_A_DIRECTORY    = 0xC0000103;
constexpr uint32_t STATUS_OBJECT_NAME_COLLISION = 0xC0000035;
constexpr uint32_t STATUS_ACCESS_DENIED      = 0xC0000022;

// IO_STATUS_BLOCK.Information for a create, which is a different value per outcome
// and not a success flag. A guest that opens with FILE_OPEN_IF learns from this
// field alone whether it got an existing file or a fresh one.
constexpr uint32_t FILE_SUPERSEDED  = 0;
constexpr uint32_t FILE_OPENED      = 1;
constexpr uint32_t FILE_CREATED     = 2;
constexpr uint32_t FILE_OVERWRITTEN = 3;

struct FileHandle final : KernelObject
{
    FILE* fp{};
    std::string guestPath;
    std::string hostPath;
    bool isDirectory = false;
    bool writable = false;
    uint64_t size = 0;

    // The seek and the read below it are TWO C-library calls, and NT's contract for a
    // positional NtReadFile is ONE atomic operation. This title's loader is an async
    // APC file system streaming one archive through one handle from several threads at
    // once, so two in-flight reads could interleave seek/read and each collect the
    // other's bytes — a coherent payload in the wrong buffer (a tanker wearing a
    // pickup's atlas), a format-mismatched one as banded garbage (part 35's striped
    // materials), and a different outcome on every reload. The mutex makes each
    // positional IO atomic again; `inFlight` measures how often it actually mattered,
    // because a fix for a race nobody hits is a comment, not a fix.
    std::mutex io;
    std::atomic<uint32_t> inFlight{0};

    ~FileHandle() override
    {
        if (fp)
            fclose(fp);
    }
};

// Overlapping positional IOs observed (reads or writes entering while another IO on
// the SAME handle is mid-flight) — the census that says whether the race above is
// real on this title. Printed on first occurrence and every 1024th after, because an
// end-of-run report does not survive the standard recipe's timeout kill (gotcha 284).
static std::atomic<uint64_t> g_fileIoOverlaps{0};

// CZ_FILE_RACY=1 — skip the per-handle IO lock: the pre-part-35 file layer, and the
// same-binary control arm for every claim about wrong or garbage streamed textures.
static bool FileRacy()
{
    static const bool racy = [] {
        const char* v = getenv("CZ_FILE_RACY");
        return v && *v && *v != '0';
    }();
    return racy;
}

static void NoteFileIoOverlap(FileHandle* file)
{
    const uint64_t n = g_fileIoOverlaps.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n == 1 || (n & 1023) == 0)
        fprintf(stderr, "[file] positional-IO OVERLAP #%llu on '%s'%s\n",
                (unsigned long long)n, file->guestPath.c_str(),
                FileRacy() ? " (CZ_FILE_RACY=1: NOT serialized)" : " (serialized)");
}

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

// ============================================================================
// NOT-FOUND ACCOUNTING — because a real miss is one line in a flood of expected ones.
//
// A no-input outdoor run asks for 304 distinct files that do not exist, and every one of
// them is legitimate: Case Zero is a cut-down Dead Rising 2, so its engine probes for the
// full title's content and falls back. `data/anim/weapon` misses 66 files and has exactly
// one on disk (`allweapons.big`), which is the fallback working. Nothing is wrong.
//
// The problem is what that does to the ONE miss that would matter. A path we should have
// resolved and did not — a mount that failed, a translation that dropped a component, an
// asset we never extracted — prints exactly the same line as the 304, so it is invisible.
// The log has always said this ("counted and logged sparsely so a WRONG not-found is
// still visible"), and sparse logging is not what makes it visible; classification is.
//
// Three classes, and only the last two are ours:
//
//   PROBE      the parent directory holds nothing, or does not exist. The title is asking
//              for content this package never shipped. Expected, counted, silent.
//   SIBLING    the parent directory EXISTS AND HAS FILES IN IT. The title is asking for a
//              sibling of things we do have, which is where a missed extraction or a
//              mistranslated name would land. Suspicious; named in the summary.
//   REGRESSED  we opened this exact path successfully earlier in the same run and now
//              cannot. That cannot be the title's content being absent, so it is a defect
//              in the VFS, the mount, or a handle. LOUD, immediately, every time.
//
// The classification runs ONCE per distinct path and is cached: the scan is a directory
// listing, and a title that probes the same missing bank every frame must not pay for it.
namespace {

std::mutex g_missMutex;
std::map<std::string, uint64_t> g_missCounts;      // guest path -> times missed
std::map<std::string, int> g_missClass;            // guest path -> 0 probe, 1 sibling
std::set<std::string> g_openedOk;                  // paths that HAVE opened this run

std::string MissKey(const std::string& guestPath)
{
    std::string k = guestPath;
    for (char& c : k)
        c = char(tolower((unsigned char)(c == '\\' ? '/' : c)));
    return k;
}

// Called on every successful open, so REGRESSED can be detected at all. Cheap: one
// insertion into a set that tops out at the few hundred paths this title opens.
void NoteOpened(const std::string& guestPath)
{
    std::lock_guard<std::mutex> lock(g_missMutex);
    g_openedOk.insert(MissKey(guestPath));
}

// Returns true if the miss is worth shouting about.
bool NoteMiss(const std::string& guestPath)
{
    const std::string key = MissKey(guestPath);
    std::lock_guard<std::mutex> lock(g_missMutex);
    ++g_missCounts[key];
    if (g_openedOk.count(key))
    {
        KLOG("[file] **REGRESSED**: '%s' opened successfully earlier in this run and now "
             "does not resolve. That is not the title probing for absent content — it is "
             "ours.\n", guestPath.c_str());
        return true;
    }
    auto known = g_missClass.find(key);
    if (known != g_missClass.end())
        return false;
    // Classify once. The parent is taken from the TRANSLATED path so this asks about the
    // host directory the file would live in, not about the guest's spelling.
    int cls = 0;
    const std::string host = VfsTranslate(guestPath);
    const size_t slash = host.find_last_of('/');
    if (!host.empty() && slash != std::string::npos)
    {
        std::error_code ec;
        const std::string dir = host.substr(0, slash);
        if (std::filesystem::is_directory(dir, ec))
            for (const auto& e : std::filesystem::directory_iterator(dir, ec))
            {
                (void)e;
                cls = 1;      // the directory exists and is not empty
                break;
            }
    }
    g_missClass[key] = cls;
    // SIBLING prints ONCE, immediately, rather than waiting for a summary at exit. Most
    // runs of this title are killed by `timeout`, so an exit-time report is a report
    // nobody receives -- and the actionable classes have to survive that.
    if (cls == 1)
        KLOG("[file] SIBLING MISS: '%s' -- its directory exists and holds files, so this "
             "is not the title probing for content we never shipped. Check the "
             "extraction and the name translation.\n", guestPath.c_str());
    return cls == 1;
}

} // namespace

void FileImports_ReportMisses()
{
    std::lock_guard<std::mutex> lock(g_missMutex);
    if (g_missCounts.empty())
        return;
    uint64_t total = 0, probeN = 0, sibN = 0, sibHits = 0;
    for (const auto& [p, n] : g_missCounts)
    {
        total += n;
        if (g_missClass[p] == 1) { ++sibN; sibHits += n; }
        else ++probeN;
    }
    fprintf(stderr,
            "[file] not-found summary: %llu opens missed over %zu distinct paths — "
            "%llu PROBE (parent empty or absent; the title asking for content this "
            "package never shipped), %llu SIBLING (%llu opens) in a directory that DOES "
            "hold files, which is where a missed extraction would land\n",
            (unsigned long long)total, g_missCounts.size(),
            (unsigned long long)probeN, (unsigned long long)sibN,
            (unsigned long long)sibHits);
    if (!sibN)
        return;
    // Only the suspicious class is listed, most-missed first. Printing all 304 is what
    // the raw log already does and is why nobody reads it.
    std::vector<std::pair<uint64_t, std::string>> rows;
    for (const auto& [p, n] : g_missCounts)
        if (g_missClass[p] == 1)
            rows.emplace_back(n, p);
    std::sort(rows.rbegin(), rows.rend());
    for (size_t i = 0; i < rows.size() && i < 24; i++)
        fprintf(stderr, "[file]     %6llu x  %s\n",
                (unsigned long long)rows[i].first, rows[i].second.c_str());
    if (rows.size() > 24)
        fprintf(stderr, "[file]     ... and %zu more\n", rows.size() - 24);
}

// The one place a host errno becomes an NTSTATUS. Kept separate so the mapping is
// visible rather than scattered — RtlNtStatusToDosError translates these onward and
// this title branches on the DOS error.
uint32_t OpenStatusFor(const std::string& guestPath)
{
    return guestPath.find('\\') != std::string::npos ? STATUS_NO_SUCH_FILE
                                                     : STATUS_OBJECT_NAME_NOT_FOUND;
}

// NT's create dispositions. The names matter more than usual here because three of
// the six create a file that does not exist and three do not, and getting that
// backwards turns "the save wrote nothing" into "the save overwrote the disc".
enum : uint32_t
{
    FILE_SUPERSEDE    = 0,   // replace if present, create if not
    FILE_OPEN         = 1,   // must exist
    FILE_CREATE       = 2,   // must NOT exist
    FILE_OPEN_IF      = 3,   // open, or create
    FILE_OVERWRITE    = 4,   // must exist, truncate
    FILE_OVERWRITE_IF = 5,   // truncate, or create   <- what this title's save uses
};

// The access bits that mean "this handle will be written through". A3's save open is
// 0x40100080; every boot-era open is 0x80100080, so this is exactly the bit that
// separates the save path from the 312 disc reads.
bool WantsWrite(uint32_t desiredAccess, uint32_t createDisposition)
{
    constexpr uint32_t GENERIC_WRITE     = 0x40000000;
    constexpr uint32_t GENERIC_ALL       = 0x10000000;
    constexpr uint32_t FILE_WRITE_DATA   = 0x00000002;
    constexpr uint32_t FILE_APPEND_DATA  = 0x00000004;
    if (desiredAccess & (GENERIC_WRITE | GENERIC_ALL | FILE_WRITE_DATA | FILE_APPEND_DATA))
        return true;
    // A disposition that can create or truncate is a write intent even if the access
    // mask is sloppy about saying so.
    return createDisposition == FILE_SUPERSEDE || createDisposition == FILE_CREATE ||
           createDisposition == FILE_OVERWRITE || createDisposition == FILE_OVERWRITE_IF;
}

// Saves are RARE and disc reads are not, so anything that is not the game disc gets
// an uncapped log line. The capped `NtCreateFile #n` printer below stops naming paths
// after 512 opens and then prints every 64th, which is right for a boot that opens
// hundreds of archives and useless for the one open a save makes thousands deep — the
// part-17 operator log could not say whether the save had opened a file at all, and
// that is a printer limit being read as a fact about the title (gotcha 109).
bool ChattyDevice(const std::string& guestPath)
{
    const size_t colon = guestPath.find(':');
    if (colon == std::string::npos)
        return true;
    std::string device = guestPath.substr(0, colon);
    for (char& c : device)
        c = char(tolower(static_cast<unsigned char>(c)));
    return device == "game" || device == "d";
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

    std::string hostPath = VfsResolveExisting(guestPath);
    std::error_code ec;
    const bool existed = !hostPath.empty();
    const bool wantsWrite = WantsWrite(desiredAccess, createDisposition);
    const bool mayCreate = createDisposition == FILE_SUPERSEDE ||
                           createDisposition == FILE_CREATE ||
                           createDisposition == FILE_OPEN_IF ||
                           createDisposition == FILE_OVERWRITE_IF;

    if (existed && createDisposition == FILE_CREATE)
        return STATUS_OBJECT_NAME_COLLISION;

    if (!existed)
    {
        if (!mayCreate)
        {
            // FILE_OPEN (1) on a file that does not exist. This is a legitimate answer,
            // not a runtime failure: A1 shows the title probing for optional files
            // (game:\data\capcom.txt) and carrying on. Counted and logged sparsely so a
            // WRONG not-found (a path we should have resolved) is still visible.
            static std::atomic<int> misses{ 0 };
            const int n = misses.fetch_add(1);
            const bool loud = NoteMiss(guestPath);
            if (loud || n < 32 || FileTrace() || !ChattyDevice(guestPath))
                KLOG("NtCreateFile('%s') -> not found\n", guestPath.c_str());
            return OpenStatusFor(guestPath);
        }
        // A creating disposition needs the path the file WOULD have, which is the
        // untranslated mapping rather than the case-insensitive existing-file scan.
        hostPath = VfsTranslate(guestPath);
        if (hostPath.empty())
        {
            KLOG("NtCreateFile('%s', disposition %u): device is not mounted, so there "
                 "is nowhere to create it\n", guestPath.c_str(), createDisposition);
            return OpenStatusFor(guestPath);
        }
        fs::create_directories(fs::path(hostPath).parent_path(), ec);
        // The lookup two lines up cached a MISS for this path, and it is about to stop
        // being true. Without this the file we are creating can never be re-opened —
        // which is the save's load half, and the self-test's first failure.
        VfsForget(guestPath);
    }

    const bool directory = existed && fs::is_directory(hostPath, ec);
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
    // Recorded so a later miss on the SAME path can be told apart from the title
    // probing for content that was never shipped. That distinction is the whole
    // value of the not-found summary.
    NoteOpened(guestPath);
    file->hostPath = hostPath;
    file->isDirectory = directory;
    file->writable = wantsWrite;

    // Truncate on the three dispositions that say so, and only then. FILE_OPEN_IF on an
    // existing file must NOT truncate it, which is the one distinction in this table
    // that silently destroys data when it is wrong.
    const bool truncate = !existed || createDisposition == FILE_SUPERSEDE ||
                          createDisposition == FILE_OVERWRITE ||
                          createDisposition == FILE_OVERWRITE_IF;
    uint32_t information = FILE_OPENED;
    if (!existed)
        information = FILE_CREATED;
    else if (createDisposition == FILE_SUPERSEDE)
        information = FILE_SUPERSEDED;
    else if (createDisposition == FILE_OVERWRITE || createDisposition == FILE_OVERWRITE_IF)
        information = FILE_OVERWRITTEN;

    if (!directory)
    {
        // Read-only is the whole boot era (A1: desiredAccess 0x80100080 on every
        // open); "w+b"/"r+b" is the save path and nothing else reaches it. The mode is
        // derived from the guest's own two arguments rather than from the device name,
        // so a title that writes somewhere else is served by the same code.
        const char* mode = !wantsWrite ? "rb" : (truncate ? "w+b" : "r+b");
        file->fp = fopen(hostPath.c_str(), mode);
        if (!file->fp)
        {
            KLOG("NtCreateFile('%s'): resolved to %s but fopen(\"%s\") failed: %s\n",
                 guestPath.c_str(), hostPath.c_str(), mode, strerror(errno));
            DestroyKernelObject(GetKernelHandle(file));
            return STATUS_UNSUCCESSFUL;
        }
        file->size = truncate ? 0 : uint64_t(fs::file_size(hostPath, ec));
    }

    const uint32_t handle = GetKernelHandle(file);
    if (handleOut)
        *handleOut = handle;
    if (iosb)
    {
        iosb->Status = STATUS_SUCCESS;
        iosb->Information = information;
    }
    if (!ChattyDevice(guestPath))
        KLOG("NtCreateFile('%s') -> handle %08X, %s, disposition %u (%s), access %08X\n",
             guestPath.c_str(), handle, wantsWrite ? "WRITABLE" : "read-only",
             createDisposition,
             information == FILE_CREATED ? "created"
                 : information == FILE_OVERWRITTEN ? "overwritten"
                 : information == FILE_SUPERSEDED ? "superseded" : "opened",
             desiredAccess);
    // 512, then every 64th — not 64.
    //
    // The old cap was 64 and a boot-to-title opens 64, so this line printed indices
    // 0..63 and fell silent EXACTLY at the depth every claim in this project quotes off
    // it ("the boot reaches prologue_z01.big"). That number was the printer's limit, not
    // the title's progress, and any change that made the boot go further would have been
    // invisible in the one column used to score it. Gotcha 109 names the trap; this is
    // the emitter it was written about. Past 512 the every-64th tail keeps depth
    // observable at a cost that does not grow with a gameplay-length run.
    const uint32_t n = g_opens.fetch_add(1);
    if (n < 512 || (n & 63) == 0 || FileTrace())
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
//
// THE COMPLETION IS AN APC, NOT AN EVENT, AND THAT IS THE WHOLE HANDSHAKE
// -----------------------------------------------------------------------
// A5's first read, on the cAsyncFileSystem thread (F8000018):
//
//   NtReadFile(F8000020, event=00000000, apc=82831B21, apcCtx=82789670,
//              iosb=E418D208, buffer=FFCA0000, length=00009000, offset=0)
//
// `event` is ZERO. Every boot-era read here is like that: this title's async file
// system delivers completion through the APC routine, and the routine's low bit is
// the kernel's own flag rather than part of the address (DrainThreadApcs masks it).
// The issuing thread then parks in an ALERTABLE wait so the APC can run —
//
//   NtSetEvent(F8000014, 0)                          <- its own queue event
//   NtWaitForSingleObjectEx(F8000014, 1, alertable=1, NULL)
//
// which is the idiom for "enter an alertable wait now".
//
// A read that fills the buffer and the IO_STATUS_BLOCK but drops the APC therefore
// looks like it worked and hangs the title anyway: the requesting thread polls a
// completion flag only the APC ever sets. That is exactly where this port's boot
// stopped, and it is the same shape as the out-parameter rule one level up — the
// contract of an async call includes its notification, not just its data.
//
// The APC belongs to the CALLING thread (t_apcQueue is thread-local) and runs at
// that thread's next alertable wait, which is NT's rule and not a convenience.
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

    if (file->inFlight.fetch_add(1, std::memory_order_acquire) > 0)
        NoteFileIoOverlap(file);
    std::unique_lock<std::mutex> ioLock(file->io, std::defer_lock);
    if (!FileRacy())
        ioLock.lock();

    // A null byteOffset means "continue from the file pointer"; NT also spells
    // "current position" as the sentinel 0xFFFFFFFFFFFFFFFE.
    if (byteOffset)
    {
        const uint64_t offset = *byteOffset;
        if (offset != 0xFFFFFFFFFFFFFFFEull)
            fseeko(file->fp, int64_t(offset), SEEK_SET);
    }

    const size_t got = fread(buffer, 1, length, file->fp);
    file->inFlight.fetch_sub(1, std::memory_order_release);
    const uint32_t status = got == 0 && length != 0 ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    if (iosb)
    {
        iosb->Status = status;
        iosb->Information = uint32_t(got);
    }
    if (FileTrace())
        // The DESTINATION address is on this line because a read that reports the
        // right byte count into the wrong place is indistinguishable from a correct
        // one otherwise — and phase A/V needed exactly that: the title reads 131,072
        // bytes of PressStartPrologue.xma while the XMA context's declared input
        // buffer stays all-zero, and only the destination says whether those are the
        // same memory.
        KLOG("NtReadFile('%s', %u bytes @ %lld) -> %zu into %08X (apc=%08X event=%08X)\n",
             file->guestPath.c_str(), length,
             byteOffset ? (long long)byteOffset->get() : -1LL, got,
             uint32_t(buffer - g_memory.base), apcRoutine, event);

    // Both notifications are owed even though the read completed synchronously.
    // Whichever the caller supplied is the one it is waiting on; this title uses the
    // APC and passes event = 0.
    SignalGuestEvent(event);
    if (apcRoutine)
        QueueThreadApc(apcRoutine, apcContext, iosb ? g_memory.MapVirtual(iosb) : 0);
    return status;
}

// The mirror of NtReadFile, and the same argument list. A3's only call is the save:
//
//   NtWriteFile(handle, event=F80002C8, apc=0, apcCtx, iosb, buffer, 0004A000, offset=0)
//
// Note which notification this one uses: the READ path passes event=0 and an APC, and
// the WRITE path passes a real event and no APC. Both are owed, and a layer that
// implemented only the one it had seen would hang the save exactly where the boot's
// reads work — so both are signalled here for the same reason they are there.
//
// fflush is not caution. The title writes its whole 303,104-byte save in this one call
// and then closes the mount; a save that reaches the C library's buffer and no further
// is a save that exists only until the process dies, which is precisely the failure
// this function was written to end, one layer down.
uint32_t NtWriteFile_x(uint32_t handle, uint32_t event, uint32_t apcRoutine,
                       uint32_t apcContext, XIO_STATUS_BLOCK* iosb, const uint8_t* buffer,
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
    if (!file->writable)
    {
        // An honest failure rather than a silent success: the handle was opened
        // read-only, so this is our open that is wrong, and saying so names the bug.
        KLOG("NtWriteFile('%s', %u bytes): handle is READ-ONLY — the open did not ask "
             "for write access\n", file->guestPath.c_str(), length);
        return STATUS_ACCESS_DENIED;
    }

    if (file->inFlight.fetch_add(1, std::memory_order_acquire) > 0)
        NoteFileIoOverlap(file);
    std::unique_lock<std::mutex> ioLock(file->io, std::defer_lock);
    if (!FileRacy())
        ioLock.lock();

    if (byteOffset)
    {
        const uint64_t offset = *byteOffset;
        if (offset != 0xFFFFFFFFFFFFFFFEull)
            fseeko(file->fp, int64_t(offset), SEEK_SET);
    }

    const size_t put = fwrite(buffer, 1, length, file->fp);
    fflush(file->fp);
    file->inFlight.fetch_sub(1, std::memory_order_release);
    const uint32_t status = put == length ? STATUS_SUCCESS : STATUS_UNSUCCESSFUL;
    const int64_t end = ftello(file->fp);
    if (end > 0 && uint64_t(end) > file->size)
        file->size = uint64_t(end);
    if (iosb)
    {
        iosb->Status = status;
        iosb->Information = uint32_t(put);
    }
    // Uncapped, because a write is rare where a read is not, and because the whole
    // question this function was added to answer is "did anything actually reach the
    // disk". A count that stops printing cannot answer it (gotcha 109).
    KLOG("NtWriteFile('%s', %u bytes @ %lld) -> %zu written, file is now %llu bytes "
         "(apc=%08X event=%08X)\n", file->guestPath.c_str(), length,
         byteOffset ? (long long)byteOffset->get() : -1LL, put,
         (unsigned long long)file->size, apcRoutine, event);
    if (put != length)
        KLOG("NtWriteFile('%s'): SHORT WRITE — %s\n", file->guestPath.c_str(),
             strerror(errno));

    SignalGuestEvent(event);
    if (apcRoutine)
        QueueThreadApc(apcRoutine, apcContext, iosb ? g_memory.MapVirtual(iosb) : 0);
    return status;
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
        // Same lock as the read/write paths: a position set that lands between another
        // thread's seek and its read moves that read to this position.
        std::unique_lock<std::mutex> ioLock(file->io, std::defer_lock);
        if (!FileRacy())
            ioLock.lock();
        fseeko(file->fp, int64_t(pos), SEEK_SET);
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

// CZ_FILE_WRITE_SELFTEST=1 — drive the create/write/read/verify round trip through the
// real entry points, at startup, and say whether it worked.
//
// THIS EXISTS BECAUSE THE FEATURE IT TESTS IS OTHERWISE UNREACHABLE FROM HERE.
// The only thing in this title that writes a file is the save, the save is reached by
// playing to a save point, and no headless recipe in this project reaches one — so
// shipping the write path without this would be shipping a prediction rather than a
// result (gotcha 67, and open-items item 9 is a list of exactly that mistake). It is
// also the answer to gotcha 30: a test that has never failed has not been shown capable
// of failing, so this one is written to fail loudly on each of the five things that can
// go wrong independently (create, mode, seek, write, read-back).
//
// It runs against its OWN device mounted on its OWN directory, which is then deleted:
// the point is to exercise the code path, not to leave state behind, and a self-test
// that wrote into `save:` could destroy a real save.
//
// The guest-memory dance is not incidental. `XOBJECT_ATTRIBUTES::Name` is an
// `xpointer`, so it is resolved against the guest base — a host-allocated attributes
// block would dereference to garbage, and the test would be testing nothing.
void FileImportsWriteSelfTest()
{
    if (!getenv("CZ_FILE_WRITE_SELFTEST"))
        return;

    const fs::path dir = fs::temp_directory_path() / "cz_file_selftest";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    VfsMountDevice("selftest", dir.string());

    const char* name = "selftest:\\roundtrip.bin";
    const size_t nameLen = strlen(name);

    // One guest block: [XOBJECT_ATTRIBUTES][XANSI_STRING][name bytes].
    auto* attrs = static_cast<XOBJECT_ATTRIBUTES*>(
        g_heap.Alloc(sizeof(XOBJECT_ATTRIBUTES) + sizeof(XANSI_STRING) + nameLen + 1));
    auto* ansi = reinterpret_cast<XANSI_STRING*>(attrs + 1);
    char* text = reinterpret_cast<char*>(ansi + 1);
    memcpy(text, name, nameLen + 1);
    ansi->Length = uint16_t(nameLen);
    ansi->MaximumLength = uint16_t(nameLen + 1);
    ansi->Buffer = text;
    attrs->RootDirectory = 0xFFFFFFFDu;   // "no root directory", as every A1 open has
    attrs->Name = ansi;
    attrs->Attributes = nullptr;

    // The payload is deliberately a pattern rather than zeros: a write that never
    // happened and a write that wrote zeros both leave a readable file, and only a
    // pattern separates them.
    constexpr uint32_t kBytes = 0x4A000;   // A3's save is exactly this size
    std::vector<uint8_t> out(kBytes), back(kBytes);
    for (uint32_t i = 0; i < kBytes; i++)
        out[i] = uint8_t(i * 31 + (i >> 8));

    int failures = 0;
    auto check = [&](bool ok, const char* what) {
        if (!ok)
        {
            failures++;
            KLOG("[selftest] FAILED: %s\n", what);
        }
    };

    be<uint32_t> handleOut{};
    XIO_STATUS_BLOCK iosb{};
    be<uint64_t> offset{};

    // Create, with A3's own arguments: GENERIC_WRITE|SYNCHRONIZE|FILE_READ_ATTRIBUTES
    // and FILE_OVERWRITE_IF on a file that does not exist.
    uint32_t st = NtCreateFile_x(&handleOut, 0x40100080, attrs, &iosb, nullptr, 0, 0,
                                 FILE_OVERWRITE_IF, 0x64);
    check(st == STATUS_SUCCESS, "NtCreateFile(FILE_OVERWRITE_IF) on a new file");
    check(iosb.Information == FILE_CREATED, "iosb.Information should say FILE_CREATED");
    const uint32_t wh = handleOut;

    st = NtWriteFile_x(wh, 0, 0, 0, &iosb, out.data(), kBytes, &offset);
    check(st == STATUS_SUCCESS, "NtWriteFile of the whole payload");
    check(iosb.Information == kBytes, "NtWriteFile should report every byte written");
    DestroyKernelObject(wh);

    // Re-open read-only through the same path the load side uses, and compare.
    st = NtCreateFile_x(&handleOut, 0x80100080, attrs, &iosb, nullptr, 0, 1, FILE_OPEN,
                        0x64);
    check(st == STATUS_SUCCESS, "NtCreateFile(FILE_OPEN) on the file just written");
    check(iosb.Information == FILE_OPENED, "iosb.Information should say FILE_OPENED");
    const uint32_t rh = handleOut;
    st = NtReadFile_x(rh, 0, 0, 0, &iosb, back.data(), kBytes, &offset);
    check(st == STATUS_SUCCESS, "NtReadFile of the whole payload");
    check(iosb.Information == kBytes, "NtReadFile should report every byte read");
    check(back == out, "the bytes read back must equal the bytes written");

    // And the negative half: a write through a READ-ONLY handle must fail rather than
    // quietly succeed, because the previous version of this layer opened every handle
    // read-only and that is precisely the failure this test is here to catch.
    st = NtWriteFile_x(rh, 0, 0, 0, &iosb, out.data(), 16, &offset);
    check(st != STATUS_SUCCESS, "NtWriteFile through a read-only handle must FAIL");
    DestroyKernelObject(rh);

    const uint64_t onDisk = fs::exists(dir / "roundtrip.bin", ec)
                                ? uint64_t(fs::file_size(dir / "roundtrip.bin", ec))
                                : 0;
    check(onDisk == kBytes, "the host file must be exactly the payload's size");

    KLOG("[selftest] file write round trip: %s (%d failures, %llu bytes on disk)\n",
         failures ? "FAILED" : "OK", failures, (unsigned long long)onDisk);
    VfsUnmountDevice("selftest");
    fs::remove_all(dir, ec);
}

GUEST_FUNCTION_HOOK(__imp__NtCreateFile, NtCreateFile_x)
GUEST_FUNCTION_HOOK(__imp__NtOpenFile, NtOpenFile_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryFullAttributesFile, NtQueryFullAttributesFile_x)
GUEST_FUNCTION_HOOK(__imp__NtReadFile, NtReadFile_x)
GUEST_FUNCTION_HOOK(__imp__NtWriteFile, NtWriteFile_x)
GUEST_FUNCTION_HOOK(__imp__NtQueryInformationFile, NtQueryInformationFile_x)
GUEST_FUNCTION_HOOK(__imp__NtSetInformationFile, NtSetInformationFile_x)
