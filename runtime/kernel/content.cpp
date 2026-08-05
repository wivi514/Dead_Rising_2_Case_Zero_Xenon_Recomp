// The content (save-data) layer.
//
// This is A1 gate positions 86-92, and it is the first subsystem in this port whose
// entire interface was recovered from the GUEST rather than from a capture — because
// the capture structurally cannot show it. Worth reading before changing anything
// here, and worth lifting wholesale for Case West, which is the same engine.
//
// WHY THE CAPTURE IS NOT ENOUGH
// -----------------------------
// Xenia's log prints an import's arguments on entry and never its return value
// (finding 29), and every interesting question here is about a return: what shape is
// the "private enum structure", what does the enumerate step hand back, what does the
// title compare it against. A1 answers none of those.
//
// The title answers all of them, because it contains the XDK's own statically-linked
// XamEnumerate wrapper. `sub_825D9460` is that wrapper, and reading it end to end
// gives the whole protocol:
//
//   1. It rejects a buffer size that is not 0x138 (`cmplwi cr6,r5,0x138`), so ONE
//      ENUMERATED ITEM IS 312 BYTES. Its caller at sub_825D90C0 agrees from the other
//      side: after creating the enumerator it writes `li r11,0x138; stw r11,0(r28)`
//      into its own caller's item-size out-parameter.
//   2. It calls XamGetPrivateEnumStructureFromHandle(handle, &priv), then checks
//      `lwz r10,4(priv); cmplw r10, 0x0002000E`. So THE PRIVATE STRUCTURE CARRIES THE
//      XAM MESSAGE ID AT +4, and this enumerator's message is 0x0002000E.
//   3. It builds a 20-byte argument block { priv, buffer, 0x138, itemsOut, overlapped }
//      and hands it to sub_825D91E0 together with the callback `sub_825D9358` — which
//      is exactly the `XamTaskSchedule(825D9358, ...)` A1 logs.
//   4. `sub_825D9358` is the task body, and it is the specification for our message
//      handler. It assembles a 32-dword message buffer:
//           +0  = [priv+0x0C]        (a value we choose; see kPrivEnumId below)
//           +8  = &priv[0x18]        (the address of a sub-block inside priv)
//           +12 = a 0x200-byte stack scratch buffer
//           +16 = 0x200
//           +20 = [priv+0x10]
//           +24 = &outLength
//      calls `XMsgInProcessCall([priv+0], [priv+4], msg, 0)` — so PRIV+0 IS THE APP ID
//      and PRIV+4 THE MESSAGE ID — and then, if the call returned a NON-NEGATIVE
//      value, copies 0x134 bytes from the scratch to the caller's item and the dword
//      at scratch+0x140 to item+0x134. 0x134 + 4 = 0x138, closing the circle with (1).
//   5. On a negative return it skips the copy entirely and completes the overlapped
//      with `XMsgCompleteIORequest(ovl, r11 & 0x65B, hresult, outLength)`. A1's
//      `XMsgCompleteIORequest(7018F3C0, 0000065B, 80070012, 00000000)` is that line
//      executing with hresult = 0x80070012, i.e. HRESULT_FROM_WIN32(ERROR_NO_MORE_FILES).
//
// So: **our handler for (0xFE, 0x0002000E) must return 0x80070012 when the
// enumeration is exhausted, and 0 with the scratch filled when it is not.** Nothing
// about that was guessed, and nothing about it is in the capture.
//
// The private structure's own layout is OURS to choose, which is the pleasant
// consequence of the guest only ever reading +0, +4, +0x0C, +0x10 and the address
// +0x18 and handing them straight back to us. It is defined below so that a memory
// dump is legible, not because the SDK says so.
//
// WHAT IS MEASURED AND WHAT IS NOT
// --------------------------------
// A1's boot enumerates with ZERO saves present and Xenia says so in as many words:
// `XamContentAggregateCreateEnumerator: added 0 items to enumerator`. That path — the
// one the boot actually takes — is fully exercised here.
//
// The item CONTENT path is not, and cannot be until the runtime reaches gameplay and
// writes a save. Its layout is derived from the guest's own copy sizes above and from
// A3 (the save round-trip capture: root name "save", mounts to \Device\Content\N\,
// one file DR2P000.DSF of exactly 303,104 bytes written in a single NtWriteFile), but
// derived is not run (gotcha 67).
//
// RETRACTED IN PLACE: this comment used to end "the dword at scratch+0x140 is neither
// derived nor guessable, so we write 0 and say so". It is the TITLE ID, it is derivable,
// and writing 0 was not a neutral choice — see WriteAggregateTail below. Saying "we do
// not know, so we wrote a zero" about a field the guest FILTERS on is how a subsystem
// ends up silently returning nothing.
#include "content.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "../cpu/guest_thread.h"
#include "guestcall.h"
#include "heap.h"
#include "klog.h"
#include "kobject.h"
#include "memory.h"
#include "vfs.h"
#include "xex_imports.h" // XexTitleId — the enumeration is filtered on it

