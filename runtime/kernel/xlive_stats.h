// The leaderboard read path — XUserReadStats and the by-rank stats enumerator.
//
// Split out for the reason xlive_session.cpp and xlive_social.cpp were: one
// mechanism, a page of derivation, and the derivation is the part worth
// keeping. XenonLive/docs/leaderboards.md is the server half; this is the
// guest half.
//
// HOW THE TITLE READS A BOARD. Its leaderboard object (sub_82598CB0 builds it,
// 284 bytes, one X_USER_STATS_SPEC at +72 for view 1 with NO columns) has two
// ways in:
//
//   sub_82598DB0  XUserCreateStatsEnumeratorByRank(title, rank, rows, 1 spec)
//                 then sub_82594930: XEnumerate(handle, buffer, cb, NULL, ovl)
//                 and it expects 997 back;
//   sub_825947C8  XUserReadStats(title, n, xuids, 1 spec, &cb, NULL, NULL)
//                 first — the sizing call, which the guest's own wrapper
//                 answers with 122 without ever sending a message — and then
//                 XUserReadStats(..., &cb, buffer, ovl), expecting 997.
//
// Both land in sub_8259AB70, the poll: XGetOverlappedResult, then for each
// view in the buffer whose id matches the spec it takes total_view_rows,
// num_rows and rows_ptr, drops every rank-0 row (sub_82594A48) and copies
// the rest out 48 bytes at a time. So what a row must carry is rank, XUID,
// gamertag and i64Rating, and a row for a player who has never played is a
// rank-0 row, which is what the console handed back too.
//
// THE BUFFER IS THE TITLE'S. The results are laid out inside the buffer the
// guest allocated — header, views, rows, columns, then any string bytes —
// with the pointers between them being guest addresses into that same
// buffer. The guest's wrapper sizes it as 8 + views*(16 + rows*52) +
// columns*rows*28, which is 4 bytes over the SDK struct per row and per
// column; the layout here uses the SDK sizes and refuses, with
// ERROR_INSUFFICIENT_BUFFER, rather than write past what was given.
//
// ASYNCHRONOUS, LIKE THE SESSION MESSAGES. Every read goes to the server on
// libxlive's worker and the overlapped is completed from a thread here when
// the answer lands. No guest thread waits on a network.
//
// ON ONLY WHEN THE TITLE IS TOLD IT IS ONLINE. CZ_XLIVE_ONLINE=1 is what makes
// the title ask for a board at all (kernel/imports.cpp, XamUserGetSigninState);
// it is also what starts the thread here. Without it every message below is
// unhandled and fails exactly as it did before.
#pragma once

#include <cstdint>

#include <xbox.h> // be<>

// Starts the completion thread. Called from CzXlive_Start; a no-op unless
// CZ_XLIVE_ONLINE=1.
void XliveStats_Start();
void XliveStats_Shutdown();
bool XliveStats_Enabled();

// XGI 0x000B0021 XUserReadStats, with the title's XOVERLAPPED. Returns false
// when the message is not this one, in which case *result is untouched. On
// true, *result is what XMsgStartIORequest should return: 0 with the
// overlapped left pending, or an error.
bool XliveStats_Dispatch(uint32_t message, void* buffer, uint32_t bufferLength,
                         uint32_t overlappedVa, uint32_t* result);

// XamUserCreateStatsEnumerator(title, type, pivot, rows, specCount, specs,
// &size, &handle). `type` is the enumerator kind the XDK wrappers pass: 0 by
// XUID, 1 by rank, 2 by rank per spec, 3 by rating. `pivot` is the 64-bit
// value in r5 — a rank, an XUID or a rating depending on the kind. Returns a
// Win32 error.
uint32_t XliveStats_CreateEnumerator(uint32_t titleId, uint32_t type, uint64_t pivot,
                                     uint32_t rows, uint32_t specCount, const void* specs,
                                     be<uint32_t>* sizeOut, be<uint32_t>* handleOut);

// The XamEnumerate seam for a stats enumerator, the twin of
// XliveSocial_Enumerate. Returns false when the handle is not one of ours.
bool XliveStats_Enumerate(uint32_t handle, uint32_t buffer, uint32_t bufferLength,
                          be<uint32_t>* itemsReturned, uint32_t overlappedVa,
                          uint32_t* result);

// A boot-time self-test of the guest ABI: struct offsets, the guest wrapper's
// sizing arithmetic, and a result laid out into a buffer of exactly the size
// the wrapper computes — then read back the way sub_8259AB70 reads it. Off
// unless CZ_XLIVE_STATS_TEST=1.
void XliveStats_SelfTest();
