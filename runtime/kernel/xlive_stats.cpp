// See xlive_stats.h for what this is. The guest structs below are the whole
// risk surface, and every one is asserted at the size two sources agree on:
// the guest's own wrapper (named beside each) and Xenia's SDK-derived header
// (netplay_xnet.h, fetched into XenonLive/tools/reference).

#include "xlive_stats.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "content.h" // Xam_CompleteOverlapped
#include "heap.h"
#include "klog.h"
#include "kobject.h"
#include "memory.h"
#include "xlive_glue.h"

#include <xlive/client.h>

namespace
{

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

constexpr uint32_t kErrorSuccess = 0;
constexpr uint32_t kErrorNoMoreFiles = 18;
constexpr uint32_t kErrorInvalidParameter = 87;
constexpr uint32_t kErrorInsufficientBuffer = 122; // what the guest's sizing call returns
// kErrorIoPending (997) comes from content.h.
constexpr uint32_t kErrorFunctionFailed = 1627;

constexpr uint32_t XONLINE_E_LOGON_NOT_LOGGED_ON = 0x80151802;

// X_STATS_MAX_USER_COUNT, X_STATS_MAX_ROW_COUNT, XUserMaxReadStatsSpec,
// X_USER_STATS_ATTRIBUTES_IN_SPEC — the SDK's limits, and the server's.
constexpr uint32_t kMaxUsers = 101;
constexpr uint32_t kMaxRows = 100;
constexpr uint32_t kMaxSpecs = 64;
constexpr uint32_t kMaxColumnsInSpec = 64;

// The enumerator kinds the XDK wrappers pass in r4. Case Zero links only the
// by-rank one (sub_825AA998, `li r4,1`).
constexpr uint32_t kEnumByXuid = 0;
constexpr uint32_t kEnumByRank = 1;
constexpr uint32_t kEnumByRankPerSpec = 2;
constexpr uint32_t kEnumByRating = 3;

// ---------------------------------------------------------------------------
// Guest structures — all big-endian, all asserted
// ---------------------------------------------------------------------------

#pragma pack(push, 4)

// XGI_XUSER_READ_STATS: what sub_825AA8C0 stores at r1+80 and sends with
// length 28.
struct GuestReadStats
{
    be<uint32_t> titleId;     // +0x00
    be<uint32_t> xuidCount;   // +0x04
    be<uint32_t> xuidsPtr;    // +0x08
    be<uint32_t> specCount;   // +0x0C
    be<uint32_t> specsPtr;    // +0x10
    be<uint32_t> resultsSize; // +0x14  *pcbResults, as sized by the wrapper
    be<uint32_t> resultsPtr;  // +0x18
};
static_assert(sizeof(GuestReadStats) == 0x1C, "the call site passes 28");

// X_USER_STATS_SPEC. sub_825AA8C0 walks these with `lwzu r6,136(r7)`, and
// reads the column count at +4.
struct GuestStatsSpec
{
    be<uint32_t> viewId;
    be<uint32_t> columnCount;
    be<uint16_t> columnIds[kMaxColumnsInSpec];
};
static_assert(sizeof(GuestStatsSpec) == 0x88, "lwzu r6,136(r7) says 136-byte specs");

// X_USER_STATS_READ_RESULTS, at the start of the title's buffer.
struct GuestReadResults
{
    be<uint32_t> viewCount;
    be<uint32_t> viewsPtr;
};
static_assert(sizeof(GuestReadResults) == 0x08, "X_USER_STATS_READ_RESULTS is 8");

// X_USER_STATS_VIEW. sub_8259AB70 steps these by 16 and reads +0 id, +4 total,
// +8 count, +12 rows.
struct GuestStatsView
{
    be<uint32_t> viewId;
    be<uint32_t> totalRows;
    be<uint32_t> rowCount;
    be<uint32_t> rowsPtr;
};
static_assert(sizeof(GuestStatsView) == 0x10, "addi r29,r29,16 says 16-byte views");

// X_USER_STATS_ROW. sub_82594A48 tests the rank at +8 and sub_825949E0 swaps
// rows 48 bytes at a time.
struct GuestStatsRow
{
    be<uint64_t> xuid;        // +0x00
    be<uint32_t> rank;        // +0x08
    uint32_t pad0;            // +0x0C  the SDK aligns the rating to 8
    be<int64_t> rating;       // +0x10
    char gamertag[16];        // +0x18
    be<uint32_t> columnCount; // +0x28
    be<uint32_t> columnsPtr;  // +0x2C
};
static_assert(sizeof(GuestStatsRow) == 0x30, "mulli r11,r11,48 says 48-byte rows");

// X_USER_STATS_COLUMN: the ordinal, then an X_USER_DATA at +8 — its type byte
// at +8 and its 8-byte value union at +16. The same union XUSER_PROPERTY
// carries at the same offset, with the same trap: a 32-bit member sits in the
// FIRST four bytes of it.
struct GuestStatsColumn
{
    be<uint16_t> columnId; // +0x00
    uint8_t pad0[6];
    uint8_t type;          // +0x08  X_USER_DATA_TYPE
    uint8_t pad1[7];
    uint8_t value[8];      // +0x10
};
static_assert(sizeof(GuestStatsColumn) == 0x18, "X_USER_STATS_COLUMN is 0x18");

#pragma pack(pop)

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------

xlive::Client& Live() { return xlive::Client::Instance(); }

bool g_enabled = false;
std::atomic<bool> g_running{false};
std::thread g_thread;
std::mutex g_mutex;

// A read the title is waiting on.
struct PendingRead
{
    xlive::Client::Ticket ticket = 0;
    uint32_t overlappedVa = 0;
    uint32_t resultsPtr = 0;
    uint32_t resultsSize = 0;
    // The specs, so the answer can be laid out in the order they were asked.
    std::vector<xlive::Client::StatsReadSpec> specs;
    // What the overlapped's length should say on success: 1 item for an
    // enumerate, 0 for a read.
    uint32_t lengthOnSuccess = 0;
    const char* what = "";
};
std::vector<PendingRead> g_pending;

// A stats enumerator. A KernelObject because the handle is the title's; a
// snapshot of the request because the buffer arrives later, at XEnumerate.
struct StatsEnumerator final : KernelObject
{
    xlive::Client::StatsReadRequest request;
    uint32_t bufferSize = 0;
    bool consumed = false;
};

template <typename T>
T* GuestPtr(uint32_t va)
{
    if (va == 0)
        return nullptr;
    return reinterpret_cast<T*>(g_memory.Translate(va));
}

StatsEnumerator* EnumeratorFromHandle(uint32_t handle)
{
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return nullptr;
    KernelObject* obj = GetKernelObject(handle);
    if (!KernelObjectIsIntact(obj))
        return nullptr;
    return dynamic_cast<StatsEnumerator*>(obj);
}

// ---------------------------------------------------------------------------
// The guest wrapper's arithmetic, kept as the guest wrote it
// ---------------------------------------------------------------------------

// sub_825AA8C0:
//     r10 = (xuids*52 + 16) * specs + 8
//     for each spec: r10 += spec.columnCount * xuids * 28
// and XUserCreateStatsEnumerator sizes the enumerate buffer the same way with
// rows in place of xuids (netplay's XamUserCreateStatsEnumerator has the same
// "+4 per column" it could not explain; the wrapper explains it — the SDK
// sized for alignment slack it never used).
uint32_t WrapperBufferSize(uint32_t rows, const std::vector<xlive::Client::StatsReadSpec>& specs)
{
    uint32_t size = (rows * 52 + 16) * uint32_t(specs.size()) + 8;
    for (const auto& spec : specs)
        size += uint32_t(spec.column_ids.size()) * rows * 28;
    return size;
}

// Reads the guest's specs into the library's shape. False when a count is
// out of the SDK's range.
bool ReadSpecs(const GuestStatsSpec* guest, uint32_t count,
               std::vector<xlive::Client::StatsReadSpec>& out)
{
    if (!guest || count == 0 || count > kMaxSpecs)
        return false;
    out.clear();
    for (uint32_t i = 0; i < count; i++)
    {
        const uint32_t columns = guest[i].columnCount.get();
        if (columns > kMaxColumnsInSpec)
            return false;
        xlive::Client::StatsReadSpec spec;
        spec.view_id = guest[i].viewId.get();
        for (uint32_t c = 0; c < columns; c++)
            spec.column_ids.push_back(guest[i].columnIds[c].get());
        out.push_back(std::move(spec));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Laying a result out in the title's buffer
// ---------------------------------------------------------------------------

void WriteGamertag(char (&out)[16], const std::string& tag)
{
    std::memset(out, 0, sizeof(out));
    std::strncpy(out, tag.c_str(), sizeof(out) - 1);
}

// The union at +16, by type. Strings and binaries point INTO the buffer,
// after everything else; `stringCursor` walks that tail and `stringEnd` is
// where it must stop. A string that does not fit is written as an empty one
// rather than past the end — the wrapper's sizing never allowed for strings,
// so this only ever has the 4-byte-per-row-and-column slack to use.
void WriteColumnValue(GuestStatsColumn* out, const xlive::Client::StatProperty& value,
                      uint8_t* base, uint32_t& stringCursor, uint32_t stringEnd)
{
    using Type = xlive::Client::StatProperty::Type;
    std::memset(out->value, 0, sizeof(out->value));
    out->type = uint8_t(value.type);
    switch (value.type)
    {
    case Type::Int32:
    case Type::Context:
        *reinterpret_cast<be<uint32_t>*>(out->value) = uint32_t(int32_t(value.integer));
        break;
    case Type::Int64:
    case Type::DateTime:
        *reinterpret_cast<be<int64_t>*>(out->value) = value.integer;
        break;
    case Type::Double:
        *reinterpret_cast<be<double>*>(out->value) = value.real;
        break;
    case Type::Float:
        *reinterpret_cast<be<float>*>(out->value) = float(value.real);
        break;
    case Type::Unicode:
    case Type::Binary:
    {
        // Unicode is UTF-16 with a terminator; binary is the bytes as they
        // are. Both are {size, pointer}.
        std::vector<uint8_t> bytes;
        if (value.type == Type::Unicode)
        {
            for (unsigned char ch : value.text)
            {
                // ASCII only — gamertags are, and that is the one string a
                // stat view carries in practice.
                bytes.push_back(0);
                bytes.push_back(ch < 0x80 ? ch : '?');
            }
            bytes.push_back(0);
            bytes.push_back(0);
        }
        else
        {
            bytes.assign(value.text.begin(), value.text.end());
        }
        auto* field = reinterpret_cast<be<uint32_t>*>(out->value);
        if (!bytes.empty() && stringCursor + bytes.size() <= stringEnd)
        {
            std::memcpy(base + stringCursor, bytes.data(), bytes.size());
            field[0] = uint32_t(bytes.size());
            field[1] = stringCursor;
            stringCursor += uint32_t(bytes.size());
        }
        else
        {
            field[0] = 0;
            field[1] = 0;
        }
        break;
    }
    }
}

// Lays the answer out in the title's buffer:
//
//     X_USER_STATS_READ_RESULTS
//     X_USER_STATS_VIEW[views]
//     X_USER_STATS_ROW[rows of every view]
//     X_USER_STATS_COLUMN[columns of every row]
//     string bytes
//
// and returns the Win32 status: success, or ERROR_INSUFFICIENT_BUFFER when it
// would not fit — which never happens for a buffer the guest's own wrapper
// sized, and is checked anyway.
uint32_t WriteResults(uint32_t resultsPtr, uint32_t resultsSize,
                      const std::vector<xlive::Client::StatsReadSpec>& specs,
                      const xlive::Client::StatsResult& result)
{
    uint8_t* base = static_cast<uint8_t*>(g_memory.Translate(0));
    auto* header = GuestPtr<GuestReadResults>(resultsPtr);
    if (!header || resultsSize < sizeof(GuestReadResults))
        return kErrorInsufficientBuffer;

    // One view per spec, in spec order, whether or not the server answered
    // for it: the title matches views by id and an absent one reads as empty.
    size_t rows = 0, columns = 0;
    std::vector<const xlive::Client::StatsView*> views(specs.size(), nullptr);
    for (size_t i = 0; i < specs.size(); i++)
    {
        for (const auto& view : result.views)
            if (view.view_id == specs[i].view_id)
            {
                views[i] = &view;
                break;
            }
        if (!views[i])
            continue;
        rows += views[i]->rows.size();
        for (const auto& row : views[i]->rows)
            columns += row.columns.size();
    }

    const uint32_t viewsAt = resultsPtr + uint32_t(sizeof(GuestReadResults));
    const uint32_t rowsAt = viewsAt + uint32_t(specs.size() * sizeof(GuestStatsView));
    const uint32_t columnsAt = rowsAt + uint32_t(rows * sizeof(GuestStatsRow));
    const uint32_t stringsAt = columnsAt + uint32_t(columns * sizeof(GuestStatsColumn));
    const uint32_t end = resultsPtr + resultsSize;
    if (stringsAt > end)
    {
        KLOG("[xlive] stats: %u bytes of results will not fit the title's %u\n",
             stringsAt - resultsPtr, resultsSize);
        return kErrorInsufficientBuffer;
    }

    std::memset(base + resultsPtr, 0, resultsSize);
    header->viewCount = uint32_t(specs.size());
    header->viewsPtr = viewsAt;

    uint32_t rowCursor = rowsAt;
    uint32_t columnCursor = columnsAt;
    uint32_t stringCursor = stringsAt;
    for (size_t i = 0; i < specs.size(); i++)
    {
        auto* view = GuestPtr<GuestStatsView>(viewsAt + uint32_t(i * sizeof(GuestStatsView)));
        view->viewId = specs[i].view_id;
        if (!views[i])
        {
            view->totalRows = 0;
            view->rowCount = 0;
            view->rowsPtr = 0;
            continue;
        }
        view->totalRows = views[i]->total_rows;
        view->rowCount = uint32_t(views[i]->rows.size());
        view->rowsPtr = views[i]->rows.empty() ? 0 : rowCursor;
        for (const auto& row : views[i]->rows)
        {
            auto* out = GuestPtr<GuestStatsRow>(rowCursor);
            out->xuid = row.xuid;
            out->rank = row.rank;
            out->pad0 = 0;
            out->rating = row.rating;
            WriteGamertag(out->gamertag, row.gamertag);
            out->columnCount = uint32_t(row.columns.size());
            out->columnsPtr = row.columns.empty() ? 0 : columnCursor;
            for (const auto& column : row.columns)
            {
                auto* outColumn = GuestPtr<GuestStatsColumn>(columnCursor);
                std::memset(outColumn, 0, sizeof(*outColumn));
                outColumn->columnId = column.column_id;
                WriteColumnValue(outColumn, column.value, base, stringCursor, end);
                columnCursor += uint32_t(sizeof(GuestStatsColumn));
            }
            rowCursor += uint32_t(sizeof(GuestStatsRow));
        }
    }
    return kErrorSuccess;
}

// ---------------------------------------------------------------------------
// Starting and finishing a read
// ---------------------------------------------------------------------------

// What the overlapped says when the server refused or never answered. The
// title reads the extended error and shows it; nothing here is a success it
// did not earn.
uint32_t StatusFor(const std::string& error)
{
    if (error == "signed_out" || error == "offline" || error == "no_server")
        return XONLINE_E_LOGON_NOT_LOGGED_ON;
    if (error == "bad_request" || error == "unknown_view" || error == "no_title")
        return kErrorInvalidParameter;
    return kErrorFunctionFailed;
}

// Queues a read the title supplied an overlapped for. Every call site in this
// title does; a caller without one is refused rather than blocked on, the
// same rule xlive_session.cpp's Begin() has.
uint32_t Begin(PendingRead pending, const xlive::Client::StatsReadRequest& request)
{
    if (pending.overlappedVa == 0)
    {
        KLOG("[xlive] %s was sent without an overlapped; refusing rather than "
             "blocking a guest thread\n", pending.what);
        return kErrorFunctionFailed;
    }
    pending.ticket = Live().ReadStats(request);
    if (pending.ticket == 0)
        return kErrorFunctionFailed;

    // Mark the overlapped pending BEFORE the request can complete, or a fast
    // answer would be overwritten by this and the title would wait forever.
    if (auto* overlapped = GuestPtr<be<uint32_t>>(pending.overlappedVa))
        *overlapped = kErrorIoPending;

    std::lock_guard<std::mutex> lock(g_mutex);
    g_pending.push_back(std::move(pending));
    return kErrorSuccess;
}

void Finish(const PendingRead& pending, xlive::Client::OpStatus status,
            const xlive::Client::StatsResult& result)
{
    uint32_t code = kErrorFunctionFailed;
    uint32_t length = 0;
    if (status == xlive::Client::OpStatus::Succeeded)
    {
        code = WriteResults(pending.resultsPtr, pending.resultsSize, pending.specs, result);
        if (code == kErrorSuccess)
        {
            length = pending.lengthOnSuccess;
            size_t rows = 0;
            for (const auto& view : result.views)
                rows += view.rows.size();
            KLOG("[xlive] %s: %zu view(s), %zu row(s) into %u bytes\n", pending.what,
                 result.views.size(), rows, pending.resultsSize);
        }
    }
    else if (status == xlive::Client::OpStatus::Failed)
    {
        code = StatusFor(result.error);
        KLOG("[xlive] %s failed: %s\n", pending.what, result.error.c_str());
    }
    else
    {
        // The ticket is gone and nobody took its answer. Completing with a
        // failure is the only honest move: leaving the overlapped reading
        // ERROR_IO_PENDING hangs the leaderboard screen forever.
        KLOG("[xlive] %s: ticket %llu vanished; failing the request\n", pending.what,
             (unsigned long long)pending.ticket);
    }
    Xam_CompleteOverlapped(pending.overlappedVa, code, length);
}

void CompletionThread()
{
    while (g_running.load())
    {
        std::vector<PendingRead> snapshot;
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            snapshot = g_pending;
        }
        std::vector<xlive::Client::Ticket> settled;
        for (const auto& pending : snapshot)
        {
            xlive::Client::StatsResult result;
            const auto status = Live().Poll(pending.ticket, result);
            if (status == xlive::Client::OpStatus::Pending)
                continue;
            settled.push_back(pending.ticket);
            Finish(pending, status, result);
        }
        if (!settled.empty())
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            for (auto it = g_pending.begin(); it != g_pending.end();)
            {
                bool done = false;
                for (auto ticket : settled)
                    if (it->ticket == ticket)
                        done = true;
                it = done ? g_pending.erase(it) : std::next(it);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

// ---------------------------------------------------------------------------
// XUserReadStats
// ---------------------------------------------------------------------------

uint32_t ReadStats(const GuestReadStats* msg, uint32_t overlappedVa)
{
    const uint32_t xuidCount = msg->xuidCount.get();
    const uint32_t specCount = msg->specCount.get();
    if (xuidCount == 0 || xuidCount > kMaxUsers || specCount == 0 || specCount > kMaxSpecs)
        return kErrorInvalidParameter;
    const auto* xuids = GuestPtr<be<uint64_t>>(msg->xuidsPtr.get());
    const auto* specs = GuestPtr<GuestStatsSpec>(msg->specsPtr.get());
    if (!xuids || !specs || msg->resultsPtr.get() == 0)
        return kErrorInvalidParameter;

    xlive::Client::StatsReadRequest request;
    request.title_id = msg->titleId.get();
    request.shape = xlive::Client::StatsReadRequest::Shape::Named;
    for (uint32_t i = 0; i < xuidCount; i++)
        request.xuids.push_back(xuids[i].get());
    if (!ReadSpecs(specs, specCount, request.specs))
        return kErrorInvalidParameter;

    // The buffer must hold what the wrapper sized it for; anything smaller is
    // the sizing call, which the wrapper answers itself and never sends.
    const uint32_t needed = WrapperBufferSize(xuidCount, request.specs);
    if (msg->resultsSize.get() < needed)
        return kErrorInsufficientBuffer;

    if (!Live().online())
        return XONLINE_E_LOGON_NOT_LOGGED_ON;

    PendingRead pending;
    pending.overlappedVa = overlappedVa;
    pending.resultsPtr = msg->resultsPtr.get();
    pending.resultsSize = msg->resultsSize.get();
    pending.specs = request.specs;
    pending.lengthOnSuccess = 0;
    pending.what = "XUserReadStats";
    KLOG("[xlive] XUserReadStats: %u player(s), %u view(s), %u-byte buffer\n", xuidCount,
         specCount, pending.resultsSize);
    return Begin(std::move(pending), request);
}

}  // namespace

// ---------------------------------------------------------------------------
// The public surface
// ---------------------------------------------------------------------------

bool XliveStats_Enabled() { return g_enabled; }

void XliveStats_Start()
{
    if (g_enabled)
        return;
    const char* on = std::getenv("CZ_XLIVE_ONLINE");
    if (!on || on[0] != '1')
        return;
    g_enabled = true;
    g_running.store(true);
    g_thread = std::thread(CompletionThread);
    KLOG("[xlive] leaderboards enabled: XUserReadStats and the stats enumerator are handled\n");
}

void XliveStats_Shutdown()
{
    if (!g_enabled)
        return;
    g_running.store(false);
    if (g_thread.joinable())
        g_thread.join();
    g_enabled = false;
}

bool XliveStats_Dispatch(uint32_t message, void* buffer, uint32_t bufferLength,
                         uint32_t overlappedVa, uint32_t* result)
{
    if (!g_enabled || message != 0x000B0021)
        return false;
    if (!buffer || bufferLength < sizeof(GuestReadStats))
    {
        *result = kErrorInvalidParameter;
        return true;
    }
    *result = ReadStats(static_cast<const GuestReadStats*>(buffer), overlappedVa);
    return true;
}

uint32_t XliveStats_CreateEnumerator(uint32_t titleId, uint32_t type, uint64_t pivot,
                                     uint32_t rows, uint32_t specCount, const void* specs,
                                     be<uint32_t>* sizeOut, be<uint32_t>* handleOut)
{
    // Out-parameters first, failure included: the title tests the handle.
    if (handleOut)
        *handleOut = 0;
    if (sizeOut)
        *sizeOut = 0;
    if (!handleOut || !sizeOut || !specs)
        return kErrorInvalidParameter;
    if (rows == 0 || rows > kMaxRows || specCount == 0 || specCount > kMaxSpecs)
        return kErrorInvalidParameter;

    xlive::Client::StatsReadRequest request;
    request.title_id = titleId;
    request.row_count = rows;
    if (!ReadSpecs(static_cast<const GuestStatsSpec*>(specs), specCount, request.specs))
        return kErrorInvalidParameter;

    switch (type)
    {
    case kEnumByRank:
    case kEnumByRankPerSpec:
        // The rank is the 32-bit r4 the wrapper zero-extended into r5; a
        // title that asks for rank 0 means the top.
        request.shape = xlive::Client::StatsReadRequest::Shape::ByRank;
        request.rank_start = uint32_t(pivot) ? uint32_t(pivot) : 1;
        break;
    case kEnumByXuid:
        // A page centred on a player. The server centres on the caller; for
        // anyone else the honest answer is that one player's row.
        if (pivot == CzXlive_Xuid(0) && pivot != 0)
        {
            request.shape = xlive::Client::StatsReadRequest::Shape::AroundMe;
        }
        else
        {
            request.shape = xlive::Client::StatsReadRequest::Shape::Named;
            request.xuids.push_back(pivot);
        }
        break;
    case kEnumByRating:
    default:
        KLOG("[xlive] stats enumerator kind %u is not supported\n", type);
        return kErrorInvalidParameter;
    }

    auto* obj = CreateKernelObject<StatsEnumerator>();
    if (!obj)
        return kErrorFunctionFailed;
    obj->request = std::move(request);
    obj->bufferSize = WrapperBufferSize(rows, obj->request.specs);
    *sizeOut = obj->bufferSize;
    *handleOut = GetKernelHandle(obj);
    KLOG("[xlive] stats enumerator %08X: kind %u from %llu, %u row(s), %u view(s), %u-byte buffer\n",
         handleOut->get(), type, (unsigned long long)pivot, rows, specCount, obj->bufferSize);
    return kErrorSuccess;
}

bool XliveStats_Enumerate(uint32_t handle, uint32_t buffer, uint32_t bufferLength,
                          be<uint32_t>* itemsReturned, uint32_t overlappedVa,
                          uint32_t* result)
{
    StatsEnumerator* obj = EnumeratorFromHandle(handle);
    if (!obj)
        return false;

    auto refuse = [&](uint32_t status) {
        if (itemsReturned)
            *itemsReturned = 0;
        if (overlappedVa)
        {
            Xam_CompleteOverlapped(overlappedVa, status, 0);
            *result = kErrorIoPending;
        }
        else
        {
            *result = status;
        }
        return true;
    };

    if (buffer == 0 || bufferLength < obj->bufferSize)
        return refuse(kErrorInsufficientBuffer);
    // One page per enumerator, which is how the SDK's worked too: the title
    // creates a fresh one for every refresh (sub_82598DB0).
    if (obj->consumed)
        return refuse(kErrorNoMoreFiles);
    if (!Live().online())
        return refuse(XONLINE_E_LOGON_NOT_LOGGED_ON);
    if (overlappedVa == 0)
        return refuse(kErrorFunctionFailed);

    obj->consumed = true;
    PendingRead pending;
    pending.overlappedVa = overlappedVa;
    pending.resultsPtr = buffer;
    pending.resultsSize = bufferLength;
    pending.specs = obj->request.specs;
    pending.lengthOnSuccess = 1;
    pending.what = "XEnumerate(stats)";
    const uint32_t status = Begin(std::move(pending), obj->request);
    if (status != kErrorSuccess)
        return refuse(status);
    if (itemsReturned)
        *itemsReturned = 0;
    // Accepted: the title's wrapper (sub_82594930) expects 997 and polls the
    // overlapped, which the thread completes when the page lands.
    *result = kErrorIoPending;
    return true;
}

// ---------------------------------------------------------------------------
// Self-test
// ---------------------------------------------------------------------------
//
// The half of this that is guest-struct arithmetic — sizes, offsets, the
// wrapper's buffer formula, and a result laid out into a buffer of exactly
// that size and read back the way sub_8259AB70 reads it — runs here on real
// guest memory with no server. CZ_XLIVE_STATS_TEST=1.

namespace {

int g_selfTestFailures = 0;

#define XLIVE_EXPECT(cond)                                                       \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "[xlive] SELF-TEST FAILED %s:%d: %s\n",              \
                    __FILE__, __LINE__, #cond);                                  \
            ++g_selfTestFailures;                                                \
        }                                                                        \
    } while (0)

struct GuestScratch
{
    void* host = nullptr;
    uint32_t va = 0;

    explicit GuestScratch(size_t size)
    {
        host = g_heap.Alloc(size);
        if (host)
        {
            std::memset(host, 0, size);
            va = g_memory.MapVirtual(host);
        }
    }
    ~GuestScratch()
    {
        if (host)
            g_heap.Free(host);
    }
};

void TestFieldOffsets()
{
    XLIVE_EXPECT(offsetof(GuestReadStats, resultsSize) == 0x14);
    XLIVE_EXPECT(offsetof(GuestReadStats, resultsPtr) == 0x18);
    XLIVE_EXPECT(offsetof(GuestStatsSpec, columnCount) == 4);
    XLIVE_EXPECT(offsetof(GuestStatsSpec, columnIds) == 8);
    XLIVE_EXPECT(offsetof(GuestStatsView, rowsPtr) == 12);
    XLIVE_EXPECT(offsetof(GuestStatsRow, rank) == 8);
    XLIVE_EXPECT(offsetof(GuestStatsRow, rating) == 16);
    XLIVE_EXPECT(offsetof(GuestStatsRow, gamertag) == 24);
    XLIVE_EXPECT(offsetof(GuestStatsRow, columnCount) == 40);
    XLIVE_EXPECT(offsetof(GuestStatsRow, columnsPtr) == 44);
    XLIVE_EXPECT(offsetof(GuestStatsColumn, type) == 8);
    XLIVE_EXPECT(offsetof(GuestStatsColumn, value) == 16);
}

// The wrapper's formula, against the numbers Case Zero's own leaderboard
// object produces: one spec, no columns, so 8 + 16 + 52 per player.
void TestWrapperSizing()
{
    std::vector<xlive::Client::StatsReadSpec> specs(1);
    XLIVE_EXPECT(WrapperBufferSize(1, specs) == 8 + 16 + 52);
    XLIVE_EXPECT(WrapperBufferSize(100, specs) == 8 + 16 + 5200);
    specs[0].column_ids = {0, 1, 2};
    XLIVE_EXPECT(WrapperBufferSize(2, specs) == 8 + 16 + 104 + 3 * 2 * 28);
    specs.resize(2);
    XLIVE_EXPECT(WrapperBufferSize(2, specs) == 8 + 2 * (16 + 104) + 3 * 2 * 28);
}

// A two-player result with rank, gamertag and score columns, into a buffer
// of exactly the size the wrapper would have computed, read back the way the
// title reads it.
void TestResultLayout()
{
    std::vector<xlive::Client::StatsReadSpec> specs(1);
    specs[0].view_id = 1;
    specs[0].column_ids = {0, 1, 2};
    const uint32_t size = WrapperBufferSize(2, specs);
    GuestScratch buffer(size);
    XLIVE_EXPECT(buffer.va != 0);
    if (!buffer.va)
        return;

    xlive::Client::StatsResult result;
    result.ok = true;
    xlive::Client::StatsView view;
    view.view_id = 1;
    view.total_rows = 40;
    for (int i = 0; i < 2; i++)
    {
        xlive::Client::StatsRow row;
        row.xuid = 0x0009000000000100ull + uint64_t(i);
        row.rank = uint32_t(7 + i);
        row.rating = 48200 - i * 100;
        row.gamertag = i == 0 ? "Frank West" : "Chuck Greene";
        for (uint16_t c = 0; c < 3; c++)
        {
            xlive::Client::StatsColumn column;
            column.column_id = c;
            if (c == 0)
            {
                column.value.id = 0x10008001;
                column.value.type = xlive::Client::StatProperty::Type::Int32;
                column.value.integer = row.rank;
            }
            else if (c == 1)
            {
                column.value.id = 0x40008002;
                column.value.type = xlive::Client::StatProperty::Type::Unicode;
                column.value.text = row.gamertag;
            }
            else
            {
                column.value.id = 0x20000004;
                column.value.type = xlive::Client::StatProperty::Type::Int64;
                column.value.integer = row.rating;
            }
            row.columns.push_back(std::move(column));
        }
        view.rows.push_back(std::move(row));
    }
    result.views.push_back(std::move(view));

    XLIVE_EXPECT(WriteResults(buffer.va, size, specs, result) == kErrorSuccess);

    // sub_8259AB70: header, views, the view whose id matches the spec.
    const auto* header = GuestPtr<GuestReadResults>(buffer.va);
    XLIVE_EXPECT(header->viewCount.get() == 1);
    const auto* views = GuestPtr<GuestStatsView>(header->viewsPtr.get());
    XLIVE_EXPECT(views && views[0].viewId.get() == 1);
    XLIVE_EXPECT(views && views[0].totalRows.get() == 40);
    XLIVE_EXPECT(views && views[0].rowCount.get() == 2);
    const auto* rows = views ? GuestPtr<GuestStatsRow>(views[0].rowsPtr.get()) : nullptr;
    XLIVE_EXPECT(rows != nullptr);
    if (!rows)
        return;
    XLIVE_EXPECT(rows[0].rank.get() == 7);
    XLIVE_EXPECT(rows[1].rank.get() == 8);
    XLIVE_EXPECT(rows[0].rating.get() == 48200);
    XLIVE_EXPECT(rows[1].xuid.get() == 0x0009000000000101ull);
    XLIVE_EXPECT(std::strcmp(rows[1].gamertag, "Chuck Greene") == 0);
    XLIVE_EXPECT(rows[0].columnCount.get() == 3);
    const auto* columns = GuestPtr<GuestStatsColumn>(rows[0].columnsPtr.get());
    XLIVE_EXPECT(columns != nullptr);
    if (!columns)
        return;
    // The 32-bit rank sits in the FIRST four bytes of the union.
    XLIVE_EXPECT(columns[0].type == 1);
    XLIVE_EXPECT(reinterpret_cast<const be<uint32_t>*>(columns[0].value)->get() == 7);
    XLIVE_EXPECT(columns[2].type == 2);
    XLIVE_EXPECT(reinterpret_cast<const be<int64_t>*>(columns[2].value)->get() == 48200);
    // The gamertag column found room in the slack and is UTF-16 with a
    // terminator: 11 characters -> 24 bytes.
    XLIVE_EXPECT(columns[1].type == 4);
    const auto* text = reinterpret_cast<const be<uint32_t>*>(columns[1].value);
    XLIVE_EXPECT(text[0].get() == 22);
    const auto* utf16 = GuestPtr<be<uint16_t>>(text[1].get());
    XLIVE_EXPECT(utf16 && utf16[0].get() == 'F' && utf16[9].get() == 't' && utf16[10].get() == 0);

    // Every byte of the answer is inside the buffer the title gave.
    const uint32_t last = text[1].get() + text[0].get();
    XLIVE_EXPECT(last <= buffer.va + size);

    // A buffer one byte too small for the rows is refused, not overrun.
    GuestScratch small(8 + 16 + 48 * 2 - 1);
    if (small.va)
        XLIVE_EXPECT(WriteResults(small.va, 8 + 16 + 48 * 2 - 1, specs, result) ==
                     kErrorInsufficientBuffer);
}

// The enumerator's create call: the size it reports is the wrapper's, the
// handle is a live kernel object, and a second page is "no more files".
void TestEnumeratorCreate()
{
    GuestScratch spec(sizeof(GuestStatsSpec));
    GuestScratch out(8);
    if (!spec.va || !out.va)
        return;
    auto* guestSpec = GuestPtr<GuestStatsSpec>(spec.va);
    guestSpec->viewId = 1;
    guestSpec->columnCount = 0;
    auto* sizeOut = GuestPtr<be<uint32_t>>(out.va);
    auto* handleOut = GuestPtr<be<uint32_t>>(out.va + 4);

    XLIVE_EXPECT(XliveStats_CreateEnumerator(0x58410B00, kEnumByRank, 1, 20, 1, guestSpec, sizeOut,
                                             handleOut) == kErrorSuccess);
    XLIVE_EXPECT(sizeOut->get() == 8 + 16 + 20 * 52);
    XLIVE_EXPECT(handleOut->get() != 0);
    StatsEnumerator* obj = EnumeratorFromHandle(handleOut->get());
    XLIVE_EXPECT(obj != nullptr);
    if (obj)
    {
        XLIVE_EXPECT(obj->request.shape == xlive::Client::StatsReadRequest::Shape::ByRank);
        XLIVE_EXPECT(obj->request.rank_start == 1);
        XLIVE_EXPECT(obj->request.row_count == 20);
    }

    // Offline, an enumerate is refused through the overlapped with the logon
    // error, and the wrapper still sees 997 — the protocol it expects.
    GuestScratch page(sizeOut->get());
    GuestScratch overlapped(28);
    uint32_t result = 0;
    XLIVE_EXPECT(XliveStats_Enumerate(handleOut->get(), page.va, sizeOut->get(), nullptr,
                                      overlapped.va, &result));
    XLIVE_EXPECT(result == kErrorIoPending);
    XLIVE_EXPECT(GuestPtr<be<uint32_t>>(overlapped.va)->get() == XONLINE_E_LOGON_NOT_LOGGED_ON);

    // Bad arguments are refused before anything is created.
    XLIVE_EXPECT(XliveStats_CreateEnumerator(0x58410B00, kEnumByRank, 1, 0, 1, guestSpec, sizeOut,
                                             handleOut) == kErrorInvalidParameter);
    XLIVE_EXPECT(handleOut->get() == 0);
    XLIVE_EXPECT(XliveStats_CreateEnumerator(0x58410B00, kEnumByRating, 1, 20, 1, guestSpec,
                                             sizeOut, handleOut) == kErrorInvalidParameter);

    // And a handle that is not ours is not claimed.
    XLIVE_EXPECT(!XliveStats_Enumerate(0xDEADBEEF, page.va, sizeOut->get(), nullptr, 0, &result));
}

// XUserReadStats offline: refused with the logon error, synchronously, after
// the arguments are checked.
void TestReadStatsOffline()
{
    GuestScratch msg(sizeof(GuestReadStats));
    GuestScratch spec(sizeof(GuestStatsSpec));
    GuestScratch xuids(16);
    GuestScratch results(8 + 16 + 2 * 52);
    if (!msg.va || !spec.va || !xuids.va || !results.va)
        return;
    auto* guestSpec = GuestPtr<GuestStatsSpec>(spec.va);
    guestSpec->viewId = 1;
    auto* guestMsg = GuestPtr<GuestReadStats>(msg.va);
    guestMsg->titleId = 0x58410B00;
    guestMsg->xuidCount = 2;
    guestMsg->xuidsPtr = xuids.va;
    guestMsg->specCount = 1;
    guestMsg->specsPtr = spec.va;
    guestMsg->resultsSize = 8 + 16 + 2 * 52;
    guestMsg->resultsPtr = results.va;

    uint32_t result = 0;
    XLIVE_EXPECT(XliveStats_Dispatch(0x000B0021, guestMsg, sizeof(GuestReadStats), 0x1000, &result));
    XLIVE_EXPECT(result == XONLINE_E_LOGON_NOT_LOGGED_ON);

    // Too small a buffer is the sizing call, which the wrapper never sends;
    // one that arrives anyway is refused the way the wrapper would have.
    guestMsg->resultsSize = 8 + 16 + 52;
    XLIVE_EXPECT(XliveStats_Dispatch(0x000B0021, guestMsg, sizeof(GuestReadStats), 0x1000, &result));
    XLIVE_EXPECT(result == kErrorInsufficientBuffer);

    // Not ours.
    XLIVE_EXPECT(!XliveStats_Dispatch(0x000B0025, guestMsg, sizeof(GuestReadStats), 0x1000, &result));
}

// The other half: the same two calls the title makes, against the real
// server, once the gateway is up. The title's own leaderboard screen is a
// menu this runtime cannot reach headless, so this is what proves the port's
// path — message in, thread, overlapped out, buffer laid out — with a server
// on the other end. It runs on a thread of its own because the gateway is
// not up yet when CzXlive_Start returns, and it waits for it.
//
// It reads this title's own board by rank and by name, with the account's
// own XUID — a read that must always answer, with a rank-0 row for an account
// that has never posted a score. Title id 0 in the message means the title
// the library was started for, so this file is the same in both ports; the
// view is the one each title's players see (docs/leaderboards.md).
constexpr uint32_t kBoardView = 4; // Case Zero: the view ArcadeInfo.xml names

void OnlineReadTest()
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!Live().online() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (!Live().online())
    {
        fprintf(stderr, "[xlive] stats self-test (online): no gateway within 20s, skipped\n");
        return;
    }
    int failures = 0;
#define XLIVE_EXPECT_ONLINE(cond)                                                \
    do {                                                                         \
        if (!(cond)) {                                                           \
            fprintf(stderr, "[xlive] SELF-TEST FAILED %s:%d: %s\n",              \
                    __FILE__, __LINE__, #cond);                                  \
            ++failures;                                                          \
        }                                                                        \
    } while (0)

    auto waitOverlapped = [](uint32_t overlappedVa) {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(15);
        auto* status = GuestPtr<be<uint32_t>>(overlappedVa);
        while (status->get() == kErrorIoPending && std::chrono::steady_clock::now() < until)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        return status->get();
    };

    const uint32_t titleId = 0;
    GuestScratch spec(sizeof(GuestStatsSpec));
    if (!spec.va)
        return;
    auto* guestSpec = GuestPtr<GuestStatsSpec>(spec.va);
    guestSpec->viewId = kBoardView;
    guestSpec->columnCount = 0;

    // --- by rank, the way sub_82598DB0 + sub_82594930 do it ---------------
    {
        GuestScratch out(8);
        auto* sizeOut = GuestPtr<be<uint32_t>>(out.va);
        auto* handleOut = GuestPtr<be<uint32_t>>(out.va + 4);
        XLIVE_EXPECT_ONLINE(XliveStats_CreateEnumerator(titleId, kEnumByRank, 1, 20, 1, guestSpec,
                                                        sizeOut, handleOut) == kErrorSuccess);
        GuestScratch page(sizeOut->get());
        GuestScratch overlapped(28);
        uint32_t result = 0;
        XLIVE_EXPECT_ONLINE(XliveStats_Enumerate(handleOut->get(), page.va, sizeOut->get(),
                                                 nullptr, overlapped.va, &result));
        XLIVE_EXPECT_ONLINE(result == kErrorIoPending);
        const uint32_t status = waitOverlapped(overlapped.va);
        XLIVE_EXPECT_ONLINE(status == kErrorSuccess);
        if (status == kErrorSuccess)
        {
            const auto* header = GuestPtr<GuestReadResults>(page.va);
            const auto* views = GuestPtr<GuestStatsView>(header->viewsPtr.get());
            XLIVE_EXPECT_ONLINE(header->viewCount.get() == 1);
            XLIVE_EXPECT_ONLINE(views && views[0].viewId.get() == kBoardView);
            if (views)
            {
                fprintf(stderr, "[xlive] stats self-test (online): view %u by rank: %u of %u row(s)\n",
                        kBoardView, views[0].rowCount.get(), views[0].totalRows.get());
                const auto* rows = GuestPtr<GuestStatsRow>(views[0].rowsPtr.get());
                for (uint32_t i = 0; rows && i < views[0].rowCount.get(); i++)
                {
                    XLIVE_EXPECT_ONLINE(rows[i].rank.get() == i + 1);
                    fprintf(stderr, "[xlive]   #%u %-16s %lld\n", rows[i].rank.get(),
                            rows[i].gamertag, (long long)rows[i].rating.get());
                }
            }
        }
        // A second page from the same enumerator is the end of it.
        XLIVE_EXPECT_ONLINE(XliveStats_Enumerate(handleOut->get(), page.va, sizeOut->get(),
                                                 nullptr, overlapped.va, &result));
        XLIVE_EXPECT_ONLINE(GuestPtr<be<uint32_t>>(overlapped.va)->get() == kErrorNoMoreFiles);
    }

    // --- by name, the way sub_825947C8 does it ------------------------------
    {
        GuestScratch msg(sizeof(GuestReadStats));
        GuestScratch xuids(8);
        const uint32_t size = 8 + 16 + 52;
        GuestScratch results(size);
        GuestScratch overlapped(28);
        *GuestPtr<be<uint64_t>>(xuids.va) = CzXlive_Xuid(0);
        auto* guestMsg = GuestPtr<GuestReadStats>(msg.va);
        guestMsg->titleId = titleId;
        guestMsg->xuidCount = 1;
        guestMsg->xuidsPtr = xuids.va;
        guestMsg->specCount = 1;
        guestMsg->specsPtr = spec.va;
        guestMsg->resultsSize = size;
        guestMsg->resultsPtr = results.va;
        uint32_t result = 0;
        XLIVE_EXPECT_ONLINE(XliveStats_Dispatch(0x000B0021, guestMsg, sizeof(GuestReadStats),
                                                overlapped.va, &result));
        XLIVE_EXPECT_ONLINE(result == kErrorSuccess);
        const uint32_t status = waitOverlapped(overlapped.va);
        XLIVE_EXPECT_ONLINE(status == kErrorSuccess);
        if (status == kErrorSuccess)
        {
            const auto* header = GuestPtr<GuestReadResults>(results.va);
            const auto* views = GuestPtr<GuestStatsView>(header->viewsPtr.get());
            XLIVE_EXPECT_ONLINE(views && views[0].rowCount.get() == 1);
            const auto* rows = views ? GuestPtr<GuestStatsRow>(views[0].rowsPtr.get()) : nullptr;
            XLIVE_EXPECT_ONLINE(rows && rows[0].xuid.get() == CzXlive_Xuid(0));
            if (rows)
                fprintf(stderr, "[xlive] stats self-test (online): my row: rank %u, %s, %lld\n",
                        rows[0].rank.get(), rows[0].gamertag, (long long)rows[0].rating.get());
        }
    }
#undef XLIVE_EXPECT_ONLINE
    if (failures == 0)
        fprintf(stderr, "[xlive] stats self-test (online): the read path answers\n");
    else
        fprintf(stderr, "[xlive] stats self-test (online): %d FAILURE(S)\n", failures);
}

}  // namespace

void XliveStats_SelfTest()
{
    const char* on = std::getenv("CZ_XLIVE_STATS_TEST");
    if (!on || on[0] != '1')
        return;
    if (!g_enabled)
    {
        fprintf(stderr, "[xlive] stats self-test skipped: CZ_XLIVE_ONLINE is not set\n");
        return;
    }
    if (Live().online())
    {
        fprintf(stderr, "[xlive] stats self-test skipped: signed in to Live\n");
        return;
    }
    g_selfTestFailures = 0;
    TestFieldOffsets();
    TestWrapperSizing();
    TestResultLayout();
    TestEnumeratorCreate();
    TestReadStatsOffline();
    if (g_selfTestFailures == 0)
        fprintf(stderr, "[xlive] stats self-test: the guest ABI is intact\n");
    else
        fprintf(stderr, "[xlive] stats self-test: %d FAILURE(S)\n", g_selfTestFailures);

    // And the online half, once there is a gateway to read from.
    std::thread(OnlineReadTest).detach();
}