namespace {

// Win32 codes this layer traffics in. ERROR_NO_MORE_FILES is the important one: it is
// what "the enumeration is finished" means, and A1 shows the title turning it into
// 0x80070012 and completing its overlapped with it.
constexpr uint32_t ERROR_NO_MORE_FILES     = 18;
constexpr uint32_t ERROR_INVALID_PARAMETER = 87;
constexpr uint32_t ERROR_FILE_NOT_FOUND    = 2;
constexpr uint32_t ERROR_ALREADY_EXISTS    = 183;
constexpr uint32_t HRESULT_NO_MORE_FILES   = 0x80070012; // HRESULT_FROM_WIN32 of the above
constexpr uint32_t E_FAIL                  = 0x80004005;

constexpr uint32_t kAppUser         = 0xFE;       // the app id A1 logs for the enumerate
constexpr uint32_t kMsgEnumerate    = 0x0002000E; // and its message
constexpr uint32_t kEnumItemSize    = 0x138;      // 312 — sub_825D9460 rejects anything else
constexpr uint32_t kContentDataSize = 0x134;      // 308 — the part sub_825D9358 memcpys
constexpr uint32_t kAggregateTail   = 0x140;      // where it reads the item's last dword

// XCONTENT_DATA as the title moves it: 0x134 bytes, and every offset in it is fixed by
// the 0x134 memcpy plus the fact that the display name is a 128-character UTF-16
// string (the console's limit) and the file name a 42-byte ASCII one.
#pragma pack(push, 1)
struct GuestXContentData
{
    be<uint32_t> deviceId;          // +0x000
    be<uint32_t> contentType;       // +0x004
    be<uint16_t> displayName[128];  // +0x008  UTF-16, big-endian
    char         fileName[42];      // +0x108
    uint8_t      padding[2];        // +0x132
};
#pragma pack(pop)
static_assert(sizeof(GuestXContentData) == kContentDataSize,
              "the guest memcpys exactly 0x134 bytes of this");

// The private enum structure. Only the marked fields are read by the guest; the rest
// exists so that a memory dump of one of these is readable.
#pragma pack(push, 1)
struct GuestPrivateEnum
{
    be<uint32_t> appId;        // +0x00  READ: passed as XMsgInProcessCall's app
    be<uint32_t> messageId;    // +0x04  READ and CHECKED against 0x0002000E
    be<uint32_t> reserved08;   // +0x08
    be<uint32_t> enumId;       // +0x0C  READ: forwarded as message word 0 — our key
    be<uint32_t> cursorHint;   // +0x10  READ: forwarded as message word 5
    be<uint32_t> reserved14;   // +0x14
    // +0x18 onward: the guest passes the ADDRESS of this, never its contents. It holds
    // the search this enumerator was created for, purely so the block documents itself.
    be<uint64_t> xuid;         // +0x18
    be<uint32_t> deviceId;     // +0x20
    be<uint32_t> contentType;  // +0x24
    be<uint32_t> flags;        // +0x28
    be<uint32_t> itemCount;    // +0x2C
};
#pragma pack(pop)
static_assert(sizeof(GuestPrivateEnum) == 0x30, "keep the documented offsets");

// The 32-byte message block sub_825D9358 builds. Field names come from what the guest
// stores into each slot, not from an SDK header.
#pragma pack(push, 1)
struct GuestEnumMessage
{
    be<uint32_t> enumId;        // +0x00  = [priv+0x0C]
    be<uint32_t> zero04;        // +0x04
    be<uint32_t> searchBlock;   // +0x08  = &priv[0x18]
    be<uint32_t> scratch;       // +0x0C  the 0x200-byte output buffer
    be<uint32_t> scratchSize;   // +0x10  0x200
    be<uint32_t> cursorHint;    // +0x14  = [priv+0x10]
    be<uint32_t> lengthOut;     // +0x18  guest address of the length slot
    be<uint32_t> zero1C;        // +0x1C
};
#pragma pack(pop)
static_assert(sizeof(GuestEnumMessage) == 32, "sub_825D9358 zeroes and fills 32 bytes");

// ---------------------------------------------------------------------------
// The host side of a save
// ---------------------------------------------------------------------------
//
// One content item is one directory under the save root, named by the content's own
// fileName. `save:` is then mounted at that directory, so the guest's
// `NtCreateFile("save:\DR2P000.DSF")` (A3) lands on
// <saveroot>/<fileName>/DR2P000.DSF. Nothing here invents a container format: the
// title writes one 303,104-byte blob and we store exactly that blob.
std::filesystem::path g_saveRoot;

struct ContentItem
{
    std::string fileName;
    std::string displayName;
    uint32_t    deviceId = 1;
    uint32_t    contentType = 1;
    // The title that owns this save. NOT decoration: the enumeration is FILTERED on
    // it by the guest — see WriteAggregateTail.
    uint32_t    titleId = 0;
};

// A file name from the guest, made safe to put on a host filesystem. The guest's is
// 42 bytes, NUL-padded, and is a save slot name rather than user input — but it
// reaches the host as a path component, so it is checked rather than trusted.
std::string SanitizeFileName(const char* raw, size_t maxLen)
{
    std::string out;
    for (size_t i = 0; i < maxLen && raw[i]; i++)
    {
        const unsigned char c = static_cast<unsigned char>(raw[i]);
        out.push_back((c < 0x20 || c == '/' || c == '\\' || c == ':') ? '_' : char(c));
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    if (out.empty() || out == "." || out == "..")
        out = "content";
    return out;
}

std::vector<ContentItem> ScanSaves(uint32_t deviceId, uint32_t contentType)
{
    std::vector<ContentItem> items;
    std::error_code ec;
    if (g_saveRoot.empty() || !std::filesystem::is_directory(g_saveRoot, ec))
        return items;
    for (const auto& entry : std::filesystem::directory_iterator(g_saveRoot, ec))
    {
        if (!entry.is_directory(ec))
            continue;
        ContentItem item;
        item.fileName = entry.path().filename().string();
        item.displayName = item.fileName;
        item.deviceId = deviceId ? deviceId : 1;
        item.contentType = contentType ? contentType : 1;
        // Every save this runtime stores belongs to the title that is running. A save
        // directory copied in from the full Dead Rising 2 would carry ITS id on a real
        // console — and the title has a whole accept-list for exactly that case
        // (sub_825D8EC0 walks a table of additional ids, which is how DR2 and Case Zero
        // share progress) — but we have no container metadata to read one out of, so
        // claiming anything other than our own id would be an invention.
        item.titleId = XexTitleId();
        items.push_back(std::move(item));
    }
    // Directory order is filesystem order, which is not stable across machines. A save
    // list the player sees in a different order on every boot is a bug, and sorting is
    // the whole fix.
    std::sort(items.begin(), items.end(),
              [](const ContentItem& a, const ContentItem& b) { return a.fileName < b.fileName; });
    return items;
}

void WriteContentData(GuestXContentData* out, const ContentItem& item)
{
    memset(out, 0, sizeof(*out));
    out->deviceId = item.deviceId;
    out->contentType = item.contentType;
    for (size_t i = 0; i < item.displayName.size() && i < 127; i++)
        out->displayName[i] = uint16_t(static_cast<unsigned char>(item.displayName[i]));
    strncpy(out->fileName, item.fileName.c_str(), sizeof(out->fileName) - 1);
}

// The dword at scratch+0x140, which sub_825D9358 copies to the item's +0x134 — the
// 4 bytes that make an item 0x138 rather than 0x134.
//
// IT IS THE TITLE ID, and finding that out is the difference between an enumerator
// that works and one that silently returns nothing. The chain:
//   sub_825D9358 (the enumerate task) does `lwz r3,0x1c0(r1)` — the scratch buffer is
//   at r1+0x80, so that is scratch+0x140 — and passes it to sub_825D8E60, which calls
//   XamGetExecutionId and compares `[execInfo+12]` against it. +12 of
//   XEX_HEADER_EXECUTION_INFO is the title id.
// If they differ the task consults sub_825D8EC0, which walks a table of ADDITIONAL
// accepted ids (that is how the full Dead Rising 2 and Case Zero share saves), and if
// that also says no, the task loops and pulls the NEXT item. So an item with the wrong
// value here is not rejected loudly — it is skipped, and an enumeration of nothing but
// wrong-id items is indistinguishable from an empty one.
//
// This was written as 0 first, which is exactly that failure: a save was enumerated,
// the title filtered it out, and the run ended in ERROR_NO_MORE_FILES looking correct.
void WriteAggregateTail(uint8_t* scratch, const ContentItem& item)
{
    *reinterpret_cast<be<uint32_t>*>(scratch + kAggregateTail) = item.titleId;
}

// ---------------------------------------------------------------------------
// The enumerator object
// ---------------------------------------------------------------------------
//
// A KernelObject because the title closes it with NtClose (A1: `NtClose(F8000228)`
// right after the enumeration finishes), and NtClose in this runtime destroys kernel
// objects by their guest address.
struct ContentEnumerator final : KernelObject
{
    std::vector<ContentItem> items;
    size_t                   cursor = 0;
    uint32_t                 privAddr = 0; // guest address of the GuestPrivateEnum

    ~ContentEnumerator() override;
};

std::mutex g_enumMutex;
// enumId (which is the priv guest address) -> the object. The guest hands the enumId
// back to us inside the message and nothing else identifies the enumeration, so this
// map IS the protocol's state.
std::map<uint32_t, ContentEnumerator*> g_enumerators;

ContentEnumerator::~ContentEnumerator()
{
    std::lock_guard lock(g_enumMutex);
    if (privAddr)
    {
        g_enumerators.erase(privAddr);
        g_heap.Free(g_memory.Translate(privAddr));
    }
}

// Create the enumerator and its private structure. Shared by the aggregate form
// (resolved by ordinal) and the imported per-device form, because the only difference
// between them is which saves are in scope — and with one local user and one save
// device on this host, that is no difference at all. Said out loud rather than left
// implicit: if a second device ever exists, this is the function that has to split.
uint32_t CreateEnumerator(uint64_t xuid, uint32_t deviceId, uint32_t contentType,
                          uint32_t flags, be<uint32_t>* handleOut)
{
    if (!handleOut)
        return ERROR_INVALID_PARAMETER;
    *handleOut = 0; // finding 14: fill the out-parameter before any failure return

    void* privHost = g_heap.Alloc(sizeof(GuestPrivateEnum));
    if (!privHost)
        return E_FAIL;
    const uint32_t privAddr = g_memory.MapVirtual(privHost);

    auto* obj = CreateKernelObject<ContentEnumerator>();
    if (!obj)
    {
        g_heap.Free(privHost);
        return E_FAIL;
    }
    obj->items = ScanSaves(deviceId, contentType);
    obj->privAddr = privAddr;

    auto* priv = static_cast<GuestPrivateEnum*>(privHost);
    memset(priv, 0, sizeof(*priv));
    priv->appId = kAppUser;
    priv->messageId = kMsgEnumerate; // the guest CHECKS this one
    priv->enumId = privAddr;         // our key, round-tripped through the message
    priv->cursorHint = 0;
    priv->xuid = xuid;
    priv->deviceId = deviceId;
    priv->contentType = contentType;
    priv->flags = flags;
    priv->itemCount = uint32_t(obj->items.size());

    {
        std::lock_guard lock(g_enumMutex);
        g_enumerators[privAddr] = obj;
    }

    *handleOut = GetKernelHandle(obj);
    KLOG("content enumerator %08X: xuid=%016llX device=%u type=%u flags=%08X -> %zu item(s)"
         " (priv %08X)\n",
         handleOut->get(), (unsigned long long)xuid, deviceId, contentType, flags,
         obj->items.size(), privAddr);
    return 0;
}

// Take the next item, or say the enumeration is finished. `dataOut` may be null when
// the caller only wants the count moved on.
bool NextItem(ContentEnumerator* obj, ContentItem* out)
{
    std::lock_guard lock(g_enumMutex);
    if (!obj || obj->cursor >= obj->items.size())
        return false;
    if (out)
        *out = obj->items[obj->cursor];
    obj->cursor++;
    return true;
}

ContentEnumerator* EnumeratorFromId(uint32_t enumId)
{
    std::lock_guard lock(g_enumMutex);
    auto it = g_enumerators.find(enumId);
    return it == g_enumerators.end() ? nullptr : it->second;
}

ContentEnumerator* EnumeratorFromHandle(uint32_t handle)
{
    if (!IsKernelObject(handle) || !IsLiveKernelHandle(handle))
        return nullptr;
    return GetKernelObject<ContentEnumerator>(handle);
}

// ---------------------------------------------------------------------------
// The mounted content
// ---------------------------------------------------------------------------

std::mutex                          g_mountMutex;
std::map<std::string, std::string>  g_mounts; // root name -> host directory

} // namespace

// ---------------------------------------------------------------------------
// Exports
// ---------------------------------------------------------------------------

void ContentSetRootFromGameDir(const std::string& gameDir)
{
    // Deliberately NOT inside the package directory. assets/game/ is what
    // tools/extract_stfs.py produced out of a copyrighted container and is treated as
    // read-only throughout this project; writing saves into it would mean a save could
    // be lost by re-running the extractor, and would put player data inside a tree the
    // gitignore describes as "the package as delivered".
    if (const char* env = getenv("CZ_SAVE_DIR"))
        g_saveRoot = env;
    else
        g_saveRoot = std::filesystem::path(gameDir).parent_path() / "save";
    std::error_code ec;
    std::filesystem::create_directories(g_saveRoot, ec);
    KLOG("content: saves live in %s%s\n", g_saveRoot.string().c_str(),
         ec ? " (COULD NOT BE CREATED — saving will fail)" : "");
}

// XamContentAggregateCreateEnumerator — xam ordinal 0x279, resolved dynamically.
//
// Written against the raw context rather than through GUEST_FUNCTION_HOOK because it
// is not an import: there is no `__imp__` symbol for it, the guest reaches it through
// a minted thunk, and its first argument is a 64-bit XUID in a single register.
// sub_825D90C0 is the call site:
//     ld   r3, 0x50(r1)   ; the XUID from XamUserGetXUID
//     mr   r4, r27        ; device id      (A1: 1)
//     mr   r5, r31        ; content type   (A1: 1)
//     li   r6, 0          ; flags          (A1: 0)
//     mr   r7, r26        ; handle out
//     bctrl
// and it does not test the return before storing the item size, so success must be 0.
static void XamContentAggregateCreateEnumerator_x(PPCContext& ctx, uint8_t* base)
{
    KCALL("XamContentAggregateCreateEnumerator");
    auto* handleOut =
        ctx.r7.u32 ? reinterpret_cast<be<uint32_t>*>(base + ctx.r7.u32) : nullptr;
    ctx.r3.u64 =
        CreateEnumerator(ctx.r3.u64, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, handleOut);
}

PPCFunc* ContentMintedExportForOrdinal(uint32_t ordinal)
{
    return ordinal == 0x279 ? &XamContentAggregateCreateEnumerator_x : nullptr;
}

// The per-device form, which IS an import. Same work, one fewer argument.
static uint32_t XamContentCreateEnumerator_x(uint32_t userIndex, uint32_t deviceId,
                                             uint32_t contentType, uint32_t contentFlags,
                                             uint32_t itemsPerEnumerate,
                                             be<uint32_t>* handleOut)
{
    (void)userIndex;
    (void)itemsPerEnumerate; // we return one item per step; see NextItem
    return CreateEnumerator(0, deviceId, contentType, contentFlags, handleOut);
}

// XamGetPrivateEnumStructureFromHandle(handle, out) -> the block whose +0/+4 the
// guest reads to find out which XAM app and message drive this enumerator.
//
// A1 shows it called TWICE with the same handle and the second call's pre-value being
// what the first wrote, so it is idempotent and does not consume anything. The guest
// releases the result with ObDereferenceObject, which is a no-op in this runtime.
static uint32_t XamGetPrivateEnumStructureFromHandle_x(uint32_t handle, be<uint32_t>* out)
{
    if (!out)
        return ERROR_INVALID_PARAMETER;
    *out = 0;
    ContentEnumerator* obj = EnumeratorFromHandle(handle);
    if (!obj)
        return ERROR_INVALID_PARAMETER;
    *out = obj->privAddr;
    return 0;
}

// The enumeration step. See this file's header comment for the derivation; the short
// version is that a non-negative return means "the scratch holds an item" and a
// negative one means "stop", and the title turns 0x80070012 into the
// ERROR_NO_MORE_FILES its own caller expects.
bool ContentDispatchAppMessage(uint32_t app, uint32_t message, void* buffer,
                               uint32_t bufferLength, uint32_t* result)
{
    if (app != kAppUser || message != kMsgEnumerate)
        return false;
    if (!result)
        return false;
    if (!buffer || (bufferLength != 0 && bufferLength < sizeof(GuestEnumMessage)))
    {
        // bufferLength is 0 for the in-process form (XMsgInProcessCall passes no
        // length), which is the form this path actually uses.
        *result = E_FAIL;
        return true;
    }

    const auto* msg = static_cast<const GuestEnumMessage*>(buffer);
    ContentEnumerator* obj = EnumeratorFromId(msg->enumId.get());
    if (!obj)
    {
        KLOG("enumerate message for unknown enumerator %08X\n", msg->enumId.get());
        *result = E_FAIL;
        return true;
    }

    // The length slot is an out-parameter on EVERY path, including this one — the
    // guest passes it straight to XMsgCompleteIORequest, and A1's zero-item completion
    // shows it as 0.
    if (const uint32_t lengthOut = msg->lengthOut.get())
        *reinterpret_cast<be<uint32_t>*>(g_memory.Translate(lengthOut)) = 0;

    ContentItem item;
    if (!NextItem(obj, &item))
    {
        *result = HRESULT_NO_MORE_FILES;
        return true;
    }

    const uint32_t scratch = msg->scratch.get();
    const uint32_t scratchSize = msg->scratchSize.get();
    if (!scratch || scratchSize < kAggregateTail + 4)
    {
        *result = E_FAIL;
        return true;
    }
    uint8_t* out = static_cast<uint8_t*>(g_memory.Translate(scratch));
    memset(out, 0, scratchSize);
    WriteContentData(reinterpret_cast<GuestXContentData*>(out), item);
    WriteAggregateTail(out, item);

    KLOG("enumerate: item '%s' (device %u type %u)\n", item.fileName.c_str(),
         item.deviceId, item.contentType);
    *result = 0;
    return true;
}

// The imported XamEnumerate. The title mostly uses its own statically-linked wrapper
// (sub_825D9460, the one this file's protocol was read off), which reaches us through
// the message above; this import is the other door into the same enumerator, so it
// does the work directly rather than round-tripping a message it would only have to
// decode again.
static uint32_t XamEnumerate_x(uint32_t handle, uint32_t flags, uint32_t buffer,
                               uint32_t bufferLength, be<uint32_t>* itemsReturned,
                               uint32_t overlapped)
{
    (void)flags;
    (void)overlapped; // the caller supplies none on this title's only call path
    if (itemsReturned)
        *itemsReturned = 0;
    ContentEnumerator* obj = EnumeratorFromHandle(handle);
    if (!obj || !buffer)
        return ERROR_INVALID_PARAMETER;
    if (bufferLength < kEnumItemSize)
        return ERROR_INVALID_PARAMETER;

    ContentItem item;
    if (!NextItem(obj, &item))
        return ERROR_NO_MORE_FILES;

    uint8_t* out = static_cast<uint8_t*>(g_memory.Translate(buffer));
    memset(out, 0, kEnumItemSize);
    WriteContentData(reinterpret_cast<GuestXContentData*>(out), item);
    // The item's own last dword, at +0x134 rather than the scratch's +0x140: this
    // path hands the caller the FINISHED 0x138-byte item, where the message path
    // hands back a scratch the guest reassembles. Same field, two offsets, and
    // getting it wrong here would be invisible for the same reason.
    *reinterpret_cast<be<uint32_t>*>(out + kContentDataSize) = item.titleId;
    if (itemsReturned)
        *itemsReturned = 1;
    return 0;
}

// XamContentCreateEx(user, rootName, contentData, flags, dispositionOut,
//                    licenseMaskOut, cacheSize, contentSize, overlapped)
//
// Nine arguments, so the ninth arrives in the caller's parameter save area rather
// than a register. Written against the raw context for that reason — gpu/vd.cpp made
// the same call for VdSwap, and the reason is the same: the marshaller's spill path
// has never been exercised by this port and a save-data seam is the wrong place to
// find out that it is wrong.
//
// A3 is the ground truth for the shape:
//   XamContentCreateEx(00000000, 8209089C(save), 7018F540, 00001012, 0, 0, 0, 0, E42389D0)
//   -> Registered symbolic link: save: => \Device\Content\1\
//   -> NtCreateFile(save:\DR2P000.DSF), NtWriteFile(length=0x0004A000)
// so the export's whole job is to make `save:` mean a directory. The low nibble of
// `flags` is the disposition, in the CreateFile sense.
PPC_FUNC(__imp__XamContentCreateEx)
{
    KCALL("XamContentCreateEx");

    const uint32_t rootNamePtr = ctx.r4.u32;
    const uint32_t dataPtr = ctx.r5.u32;
    const uint32_t flags = ctx.r6.u32;
    const uint32_t dispositionPtr = ctx.r7.u32;
    const uint32_t licenseMaskPtr = ctx.r8.u32;

    if (!rootNamePtr || !dataPtr)
    {
        ctx.r3.u64 = ERROR_INVALID_PARAMETER;
        return;
    }

    const char* rootName = reinterpret_cast<const char*>(base + rootNamePtr);
    const auto* data = reinterpret_cast<const GuestXContentData*>(base + dataPtr);
    const std::string name = SanitizeFileName(data->fileName, sizeof(data->fileName));
    const std::filesystem::path dir = g_saveRoot / name;

    std::error_code ec;
    const bool exists = std::filesystem::is_directory(dir, ec);
    const uint32_t disposition = flags & 0xF;
    constexpr uint32_t kCreateNew = 1, kCreateAlways = 2, kOpenExisting = 3,
                       kOpenAlways = 4, kTruncateExisting = 5;

    uint32_t status = 0;
    switch (disposition)
    {
        case kCreateNew:
            if (exists)
                status = ERROR_ALREADY_EXISTS;
            break;
        case kOpenExisting:
        case kTruncateExisting:
            if (!exists)
                status = ERROR_FILE_NOT_FOUND;
            break;
        case kCreateAlways:
        case kOpenAlways:
        default:
            break;
    }

    if (status == 0 && !exists)
        std::filesystem::create_directories(dir, ec);

    // Out-parameters on every path, failure included (gotcha 5's corollary). The
    // disposition tells the caller which of "created" and "opened" happened.
    if (dispositionPtr)
        *reinterpret_cast<be<uint32_t>*>(base + dispositionPtr) = exists ? 2u : 1u;
    if (licenseMaskPtr)
        *reinterpret_cast<be<uint32_t>*>(base + licenseMaskPtr) = 1u; // full game, finding 1

    if (status == 0)
    {
        std::lock_guard lock(g_mountMutex);
        VfsMountDevice(rootName, dir.string());
        g_mounts[rootName] = dir.string();
        KLOG("XamContentCreateEx('%s', content '%s', flags %08X) -> mounted at %s\n",
             rootName, name.c_str(), flags, dir.string().c_str());
    }
    else
    {
        KLOG("XamContentCreateEx('%s', content '%s', flags %08X) -> %u\n", rootName,
             name.c_str(), flags, status);
    }
    ctx.r3.u64 = status;
}

// XamContentClose(rootName, overlapped) — A3: `(8209089C(save), 00000000)`, and Xenia
// answers by unregistering both the symlink and the device. Unmounting is the whole
// job; the files stay on disk, which is the point of a save.
static uint32_t XamContentClose_x(uint32_t rootNamePtr, uint32_t overlapped)
{
    (void)overlapped;
    if (!rootNamePtr)
        return ERROR_INVALID_PARAMETER;
    const char* rootName = reinterpret_cast<const char*>(g_memory.Translate(rootNamePtr));
    std::lock_guard lock(g_mountMutex);
    auto it = g_mounts.find(rootName);
    if (it == g_mounts.end())
        return ERROR_FILE_NOT_FOUND;
    VfsUnmountDevice(rootName);
    g_mounts.erase(it);
    KLOG("XamContentClose('%s') -> unmounted\n", rootName);
    return 0;
}

GUEST_FUNCTION_HOOK(__imp__XamContentCreateEnumerator, XamContentCreateEnumerator_x)
GUEST_FUNCTION_HOOK(__imp__XamGetPrivateEnumStructureFromHandle,
                    XamGetPrivateEnumStructureFromHandle_x)
GUEST_FUNCTION_HOOK(__imp__XamEnumerate, XamEnumerate_x)
GUEST_FUNCTION_HOOK(__imp__XamContentClose, XamContentClose_x)
