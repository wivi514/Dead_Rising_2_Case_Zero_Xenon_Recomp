# Adding co-op to Case Zero

Operator ask (2026-09-11): *"add co-op that was not present in the main game … we'll
test it ourselves … if it's impossible in the end we'll just revert back to solo only
with leaderboard working."* So this is a build-and-try, with a clean fallback (solo +
XenonLive leaderboards, which already ship).

Case Zero shipped **single-player only**. But it is the same Blue Castle engine as the
full Dead Rising 2 and Case West, and **the entire co-op layer is linked into the image
and runs at boot** — it was disabled at the frontend, not compiled out. So the work is
not "write netcode"; it is "find the one place the frontend would have kicked the
session, and fire it ourselves", then content-test. This doc is the map.

## What is already done (the kernel/Live half)

The `xlive-integration` branch mirrors Case West's HEAD (commit 7e9662b). Sessions,
sockets, QoS, invites, friends, XNADDR — all transplanted and self-tested against guest
memory. Switches: `CZ_XLIVE_COOP=1` (handle the XGI session messages),
`CZ_XLIVE_ONLINE=1` (be told we are signed in), `CZ_NET_LOG=1`. Signed in to a local
`xlived` (needs `XLIVE_ALLOW_INSECURE=1`) the title reads
`netconfig status: |online|dhcp|ethernet` and opens its UDP socket.

**This is exactly the state Case West was in before its own first two-machine session.**

## The instrument: CZ_ONLINE_LOG (part 1, DONE)

`runtime/kernel/online_log.cpp` taps the title's own online logger — the routine every
co-op subsystem reports through, dropped by a retail build. Without it a refused join is
silence.

- `sub_8255B968(this, level, fmt, ...)` — level 1 error .. 4 chatter. `[title:N]`
- `sub_8255B910(this, level0, fmt, ...)` — same, zero-based. `[title:N*]`
- `sub_82567080(ok, file, msg, line)` — the DESYNC check. `[title:desync]`

`CZ_ONLINE_LOG=N` prints levels up to N (default 3 when set to 1). It is the twin of
Case West's `runtime/kernel/guest_log.cpp` (`sub_8252A030`/`8252A098`), found from the
shared format string `"HW MM session state transition to %s"` (passed to `sub_8255B968`
at 0x825CB3D4). Confirmed working: a signed-in boot prints the full
`cReliableLayer::CreateNBindSocket … 0.0.0.0:5679`, `cP2PConnMesh Constructed`,
`HW MM session: entering LIVE_STATE_IDLE`, `UpdateRecv/UpdateSend` pump.

## The session state machine (the map)

Session object vtable: **`0x8208e474`** (40 entries). The state setter is
**`sub_825CB2A8(this, newState)`** — it logs "HW MM session state transition to %s".

LIVE_STATE enum (name table `0x829DFD40`), the create path in order:

| # | state |
|---|---|
| 0 | IDLE |
| 1 | SETTING_SESSION_CONTEXTS |
| 2 | SETTING_SESSION_PROPERTIES |
| 3 | CREATING_SESSION |
| 4 | QOS_SESSION |
| 5 | SEARCHING_FOR_HOST_SESSION |
| 6 | SEARCHING_FOR_HOST_SESSION_BY_ID |
| 10 | ADDING_LOCAL_GAMER_TO_SESSION |
| 11 | ADDING_REMOTE_GAMER_TO_SESSION |

A **host** walks 1 → 2 → 3 (Case West's log). A **joiner** searches (5/6) then adds
gamers (10/11). This is what we have to make Case Zero's game flow do.

Command dispatcher / session object ctor: `sub_825CD3C0` region; the 36 callers of the
setter are the command handlers (state 1 pushed at 825CD4E4, 825CD65C, 825CDA28,
825CDAC0).

## The lever, as it turned out (part 2, DONE)

Part 1's guess — `sub_824C0668`, the create-coop-session method — was right about
the *routine* and wrong about what was missing. Driven by `CZ_ONLINE_LOG` and a
predicate-by-predicate instrument on it (`runtime/kernel/coop_host.cpp`), the
recreate path turned out to be **fully armed by the title itself**: the session's
IS-COOP byte (`+0x98`, read by `sub_82547920`) is already 1, `GameplayFlow::Enter`
(`sub_82537FA0`, flow table 0x8207A59C) writes `mm_info{host=1, COOP}` and sets the
game session's request byte, and `sub_824C0668` runs every frame from the game
session's Update (`sub_824C2268` at 0x824C2484). A signed-in solo Case Zero game
**already creates a Live session on entering gameplay** — three things stood between
that and a joinable host, and none of them was the frontend:

1. **`XamUserGetSigninState(1)`.** The matchmaking object the create path consults is
   the one for the ACTIVE USER (`online+0x5C`, set by `sub_825C2E20` from the
   frontend's `ProfileChange` event, which the title-screen START handler
   `sub_824D83F0` raises for the pad that pressed START). Headless, the synthetic input
   arm answered every pad index, so one START was four pads, `ProfileChange` fired for
   0 then 1, user 1 became active, and its `XamUserGetSigninState` is 0. Fixed at the
   source: synthetic input is pad 0 only (`kernel/imports.cpp`). A real player could
   never have hit it; a headless gate could never have shown it any other way.
2. **The host's XSession flags — THE one place this build disabled co-op.**
   `sub_825C61B0` (online vt[33], the session-description builder) gives a host
   `0x706` here — PRESENCE|STATS|INVITES_DISABLED|JOIN_VIA_PRESENCE_DISABLED|
   JOIN_IN_PROGRESS_DISABLED, no HOST bit, no PEER_NETWORK — for every privacy value,
   where Case West's (0x82597A0C) switches on privacy: 0 → `0x42F`, 1 → `0x827`,
   2 → `0x227`. The joiner's `0x42E` and system link's `0x21` are untouched, and so is
   everything else on the path. `CZ_XLIVE_HOST=1` rewrites the word after the title's
   own builder runs. The kernel then answers `hosting session <XNKID> (2 public
   slot(s))`.
3. **`XamGetSystemVersion` below 0x200CE900.** The `XSessionGetDetails` wrapper
   (`sub_825F2078`) and `XSessionMigrateHost` (`sub_825F21F0`) refuse with 1627 before
   sending their message; the state machine asks for details right after creating
   (LIVE_STATE_GETTING_SESSION_DETAILS) and deletes the session on the 1627. Case
   West's fix (its 5e14cdb) was address-specific and had not been mirrored; it is now.

With all three, a headless DebugJump run walks **IDLE → SETTING_SESSION_CONTEXTS →
SETTING_SESSION_PROPERTIES → CREATING_SESSION (flags 1071 = 0x42F) →
GETTING_SESSION_DETAILS (1 member) → ADDING_LOCAL_GAMER_TO_SESSION →
`LOGIN_STATE_CONNECTED`**, and stays there (`session already live`). The recipe:

```
(cd runtime/build && CZ_NO_WINDOW=1 CZ_DEBUG_MENU=1 CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 \
  CZ_XLIVE_HOST=1 CZ_ONLINE_LOG=3 CZ_FAKE_START_MS=8000 \
  CZ_FAKE_PRESS_SEQ=F2,START,WAITJUMP,NONE,DOWN,A,NONE,NONE,A,NONE,A,NONE,NONE,NONE,NONE,NONE \
  timeout 160 ./cz_runtime > /tmp/host.log 2>&1)
grep -E "host session flags|hosting session|LOGIN_STATE_CONNECTED" /tmp/host.log   # three lines
```
The DebugJump route reaches gameplay on roughly half its attempts (the A at 48 s
sometimes lands on the main menu's leaderboard row instead — a stats enumerator in the
log is the tell); retry rather than tune. Known chatter: `User 0 cannot be added to
the chat` every frame — `XamVoiceCreate` is an honest-failure stub and voice is out of
scope; the online log now collapses consecutive repeats.

Retained from part 1, still true: `sub_824C0668` holds the asserts
`mm_info.IsHost() && "only host can recreate session."` (0x820720EC) and
`mm_info.GetGameType() == DR2Online::GAME_TYPE_COOP && "can only recreate a coop
session."` (0x82072090); GAME_TYPE_COOP = 1. `mm_info` is 24 bytes (ctor
`sub_82547008`): +0 u8 IsHost, +4 GameType, +8, +C, +10 u8 systemlink, +14. The six
frontend callers of `sub_824BD960` (the start-session routine) pass TIR matchmaking
(`FindMatch`, gametype 0) or the co-op JOIN (`sub_825026B0`, isHost 0, COOP) — no
host+COOP caller exists in the frontend, which is consistent with the flags finding:
DR2 hosts implicitly on entering gameplay, and Case Zero left that path in with the
joins switched off in one word.

Online event-type table: `0x829DE580` (triples of filter/enum/display). Relevant:
`EVENT_TYPE_MATCH_MAKING_HOST` (idx 6), `_CLIENT` (7), `_HOST_MIGRATION_RESULT` (9),
`P2P_*`, `SYSTEM_LINK_SEEK_HOST` (21), `HOST_DECLINE_JOIN` (22).

## The joiner, and the first two-machine sessions (part 3, DONE — CO-OP WORKS)

**2026-09-11, host on the Linux box, joiner on the Windows laptop (`czwin`), both
signed in to the operator's own XenonLive (Frank West / wivi514). Two Chucks in Still
Creek, Case 0-4, playing.** Three rounds, ~10 minutes each; the record of what it took
is below, in the order it was found. The operator's instruction was *"put the joiner on
the windows laptop so they are not from the same PC and use our xenonlive"* — the
same-box pair (`CZ_XLIVE_PEER_PORT=3075`, `XLIVE_DATA_DIR=~/.config/XenonLive-host`)
was used only to shake the guest-side path out before the laptop leg.

### The lever: `CZ_XLIVE_JOIN=1` (`runtime/kernel/coop_join.cpp`)

DR2's JoinGame screen has an "XboxLive" row whose handler (`sub_824DAA10`) opens the
**GameSelect** screen in mode 1 through the frontend transition manager —
`sub_827F6D40(manager, hash("GameSelect"), {1, 1})`. GameSelect (ctor `sub_824C8898`,
vtable `0x82073AA8`; Init vt[3] `sub_82502488` reads the mode from params+4; Update
vt[5] `sub_825026B0`) is the case-file picker: in mode 1 it shows a dialog (string
11522, *"If you join an online game, all unsaved progress will be lost"*), waits for a
save slot (UI event `tv_45` sets +0x548), then does the one thing that matters —
`sub_824BD960(gameSession, mm_info{host=0, COOP, -1, 0, 0, 0})` once `sub_824BDD10`
says online is ready. Case Zero's menus never open that screen; everything below it is
linked. The switch opens it the way the row does, from the game session's per-frame
Update, only in game state 3 (the main menu) and only while the HW MM session's
LIVE_STATE (`+0x118` of the object `sub_825CB2A8` is called on) is IDLE; retries every
`CZ_XLIVE_JOIN_RETRY_MS` (12 s) if the walk comes back to IDLE. `CZ_XLIVE_JOIN=call`
makes the call directly instead — it joins (seat, transport, `LOGIN_STATE_CONNECTED`)
and then sits in the menu, because nothing is listening; the screen is what listens.

The player's part on the joiner: leave the title screen, confirm the dialog, pick a
save. Then: SEARCHING_FOR_HOST_SESSION → `search found 1 session(s)` → QOS_SESSION →
CREATING_SESSION (`registered session … to join`, the seat taken at create as Case West
established) → GETTING_SESSION_DETAILS (2 members) → `LOGIN_STATE_CONNECTING` → the
reliable layer (Syn Sent → Open on the joiner, Listen → Syn Recvd → Open on the host)
→ `LOGIN_STATE_CONNECTED` in ~10 s. Host side: `Accepted client from 198.18.0.49`,
`Pending client IS confirmed! (Frank West - 31)`, outfit transfer, `TYPE_FLOW` /
`TYPE_SYNC_POINT` / `P2P CLIENT PULSE`, and the client loads the host's level.

### What stood in the way, in order

1. **The host crashed on its first client** (guest fault at `0x80`): `sub_8256E6E8`,
   the connection listener's update, reads the release byte `0x829EC974` (gotcha 266)
   and, when it is set — retail, 1 — passes **NULL** for the endpoint list to
   `sub_82545DF0`, which dereferences it. Case West's `sub_8253EC68` is the same routine
   without the test. This is the transport half of "co-op was removed from Case Zero",
   beside the flags word of part 2. `runtime/kernel/coop_transport.cpp` runs the Case
   West form in C++ (a re-implementation, because clearing the byte for the call would
   change 2,012 other sites); `CZ_COOP_LISTENER_STOCK=1` is the control.
2. **The session died after 60-120 s every time**: `HW MM session found account: 0 is
   not signed in to xbox live!` on both sides. libxlive's hourly token refresh closes the
   gateway (`401 Unauthorized`, reopened seconds later); `xlive_glue.cpp` posted
   `XN_SYS_SIGNINCHANGED` on the close, the title re-read `XamUserGetSigninState`, got 1,
   and shut the session. Now held for `CZ_XLIVE_SIGNIN_GRACE_MS` (30 s; 0 = the old
   behaviour): the title reads 2 throughout and is told only if the drop outlasts the
   grace. The headless runs had read this as a "120 s join timeout" — it was the refresh.
3. **The joiner faulted at `0x14`** when the retry re-opened GameSelect while the title
   was still in gameplay after a drop. Hence the game-state-3 gate.
4. **The DebugJump host route flaked 3 of 3**: `F2` at 8 s lands during the title →
   menu transition and the request is lost. `NONE,START,NONE,F2,WAITJUMP,DOWN,A,…`
   (F2 after the menu is up) reached gameplay 4 of 4. The CLAUDE.md recipe still
   works when it works; use this order for anything that must not flake.
5. **The Windows exe needs `libcurl-x64.dll` beside it** (`C:\cw\curlin`); without it
   the process exits with 0xC0000135 and an empty log. Build there with
   `C:\cz\build_cz_xlive.ps1` (Case West's recipe: `-DXLIVE_ROOT=C:/cw/XenonLive`,
   curl from `C:/cw/curl`, overlay off).

### What is open after part 3

- ~~**The joining Chuck has no chest piece**~~ — FIXED IN PART 4, see below; the paragraph
  stands as the record of the wrong guess. On both machines, head and hands placed
  correctly (operator-confirmed against `~/DR2CZ-troubleshooting/coop/*.png`); changing
  clothes on the client makes it appear. NOT the `OUTFIT_COOP_DEFAULT` row of
  `outfits.csv` (chest `champions_jacket2`, which Case Zero does not ship):
  `tools/patch_coop_outfit.py` rewrote it to `chest_default` in both archives, the
  laptop read the patched files, nothing changed — and the jeans the client wears are
  not `leg_default` (no such file), so the client's spawn pieces are not that row as
  read. Next: hook the outfit application (`sub_82167428` returns the entry, 0x11C bytes
  each from db+0x88; `sub_82167450` names index → `0x829D44F8`) and print the seven
  pieces the client sends (`POUT: Client:tEventOutfit` ×7).
- ~~**No incoming-call HUD on the host.**~~ — CLOSED IN PART 4: the element is compiled
  out of both XBLA builds (see below). DR2 shows *"Incoming co-op call…"* (11546) with
  D-pad RIGHT to answer; here the join goes through without it and the call is only
  visible in the pause menu. The session works regardless (the host confirmed the client
  without answering). Whether Case Zero's HUD lacks the element or the trigger is
  another release-byte site is unmeasured; `ON_HOST_CONFIRM_COOP_JOIN` /
  `ConfirmToAcceptClient` (`sub_824BDBE8`, `sub_824C24A0`) are where to look.
- **Voice**: `User 0 cannot be added to the chat` every frame — `XamVoiceCreate` is an
  honest-failure stub. Out of scope.
- **Release**: `coop_transport.cpp` and the grace are always-on fixes that are inert
  without a session; the host/join switches are env vars with no panel row; and if the
  outfit patch ever becomes real it needs porting to `runtime/host/overlay_gen.cpp`.

### Recipes

Host, windowed, on the Linux box:
```
TAG=coop_host tools/play_session.sh CZ_DEBUG_MENU=1 CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 \
    CZ_XLIVE_HOST=1 CZ_ONLINE_LOG=3 CZ_NET_LOG=1
```
Joiner on the laptop: `C:\cz\play.bat` is the joiner launcher (`XLIVE_ALLOW_INSECURE=1
CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_XLIVE_JOIN=1 CZ_XLIVE_JOIN_RETRY_MS=90000
CZ_ONLINE_LOG=3 CZ_NET_LOG=1`, plus the guest diagnostics as left on 2026-09-11);
`play_solo.bat` is the old one; `schtasks /run /tn cz_play` puts it on the desktop.
Headless joiner, same box: `XLIVE_DATA_DIR=~/.config/XenonLive-host XLIVE_ALLOW_INSECURE=1
CZ_XLIVE_PEER_PORT=3075 … CZ_FAKE_PRESS_SEQ=START,NONE,NONE,A,NONE,A` (the two A's are
the dialog and the slot).

## Part 4: the chest piece, and the first content tests (DONE except two)

**2026-09-11 evening, three more two-machine joins, operator-played.** The joining
Chuck has his torso on both screens; cinematics, a host save, item pick-up/drop and
one side quitting all behaved; 0 `[title:desync]` lines over a 1.65 M-line host log.
Owed: a mission transition and a crowd (§ "Plan", item 4).

### The chest piece: `OUTFIT_COOP_DEFAULT_UNDER` has no row, so the chest was `chest_NONE`

Part 3 guessed the `OUTFIT_COOP_DEFAULT` row (DR2's champion's jacket) and measured a
null. Part 4 built `CZ_OUTFIT_TRACE=1` (`runtime/kernel/coop_outfit.cpp`) and read the
pipeline on the host across two joins:

1. The client's seven `tEventOutfit` messages are its SAVE outfit — decodable from the
   packet sizes alone with a 19-byte header (31, 31, 20, 31, 25, 31, 37 = `young_chuck`
   ×2, `""`, `young_chuck`, `naked`, `young_chuck`, `young_chuck_under`) — and the
   receiver `sub_82570E78` records them on the remote player's clothing object
   (player + 0xCE74) through `sub_82371978`, correctly, chest included.
2. The co-op flow (`sub_82582888`) then posts one change-part event per part, all seven.
3. The load requester `sub_82270290` builds the file name — and it has a special case
   **for the chest only**: a player other than player 0 asking for `OUTFIT_DEFAULT_UNDER`'s
   chest (db entry 17, `db+0x1364`) is handed `OUTFIT_COOP_DEFAULT_UNDER`'s chest (entry
   16, `db+0x1248`) instead, and player 0 the reverse. DR2's way of dressing the two
   Chucks differently.
4. Case Zero's `outfits.csv` has **no `OUTFIT_COOP_DEFAULT_UNDER` row**. The name is in
   the title's table (`0x829D44F8`, index 16) but the loader `sub_821B67C0` fills entries
   by NAME, so entry 16 keeps its constructor's seven `NONE`s and the requester asks for
   `chest_NONE`. Nothing loads it, the per-file completion never reaches its count, the
   normal set-piece never runs for part 3, and every other part goes through untouched —
   which is exactly "only the torso is missing, on both machines" (the client's own Chuck
   is player 1 there too), and why changing clothes fixed it (a non-default chest takes
   the plain path).

The fix is data: `tools/patch_coop_outfit.py` now makes BOTH co-op rows copies of the
default rows (`OUTFIT_COOP_DEFAULT = OUTFIT_DEFAULT`, `OUTFIT_COOP_DEFAULT_UNDER` added
`= OUTFIT_DEFAULT_UNDER`) — the operator's instruction: the partner spawns in what player
one spawns in, and the substitution becomes the identity both ways. Both archives
(`datafile.big`, `preload4.big`), both machines (scp; the laptop's game must be closed —
it holds `datafile.big` open), hashes matched. Verified by trace (`PieceFile part 3 ->
'chest_young_chuck'`, `SetPart … part 3` for `B9288350`) and by eye on both screens.
To make the partner visibly different instead, change the chest of both co-op rows to
`default` (DR2 Chuck's leather jacket, shipped). Still not in `overlay_gen.cpp`.

Two things the trace found on the way that are NOT the defect, recorded so they are not
re-bought: the clothing manager sizes each part's buffer from a table (`0x829D42F0`)
with a solo and a two-player column at exactly half (chest 2006 → 1003 KB), and the
texture-create path sets a byte for the two-player case (`g_82AC4878+0xB54`) — the
`young_chuck` chest loads fine under both once its name is right. And
`sub_8254BB70` (co-op level entry dressing player 1 in row 16) did not fire in any of
these sessions; the outfit arrives by report, not by that row.

### The host's "incoming co-op call" — the HUD element is not in this build, nor in Case West's

String 11546 *"Incoming co-op call..."* exists in `str_en.bcs` and is referenced by NO
instruction in Case Zero's image (no `li`/`addi` with 0x2D1A, no data word) — **and the
same is true of Case West's image**, so the walkie-talkie call element is compiled out
of both XBLA builds and the operator will not get it back by flipping a byte. What IS
linked is the dialog: `sub_824BDBE8` formats 11533 (*"%s wants to join your game?"*)
and posts it, from `sub_82224F30` — the call-element's "answered" handler, state 3 of
the object at `sub_82224A38` — and the answer is read back at `0x824C10C0` (yes/no →
`sub_82582188(session, accept)` → *"Pending client IS confirmed!"*). Auto-accept is
`sub_82582188(session, 1)`, which `0x824C11E0` passes on one message path. Which path
confirmed the client in our sessions was not instrumented; the session works without the
prompt and the plan ranks it low. If it is ever wanted, the dialog path is the one to
drive (post 11533 from the confirm-pending site and feed the answer to
`sub_82582188`), not the HUD text.

## Part 5: the menus, the host's prompt, and a friend's game (DONE — and the branch is on master)

**2026-09-11, late evening; three more two-machine joins, operator-played; every piece
below was seen working by the operator on both screens.** The operator's instructions,
in order: *"Do the start coop ui in the main menu and seeing the ui notification of
player request of joining your game"*, then *"can you add the option to join a friend
game in the start coop menu like case west"*, then *"instead of seeing a list of
server of your friends that are playing and you can choose which one to play with"*,
then *"push this to master and we'll do the release build"*.

### JOIN CO-OP GAME on the main menu — data only (`tools/patch_coop_menu.py`)

The main menu is `data/frontend/mainmenu.big`'s `title.txt`, a cFEScreen layout script
(the grammar `gen_pc_options.py` cracked in part 60), and DR2's whole JoinGame screen
SHIPS in the same archive (`joingame.txt`: "JOIN XBOX LIVE GAME" → `ACT:XboxLive`,
whose handler `sub_824DAA10` opens GameSelect in mode 1 — the exact call
`coop_join.cpp` makes). Only the ROW that leads to it was cut from `title.txt`, and the
transition graph (`fecmn.big`'s `path_fe.txt`) lists JoinGame under GameSelect only.
The tool clones the START GAME button as `JoinCoop` (string 108 "JOIN CO-OP GAME",
shipped in every bank; `onSelect="FWD:JoinGame"`, the framework's own forward verb),
shifts the rows below one slot and re-links the focus chain, and adds
`JoinGame=">Normal"` to TitleScreen in both `fecmn.big` overlays (game_patched and the
bootskip copy). Headless: `START,NONE,DOWN,A,NONE,A,NONE,A,NONE,A` walks title →
JoinGame → GameSelect(1) → the dialog → a slot → `SEARCHING_FOR_HOST_SESSION`. The
laptop's `play.bat` no longer sets `CZ_XLIVE_JOIN`; the player uses the row. All of it
is now in `runtime/host/overlay_gen.cpp` too (generator v5), byte-identical.

### The host's "wants to join" prompt (`runtime/kernel/coop_call.cpp`)

Part 4 closed the walkie-talkie HUD as compiled out; part 5 read the two routes from
"a client is pending" to the confirm, `sub_82582188(session, accept)`:

1. `sub_82587228`, the session-details handler: when the joiner's member record
   arrives it stores the pending client (session+0xDC) and, if a game-state word
   (`0x82A57428→+0x78→+0xC→+0x30→+0x70`) is 5 or 6, posts **a synthetic "Yes"** —
   `{7, hash("DlgOnHostConfirmCOOPJoin"), hash("Yes")}` — to the game session, whose
   event handler (`sub_824C0958` at 0x824C10C0) turns it into the confirm. Otherwise
   it enables the pending call (`sub_8256FC60`, session+0x93), which the session's
   Update turns into `sub_8254B108` → `sub_82224DF0`, the walkie-talkie ring whose
   "answered" handler (`sub_82224F30`, D-pad RIGHT) is what raises the dialog
   (`sub_8254B300` fetches the pending gamertag, `sub_824BDBE8` formats 11533 and
   raises `DlgOnHostConfirmCOOPJoin`; the answer comes back through the same
   0x824C10C0 path — Yes / No / SetToPrivate, the last also sets privacy). An
   unanswered ring is auto-declined by `sub_82224A38` on a timer.
2. Measured on both a same-box pair and the two-machine sessions: **the state-word
   chain is null here** (reads as 0xFFFFFFFF), so the walkie route is the one taken —
   the ring nobody can see, then a timed decline. (`Pending client IS confirmed!` is
   the transport-level confirm, `sub_825719E0` from the kind-13 client event, and
   comes BEFORE either route; it is not the accept.)

Both routes now ASK: the synthetic Yes is caught in the event handler and turned into
the prompt (nothing else in that handler runs, so neither the confirm nor its
"EXCHANGING_DATA" wait dialog does); `sub_8254B108` raises the prompt directly instead
of ringing. The player's answer goes through the title's own path untouched. The
operator saw it three times: *"Frank West wants to join your game. Let the player
join?"*, Yes, and the laptop loaded in. `CZ_COOP_JOIN_PROMPT=0` is the control arm,
`CZ_COOP_CALL_TRACE=1` the instrument. Hosting is now implied by `CZ_XLIVE_COOP=1`
(`CZ_XLIVE_HOST=0` opts out) because the prompt is the gate.

### JOIN FRIENDS — the title's own friends list, and X joins (`runtime/kernel/coop_friends.cpp`)

The JoinGame screen's second row raises `ACT:Friends` → `LaunchFriends` on the game
session → `DlgOnFriends`, DR2's FRIENDS screen (`fecmn.big`'s `on_friends.txt`; class
around 0x824DBFxx-0x824DD2xx): a scrolling list from the friends enumerator
(`xlive_social.cpp` serves it from libxlive, presence included) with A = send invite,
X = `join_session_or_accept_invite`, Y = gamercard, B = back. Two things stood in the
way: the LaunchFriends handler returns at once when `enable_prolog_experience`
(0x82A57BFA, =1 here) is set — cleared for that one event — and the X button's join
goes through the friends interface (matchmaking vt[31] → vt[25]/vt[27]) and matchmaking
vt[38], a path this port has never run. So X is answered on the proven path instead:
the friend under the cursor is resolved exactly as the handler resolves it (focused row
→ "c0" → "text" → its record → `sub_825479E8` → the entry, xuid at +8), the session
search is told to keep only that host (`XliveSession_SetSearchHostFilter` mode 2), the
screen is closed as its own B does, and GameSelect mode 1 is requested for the next
frame (`CoopJoin_RequestJoinScreen`; a transition asked from inside a dialog's handler
is refused while the dialog is current). A friend not in a joinable session gets the
title's own "Join Session Failed!" dialog. The first form of this row (a friends-only
search with no picker) was built, seen working, and replaced the same evening on the
operator's instruction. The laptop's log for the final form: `JOIN FRIENDS` →
`LaunchFriends: raising DlgOnFriends (enable_prolog_experience was 1, cleared)` →
`friends screen: joining 'Chuck Greene'` → `transition request returned 1` → `search
found 1 session(s), 1 hosted by 0009000000000030` → the host's prompt.

The two accounts must be FRIENDS on XenonLive for the row to find anything: the
operator's default account here (wivi514) has none, so the hosts for these tests ran
as Chuck Greene (`XLIVE_DATA_DIR=~/.config/XenonLive-host`), who is Frank West's
friend. A friend's PRIVATE session (SetToPrivate) is not searchable; that needs the
invite path (`xlive_social.cpp`), which is separate and not built into a menu.

### Recorded on the way

- A same-box headless pair (host by DebugJump, joiner by the menu row) reproduces the
  whole join including the prompt; its first run faulted the host at guest 0xC inside
  an INSTRUMENT — `LoadU32` guards only a zero address, and a null link two steps into
  a pointer chain reads 0xC (gotcha 556).
- `sub_824C0958` is reached only through a vtable, and a `PPC_FUNC` hook on it works
  (the function table names the C++ symbol); a trace line inside the hook that never
  printed for the dialog's answer is unexplained and unimportant — the answer path
  was confirmed by the confirm's own caller address (0x824C1178).
- `sub_82587228`'s "state word" was 5/6 in NO session; the synthetic-Yes route exists
  in the image and is handled, but has not been seen to fire.
- The "OnSLHosts" system-link host browser (`hosts.txt`) is registered in the screen
  factory and untouched; DR2's LAN join path is still there for whoever wants it.

## Part 6: the call rings, and the regression that parked every join (DONE, 2026-09-12)

The operator's ask: *"show a proper message to press right on d pad or right on the
keyboard when someone asks to join instead of showing the message directly (like main
Dead Rising 2 and Case West)"*. Delivered and operator-verified the same evening, then
the join that followed sat in infinite loading for two hours of bisection — and the cause
was a commit from the night before, not anything part 6 built.

### The ring (`runtime/kernel/coop_call.cpp`, the overlay's `Notify`/`Dismiss`)

- The incoming call now RINGS: `sub_8254B108` (the title's own ring) runs as shipped, and
  the XenonLive overlay shows a tagged toast (*"<name> wants to join — press RIGHT"*) for
  the ring's 120 s (`0x8200A1CC`). `CZ_COOP_CALL_RING=0` is the part-5 form (the prompt
  straight away).
- D-pad RIGHT answers through the title's `COMMAND_AI_INTERACT_WITH_PHONE`; on the
  keyboard the same command is `KEY_RIGHT` — it had been on `KEY_C`, and the native-KB/M
  splice takes only the FIRST key of a kbmap line (one free `src2` slot in the pad record),
  so `KEY_RIGHT` goes first in `kbm_default_map.h` (`gen_kbm_map.py` mirrored; regenerating
  that header reverts parts 99/108's hand edits — note in the generator).
- **The title's own answered body must NOT run.** `sub_82224F30` posts event 0x5C and sets
  call state 3 — the host is then "in the call" and the joiner never loads. The hook raises
  the part-5 dialog itself (`RaisePrompt`), takes the ring down, and leaves the call queue
  empty and the state idle (`player+0x1A3C/+0x1A40`).
- The toast API is the launcher's: `Notify(text, seconds, tag)` replaces a toast with the
  same tag, `Dismiss(tag)` removes it (XenonLive_Launcher 2e250d2; glue in
  `xlive_overlay_glue.cpp`, both no-ops without the overlay).

### Found on the way (each a real defect, none the stall)

- **`enable_trial_experience` (0x82A57BFE)** is forced to 1 by init (`sub_82496D98`) and
  cleared by the licence consumer only behind a user-state test — with the diag byte
  cleared it stayed 1 (the laptop's UNLOCK FULL GAME row). `cpu/trial_flag.cpp` pins it to
  0 after init unless `CZ_TRIAL_EXPERIENCE` is set.
- **The listener gate** (`coop_transport.cpp`) read only `CZ_XLIVE_HOST/JOIN`, so a host
  launched the part-5 way (`CZ_XLIVE_COOP=1`) crashed at guest 0x80 on the first join. It
  follows `XliveSession_Enabled()` now.
- **Per-profile saves (part 115) left the joiner's `<gamertag>/` folder EMPTY**: the joiner
  sent 1 outfit event where host9 saw 6. Seeding it from `default/` restored the 6 — real,
  and not the stall. A runtime rule for a save-less joiner is still owed.
- Case West's offline-overlay fix (no client → no Shift+Tab) ported (16fcc7e).

### THE STALL: `e7a9e25` put the host hook on the joiner (gotcha 575)

The last working join was host9 (09-11 21:57). Seven minutes later `e7a9e25` made
`CZ_XLIVE_COOP=1` imply hosting — and the launcher sets that on BOTH machines, so the
JOINER ran `coop_host.cpp`'s hooks for the first time. Everything bisected afterwards
(pin, timer, trial, ring, outfit trace, and the "v1.1.0 joiner control" exe — built at
22:29, AFTER the commit, so never a control) was downstream of it.

The mechanism, all the title's own code:

1. The `GameplayFlow::Enter` hook stored 1 into session `+0x98` unconditionally. The
   joiner's log said so: `IS-COOP 0 -> 1` — the title holds it at 0 on a joined session.
2. The sync-point sender (`0x82496118`, the one caller of `SignalSyncPoint` 0x82581A08)
   checks before EVERY send: `if (sub_82547920(session) /* +0x98 */ && session+0x92)
   { pending = 1; return; }`. `+0x92` is `EnableLoadingPrevGame(HOST|CLIENT)`
   (`sub_8256FBF0`, called from 0x8218C528), and the joiner sets it as CLIENT during the
   load.
3. So the joiner's `SYNCPOINT_TYPE_GAMESTATE_FINALIZE_START_LEVEL` — the next line in
   host9 after the second `TRANSITION_BEGIN`, and exactly where every session since
   stopped — was parked in the pending slot, which only the host's path (`0x82499BB0`,
   clears `+0x92`) ever flushes. Infinite loading.

The fix (b981df6): the hook stores the byte only where the title's own next predicate
would let it matter — a session NOT already live (`sub_8254AF30`) and whose mm_info is not
a JOIN's (IsHost 0, GameType COOP). A host's recreate after a level change (session torn
down → not live) is unchanged. Operator-verified: *"It worked!"* — host17 receives
`FINALIZE_START_LEVEL` then `READY_FOR_PLAY -> RESULT_HOST_SUCCESS`, as host9 did.

Two readings retracted on the way: `GamestateMan (SP)` is a fixed format string
(0x8206AC28), not a single-player mode; and the empty save folder was incidental.

### The military arrival (the motorcycle escape at the end): SHIPPED AS SINGLE-PLAYER

The first co-op run through the ending crashed the HOST on the frame the mission action
`ArmyPA` ran. Six operator runs on the two machines (each read from both logs — no
headless reproduction, the operator's instruction) established the chain; the record
is `runtime/cpu/prop_attach_guard.cpp`'s header comments and these points:

1. `ArmyPA` destroys the two landed helicopter props (`DestroyProp: 2457x-ArmyHelicopterN`);
   the pool ZEROES a released prop (its first word is a vtable while alive), so a
   stale reference is a NULL write in the prop's SetPosition (`sub_822CF898`, `+0xB0`).
2. Holder one: an attachment RIG (`sub_82295D20`, five slots at +0x58 / 0x2C) still
   carried a helicopter — guarded (slot dropped, one log line). Not enough.
3. Holder two, the one that matters: an ACTOR in its MOUNTED mode. The mode block at
   `actorData+0x3794` {vtable 0x820446A4, SEAT ptr, float, index} is 0x1C0 bytes the
   title **replicates raw over the wire, seat pointer included** (vt[1] writes it out,
   vt[2] = `sub_82278468` memcpy's it in) — sound on the 360's deterministic heap, and
   the census (`CZ_PROP_HOLDER_SCAN=1`) showed the same seat addresses on both machines
   here. The remote player's actor on the host is mounted wherever the joiner's Chuck
   is: in the landed helicopter, while the host — ahead in the flow — has already
   destroyed it. Nothing on the host ever writes that reference; it arrives. No
   single-player run can show it.
4. Clearing the seat reference is WRONG (run 5 crashed on it — the mounted update's
   caller at 0x822A4874 dereferences the seat unconditionally). The shipped guard is
   on the prop's own `SetPosition`/`SetRotation` (`sub_822CF898`/`sub_822CF958`):
   a zeroed prop is refused, printed once. `CZ_NO_ATTACH_GUARD=1` is the control.
5. With the guards, run 6 did not crash — and the second player was DISCONNECTED
   during the host's LEVEL LOAD before `ArmyPA`: the host's main thread blocks for the
   load (`[DRAW] Main thread is blocked, suspending the D3D Device`) and sends and
   receives nothing for ~11 s (vblank #86500 -> #98000 at the 1 ms vblank); the
   endpoints on both sides go `Open -> Error` ("link shut down"). The 360 loads faster
   than that window, or services the link during the load — not yet established.

**The operator's decision (2026-09-12, 21:40):** ship it as it is — the ending is
single-player, the second player is dropped there (today by that timeout, not by a
switch of ours), the release notes and README say so and that a fix is in progress.
What the fix needs: (a) keep the link alive through the host's load (find why the net
thread does not run during it, or lengthen the endpoint's expiry), then (b) with both
players in, see whether the guards in point 4 are enough for the ending or whether the
partner needs the title's own dismount when the helicopter goes.

### Still owed after part 6

- The military arrival in co-op (above): the link through the host's load, then the
  partner's dismount.
- Invites: *"sending an invite and trying to join by the invite doesn't work"* — the invite
  path in `xlive_social.cpp` was never wired to a menu.
- A save-less joiner: seed from `default/` or refuse with a message.
- Docs for the Windows release leg (open item 0z) now that the joiner fix is in.

## Player issue #7: the partner under the map after a host level load (OPEN, 2026-09-14)

The first co-op bug report filed through the launcher (`~/XenonLive/Player Issues/#7`):
*"Loading into still creek in coop makes the client falls through the map and crashes"*.
Host: Windows 10 LTSC, Ryzen 9 9900X, GTX 1660 Ti, 1080p windowed, **fps cap 30**, v1.1.0.
Client: unknown machine, no log. The operator could not reproduce it on the two test
machines. What the host's F9 capture establishes, read against the operator's own logs:

1. **It is a level load ON THE HOST with the client attached, not the join.** The 60 s
   before F9: gameplay with a live session (`XGI 8001 = 2`, the mouse captured), then
   `loading.big` at −20 s, all ten `prologue_zNN.big` and `Prologue.txt` at −5 s — a full
   level load that took **14.5 s** (this box: ~1 s). No `closesocket` until after F9, so
   the client stayed connected through it. The screenshot at F9 is LV 1, $2,000, 0 killed,
   "Find Katey Zombrex", Chuck at the junkyard gate — the game's initial checkpoint —
   with the partner's marker (name, health bar, an arrow pointing DOWN) at the host's own
   feet: the client's replicated position is directly below the host. So the host restarted
   or reloaded to the start with the partner in the session.
2. **The client died ~8 s after the host's load finished**: `closesocket(1005)`,
   `closesocket(1006)` at +3 s after F9, then the title's own re-host (`host request
   armed ... online ready says no` → `hosting session <new id>`), which is what the host
   does after its last client drops (coop_host17 shows the same sequence on a joiner loss).
   The endpoint takes ~10 s of silence to error, so the client process most likely died
   around the moment the host came back from its load — i.e. during the client's own
   level-start handshake (FINALIZE_START_LEVEL / READY_FOR_PLAY).
3. **The shape the test machines never made: a joiner FASTER than the host.** Every
   operator session had this box hosting (1 s loads) and the laptop joining; the reverse
   pair (laptop host) lost the joiner to the endpoint timeout before the load even began
   (part 6's military arrival). The operator's live re-test on 2026-09-14 (three reloads
   with the laptop attached, AppImage host) did not reproduce it. A Windows host at 14.5 s
   against a client that loads in a few seconds is a client waiting ~10 s at its level
   start with the host silent.

What was built (commit 8576104) so the next report answers the question by itself:

- **The fall watch, on every build** (`[pos]`/`[fall]`, `docs/instruments.md`): both
  player slots' positions, ten samples a second off the player object's position field,
  printed every 5 s and on every second of a continuous descent. A player's
  `cz_runtime.log` or F9 capture now says where each Chuck appeared and whether he fell.
- **`CZ_SLOW_ZONE_OPEN_MS=N`**, a dev arm that makes a level load slow on purpose, and
  **`tools/coop_pair_reload.sh`**: the same-box pair with a slow host that jumps to a new
  case (F2) after the joiner is in — a host reload with a client attached, headless.

The candidates, to be read off the joiner's `[pos] player 1 appeared at (...)` line:
(a) the joiner's Chuck placed at the ZERO vector (a level start whose per-player restore
table — `sub_821AE578` reads `*(*0x82A59CD4)[0]+8` as "use saved positions" and copies
five words per player index — is flagged but empty for player 1) — if the junkyard sits
near the level origin, y=0 is under its floor; (b) placed at the host's position while the
joiner's zone collision is not resident (a teleport before streaming); (c) the joiner's
simulation running through the wait with no floor. The client's log is owed either way:
ask the reporter for the CLIENT's `cz_runtime.log` (next to its `assets/`) — the crash
block is in it — and what the host did just before (restart? died and reloaded? a save).

### The fix that shipped: the fall guard (2026-09-14)

The operator's call once the crash was understood: *"the crash from falling off the map is
not that important because the user shouldn't be able to fall out the map so let's fix why
he fall through the ground."* The root cause is a race (client gravity beats zone-collision
residency) that neither the operator nor a headless pair could trigger on demand, so the fix
is a **symptom guard that engages only in the one situation that is never legitimate** — a
player who drops below his spawn without ever having been grounded.

Two things were established first, by measurement:

1. **A per-frame write to the player's position on an engine thread PINS him** — against
   gravity and against walking input (`CZ_HOLD_PLAYER_TEST`, 138/138 `HELD` with AutoChuck
   trying to move him). §6bn's finding that "the body re-imposes" is true only for a
   ONE-SHOT write; a continuous write on the `sub_825F9CF0` engine-thread hook wins. This
   is the linchpin the safety net needed and the reason it is buildable at all.
2. **The guard does not false-fire.** A full AutoChuck exploration of Still Creek (ledges,
   stairs, curbs, 35 distinct positions) produced 0 `[fallguard]` pins and 0 `OUT OF THE
   WORLD` — because a legitimate fall is always a fall AFTER being grounded, and the guard
   only ever acts on a player who has NEVER grounded since spawning.

`runtime/cpu/debug_tunables.cpp` `PumpFallGuard`: for the LOCAL player (index 0 — the host's
fall watch showed 0 = local, 1 = the remote joiner, so on the client index 0 is the client's
own Chuck), capture the spawn; if he stands 0.75 s → grounded, never touched again; if he
drops >1.5 below spawn first → pin at spawn, probe every 1.5 s (stop writing 150 ms and
look), release when the floor catches him. ON by default (`CZ_NO_FALL_GUARD=1` is the
control), because it acts only in the pathological case.

**What is NOT yet verified, stated plainly:** the end-to-end catch of the *actual* co-op
fall. The map always has a floor in single-player, so a faithful "fall past spawn with no
floor" cannot be staged locally; the pin/probe/release primitive and the no-false-fire
behaviour are proven, the trigger is impossible in normal play, and it is one env flag to
disable. The `[fallguard]` / `[fall]` log lines confirm it the next time a client hits the
race in the wild — ask a reporter for the CLIENT's `cz_runtime.log`.

## Part 7: the title's OWN friend join, run to its end and parked (2026-09-14)

**The question:** the operator — *"Is there logs of Case West that we can use so we
properly implement join a friend from the main menu in Case Zero? Because for now it
act like it randomly search a game when we try to join a friend."* **The answer: no
such log exists anywhere and none can.** Every Xenia capture of either title is a solo
run and Xenia has no Live layer (`grep XSession` over A1/A5 is 0; Case West's capture
index says the same). Case West's runtime has no friend-join of its own — its
`XliveSession_SetSearchHostFilter` is ours imported back, with zero callers — and the
`cw_runtime.log`s on this box are the HOST side of sessions. What exists is our own
laptop joiner log (`~/DR2CZ-troubleshooting/coop/laptop_joiner_diag_0912.log`), and it
shows the complaint with a signature: one attempt has `JOIN FRIENDS: opening the
title's friends screen` followed by `session search: any joinable session` and a plain
`SEARCHING_FOR_HOST_SESSION` with **no `friends screen: joining` line** (the X never
reached our hook and the player fell back to the LIVE row); the next attempt has the
filtered search finding the friend's session. **Both look identical on screen** — the
same "unsaved progress" dialog, the same slot pick, the same "searching" — because the
part-5 row IS the JOIN XBOX LIVE GAME search, filtered to one host.

So the title's own path was run instead, on a same-box headless pair
(`tools/coop_pair_friends.sh`; the two test accounts were made friends on the
operator's server for it — `wivi514` and `Chuck Greene` 0x30), fifteen runs, under
`CZ_COOP_FRIENDS_NATIVE=1` (X on the friends screen goes to the title's handler,
traced; OFF by default, the part-5 filtered search stays the shipped behaviour).

### What the title's friend join is (read from the image, then watched running)

`join_session_or_accept_invite` (friends screen X, 0x824DCC54) → friends interface
vt[25] has-invite? / vt[27] join → matchmaking vt[38] `sub_825C4290` (queues a
command with the friend entry; `0x82A57BFC` set = refuse) → `sub_825C3950(R, entry)`
→ `sub_82553D18(session, record, friendXuid, selfXuid)` → **session vt[8]
`sub_825CD500(session, sessionInfo, ?, friendXuid, selfXuid)`**: stores the two xuids
at session+0x328/+0x330, refuses with *"MM Session is unclear when calling search
session by id"* if +0x110 is set, and enters **LIVE_STATE_SEARCHING_FOR_HOST_SESSION_BY_ID
(6)** → `XSessionSearchByID` with the friend's session XNKID from the friends
enumerator (`XONLINE_FRIEND` +0x1C, which `xlive_social.cpp` already fills) — answered
by our kernel (`0x000B001B` → `GetSessionDetails`; a KLOG names it now). Then the
title runs **cFESynchronizer's INVITE machine** (`UpdateInvite`, `sub_824BC7D8`,
state at +0x80): 1 RECEIVED_INVITE → 2 (wait for game state 3) → 3
QUITING_ONLINE_GAME (transition to `PressStart`) → 4 SWITCH_PROFILE_LOADING (posts a
**`GameInvites`** event {7, hash, 0} to the frontend manager's sink, manager+0xC0) →
5 (waits for frontend state 0xE) → 6 GETTING_DETAIL (matchmaking vt[46]; a
`LIVE_STATE_GET_SESSION_SLOT_NUM` search-by-id) → 7 GETTING_INVITE_TYPE (vt[47] → the
type into `0x82A59CD4→+0x10`) → 8 SWITCH_PROFILE_LOADING_DONE (posts `GameInvites`
AGAIN) → 9 CHAR_LOADING (prints, and waits for someone else to clear it). A friend
join is, by design, a synthesised accepted invite.

The `GameInvites` handler is the **PressStart screen's** (`sub_82501880`, vt[9] of
0x82073994; its vt[8] `sub_824D83F0` is the START handler): it takes the online object
`R` (= `*(0x82AD6E90)`→vt[9], vtable 0x8208CBE0), asks `R->vt[13]()` for the local
player whose xuid equals the invite record's **invitee** (`R+0x130`), calls
`SetActiveUser(pad)` and `sub_824BEAF8(gameSessionOwner, pad)`, and sets `this+0x21`.
`R+0x130..0x183` is the title's copy of `X_INVITE_INFO` (0x54 bytes: invitee, inviter,
title id, XSESSION_INFO, fromGameInvite), and its only writer is `sub_825C78F0` — the
`XN_LIVE_INVITE_ACCEPTED` → `XInviteGetAcceptedInfo` path — plus the search-by-id
completion for this join.

### Two defects in the shipped title on that path, and the port's arms for them

1. **The invitee slot holds the FRIEND.** The state-6 completion copies
   session+0x328 (friend) into `R+0x130` and +0x330 (self) into `R+0x138`, so
   `vt[13]` finds no local player, and the handler's else branch (`vt[14]` = the
   local player matching +0x138, i.e. us — found) then **dereferences the null it
   just tested** (0x82501BBC: `lwz r11,0(r30)` with r30 = 0 and `this` folded to 0 —
   the compiler's treatment of undefined behaviour after a null check). On the
   console this is a crash too; the JoinGame screen was cut from Case Zero, so the path
   never ran there. `CZ_COOP_FRIENDS_INVITEINFO=1` swaps the two xuids when the
   handler runs with `vt[13]` null and `vt[14]` set — the record the console's guide
   delivers for "join session in progress" — and the flow proceeds: `SetActiveUser`,
   states 5-9 (runs 6+).
2. **The second `GameInvites` is posted to nobody.** State 8 posts the moment the
   frontend state reads 0xE, and by then the PressStart screen has handed the sink's
   top-level target to the main menu (`TitleScreen`, vtable 0x820739E0, handles
   Achievements/OnLeaderBoard, not GameInvites): the sink reports `handled=0`
   (`sub_827F01B8` traced), the machine sits in CHAR_LOADING, and the only thing that
   ever took the event was the PressStart screen coming back after the 100 s idle
   timeout (run 11: `handled` at 211 s, then nothing). Re-dispatching it every frame
   (built in) is not taken by anyone at frontend state 0xE; handing it to the surviving
   PressStart object directly (`CZ_COOP_FRIENDS_DIRECT=1`, run 15) is "handled" without
   `SetActiveUser` and still starts nothing. **What the second handling is supposed to
   do — presumably open GameSelect in an invite mode and join from the record's
   XSESSION_INFO — is the unread half**, and it is where this stops.
3. (Port-side, fixed for good) **`GameplayFlow::Enter` armed a HOST request on the
   invited joiner** — the title's own Enter hosts whenever IS-COOP is set and the
   session is not live (see "The lever"), and so did our hook's reading of it. The
   hook now asks `CoopFriends_InviteState()` (the machine's +0x80) and leaves a
   running invite alone; the title's own Enter still hosts (run 7: `host-requested
   byte = 1` after it), which says the console joins BEFORE the level load, i.e. the
   invite flow must reach the join from the second `GameInvites`, not after loading.

### Where this leaves "join a friend"

* **Shipped behaviour is unchanged**: JOIN FRIENDS → the title's friends screen → X →
  the part-5 filtered search (`ONLY the session hosted by <xuid>`). It is not a random
  search; it looks like one. When the X never reaches the hook (attempt 1 on the
  laptop) the player is left on a friends list that did nothing and backs out into the
  LIVE row, which IS a random search — the two are told apart in the log by the
  `friends screen: joining` line.
* **The native path is 60% run and documented**; finishing it means reading what the
  PressStart handler does on its second `GameInvites` (the `this+0x21` branch and
  `sub_824BEAF8`'s consumer, `gameSessionOwner+0x40`) and getting that to fire at
  frontend state 0xE. It would replace the slot-picker-plus-search with a
  profile-switch-plus-slot-picker; the player would see `Press Start` flash by. Not
  obviously better, and two title bugs deep — parked as `open-items.md` 0zb.
* **A cheap UX fix on the shipped path** is to say what is happening: the search
  dialog's "Searching for host session" could read "Joining <friend>'s game" — a string
  edit in the .bcs (the table is rebuildable since part 92).

**Harness:** `tools/coop_pair_friends.sh` (`NATIVE=1` for the arms). Fifteen logs in
`~/DR2CZ-troubleshooting/coop-native/`. The joiner's press timing: the title screen
accepts START ~30 s in, so `START,NONE,NONE,A,NONE,DOWN,A,NONE,DOWN,A,NONE,NONE,X`.

## Part 8: the save-less guest is dressed 1 s after HIS level is up (2026-09-15)

A player told the operator his guest, on a slower machine, loaded after the host had
already dressed him and spawned invisible. Part 6's host-side dress was a fixed 10 s
from the guest's empty outfit report; the row applier's events land on a player that
does not exist yet and are dropped (the same thing part 6 saw at 3 s). The moment the
host KNOWS the guest's level is up is the guest's `FLOW_COMMAND_READY_FOR_PLAY`: the
flow-command receive handler `sub_8257CDD0` names every command it takes through the
online log's `\t%s` print (`sub_8255B910` from 0x8257CE74, the name-table entry
0x8207B308 for this one), and `online_log.cpp` already hooks that print. It now calls
`CoopOutfit_OnJoinerReadyForPlay()` on that (lr 0x8257CE78, r6 0x8207B308), which
moves the pending dress to 1 s from then; the report-time arm is a 45 s fallback. The
2026-09-12 two-machine log (`issue7/slowhost_0914_003403.log`) shows the order the
trigger relies on: EMPTY report → CONNMESH → FINALIZE_START_LEVEL → `READY_FOR_PLAY`
received → (the old 10 s dress landed after it there) → `RESULT_HOST_SUCCESS`.

**Verification owed:** the same-box pair could not complete a join today (both
`coop_pair_reload.sh` runs reached `LOGIN_STATE_CONNECTED` and then lost the peer
link after ~2 min with the path flapping between 192.168.0.58, 100.85.0.1 and
10.2.0.2 — a VPN interface is up on this box; the 09-14 pair run had not joined either).
The trigger's site is read from the image and from that real log; a two-machine
session with a save-less guest is the test, and its host log must show `the other
player is READY_FOR_PLAY (his level is up): dressing slot 1 in 1 s` followed by the
`SetPart` lines.

## Online tunables (dataflow-bound, gotcha 241)

Loader `sub_824A2470`; bank based at 0x82A57xxx. The knobs we will want:

| addr | name |
|---|---|
| 0x82A57D00 | disable_online |
| 0x82A57D01 | online_is_enable_systemlink (readers 824C3530, 825C93B8) |
| 0x82A57D02 | online_is_systemlink_host |
| 0x82A57D23 | online_disable_coop_triggers |
| 0x82A57B94 | online_num_players |
| 0x82A57B98.. | online_net_sim_latency / lost_rate / lost_type |
| 0x82A57BB0 | online_log.verbose_level |

These are the debug knobs — `online_net_sim_*` for shaking out desyncs, and the
systemlink pair is the LAN path Case West dropped but Case Zero kept.

## Player issue #9: the client places a bike part and the WRONG part ticks off (OPEN, 2026-09-21)

`~/XenonLive/Player Issues/#9 - Dead Rising 2_ Case Zero - pokisal`, filed
2026-09-18 against `v1.1.0-39-g8efd9d1`: *"Key Items desynced and replaced — after the
Client used a key item on the bike most key items were either missing or replaced with
other key items."* The reporter's screenshot is the Case 0-4 HUD in the safehouse
garage with both Chucks standing at the bike, two of the five part slots ticked. The
F8 log is the plain host log: no `[crash]`, no `closesocket`, both players' `[pos]`
within two metres of each other, and nothing else — the capture predates every
instrument this section adds.

### What the title does, read out of the image

Case 0-4's bike is one mission trigger and one mission action. `missions.txt`:

```
cMissionDefinition PrologueBikeParts          # ParentMission = PrologueCase0-4
    cMissionLevelReady PrologueBikeParts-Start
        cMissionOnTrigger ExamineBike1
            InteractButton = "true"   Radius = "5"   Location = "-270.054,4.089,-61.046"
            cMissionSetChuckState TryPlaceItem  { ChuckState = "61" }
```

Chuck state **61** lands at `0x8240AF7C` inside `sub_82409900`
(`cMissionSetChuckState::Execute`), and the whole decision is five hash compares:

```
playerIdx = *(u32*)(actionCtx + 0x10)                 // NOT an argument — a FIELD
actor     = sub_8247B020(world->0x7C, playerIdx)      // the user-player array, 0..3
inv       = sub_8215D330(world->0x78->0x30, actor)    // that actor's inventory block
item      = *(u32*)(inv + inv->0x68 * 8 + 4)          // the SELECTED slot
switch (item->0x100) {                                // GetItemDefHashname()
    hash("WheelPawn")        -> raise "WheelPawnPlaced"
    hash("HandleBar")        -> raise "HandleBarPlaced"
    hash("GasolineCanister") -> raise "GasCanPlaced"
    hash("BikeEngine")       -> raise "FuelTankPlaced"
    hash("BikeForks")        -> raise "BikeForksPlaced"
    default                  -> raise "NoPartsPlaced"
}
```

The five objectives of `PrologueCase0-4` listen for exactly those five event strings,
in the HUD's slot order (wheel, handlebar, gas can, fuel tank, forks). **The mapping
itself cannot desync** — it is the same table on both machines, hashed from the same
image, by the same `h = h*33 ^ (signed char)c` (`tools/name_hash.py` is that hash in
Python, and reverses any hash in a trace against `items.txt`). So "the wrong part
ticked off" means the code read the wrong ITEM, and there are only two ways:

1. **the wrong `playerIdx`** — the machine that ran the action resolved the part out
   of the OTHER Chuck's hands; or
2. **the right player, a different inventory** — the two machines disagree about
   `inv->0x68` or about which item sits in that slot for the replicated player.

They predict different logs, which is what the instrument below is shaped around.

### ANSWERED on two real machines, 2026-09-25: the INDEX is right, the ITEM is not

The two-player reading the section below calls owed has landed, on the operator's own
two machines (host `wivi514` here, joiner `Alaska69420` on czwin, both at `01a0773`,
both `CZ_ITEM_TRACE=1`, the arm `CZ_COOP_TRIGGER_PLAYER` deliberately OFF so this is the
defect and not a fix). Logs: `~/DR2CZ-troubleshooting/issue9/bike_0925_{host,joiner}.log`.

**Mechanism (1) is REFUTED and the candidate fix below is NOT the fix.** Every one of
the seven placement attempts named the same actor on both machines:

```
[item] SetChuckState 61 TryPlaceItem (coop=1 isHost=1) ... -> playerIdx 1   # host
[item] SetChuckState 61 TryPlaceItem (coop=0 isHost=0) ... -> playerIdx 1   # joiner
```

`ctx->0x10` tracks the acting player correctly with two players in the level — 1 when
the client interacts, 0 when the host does, on **both** sides, seven for seven. The solo
run that measured it "0 and never changes" was reading a one-player level, which the
section below already flagged as correct-and-silent; with a real partner it moves. So
the sphere trigger's `updateCtx + 0x10` is not a stale field, `CZ_COOP_TRIGGER_PLAYER=1`
would be a no-op on this path, **and it must not be shipped as a fix for issue #9** —
arming it would retire the report while the defect stayed. (Gotcha 611.)

**Mechanism (2) is CONFIRMED: the two machines hold DIFFERENT ITEMS at the same
inventory slots.** The actor, inventory and item-object addresses are identical on both
sides — the objects are replicated — but the item each address *is* differs:

| item object | host calls it | joiner calls it |
|---|---|---|
| `AABAD210` | `A55F8BAB` BikeEngine | `52EA0EA6` BikeForks |
| `AABACCE0` | `5F8D0521` GasolineCanister | `A55F8BAB` BikeEngine |
| `AABAC518` | `C32E815B` HandleBar | `C32E815B` HandleBar |

and the client's own inventory is a slot short on the host:

```
host:    player 1 ... inv AADAC818 selected 0 -> AABAD4A8 7EFC90B8 (WrenchLarge)   # 1 slot
joiner:  player 1 ... inv AADAC818 selected 0 -> AABAD740 5F8D0521 (GasolineCanister)
                                       slot 1 -> AABAD4A8 7EFC90B8 (WrenchLarge)   # 2 slots
```

The client picked a gas can up; the host never learned of it, so the host still believes
that Chuck holds only the wrench. Hence the decision table, seven attempts in order —
**same actor every time, different part four times out of seven**:

| # | acting | host raises | joiner raises | |
|---|---|---|---|---|
| 1 | player 1 | `NoPartsPlaced` | `GasCanPlaced` | ✗ |
| 2 | player 1 | `NoPartsPlaced` | `GasCanPlaced` | ✗ |
| 3 | player 1 | `NoPartsPlaced` | `NoPartsPlaced` | ✓ |
| 4 | player 0 | `GasCanPlaced` | `NoPartsPlaced` | ✗ |
| 5 | player 0 | `FuelTankPlaced` | `BikeForksPlaced` | ✗ |
| 6 | player 0 | `HandleBarPlaced` | `HandleBarPlaced` | ✓ |
| 7 | player 1 | `NoPartsPlaced` | `NoPartsPlaced` | ✓ |

Row 5 is the reporter's sentence rendered exactly — *"replaced with other key items"*:
one interact, and the two machines tick off **two different objectives**. Row 1 is the
other half, *"missing"*: the client places a part and the authoritative host scores
nothing. And the disagreement runs **both ways** — rows 4 and 5 are the HOST placing,
mis-seen by the client — so this is not "the host cannot see the client", it is that
**item identity within a replicated inventory is not synchronised at all**.

Note also that the host's `RaiseMissionEvent` for `NoPartsPlaced` comes from a different
call site (`lr 8240B0EC`) than a real part (`lr 8240B088`) — the `default` arm of the
hash switch, i.e. the host genuinely fell through all five compares.


#### The operator's own eye on the same session, which names the cause upstream of all of it

The trace above says the host does not know what the client is holding. The operator,
playing it, says **why** — and it is one defect, not four. Their words, 2026-09-25:

> *"A lot of issues happened like being able to get the canister a second time since the
> guest picked it up first it didn't register in picked item in the list it should track
> both player inventory and shouldn't be able to respawn if guest already picked it up.
> And in the picked up stuff only the canister showed up in the picked up stuff for the
> bike since both grabbed it but didn't appear until the first player picked it up."*

So: **a guest's pickup of a world item is not replicated to the host at all.** Every
symptom in this issue falls out of that one fact:

> **RETRACTED THE SAME EVENING — see "The second session" below.** With the pickup watch
> running on both machines the HOST printed `PICKUP player 1 ... (WheelPawn)` in its own
> log: a guest pickup DOES cross the link. The host's copy being short at the bike is a
> CONSEQUENCE; what is wrong is the item's IDENTITY. The list that follows is still what
> the operator saw and still worth reading — only its opening claim is too strong.

- the host's copy of the guest's inventory is short exactly the items the guest picked
  up (the trace's `player 1` holding only `WrenchLarge`, never the `GasolineCanister`);
- the world item is therefore still "not taken" on the host, so **it can be picked up a
  second time** — by the host, or again by the guest;
- the mission's picked-up list only ticks when **the host** takes something, because the
  host is the only machine whose pickups reach the authority;
- and a client placing a part at the bike raises `NoPartsPlaced`, because the part is
  not in the hands the host is looking at.


#### The matched F9 pair: both machines photographed at the same moments

The operator pressed F9 on **both** machines through the session, within seconds of each
other, which turns the screenshots into a two-machine trace of the same events. Guest
captures are on czwin at `%APPDATA%\XenonLive\captures`, copied to
`~/DR2CZ-troubleshooting/issue9/guest_captures/`; host captures at
`~/.config/XenonLive/captures/`.

| time (UTC) | machine | what the frame shows |
|---|---|---|
| 21:00:55 | guest | running at Buck's carrying a **wheel**; `Case 0-4 - Find Bike Parts` five slots EMPTY |
| 21:01:14 | guest | **acquires the GASOLINE CANISTER** — the key-item card |
| 21:01:20 | host | the pawnshop, `Dossier 0-4` five slots still **EMPTY** — the guest's canister, six seconds old, did not register |
| 21:02:58 | guest | **acquires the BIKE ENGINE** — the card, at the Still Creek yard |
| 21:03:05 | host | at the gas pumps **holding a gasoline canister**, and the tracker now carries it — **a SECOND canister, still there because the guest's pickup never registered** |
| 21:06:01 | guest | **acquires the BIKE FORKS** — the card, the same yard |
| 21:08:06 | guest | the **BIKE PARTS notebook: all five NOT FOUND** — after personally picking up four of them |
| 21:08:15 | host | the same notebook page, same state, in French |
| 21:09:22 | host | the garage, both Chucks, the canister on the floor, tracker **three of five** — exactly the host's three placements in the trace |

Rows 2-4 are the whole defect in three frames and they need no code reading: **the guest
takes a key item, the host's mission state does not move, and the item is therefore still
available for the host to take again.** That is the operator's *"being able to get the
canister a second time"* photographed from both ends.

Row 7 is the harder one and it goes further than the trace did: **the guest's own
notebook does not register the guest's own pickups either.** Four key items carried, five
listed `NOT FOUND`. So this is not only "guest → host does not replicate" — the guest's
local mission state does not move on a local pickup, which means a joining player cannot
make bike-part progress at all, by any route.

**One thing NOT established, and it needs a cheap control before anyone reads row 7 as
above.** Row 8 shows the HOST's notebook in the same all-`NOT FOUND` state at 21:08:15,
and the host had certainly picked parts up. Either the notebook's found-state is broken
for both players (a different defect, possibly not even co-op's), or that page means
something narrower than "collected". **A single-player run to the same page settles it
in minutes and nothing below should be built on row 7 until it has been run.** The
HUD tracker and the notebook are two different displays and this session has not
established that they read the same flag.

Also worth noting for whoever opens the replication path: the guest picked up the
**engine** and the **forks** at the same spawn yard, and those are exactly the two items
the two machines disagree about in the trace (`AABAD210` BikeEngine/BikeForks,
`AABACCE0` GasolineCanister/BikeEngine). A spawn point whose item identity is decided
locally, per machine, would produce precisely that.



#### The second session, with the watch on both sides (2026-09-25 evening) — and a correction

The pickup watch ran on both machines. It resolves the mechanism further and **corrects
the reading above in one important way**, so that is stated first:

**"A guest's pickup never reaches the host" is TOO STRONG and is retracted.** The host
printed, in its own log, with its own session bytes:

```
[item] PICKUP player 1 slot  0 (coop=1 isHost=1): item AABACCE0 hash 878FC97B (WheelPawn)
```

Player 1 is the guest. So a guest pickup DOES cross the link and the host's copy of that
inventory DOES gain a slot. The earlier reading was built on the host's copy being short
at the bike, which is true and is a CONSEQUENCE, not the mechanism.

**What is actually wrong is the item's IDENTITY, and it was caught live on both sides
within the same minute:**

| | host (`coop=1 isHost=1`) | joiner (`coop=0 isHost=0`) |
|---|---|---|
| player 1's slot count | **one** | **two** |
| object `AABACCE0` | `878FC97B` **WheelPawn** | `5F8D0521` **GasolineCanister** |
| object `AAC37158` | `878FC97B` WheelPawn, slot 0 | `5F8D0521` GasolineCanister, slot 1 |

The same object address is a different item on each machine — the `AABAD210`
disagreement of the afternoon session, reproduced deliberately and caught in the act
rather than inferred twenty seconds later at the bike.

**And the watch shows a mechanism the bike trace could not have.** Both machines print a
continuous ping-pong of the LOCAL player's item objects between two parallel address
sets, hashes unchanged:

```
REPLACE player 0 slot 0: AABACA48 (SpikedBat) -> AAC36EC0 (SpikedBat)
REPLACE player 0 slot 1: AABAC7B0 (Whiskey)   -> AAC36C28 (Whiskey)
REPLACE player 0 slot 2: AABAC518 (Whiskey)   -> AAC36990 (Whiskey)
... and back again, indefinitely
```

For the local player this is harmless — the same items either way. For the REMOTE
player the same ping-pong lands on objects that hold different items on the two
machines, which is where the identity disagreement becomes visible.

**Stated as an inference, not a finding:** the address pattern looks like two parallel
item-object arrays being alternated, with a remote player's inventory rebuilt from
replicated data each frame and binding the wrong definition. That has NOT been confirmed
in the code — the evidence is the address pattern alone, and the alternative (an
allocator handing the same addresses back in a different order on each machine) predicts
the same trace. Distinguishing them is the next piece of work, and it is a code question,
not another session.

**What it explains, either way:** the guest places a gasoline canister; the host looks up
the item in the guest's selected slot, finds a WheelPawn, and raises the event for that
part or for none. *"Most key items were either missing or replaced with other key
items"* is then the literal truth, and the afternoon's decision table (same actor every
time, the wrong part four times of seven) follows without any further mechanism.

**Also established this session**: nine of the eleven broadcast subtypes were seen live
(0, 1, 2, 3, 6, 7, 8, 9, 10; only 4 and 5 unseen), against the ONE the trace could print
before today.



#### Four placements, both machines, with the held item printed — the complete picture

`~/DR2CZ-troubleshooting/issue9/bike2_0925_{host,joiner}.log`. Same session, four
`TryPlaceItem` interacts, every one naming the same acting player on both sides (1, 1,
0, 0 — the actor agrees, as it did in the afternoon):

| # | acting | host: selected slot -> item | joiner: selected slot -> item | event |
|---|---|---|---|---|
| 1 | guest | slot 0 -> `AABAD4A8` **WheelPawn** | slot 0 -> `AABAD4A8` **WheelPawn** | both `WheelPawnPlaced` ✓ |
| 2 | guest | slot 0 -> **`00000000` EMPTY** | slot 0 -> `AABAD210` GasolineCanister | `NoPartsPlaced` vs `GasCanPlaced` |
| 3 | host | slot 3 -> `AABAC7B0` GasolineCanister | slot 3 -> `AABAC518` HandleBar | `GasCanPlaced` vs `HandleBarPlaced` |
| 4 | host | slot **3** -> `AABAC518` HandleBar | slot **0** -> `AABACCE0` Whiskey | `HandleBarPlaced` vs `NoPartsPlaced` |

**Placement 1 is the most informative row in this whole investigation, because it
WORKED — and it worked exactly.** Both machines resolved the guest's selected slot to
the *same object address*, `AABAD4A8` (pool index 6), holding the same item, and both
raised `WheelPawnPlaced`. So the two machines are not operating in permanently private
namespaces: **they can agree completely, and when they do, the bike works.**

That forces a correction to the reading recorded above.

> **REFINED: "the pool index is not a shared namespace" is too absolute.** Index 6 was a
> `WheelPawn` on BOTH machines and index 0 a `HandleBar` on both; only index 5 disagreed
> (`BikeEngine` on the host, `GasolineCanister` on the joiner). The pool is shared *by
> construction* — same base, same `0x298` stride, same indices — and the machines
> allocate into it **independently**. So they start in agreement and DRIFT as items are
> acquired and consumed in different orders. It is a progressive desync of a
> shared-shaped structure, not two unrelated numberings.

Rows 2-4 then say what drifts, and it is more than the pool:

- **Row 2 — contents.** The host's copy of the guest's slot 0 is `00000000`, *empty*,
  while the guest is holding a gas can. The host has already consumed or never received
  that entry.
- **Row 3 — the slot-to-index mapping.** Both machines read selected slot **3**, and get
  different pool entries: index 1 on the host, index 0 on the joiner. Same slot number,
  different item.
- **Row 4 — the selection itself.** The host believes slot **3** is selected; the joiner
  believes slot **0** is. The two machines no longer agree on which item the player is
  even holding.

**What this means for a fix, stated as a direction and not a design.** Nothing here is
repaired by changing the bike, the trigger, or the player index — all three are correct
on both machines in all four rows. The repairable thing is the divergence itself: the
inventory's contents, its slot ordering and its selected index are each maintained
locally on both sides and never reconciled, and the mission action reads whichever local
answer its machine has. A fix has to make one side authoritative for "what is this
player holding" at the moment the action runs, rather than letting each machine answer
from its own drifted copy. **Which code allocates a pool entry, and what (if anything)
about an inventory crosses the wire, are the two open questions — and both are code
questions answerable without another two-machine session.**

> **THE FIRST IS ANSWERED (2026-09-25) — see "WHO ALLOCATES A POOL ENTRY" below.** An id
> is popped off a 2,048-entry LIFO free-list private to one manager object
> (`sub_8223B000`), pushed back on release (`sub_8221E9C8`), and touched by nothing else
> in the image. It cannot arrive from the network: there is no spawn path that accepts an
> id. The divergence is therefore a predicted consequence of the allocator, not an extra
> defect on top of it.

#### The item pool, and why the same index is a different item on each machine

*(Read the four-placement table above first — it refines this
section's conclusion: the pool is shared by construction and DRIFTS, rather than being
two private numberings.)*

Every item address the watch printed in the second session — twelve of twelve, no
exceptions — lands **exactly** on a 664-byte (`0x298`) stride in one of two pools. That
is arithmetic over the observed set, not a guess, and it is what turns the ping-pong
from noise into a structure:

```
pool A base ...AC518     pool B base ...36990     stride 0x298
index   0  1  2  3  4    index   0  1  2  3  4
```

| observed | pool | index | item |
|---|---|---|---|
| host player 1 slot 0 | A then B | **3** | WheelPawn |
| joiner player 1 slot 0 | A then B | **4** | WheelPawn |
| joiner player 1 slot 1 | A then B | **3** | **GasolineCanister** |
| player 0 slots 2/1/0 | A then B | 0/1/2 | Whiskey, Whiskey, SpikedBat |

Three things follow, and only the third is an inference:

1. **The two pools are MIRRORS.** Within one machine, `A[i]` and `B[i]` always hold the
   same item hash, and an inventory slot's pointer alternates between them indefinitely.
   The indices are shared across players — player 0 holds 0..2 and player 1 holds 3..4 —
   so this is one item-instance array with two backing pools, not one pool per player.
2. **The index IS the item's identity within a machine AT A GIVEN MOMENT, and the two
   machines number them differently.** The time qualifier is not hedging — it was
   measured later in the same session: pool A index 3 was a `WheelPawn` on the host and
   became an `M16` a few minutes afterwards, and index 5 became a `BikeEngine`. **The
   pool RECYCLES**, so the mapping is not stable even within one machine over time. That
   makes the comparison below a snapshot of one moment (a valid one — both machines were
   read within the same minute) rather than a standing table, and it makes any scheme
   that replicates an item by pool index worse, not better: the far side can resolve the
   index to something the near side has since recycled. Index 3 is a WheelPawn on the host and a GasolineCanister on the
   joiner. That is the whole of the "same address, different item" disagreement, stated
   without reference to addresses: **the pool index is not a shared namespace.**
   Anything replicated by index therefore resolves to the wrong item on the far side,
   which is exactly the reported defect.
3. *(Inference.)* The continuous A/B alternation looks like a replication double-buffer
   — an authoritative copy and a received copy, with the slot pointer flipped each
   update. **Not confirmed**, and one observation argues against reading too much into
   it: both mirrors agree on the hash within a machine, so the alternation is not itself
   producing the disagreement. It may equally be an unrelated instance-recycling scheme.

**What this does and does not settle.** It settles that the disagreement is systematic
and index-shaped rather than a corrupted value or a dropped message, and it gives the
next session a precise question instead of a symptom. It does NOT yet name the code that
assigns an index, nor show that an index is what crosses the wire — both are code
questions now, answerable without another two-machine session. The pool base addresses
are per-run and are recorded only to make the arithmetic checkable; the finding is the
STRUCTURE (two mirrors, stride `0x298`, index = identity), not the addresses.

#### WHO ALLOCATES A POOL ENTRY — answered, 2026-09-25

The first of the two open code questions is closed. **A pool entry is an index popped
off a LIFO free-list of 2,048 `int16` ids that lives entirely inside one manager object,
is initialised once at world construction, and is never read or written by anything
else.** Three functions are the whole mechanism, and the census below shows there is no
fourth.

```
sub_821A0970   @ 0x821A0A20   INIT     freeTop = 2047; list[0..2047] = 2047..0
sub_8223B000   @ 0x8223B014   ALLOC    id = list[freeTop--]          (the spawner)
sub_8221E9C8   @ 0x8221ED04   RELEASE  list[++freeTop] = id          (the destroyer)
```

##### The manager's per-id layout, read off the code

`mgr + 8` is the world (the same world the part-5 chain resolves — `sub_8221E9C8` reads
`world+0x78` for the game and `world+0x7C` for the user players, exactly as
`LookupPlayerObject` does). Every per-id array is sized 2,048 and they tile the object
without a gap, which is the cross-check that the bounds below are right rather than
guessed:

| offset | stride | what |
|---|---|---|
| `mgr + 0x0030 + id*4` | 4 | **object pointer table.** `NULL` = the id is free |
| `mgr + 0x2E72` | — | `int16` free-list TOP (the stack pointer) |
| `mgr + 0x2E74 + i*2` | 2 | `int16` free-id stack, 2,048 entries, ends at `0x3E74` |
| `mgr + 0x4D00 + id*4` | 4 | a per-id word; its ADDRESS is handed to the object as `obj+0x210` |
| `*(mgr + 0x6D00) + id*0x298` | **664** | **the item record — the object itself** |
| `*(mgr + 0x6D04) + id*0x4C` | 76 | the aux record |

`0x4D00 + 2048*4 = 0x6D00` exactly, and `0x0030 + 2048*4 = 0x2030` sits below the free
list. The two heap arrays are allocated in the constructor `sub_8224A268` at
`0x8224A4B0` and `0x8224A4C8` with literal sizes `1359872 = 2048 * 664` and
`155648 = 2048 * 76`, so the 664-byte stride the watch measured is stated by the
allocation, not inferred from the addresses.

##### The allocation, instruction by instruction

```
8223B014  lha   r11, 0x2e72(r3)     ; r11 = freeTop            (signed!)
8223B01C  addi  r10, r11, 0x173a    ; 0x173A*2 = 0x2E74
8223B028  slwi  r10, r10, 1
8223B03C  lhax  r24, r10, r3        ; id = list[freeTop]       <-- THE POP
8223B048  cmpwi r11, 0
8223B04C  bge   0x8223b104          ; freeTop < 0 -> assert, return 0
8223B104  addi  r11, r11, -1
8223B10C  sth   r11, 0x2e72(r26)    ; freeTop--
8223B108  cmpwi cr6, r24, 0         ; and 0 <= id < 0x800
...
8223B1E4  r3 = *(mgr+0x6D04) + id*76  -> sub_822D1D30(r3)          = the aux record
8223B1F8  r3 = *(mgr+0x6D00) + id*664 -> sub_8231BB88(r3, world, aux)
8223BB30  addi  r11, r24, 0xc
8223BB38  slwi  r9,  r11, 2
8223BB44  stwx  r31, r9, r26        ; objTable[id] = obj
8223BB50  stw   r11, 0x210(r31)     ; obj->0x210 = &mgr[0x4D00 + id*4]
```

`sub_8231BB88` is a **placement constructor** — it keeps `r3` in `r31` and writes fields
into it out to at least `+0x1EC`, which fits a 664-byte record. So the object address the
inventory watch printed *is* `poolBase + id*664`; the `0x298` stride was never a
coincidence of the heap.

The release is the exact mirror, and it is what makes the recycling visible:

```
8221ECD4  memset(*(mgr+0x6D00) + id*0x298, 0x298 bytes, 0)   ; the record is ZEROED
8221ECEC  memset(*(mgr+0x6D04) + id*0x4C,  0x4C  bytes, 0)
8221ED04  objTable[id] = 0
8221ED08  freeTop++ ; list[freeTop] = id                      <-- THE PUSH
```

(That zeroing is the mechanism behind the earlier note that a released prop's first word
stops being a vtable.)

##### Why this is the answer to the desync, and what it predicts

`sth`/`lha` at `0x2E72` occurs **seven times in the whole 57,822-function image**, in five
functions. Three are the init, the pop and the push above. The other two are read-only and
neither is a network path:

- `sub_82187708` @ `0x8218776C` — a spawn budget: return 1 only if `freeTop >= 10`.
- `sub_82568460` @ `0x8256870C` — a debug stat: `2047 - freeTop`, converted to float for
  display. The number of live items.

And `mulli ..., 664` occurs **twice in the image** — the construct at `0x8223B1FC` and the
zero at `0x8221ECD8`. There is no third site, so there is no second way to turn an id into
a record, and in particular **no spawn path that accepts an id from outside**.

So the id is a pure function of each machine's own local spawn/release history:

1. Both machines initialise the list identically (2047..0), so ids issue **0, 1, 2, …**
   ascending on a fresh world. **That is why the machines start in agreement** — and why
   placement 1 resolved to the same index 6 on both sides and worked exactly.
2. The stack is LIFO, so a released id is the *next* one handed out. **That is the
   recycling** already measured (pool A index 3 was a `WheelPawn` and became an `M16`).
3. Any spawn or release one machine performs and the other does not — a zombie dropping
   an item, a prop broken on one side, a pickup replicated late — **permanently offsets
   the two stacks**. From then on the same id names two different objects, and it never
   re-converges, because nothing ever reconciles the list.

This is a progressive, one-way divergence of a structure that is shared only by
construction. It matches every row of the four-placement table without needing anything
else to be wrong, and it is consistent with the one row that worked being the earliest.

##### What it does NOT settle

It does not show what crosses the wire. It shows only that **an id cannot be what
arrives** — nothing can spawn at a given id — so if items are replicated at all, they are
replicated by something else, and that is still the second open question. It also does not
prove the drift is what the operator saw; it predicts it. The cheap test is an instrument
on `sub_8223B000` printing `(id, object, name hash)` on both machines: if the mechanism is
right, the two logs agree entry for entry until the first unmatched spawn and disagree by
a constant offset thereafter.

#### `CZ_COOP_ACTING_PLAYER` IS REFUTED BY CENSUS — it can never fire (2026-09-26)

**5,820,000 `sub_8247B020` calls on a 240 s roam, 0 out of range.** The arm below is dead
code: the substitution it performs is never reached, in co-op or out of it. Two
two-machine runs printed nothing, and rather than leave that as an absence the hook was
given an unconditional counter (gotcha 151 — an arm with no counter cannot be shown to
have engaged, and "the index is never out of range" and "this hook is dead" are the same
silence). The counter says the hook is emphatically alive and the index is always in range.

**So the reading was wrong somewhere between two instructions that are both real:**
`0x8248B844` writes the constant 9 to `ctx+0x10`, and `0x82409918` loads `ctx+0x10` as
`cMissionSetChuckState`'s player index. What is false is the CONNECTION — almost certainly
link (A) below: the type-9 event object is not what reaches the action as its third
argument. `sub_823E7890` publishes that object to a listener
(`sub_82188488(..., &ev, __FILE__, 51)`); the action dispatcher `sub_82378FA0` is reached
only through vtables; and nothing established that the object the listener eventually hands
an action is the same one. It was flagged as unverified and shipped anyway, which is the
error.

**Three attempts on this defect, all from inference, all refuted by the operator's own
sessions** — the event-substitution arm (the event already agreed), the call-stack acting
player (the dispatch is not synchronous), and this one (the index is never out of range).
What survives every time is the same fact: **the host raises the CORRECT event for the
guest's placement** (`WheelPawnPlaced`, then `FuelTankPlaced`/`GasCanPlaced` on the second
run). The decision is right and something downstream takes the item from the wrong Chuck.

**THE NEXT STEP IS NOT ANOTHER CANDIDATE.** It is to hook the item removal itself and print
which actor it is called on — a measurement that names the culprit instead of reasoning
about which field carries the player. Everything above is what reasoning about that field
produced.

#### THE REFUTED FIX — `CZ_COOP_ACTING_PLAYER` (2026-09-26)

One substitution, at the point of the out-of-bounds load. No network, no guest-memory
write, no change to any in-range path.

##### What it does

Every `cMissionSetChuckState::Execute` whose context carries an **in-range** player
publishes it, thread-locally, for the duration of its own call — and restores the previous
value on the way out, so nothing leaks past the action. The hook on
`sub_8247B020` (`GetUserPlayer`) then does one thing: when it is handed an index outside
`[0, MAX_USER_PLAYERS)` **and** a published acting player exists, it rewrites `r4` to that
player and lets the guest perform its own, now-valid, lookup.

The nesting is what makes it work, and it is a property of the engine rather than
something arranged: **state 61's raise is synchronous.** The objective-event response runs
inside it, on the same thread, before it returns. So the enclosing action's acting player
is still live when the response's `ChuckState = 34` asks for player 9 — and because 9 is
out of range it does not overwrite the published value, it inherits it.

The consumer was verified rather than assumed: the jump table at `0x82043388` resolves
state 61 to `0x8240AF7C` (matching the disassembly already recorded) and state **34 to
`0x8240A930`**, whose first two instructions are
`lwz r3, 0x7C(r31)` / `bl 0x8247B020` — it really does call the lookup with the index from
`ctx+0x10`, and it even tests the result for NULL.

##### Why it is safe, stated as what it cannot do

- **An in-range index is never touched.** The entire trigger-driven mission system — every
  state change in the game that already works — takes the identical path.
- It engages **only** where the title was about to read past the end of its player array,
  which is a defect wherever it happens, co-op or not.
- Nothing is written to guest memory. `ctx+0x10` is left holding its type tag, because it
  *is* a type tag and something else may read it as one; only the argument register of one
  call is changed.
- The published value is restored unconditionally on exit, so an action cannot leak an
  acting player to the next one.

##### The arms

- `CZ_COOP_ACTING_PLAYER=0` — the control. The out-of-range lookup is left alone and the
  title reads past the array, exactly as it ships.
- `=1` (default) — substitute. Prints once per distinct `(bad index -> acting player)`
  pair; a silent fix is indistinguishable from an absent one.
- **`=2` — OBSERVE ONLY.** Print every out-of-range lookup and substitute nothing. This is
  the measurement arm: it answers "does this happen at all, and with which index", and it
  is the first thing to run if the fix is ever suspected of changing something it should
  not.

##### Gates

- `--smoke` OK.
- **Single-player control, observe arm (`=2`): 0 out-of-range lookups** over a 300 s
  `AUTOCHUCK=EXPLORER` roam that reached gameplay. So the fix does not fire during ordinary
  single-player play and cannot change it.
- **The honest limit of that control**: the headless route never places a bike part, so it
  does not show the fix is a null *at the bike* in single player. It almost certainly fires
  there — the objective response builds the same type-9 context offline — and substitutes
  player 0, which in single player **is** the acting player. So the single-player effect at
  the bike is "the out-of-bounds read stops happening and the same Chuck animates", which
  is a correction rather than a change. Stated rather than measured; a solo run to the bike
  would settle it and is the cheap thing to do next.
- State 34's consumer verified by disassembly, not assumed (jump table `0x82043388` ->
  `0x8240A930`, whose first two instructions are the `GetUserPlayer` call).

##### THE ASSUMPTION IS NOT CONFIRMED — read this before trusting the fix

The fix rests on the response running **synchronously** inside state 61's raise, on the
same thread, so the enclosing action's published player is still live. That is **not
established**, and two links in the chain are inference rather than reading:

- **(A)** that the type-9 event object `sub_8248B838` fills is what reaches
  `cMissionSetChuckState::Execute` as its third argument. `sub_823E7890` does not call any
  action itself — it builds a 0x68-byte object with a vtable (`sub_821AD238`, vtable
  `0x820121B4`), tags it 9, and **publishes it** via
  `sub_82188488(mission->0x1C->0x78->0x70, &ev, __FILE__, 51)`, which reads `ev+4`, `ev+8`
  and calls `ev->vt[4]` — a listener publish, not an action loop.
- **(B)** that the publish dispatches inline rather than deferring. The action dispatcher
  is `sub_82378FA0` (identified from the `lr 0x82378FFC` in the operator's own log) and it
  has **no static callers** — it is reached only through vtables — so the call graph cannot
  answer it.

**Measured, and it is not encouraging**: `CZ_COOP_ACTING_TRACE=1` over a 300 s
`AUTOCHUCK=EXPLORER` roam saw **20 objective-event responses and 0 nested
`cMissionSetChuckState`** — `nested states 0, of which out of range 0`. That is consistent
with *either* the responses on that route having no `SetChuckState` child (most objectives
do not) *or* the dispatch being deferred. **The instrument cannot tell those apart**, so
this is not yet evidence against the fix — but it is not the positive control the fix
needs either, and it is the shape of gotcha 151: an arm with no counter cannot be shown to
have engaged.

**The one cheap thing that settles it is a single placement by the guest.** If
`[acting] user-player lookup asked for index 9 ...` prints, both (A) and (B) hold and the
fix is engaged. If it does not print, the response is deferred and the fix must be rebuilt
around the queue instead of around the call stack. **One part is the whole test — not
five.**

##### Known limits, said out loud

The published value is trusted because it is in range, and `ctx+0x10` is a **type tag** in
the objective-event context. If some other context type's tag happened to fall in
`[0, 4)`, that action would publish a bogus acting player. That would be wrong — but it is
not a regression, because the title uses that same value as a player index today. The two
tags actually observed are 9 and 10 (`0x8248B844`, `0x8248B85C`), both out of range.
#### THE RESPONSE CODE, AND THE DEFECT: `cMissionSetChuckState` READS A CONTEXT TYPE AS A PLAYER INDEX (2026-09-26)

The operator's instruction was *"find the response code then"*. It is found, the chain is
complete from the script to the out-of-bounds load, and it accounts for every symptom
they reported — including why the host's own placements work.

##### The response is DATA, and it is in the game's own missions.txt

`data/datafile.big` -> `missions.txt` (222,152 bytes decompressed), line 6970:

```
cMissionObjectiveEvent GetWheelPawn
{
    EventString = "WheelPawnPlaced"
    cMissionSetChuckState PlaceItemAnimation11 { ChuckState = "34" }
    cMissionSendAudioEvent EmotePos       { AudioEvent = "ChuckEmotePositive" }
    cMissionTimer WaitforPlacement7
    {
        DeltaTimeSecondsRealTime = "0.5"   TriggerXTimes = "1"
        cMissionSendCommandToProp Destroy3 { PropCommand = "17"  PropName = "WheelPawn" }
    }
}
```

`HandleBarPlaced`, `GasCanPlaced` and the rest are identical in shape. So the response
that puts the part on the bike is: **play Chuck state 34 (the place animation), emote, and
half a second later destroy the world prop by NAME.** Note `PropName` — the prop is found
by name, not by pool index, so the item-pool drift is *not* what breaks this one.

##### The dispatch chain, all read from the image

```
0x8240B084  state 61: sub_821AFE48(missionMgr, <event hash>, 0)   <-- r5 = 0, NO PLAYER
0x821AFE48  RaiseMissionEvent: two listener lists (+0x1404/+0x15E8, +0x15F0/+0x17D4)
              -> sub_821AD6B0(mission, hash, param, listIndex) for each
0x821AD6B0  per-mission: walks a linked list, RTTI-checks each node, then
              -> sub_823E7890(objective, mission, hash, param)
0x823E7890  0x823E7900  hash the objective's EventString; 0x823E7904 compare
            0x823E790C  bl 0x825530D0          <-- IsHost()
            0x823E7914  beq -> RETURN           <-- NOT HOST: THE WHOLE RESPONSE IS SKIPPED
            0x823E7920  sub_821AD238(&ctx)      <-- a stack context
            0x823E7934  sub_8248B838(&ctx, param, mission, objective)
0x8248B838  stw r4,0x14(ctx)   ; param
            stw r5,0x60(ctx)   ; mission
            stw r6,0x64(ctx)   ; objective
            stw 9, 0x10(ctx)   <<<< ctx+0x10 = 9, A CONTEXT TYPE TAG
```

And the consumer, `cMissionSetChuckState::Execute`:

```
0x82409910  lwz r11, 0x40(r3)   ; the action's ChuckState
0x82409918  lwz r4,  0x10(r5)   <<<< THE PLAYER INDEX COMES FROM ctx+0x10
0x8240AF80  bl  0x8247B020(world->0x7C, r4)
```

**The same offset is a player index in one context and a type tag in the other.** From a
trigger, `ctx+0x10` really is the acting player — measured 0/1 correctly across three
sessions, which is why the player index kept clearing. From an objective event it is
**9**, the context's type id (the neighbouring constructor at `0x8248B850` writes 10, and
`0x8247B108` bounds a different enum at 10, so 9 is an ordinary member of that enum and
not a sentinel).

##### And the load is out of bounds, with the guard rail disabled in a shipped build

```
0x8247B034  if (index < 0)  goto assert
0x8247B03C  if (index < 4)  goto lookup        ; MAX_USER_PLAYERS = 4
0x8247B0A8..0x8247B0EC  the assert: "index >= 0 && index < MAX_USER_PLAYERS",
                        actormanager.cpp:749 — GATED ON THE 0x829EC974 DIAG BYTE
0x8247B0F0  addi r11, r28, 3 ; slwi r11,r11,2 ; lwzx r3, r11, r27
            return *(players + (index + 3) * 4)   <-- REACHED ANYWAY, EVEN FOR 9
```

**The assert falls THROUGH to the same load.** In the shipped build the diag byte is 1, so
the assert neither prints nor traps (gotcha 266 — the release kill switch), and
`sub_8247B020(players, 9)` quietly returns `*(players + 48)`: **five entries past the end
of a four-entry array.** Whatever object sits there is what the place animation is applied
to.

##### Why this accounts for every symptom, including the one that made it look intermittent

- *"it makes the first player drop his currently held item"* — state 34 is applied to
  whatever `players[9]` reads, which is not the acting player. On the host, the item that
  comes out of a Chuck's hands is not the guest's.
- *"it appears next to the bike but is not added to the bike parts"* — the acting player's
  item is never consumed by the animation, so it ends up in the world; the prop-destroy
  fires 0.5 s later against a prop found **by name**.
- *"everything works fine if it's the host doing it"* — the wrong answer is a fixed
  address, so when the acting player *is* the one the wrong answer lands on (or is
  harmless to), nothing looks broken. **This is also why the whole defect is invisible in
  single player**, where there is one user player and no second Chuck to take an item
  from. *(This last step is the one inference left in the chain: that `players[9]`
  resolves to something benign for player 0. It is one print to settle.)*
- The **host-only gate at `0x823E790C`** is why the joiner sees nothing happen on its own
  screen: its copy of the response returns before doing anything.

##### What this retires, and what the fix is

It retires the whole *decision* side as a suspect. State 61 resolves the acting player
correctly, reads the right item, and raises the right event — measured on both machines,
three sessions. **`CZ_COOP_ITEM_SYNC` was aimed at a part of the system that was already
correct**, which is exactly what gotcha 611 warns about and the second time this
investigation has been caught by it (gotcha 612: the place a defect is visible is not
where it lives).

**The repair is one substitution and needs no wire.** State 61 already knows the acting
player, and the raise it makes is synchronous — the response runs inside it, on the same
thread. So: remember the acting player around state 61's raise, and when
`cMissionSetChuckState` is handed a `ctx+0x10` that is **out of range for
MAX_USER_PLAYERS**, use the remembered player instead of letting the out-of-bounds load
happen. Nothing else changes; an in-range index is untouched, so every trigger-driven
state change behaves exactly as it does today.

Predictions it must be judged on: the guest places a wheel, **the guest's** Chuck plays
the place animation, the **host keeps** his held item, and the wheel is added to the bike.
And the arm must be a null in single player, where no index is ever out of range.

#### THE FIX IS REFUTED BY THE OPERATOR'S RUN (2026-09-26) — and the run names the real mechanism

`CZ_COOP_ITEM_SYNC` is **OFF by default as of this entry**. It is kept as an arm, because
the channel and the published field are sound and are what a real repair will need, but it
is not the fix and must not be quoted as one.

##### The refutation, in one row

Both machines ran six `TryPlaceItem` actions, in the same order, agreeing on the acting
player every time (1, 1, 0, 0, 0, 0 — the index is *still* correct, for the third session
running). Their raises:

| # | acting | host raised | joiner raised | agree? |
|---|---|---|---|---|
| 1 | guest | `WheelPawnPlaced` | `WheelPawnPlaced` | **✓** |
| 2 | guest | `NoPartsPlaced` | `GasCanPlaced` | ✗ |
| 3 | host | `BikeForksPlaced` | `GasCanPlaced` | ✗ |
| 4 | host | `GasCanPlaced` | `HandleBarPlaced` | ✗ |
| 5 | host | `HandleBarPlaced` | `NoPartsPlaced` | ✗ |
| 6 | host | `NoPartsPlaced` | `NoPartsPlaced` | ✓ |

**Row 1 is the refutation and it cost nothing to read.** The guest placed a wheel, *both
machines raised `WheelPawnPlaced` for player 1* — the mission event was already in
agreement, with no help from the arm — and the operator reports the wheel **appeared next
to the bike and was not added to the bike parts.** So a correct, agreed mission event does
not produce the placement. The whole premise of the arm ("make the two machines raise the
same event and the bike works") is dead, independently of whether the channel delivered.

That premise came from placement 1 of the 09-25 session, which *did* work and *did* agree.
The inference "they agreed, therefore agreeing is sufficient" was a single sample
(gotcha 133) and this run is the second: agreement is **necessary and not sufficient.**

##### What the operator's three symptoms name

1. *"gives some random key item like the shed key when the co-op partner grabs one of the
   bike parts"* — **THIS IS THE MOST VALUABLE OBSERVATION IN THE INVESTIGATION.** The far
   machine materialises a *different key item*. So a pickup **does** cross the wire, and it
   crosses as an **identifier the far side resolves to the wrong object**. That is the LIFO
   free-list drift, caught in the act: `objTable[id]` on the far machine holds the shed key
   at the id the near machine used for a wheel.
2. *"it appears next to the bike but is not added to the bike parts"* — the item is
   released into the world instead of being attached. The attach did not happen even though
   the event was raised.
3. *"it makes the first player drop his currently held item"* — the **effect was applied to
   the local player**. On the host, the guest's placement took the *host's* item out of the
   host's hands.

> **RETRACTED IN PLACE: "Case Zero's co-op layer never replicated item pickups at all — a
> feature that was never written."** That inference (from the named event vocabulary having
> no item entry, and from none of the eleven broadcast subtypes carrying one) is **wrong**.
> Symptom 1 is a replicated pickup arriving and resolving to the wrong object. It was a
> reasonable reading of an absence, and an absence in a vocabulary is not an absence in the
> wire (gotcha 25 again). The pickup path exists and has not been found yet.

##### State 61, read properly this time

`0x8240AF7C`, and the correction matters: the action **decides and raises, and its effect
path takes no actor at all.**

```
8240AF7C  r3 = world->0x7C                     ; user players
8240AF80  bl  0x8247B020(players, playerIdx)   ; the acting actor  -- CORRECT
8240AF90  r28 = game->0x5C                     ; mission manager
8240AF94  bl  0x821A6C18(game->0x30, actor)    ; -> THE HELD ITEM
8240AFA8..B064  five hash compares on item->0x100
8240B084  bl  0x821AFE48(r28, <event>, 0)      ; the raise
8240B094  r3 = game->0xBC
8240B0A0  bl  0x821CF0E0(game->0xBC, 7)        ; <-- NO ACTOR
8240B0A4  bl  0x82443DC0() -> vt[0x1C](r3, "BikePartPlaced", 0)   ; <-- NO ACTOR
```

`sub_821A6C18` is exactly `sub_8215D330` plus the selected-slot arithmetic, so the
documented two-step model and the instrument that replicates it are both correct — a
theory that the instrument read a different object than the game is **refuted**, not
merely unproven.

**The item is never removed and never attached anywhere in state 61.** So both happen in
the mission's *response* to `WheelPawnPlaced` / the new string `BikePartPlaced`
(`0x8205B61C`), and that response is where an actor is resolved — or, on the evidence of
symptom 3, is not resolved and the local player is used instead. `world+0x80` is the
standing candidate for that field.

**THE NEXT WORK IS THE RESPONSE, NOT THE DECISION**: find the mission action that reacts to
`BikePartPlaced`/`WheelPawnPlaced`, and read how it picks the player whose item it takes.
Everything about the decision side is now measured and correct on both machines.

##### Three blind spots in my own instrument, and they are why this run was hard to read

Fixed in the same commit, and each is gotcha 25 in miniature:

1. **The receive half logged nothing.** "The peer is publishing and we are filing it" and
   "nothing has ever arrived" printed identical logs. It now says so once, then on change.
2. **Silence was ambiguous.** "Both machines agree" and "the peer holds something that is
   not a bike part (including nothing)" both printed nothing, so the host's six silent
   placements could not be told apart. Each case now prints.
3. **The publish line only printed when a peer was reached**, so a dead outbound channel
   published invisibly. The host logged **6** `coop-link broadcast` lines for a whole
   session that should have produced thousands — so the channel was mostly not connected,
   and *that* was invisible too.

Because of (1) and (2) the honest verdict on the transport is **unknown**, not working and
not broken. It does not change the refutation, which rests on row 1 alone.

#### THE FIX — `CZ_COOP_ITEM_SYNC`, one field across the link (2026-09-25)

Built, gated and self-tested; **not yet run on two machines**, which is the one thing
owed. `CZ_COOP_ITEM_SYNC=0` is the control arm and restores the shipped behaviour
exactly.

##### What it repairs, and what it does not

It repairs **the decision** — which bike part state 61 decides was placed — and nothing
else. That is the defect the operator's notebook screenshot shows (three of five slots
ticked, exactly the three the HOST placed) and the one the four-placement table measures.

It does **not** repair the inventories. The host's copy of the guest's bag is still
short, the pool indices still drift, and **an item the guest picked up can still be
picked up a second time by the host** — that is world-item replication, a larger subject
this does not open. Do not read a successful bike as evidence that the drift was fixed;
read it as the drift no longer being *consulted* at the one place it was visible.

##### The shape

The machine a player is local to is always right about what that player holds: it owns
the input, the pickup and the inventory. So each machine publishes one field — the name
hash of its own player's selected item — every 200 ms, and when state 61 runs for a
player who is remote *here*, the answer computed from the local copy is replaced by the
owning machine's.

**State, not an event, and that is the load-bearing choice.** The obvious design sends
"I placed a WheelPawn" at the moment of the placement, and it races: our datagram and the
title's own trigger-fire event travel by different mechanisms, so the far machine can run
the placement before the answer arrives. Publishing the held item *continuously* has no
such moment — the value is already there when the placement runs, a lost datagram costs
one tick of freshness, and there is no ordering to get wrong.

##### The transport, which cost one branch

There was no need for a new socket, a new connection or a new thread. Every guest
datagram is already framed with a source and destination **guest port** in front of the
payload and carried over libxlive's one punched socket (`xlive_net.cpp`, "THE PORTS
TRAVEL WITH THE BYTES"), and a datagram addressed to a port no guest socket has bound is
dropped — with a log line. So a reserved port (`0xCF01`, `coop_link.h`) is a whole
channel: same socket, same punched path, same NAT hole, invisible to the title. If the
title ever binds it, the receive path says so and the channel stands down rather than
eating the title's packets.

##### The three guards on the substitution

Each is the difference between a repair and a new defect:

1. The raise must be one of the six events state 61 can produce, so an unrelated mission
   event raised inside the same call is untouched.
2. The remote value must name one of the five parts, so a peer holding nothing — or
   holding a katana — cannot cause a placement.
3. It must actually change the answer, so the ordinary agreeing case is silent.

**The substitution is one-way, and that is a safety property.** It can turn "no part" or
"the wrong part" into a named part; it can never turn a part the local machine recognised
into `NoPartsPlaced`. So the worst case of a wrong reading here is the behaviour that
already ships, and the failure mode cannot be "the fix removed a placement that used to
work".

##### The assumption, stated and checked

The host's Chuck is index 0 and the joiner's is 1, in the same numbering on both machines
— which is what the `[pos]` lines measured, and why the player index was cleared as a
mechanism in the first place. It is an assumption all the same, so `CZ_COOP_LOCAL_PLAYER`
overrides it, and a message from a peer claiming **the same side of the session as this
machine** is refused loudly: two machines that both believe they are the host would both
publish index 0 and silently overwrite each other's view.

##### Why there is a self-test, and what breaking it proved

Every guard above is on a path that cannot run on one machine, so without a test the
whole receive half would ship unexecuted until two people sat down to play.
`CZ_COOP_ITEM_SYNC_TEST=1` runs the contract offline — tables, encoding, sequence order,
bounds, the side check — and **it was verified by breaking all four guards on purpose:
five distinct failures, then clean again when restored** (gotcha 30).

One of those breaks is worth recording, because it nearly shipped as an untestable guard.
The player-index bound was originally checked by reading the four slots — and a *broken*
bound writes **past the end of the array**, where no in-range value test can see it. The
test now counts messages FILED, so a guard's failure is observable as a number rather
than only as a wrong value.

##### What to look for on the two-machine run

```
[itemsync] this machine is the HOST | JOINER
[itemsync] publishing player N's held item to 1 peer(s) every 200 ms
[itemsync] player N now holds 878FC97B (WheelPawn)
[itemsync] placement by player 1 (remote here): this machine read F574775A
           (NoPartsPlaced) out of its own copy, player 1's own machine says
           5F8D0521 (GasolineCanister) -> raising D4AF6D06 (GasCanPlaced)
```

**The prediction, so a run can refute it:** with the guest placing all five parts and the
host placing none, the host's notebook should tick all five. Before this, it ticked only
what the host placed. If the substitution line never prints, the channel is not
arriving — check that both machines run this build, and that the `HOST`/`JOINER` line
disagrees between them.

#### The broadcast-event wire, decoded (2026-09-25)

The listener is `sub_82245650(listener, header, event)`. It accepts exactly one event
CLASS — `*(u8*)(header + 5) == 0x68` — and switches on a SUBTYPE at `event + 0x10`,
bounded at 10, through a byte index table at **`0x820099C8`** into handlers based at
**`0x822456B0`**. Eleven subtypes, eleven distinct handlers:

| # | handler | what it does |
|---|---|---|
| 0 | `822456B0` | **the trigger fire** — `sub_823B0068(trigger, player)`, player at `event+0x14`, trigger at `event+0x18`. The only one the trace decoded before today |
| 1 | `822456F4` | `world->0x78` vt[0x1E8], args `event+0x1C/0x20/0x24/0x28` |
| 2 | `82245714` | `world->0x78` vt[0x1EC], same four args |
| 3 | `82245734` | `sub_8223E018(event+0x2C, player, event+0x30, event+0x31)`, then `world->0x78` vt[0x2C0](.., `event+0x2C`, **1**) |
| 4 | `82245758` | `sub_8223E2F0(...)` then the same vt[0x2C0] with **0** — the sibling of 3, so 3/4 are an on/off pair |
| 5 | `82245798` | `event+0x34`, `event+0x38`, compares `event+0x14` against 4 |
| 6 | `82245808` | `sub_8223D530(game->0x38, player, event+0x3C, event+0x40)` — and BOTH of those last two are bounds-checked against **4**, so this message carries **two player indices** |
| 7 | `82245824` | compares `event+0x4C` against the constant **`0x00014C09`**, and on a match calls `sub_825399B0`/`sub_8253CD70` on the online object at `0x82A69CD4 + 8`; then `sub_82482AD8(listener, player)` and vt[0x1FC] |
| 8 | `822458F0` | **the function's own exit — a NO-OP.** The host received subtype 8 in the 09-25 session and did nothing with it |
| 9 | `822458A4` | `event+0x60`, `event+0x64`, then `sub_82379620(event+0x64, listener, player)` |
| 10 | `822458CC` | the sibling of 9, same two fields |

**Seven of the eleven were seen live within minutes of a session starting** (0, 1, 2, 3,
6, 8, 9), which is the point: **the trace before today printed subtype 0 and nothing
else**, so six live message types had been invisible to every co-op session this project
has run. That is gotcha 25 exactly — the filter could not match, so its silence was read
as "nothing else happens".

`game->0x78` is the object whose `+0x30` is the INVENTORY MANAGER the bike path walks;
subtype 6 reaches into the same object's `+0x38`. That adjacency is a lead and not a
finding — nothing here has yet been shown to carry an item.

**What this does NOT yet say.** None of the eleven has been identified as an item or
pickup message, and the engine's named event vocabulary (`TYPE_EVENT_OUTFIT`,
`TYPE_EVENT_PLAYER`, `TYPE_EVENT_CLIENT`, `TYPE_SYNC_POINT`, `TYPE_FLOW`,
`TYPE_CINEMATIC`, `TYPE_EVENT_SYNC_FREE_ZOMBIES`, `TYPE_HOST_MIGRATE`, `TYPE_READY`,
`TYPE_END_OF_GAME`, `TYPE_GAMEDATA`, `TYPE_GAMEINFO`, `TYPE_MM_PARAMETERS`) has **no
entry for items or inventory**. If that holds after the subtypes are identified, the
conclusion is that Case Zero's co-op layer never replicated item pickups at all — a
feature that was never written rather than one that broke — which is consistent with a
single-player title whose co-op path no one ever ran with two inventories.

**This reframes the fix.** The bike is not where the defect is — it is merely where it
becomes visible, because Case 0-4 is the one mission that reads an inventory slot's item
identity and branches on it. The subject is world-item pickup replication, and the
bike's five slots are just its most legible symptom. (Gotcha 612.)

**Evidence in hand**: five F9 captures with screenshots,
`~/.config/XenonLive/captures/20260925-2101*` .. `-2109*`. The last (21:09:22) is the
decisive picture — the garage, both Chucks, the gas canister still sitting on the floor
beside the bike, and `Dossier 0-4 - Pièces de moto` showing **three** of five slots
ticked. Three is exactly the number of times the HOST placed a part in the trace. The
guest's four attempts are the two empty slots.

**Not yet instrumented, and it is what the next session needs**: nothing in
`CZ_ITEM_TRACE` watches a pickup. It traces the bike path only, so the pickup claim above
rests on the operator's eye and on the inventory shortfall the trace measures downstream
of it. An instrument on the pickup/despawn path — the world item, which player took it,
and whether that crossed the link — would turn this from an inference into a measurement,
and it is the same shape as the hooks already in `coop_items.cpp`.

**Where this leaves the fix.** The defect is upstream of everything this section decoded:
not the trigger, not the action context, not the hash table, but the inventory replication
that gives each machine its own answer to "what is item `AABAD210`". That is a different
and larger subject than the one-store arm below, and it has not been opened. What is owed
now is the item-replication path — how an inventory slot's item definition crosses the
link — and this pair of logs is the specification for it, because it names four concrete
disagreements with addresses on both sides.

### Why (1) WAS the standing suspicion: two sibling triggers disagree

**RETRACTED 2026-09-25 by the two-machine reading above — mechanism (1) does not
happen.** The reasoning below is kept because it is a correct reading of the image and
because it is what the instrument was shaped around; only its conclusion is wrong. The
field it predicts would be stuck at 0 is measured tracking the acting player on both
machines.

`cMissionOnTrigger::Update` (`sub_823E79B8`) and `cMissionOnTriggerCuboid::Update`
(`sub_823E7C48`) are the same routine twice. Both loop `r27/r28 = 0..3` over the user
players, test each one's position against the volume, and fire. **They do not fire
with the same thing:**

| | fires `sub_823B0068(trigger, X)` with |
|---|---|
| `sub_823E7C48` cuboid | `X = r28` — **the loop index, i.e. the player who is inside** |
| `sub_823E79B8` sphere | `X = *(u32*)(updateCtx + 0x10)` — a FIELD of the mission update context |

`ExamineBike1` is a sphere trigger. And `sub_823B0068`'s argument is what ends up in
the action context the state-61 code reads. A solo run measured that field: it is
**0 and never changes** (`CZ_ITEM_TRACE=1`, prologue route, 2026-09-21) — correct with
one player and silent about two. If it is still 0 on the host while the partner is the
one in the volume, every bike part either player places is resolved out of **player
0's** hands, which is exactly "it gives other key item completed instead of the one
that was given" and also explains "most key items missing or replaced" (the host's own
key item is consumed each time).

Not yet established, and stated as such: which of the three callers of `sub_823B0068`
actually fires for an `InteractButton` trigger. The sphere `Update` explicitly skips
its own auto-fire when the trigger's `+0x5B` is set (`InteractButton`), so the live
path is probably the third caller, `sub_82245650` — the broadcast-event listener,
whose subtype 0 carries a player index at `event+0x14` and a trigger at `event+0x18`.
The instrument prints the caller's `lr`, so one co-op session names the path.

### And the argument the fire is given is DEAD — true, and it does not matter

Following the fire down settles what mechanism (1) would have to be. `sub_823B0068`:

```
ctx = sub_823A4768(trigger)          // = mission->0x104, the mission ACTION CONTEXT
trigger->vt[0x2C](trigger, ctx->0x1C /*world*/, ctx, playerIndex, 0)
```

`vt[0x2C]` is `sub_823A4878` — the vtable fragment ending in `cMissionOnTrigger::Update`
at `0x8204E81C` puts it at `+0x2C` of a table based at `0x8204E7D4`, immediately above
the `missionontrigger.cpp` string — and `sub_823A4878` is the action-list walk: for
each action node, `node->vt[0x1C](node, world, ctx, playerIndex)`.

**`cMissionSetChuckState::Execute` does not read that fourth argument.** Its prologue
is `r31 = world; r4 = *(u32*)(ctx + 0x10)`, and state 61 uses only those two. So the
player index threaded from the trigger to the action is discarded, and the part is
resolved out of the hands of whoever `ctx->0x10` names.

Measured on a real co-op host (`tools/coop_pair_items.sh`, 2026-09-21, host
`coop=1 isHost=1`, hooks confirmed alive): **`ctx->0x10` is 0 and never changes.**
If the client's interact reaches the host through the broadcast event with
`event+0x14 == 1`, the host still places whatever **player 0** — its own Chuck — is
holding. That is the reported defect exactly, including "most key items missing":
each placement eats the host's key item.

The joiner half of that pair did not land (its `[coop] JOIN attempt N` loop never
left IDLE after the title printed *"Lost connection with server"*; the search DID
find 2 sessions, so this is the join path being flaky on a same-box pair and not the
measurement). **The host-side reading stands on its own; the joiner-side reading is
owed.**

### The candidate fix, built and OFF — and now REFUTED: `CZ_COOP_TRIGGER_PLAYER=1`

**Do not ship this as the issue-#9 fix.** Its pre-registered prediction — *"with two
players at the bike, the client places a part and that part ticks off; without the arm,
the host's own held item ticks off"* — was tested on 2026-09-25 and the CONTROL half
of it is already false: without the arm the host acts as the client, not as itself. The
arm writes a value that is already there. The text below stands as the design that was
built; the mechanism it was built for is refuted above.

The minimal shape is to stop the argument being dead: before the fire runs, write the
player index the fire was given into `ctx->0x10`, and restore it afterwards
(`runtime/kernel/coop_items.cpp`). It is one store, scoped to the fire, and it makes
the mission action act as the player who actually triggered it — which is what the
cuboid sibling's spelling already implies the engine meant.

**The prediction, pre-registered:** with two players at the bike, the client places a
part and *that* part ticks off; without the arm, the host's own held item ticks off.
Off by default because the mechanism has one half measured, not two, and because the
same field is read by other mission actions (`sub_823A9450`, `sub_823AC018` compare it
against `world->0x80`, the local player) — changing it changes them too, and that
needs an operator's eye on ordinary single-player missions before it ships.

### The instrument: `CZ_ITEM_TRACE=1` (`runtime/kernel/coop_items.cpp`)

Off by default, every hook a straight pass-through when off, none on the frame path.
It prints, with the raw `coop=` / `isHost=` session bytes beside each line so the side
is evidence and not a label:

- `mission update context player index is now N` — one line per distinct value, from
  `cMissionOnTrigger::Update`. **This is also the positive control**: it runs every
  frame for every mission trigger in a level, so a run that prints nothing else has
  still shown the hooks alive (gotcha 30).
- `TriggerFire trigger %08X playerIdx %d ... lr %08X` — the fire, and which of the
  three call sites it came from.
- `Event subtype 0: player %d trigger %08X` — the broadcast event, the one path that
  carries a player index across the link.
- `SetChuckState 61 TryPlaceItem ... -> playerIdx %d` followed by **every player
  slot's actor, self-index, selected slot and whole 12-slot inventory with name
  hashes** — the two candidate mechanisms side by side, diffable line for line
  between the host's log and the client's.
- `RaiseMissionEvent %08X (%s)` — the answer: which part the title decided.

`CZ_ITEM_TRACE=2` adds every Chuck state and every mission event, not just the bike's.

### The harness: `tools/coop_pair_items.sh`

A same-box host+joiner pair (the part-3 recipe, the two XenonLive identities), both
with the trace on, both driven by AutoChuck so neither is a statue. It needs no bike
and no bike parts: with two Chucks in one level, the context-index line alone settles
mechanism (1). `CASE=3` jumps the host to Case 0-4, the safehouse garage where the
bike is; `CASE=1` is Case 0-2, outdoors.

### What is owed

- ~~**The two-player reading.**~~ **DONE, 2026-09-25** — see the answer at the top of
  this issue. It refuted mechanism (1) and confirmed mechanism (2).
- **NEW, and the whole of what is left: the item-replication path.** Two machines give
  different answers to "what item is object `AABAD210`". Find where an inventory slot's
  item definition crosses the link and why the two ends disagree. The 09-25 log pair is
  the specification.
- ~~**The two-player reading (original text).**~~ Either fix the same-box pair's join (it found the host's
  session and then sat in IDLE) or — better, because it is the reporter's own
  hardware and the reporter has already reproduced it once — ask pokisal to run both
  machines with `CZ_ITEM_TRACE=1`, place one part as the client, and hand back both
  `cz_runtime.log`s. The three lines that answer it are `TriggerFire ... arg playerIdx
  N, action context %08X says M`, the `SetChuckState 61` block with every inventory,
  and `RaiseMissionEvent`.
- **Then the A/B**: the same placement with `CZ_COOP_TRIGGER_PLAYER=1`. Same binary,
  one variable.
- It is a title bug either way (Case Zero shipped without co-op, and `TryPlaceItem`
  and the bike are Case Zero-only content, so this pair was never run with two
  players), so the fix belongs in the port.
- Unrelated but found on the way, and owed its own check: `PumpFallGuard`
  (`runtime/cpu/debug_tunables.cpp`) guards **index 0 only**, on the comment "index 0
  is the local player on every machine". The `[pos]` lines of the 2026-09-14 pair
  disagree — host and joiner print the SAME two positions for indices 0 and 1, so the
  index is a session slot and slot 0 is the host's Chuck on both machines. If that is
  right, the issue-#7 fall guard has never protected a joiner.

## Plan, in order

1. **DONE** — `CZ_ONLINE_LOG`.
2. **DONE — Host kick.** `CZ_XLIVE_HOST=1`; see "The lever, as it turned out". The
   host reaches `LOGIN_STATE_CONNECTED` headlessly with 1 member. No F4 row yet — the
   switch is enough for the two-instance test, and a row belongs with the privacy
   setting once part 4 says co-op is worth shipping.
3. **DONE — Joiner.** `CZ_XLIVE_JOIN=1`; see "The joiner, and the first two-machine
   sessions". Two machines, two Chucks in Still Creek, operator-played.
4. **Content test — MOSTLY DONE (part 4), and the SHIP items DONE (part 5).** The
   menu row, the host's prompt and the friends list are in; hosting rides
   `CZ_XLIVE_COOP=1`; the data patches are in `overlay_gen.cpp`; both release legs
   carry libcurl. Still not exposed: the privacy setting (the prompt's SetToPrivate
   button is the only way to go private mid-session).
   **Content test — MOSTLY DONE (part 4).** Still Creek takes a second Chuck: he
   spawns dressed (the chest piece, part 4), walks, fights, sees the host's case and
   HUD. Tested by the operator on 2026-09-11: a cinematic, a save on the host, items
   picked up and dropped, one side quitting (host re-hosts and continues) — all normal;
   0 `[title:desync]` in that session. **Owed: a mission transition and a crowd** (the
   desync count is the crowd's pass mark). The call notice is closed as "not in this
   build" (§ Part 4). `online_disable_coop_triggers` + `online_net_sim_*` are the debug
   knobs.
5. **If impossible:** revert the host/joiner wiring, keep `CZ_ONLINE_LOG` and the whole
   xlive stack for solo + leaderboards.

For Case West this whole doc's shape transfers — it already got past step 3.

## Player issue #9, part 10: THE RESPONSE IS READ, AND THE PROP LOOKUP IS AMBIGUOUS BY CONSTRUCTION (2026-09-26, overnight)

**Read `docs/coop-part9-kickoff.md` first for the four refutations this builds on.** That
kickoff's §3.1 asked for one thing — *"find what removes the item, by measurement"* — and
said the answer must not be another inference read upward through a call graph. This
session found the removal, read the whole response chain off the image, and then measured
the one step in it that cannot be right. **Nothing here is confirmed on two machines yet;
what is new is that the mechanism now predicts a NUMBER, and the instrument that reads
that number ships.**

### 1. The removal, and the response chain, end to end

`Inventory::RemoveItemAt` is **`sub_821A75B8`**, the exact mirror of the
`InsertItemAt` (`sub_821A7550`) the pickup trace already hooks: it bounds `slot` at 12,
`memmove`s the slots above it down (`0x821A7604`), zeroes the tail, decrements the count at
`inv + 0x64` and — only when its third argument is non-zero — the selected slot at
`inv + 0x68`. It is hooked now (`CZ_COOP_PLACE_TRACE=1`).

But the bike's response does not go through it. Read out of `missions.txt` and the image,
the response to `WheelPawnPlaced` is three actions, and the third is the one that matters:

```
cMissionObjectiveEvent GetWheelPawn { EventString = "WheelPawnPlaced"
    cMissionSetChuckState PlaceItemAnimation11 { ChuckState = "34" }
    cMissionSendAudioEvent EmotePos { AudioEvent = "ChuckEmotePositive" }
    cMissionTimer WaitforPlacement7 { DeltaTimeSecondsRealTime = "0.5"
        cMissionSendCommandToProp Destroy3 { PropCommand = "17"  PropName = "WheelPawn" } } }
```

**`cMissionSendCommandToProp::Execute` is `sub_82408908`.** Identified from the vtable, not
guessed: the class's own name-hash accessor `0x823A8B68` sits at slot 1 of the table at
`0x8204C870`, `cMissionSetChuckState`'s sits at slot 1 of `0x8204D390`, the two tables
agree slot for slot (slots 3-11 are literally the same functions), and slot 14 is
`0x82409900` for SetChuckState — so it is `0x82408908` here. The bytes right after the
table spell `TargetNPCName` and `PropCommand`. Confirmed live: the hook printed
`PROPCMD 22 on prop named "Bike2"`, which is exactly what `missions.txt` declares for the
bike body's own `NonInteractBike` action.

Its lookup, and then command 17:

```
82408BA8  bl  0x8276E398        ; hash PropName, the usual h = h*33 ^ (signed char)c
82408BB8  lwz r3, 0x20(game)    ; the item/prop manager
82408BBC  bl  0x821A2200        ; FindPropByNameHash
...
8240916C  bl  0x822D4870              ; holder = prop->0x1BC ? ... : 0
82409194  game->vt[0x38C](game, holder, prop, 0)   ; TAKE THE PROP OFF THAT HOLDER
824091B4  bl  0x8223BBB8(mgr, 0, prop, ...)        ; RELEASE IT INTO THE WORLD, actor = 0
```

**So the command takes the prop out of whoever's hands the lookup landed in, and releases
it into the world. The acting player is never consulted at any point.** That is the
operator's third symptom — *"it makes the first player drop his currently held item"* —
spelled as two instructions, and the second symptom — *"it appears next to the bike but is
not added to the bike parts"* — as the instruction after them.

### 2. And the lookup decides nothing. `sub_821A2200` is nine instructions

```
r10 = mgr + 0x30                       ; the 2,048-entry object pointer table
for i in 0 .. 0x7FF:
    obj = objTable[i]
    if (obj && obj->0x98 == hash) return obj      <-- THE FIRST MATCH WINS
```

`mgr + 0x30 + id*4` is **the same object table the item pool's LIFO free list hands ids out
of** (see "WHO ALLOCATES A POOL ENTRY" above). So *"find the prop called WheelPawn"* means
**"return the live pool entry with the lowest id whose instance name is WheelPawn"**, and
nothing else is considered — not the holder, not the acting player, not the mission.

**The two name fields, measured rather than assumed.** `+0x98` is the prop's INSTANCE name
hash and `+0x100` its item TYPE hash. The `PROPCMD 22` above resolved
`hash 04E332D7 -> id 147 ... itemHash 19CB1675`, and `tools/name_hash.py` gives
`04E332D7 Bike2` / `19CB1675 BikeBody` — the spawn action's own name and the item type,
exactly as `missions.txt` declares them (`cMissionSpawnItem Bike2 { ItemName = "BikeBody" }`).

**Which makes the bike's five destroy commands point at generic instances, and that is the
finding.** There is **no `cMissionSpawnItem` action named `WheelPawn`, `HandleBar`,
`GasolineCanister`, `BikeEngine` or `BikeForks`** anywhere in `missions.txt` — the five
world spawns are called `WheelPawnWorldSpawn`, `HandleBarRespawn`, `GasCan7`, `FuelTank3`
and `BikeForksWorld4`, and the five decorative ones at the bike are `WheelPilePawn2`,
`HandleBarpile3`, `GasCan4`, `FuelTankPile2` and `BikeForks2`. Those five names appear as a
`PropName` in exactly five places in the whole file: the five destroy commands. So the prop
the command looks for is **the generic instance a player is carrying**, whose instance name
is its own type name.

### 3. The number: in SINGLE PLAYER, at the bike, four names already name two props

`CZ_COOP_POOL_CENSUS_MS=3000` walks the 2,048-entry table and reports any instance name
with more than one live entry. Case 0-4, safehouse garage, solo, 149 live entries:

```
[census] 4 instance name(s) now name MORE THAN ONE live pool entry:
         05B7EDB9 (Nails) x5, 0768AA95 (ChuckWalkieTalkie) x4,
         A1FC9255 (fe_watch) x4, 7EFC90B8 (WrenchLarge) x2
```

and the head of the table shows both kinds of entry side by side, which is the cross-check
on the decode:

```
id    0 obj AAB94D00 instance 05F369B4 (Snack)     type 05F369B4 (Snack)     <-- generic
id    2 obj AAB95230 instance C460A007 (generated) type 05F369B4 (Snack)     <-- scripted
id   11 obj AAB96988 instance 4229A541 (CashRegister_dmg) type 4229A541 ...  <-- generic
```

So a generic instance really is named after its type, duplicates of one really do coexist,
and `FindPropByNameHash` really is resolving an ambiguous name every time one of those is
asked for. **One player carries one bike part of a given type at a time, so single player
is exact where it matters. Two players can carry two** — and the operator measured that
they do, because a bike part the guest had already taken could still be picked up a second
time. Then the destroy takes the part out of whichever Chuck holds the lower pool id, and
releases it on the floor.

**Why the host's own placements work** falls out without a second mechanism: the host's own
instance is usually the older, lower id.

### 4. What is NOT established, said out loud

- **No two-machine measurement yet.** Everything above is one machine plus the image. The
  deciding line is one the operator's next session produces for free (§6).
- **The generic-instance claim is an inference from three measured facts**, not a direct
  reading: that `+0x98` is the instance name (measured), that no spawn action is named after
  a part (read off `missions.txt`), and that generic instances named after their type exist
  (measured). What has NOT been watched is a bike part being picked up and becoming one.
- **State 34 has still never been observed.** The trace prints it now; no run has reached a
  real placement.
- **`world + 0x80` is still a live candidate for something else.** The state jump table at
  `0x82043388` re-verified against the image (77 entries, base `0x82409954`, index =
  `state - 1`; state 61 -> `0x8240AF7C` and state 34 -> `0x8240A930`, both as recorded) says
  **state 35 -> `0x8240A9C0`**, and that handler resolves its actor from `world->0x80`
  rather than from the action context, then calls `sub_8223CEF8` — a twelve-iteration loop
  that empties that Chuck's whole bag into the world. It is the one entry in the table that
  uses a world-global player index. Both are hooked; if a placement reaches them the trace
  says so, and if it does not, the candidate is closed by measurement instead of left open.

### 5. An instrument defect this session found, and it has shipped since co-op part 1

**`cMissionOnTrigger::Update` (`sub_823E79B8`) is entered ONCE in the safehouse.** It was
chosen to drive the inventory watch on the reasoning that it "already runs every frame for
every mission trigger in the level"; the new harness put an unconditional counter on it and
got `0s elapsed, 1 mission updates seen`, two minutes after the level was up. So in a level
whose missions are not trigger-driven, **`CZ_ITEM_WATCH_MS` is armed and silent** — gotcha
30 in the instrument this investigation has leaned on hardest. The driver is
`GetUserPlayer` (`sub_8247B020`) now, which this file's own census measured at 5,820,000
calls in 240 s, throttled to one resolve per 200 ms with a reentrancy guard. The watch
itself still hangs off the old hook; moving it is owed.

### 6. The instruments, and the one line that decides this

| arm | what it is for |
|---|---|
| `CZ_COOP_PLACE_TRACE=1` | the effect path: `RemoveItemAt`, the release into the world, the drop-whole-bag loop, Chuck states 34/35, every `PropCommand` with its `PropName`, and **every `FindPropByNameHash` with a census of how many live pool entries matched** |
| `CZ_COOP_POOL_CENSUS_MS=3000` | which instance names name more than one live prop, on change |
| `CZ_COOP_RAISE_EVENT=WheelPawnPlaced@20` | **a harness**: raise a mission event directly, so the response can be run without carrying a part from Still Creek to the garage. Manufactures a mission event — never a gate run |
| `CZ_COOP_PLACE_FIX=1` | **a CANDIDATE fix, OFF by default.** `=2` observes and changes nothing; `=0` is the control |

**The deciding line, on the HOST, when the guest places a part:**

```
[place] PROPFIND hash 878FC97B (WheelPawn) for PROPCMD 17 "WheelPawn" -> ........,
        N live pool entries match [...ids, holders...]
```

* **`N == 1`** refutes this whole reading in one line, and the `holder` field then says
  whose part the single candidate was.
* **`N >= 2`** names it, says which one the title took, and which Chuck was holding each.

### 7. The candidate fix — `CZ_COOP_PLACE_FIX`, and why it ships OFF

State 61 already knows exactly which item object it decided on: it is `r30` at
`0x8240AF98`, the return of `sub_821A6C18(game->0x30, actor)`. The arm remembers that
object across the mission's half-second timer and, when the prop command's lookup is
**ambiguous**, hands back the remembered one instead of the lowest id. One return value of
one call is replaced; nothing is written to guest memory.

Five guards: only inside `PropCommand = 17`; only when the guest's own search found more
than one live candidate; only when the remembered object is still live AND its own `+0x98`
still equals the hash asked for AND it is still held by someone (the pool is a LIFO free
list, so an id released in between comes straight back as something else); only within
`CZ_COOP_PLACE_FIX_MS` (default 5000, against the mission's 500 ms timer); and one-way — it
can only choose a different member of the set the title was already choosing from.

**The prediction, so a run can refute it:** the guest places a part, the GUEST's Chuck loses
it, the HOST keeps what he was holding, and the part is added to the bike.

**It is OFF because the measurement that convicts or acquits it has not been run, and four
fixes have already been refuted here for exactly that reason.** What makes it worth shipping
at all is that its own log line is the diagnosis: it can only engage where the lookup was
ambiguous, so a session where it never fires has refuted the mechanism rather than merely
failed. Note honestly that guard 2 is **not** a guaranteed null in single player — §3 shows
duplicates are ordinary — but for command 17 it needs two live instances of the *same bike
part*, and there substituting the one state 61 read is more correct rather than less.

### 8. A reproducible headless route to the bike, which did not exist before

The DebugJump "Cases" column lists the three `ShowInDebugMenu` case missions in file order
— `PrologueCase0-1`, `PrologueCase0-2`, `PrologueCase0-4` — so **`DOWN` twice selects
Case 0-4**, whose `LevelToStartAtIfDebuggingMission` is `PROLOGUE_SAFEHOUSE`. Chuck lands
at `(-271.7, 3.3, -64.0)`, about two metres from the bike trigger at
`(-270.054, 4.089, -61.046)`:

```
CZ_NO_WINDOW=1 CZ_VKDRAW=1 CZ_DEBUG_MENU=1 CZ_FAKE_START_MS=8000 \
CZ_FAKE_PRESS_SEQ='F2,START,WAITJUMP,NONE,DOWN,DOWN,A,NONE' \
CZ_ITEM_TRACE=1 CZ_COOP_PLACE_TRACE=1 CZ_COOP_POOL_CENSUS_MS=3000 \
CZ_COOP_RAISE_EVENT='WheelPawnPlaced@20' timeout 400 ./cz_runtime
```

Two things it cannot do, recorded so the next session does not re-try them: the five
**interactable** bike parts spawn out in `LEVEL_PROLOGUE` (Still Creek) and the ones at the
bike are `NonInteractableProp = "true"`, so there is nothing to pick up in the garage; and
**`CZ_AUTOCHUCK="MISSION MASTER"` does not move Chuck at all there** — a 15-minute run left
him on the spawn point. That is why the event harness exists.

### 9. STATE 34 OBSERVED FOR THE FIRST TIME — and every caller of the action dispatcher is a NETWORK EVENT LISTENER

The event harness made the response run on one machine, and the first line it produced
changes the shape of the problem:

```
[raise] raising 6AD9B344 (WheelPawnPlaced) on mission manager AADAD250 at 15s
[item]  RaiseMissionEvent 6AD9B344 (WheelPawnPlaced) param 00000000 lr 82448ABC
[place] STATE 34 ctx 88041094 -> ctx+0x10 = 0, that is player 0 (actor B925ABE0);
        world+0x80 = 0, r6=88040E64, from lr 82378FFC
```

**Three things, in order of how much they change.**

**(a) `ctx+0x10` is 0, not 9 — so part 9's link (A) is refuted by direct measurement.** The
objective-event context really does carry a type tag there (`sub_821AD238` writes 11,
`sub_8248B838` overwrites it with 9, read off the image), and the context the action actually
receives carries **0**. So that context is not what reaches the action, the out-of-bounds
`GetUserPlayer(players, 9)` that candidate 3 was built on genuinely cannot happen, and the
census that refuted it was right for a reason that is now visible rather than inferred.

**(b) `sub_82378FA0` does have static callers, and all three are network event listeners.**
Part 9 recorded it as reached only through vtables, which is why four sessions could not
follow the chain. A census over every `b`/`bl` in the image finds exactly three, each
gated on the class byte at `header + 5` in the same shape as `sub_82245650`'s `0x68`:

| site | class | how it supplies the action's context |
|---|---|---|
| `0x821898E0` (a tail `b`) | **0x6A** | `ExecuteAction(event->0x14, world, event)` — **the received event object IS the context** |
| `0x821899B4` | **0x6B** | a COUNT at `event+0x10`, an array of 0x1C-byte entries at `event+0x14` — several actions per message |
| `0x8218A1B8` (`sub_8218A070`) | **0x6C** | builds a stack context with `sub_8248BE28(ctx, .., 0, event->0x10)`, and `sub_8248BE28` is three stores: `+0x10 = r6`, `+0x14 = r4`, `+0x18 = r5`. **So the player index the action reads comes straight off the wire.** |

So the engine replicates *"run this mission action"* as a message, with a player index in the
payload, and the response to `WheelPawnPlaced` executes through that path — even in single
player, where the message is delivered to the sender.

> **RETRACTED IN PLACE, the same night: which of the three classes delivers it is NOT
> established.** The line that said so — `dispatched from 00000000 (not through
> sub_82378FA0)`, read as "the class-0x6A tail branch, because a tail branch does not pass
> the entry" — was an artifact. The hook on `sub_82378FA0` that sets that field **was never
> added**: the edit that would have added it aborted on an assertion before writing the
> file, and the field was therefore the initialiser, `0`, on every call. A zero from a hook
> that does not exist and a zero from a tail branch are the same number, which is gotcha 151
> in the instrument built to answer exactly this question. Both hooks exist now, each with a
> `hook alive` line, so a later zero is a measurement; the class stays unknown until one
> prints. The three call sites and `sub_8248BE28`'s three stores are read off the image and
> stand.

**(c) Which makes the live question ONE FIELD ON ONE PACKET, and it is now printed.** The
`STATE` line names the dispatching site, so on the host with the guest placing a part:

* **`ctx+0x10 = 1`** — the response is running for the right Chuck and the defect is the prop
  lookup of §2 (which `CZ_COOP_PLACE_FIX` addresses);
* **`ctx+0x10 = 0`** — the response is running for the LOCAL Chuck, so the animation plays on
  the host and the item comes out of the host's hands. That is the operator's third symptom
  exactly, and the fix is then to correct the index rather than the lookup.

The single-player run cannot separate those two: player 0 is both the acting player and the
local player there. **That is the whole of what the operator's next placement decides**, and
it is one line.

> **A note on what this does NOT retire.** §2 and §3 stand on their own — the prop lookup
> really is first-match-by-pool-id and duplicate instance names really do coexist — so if
> `ctx+0x10` comes back correct, the lookup is still the next suspect and its own census line
> is already in the log. The two leads are independent and the same run reads both.

### 10. THE DISPATCH IS MEASURED: a class-0x6B BATCH, and the player index is a field of the record

With the dispatcher hook actually present (see the retraction in §9) the same harness run
reads:

```
[item]  hook alive: sub_82378FA0 mission action dispatcher (its ENTRY; ...)
[place] STATE 34 ctx 88040FB4 -> ctx+0x10 = 0, that is player 0 (actor B925ABE0);
        world+0x80 = 0, from lr 82378FFC, dispatched from 821899B8 (event class 0x6B)
```

`0x821899B8` is the instruction after the `bl 0x82378FA0` inside **`sub_821898E8`, the
class-0x6B listener** — so that is the path, measured, and the `hook alive` line is what
licenses reading the number (the previous reading of this was an artifact).

**What a class-0x6B message is, read off the image.** It is a BATCH of class-0x6A action
records:

```
sub_821898E8(world, header, event):
    if (header[5] != 0x6B) return;
    count = event->0x10;                        // a BYTE
    for i in 0 .. count-1:
        entry = event + 0x14 + i*0x1C;          // guarded on entry->0x08 == 0x6A
        action = entry->0x14;  if (!action) continue;
        ExecuteAction(action, world, entry)     // 0x821899B4
```

**So each 0x1C-byte record carries its own player index at `+0x10`, and that is the field
`cMissionSetChuckState::Execute` reads and hands to `GetUserPlayer`.** The batch object's
constructor is `0x821619D0` — 25 records, queue vtable `0x8200B318`, record vtable
`0x8200B300` — and it initialises each record's `+0x10` to **4**, which is out of range for
`MAX_USER_PLAYERS = 4` and therefore a "nobody" sentinel. **A 0 in that field was written by
whoever queued the action**, so the value is not a default and the question "who wrote it"
is a real one.

`CZ_COOP_PLACE_TRACE=1` prints every record of every batch now — class, player and action —
so on the host with the guest placing a part, the record carrying the place animation states
in one field whose Chuck the response will run for. **If it says 0 while the guest is player
1, the sender wrote the local player, and that is the defect.** If it says 1, the response is
correct and §2's prop lookup is the remaining suspect.

**What is NOT established:** who fills the record. 56 sites in the image build a record with
that vtable — most of them in the mission-action module at `0x823Axxxx` — and the queue's own
enqueue was not found. That is the next static question, and the batch trace narrows it to
"whichever site produced the record that carried state 34", which the `lr` on the BATCH line
plus the record index identifies.

## Player issue #9, part 11: THE PLAYER INDEX IS FIXED AND OPERATOR-VERIFIED — and the remaining half is the FOUND flag, not the placement (2026-09-27)

The operator ran the two-machine session part 10 asked for. **The defect part 10 predicted
was confirmed line by line, the fix for it is verified and on by default, and their own
screenshot then reframed what is left.** Read part 10's sections first; this supersedes
nothing in them except where it says so.

### 1. The defect, caught in one frame on the host

The guest placed a gas canister at the Case 0-4 bike, with the arms on and the fix OFF:

| host log | |
|---|---|
| `SetChuckState 61 ... -> playerIdx 1` | the decision: **correct**, for the fifth session running |
| `RaiseMissionEvent D4AF6D06 (GasCanPlaced)` | the part: **correct** |
| `STATE 34 ... ctx+0x10 = 0, that is player 0` | **the defect** |
| `REMOVE player 0 ... loses D0B48CA7` | = **Broadsword**, the host's held item |
| `REMOVE player 1 ... loses 5F8D0521` | the guest's canister |
| `RELEASE ... actor 00000000 ... lr 824091B8` | the canister into the world — *"appears next to the bike"* |

and the operator, playing the host, without being shown the log: *"the host was holding a
broadsword and when the gas canister was placed it was removed of his hand and placed on
the ground next to him and couldn't get picked up again."* State 34 is the place
animation; run on the wrong Chuck it takes that Chuck's held item and puts it down.

**The batch records say it in one field.** Same frame, two class-0x6B batches:

```
record at 88041294: +0x10 = 1   the TRIGGER's action (state 61)
record at 8803CBE4: +0x10 = 0   the RESPONSE's action (state 34)
```

**And the 0 is not "the local player."** On the joiner `world+0x80` is **1** — its local
player really is 1 — and its record still read **0**. Nothing stamps the acting player
into the response's records at all. That is also exactly why `CZ_COOP_ACTING_PLAYER` could
never fire: it substituted only for indices OUT OF RANGE, and 0 is perfectly in range.

### 2. THE CONTROL PAIR, in one log, one process, one binary

The operator then placed one **solo** in the same session, so both arms are in the same
file with everything else held constant:

| | co-op (guest placed) | solo (host placed) |
|---|---|---|
| state 61 | `playerIdx` **1** | `playerIdx` **0** |
| response `STATE 34` | `ctx+0x10` **0** — mismatch | `ctx+0x10` **0** — match |
| `PROPFIND` candidates | **1** | **1** |
| removals | player 0 **Broadsword** + player 1 canister | player 0 canister **only** |
| operator's eyes | floor, no tick | ***"without issue"*** |

The response's record **always** says 0; solo that happens to be right. That isolates the
defect to one field, and it retires two things at once: **the prop lookup** (one candidate
in both arms — so part 10's `CZ_COOP_PLACE_FIX` is aimed at the innocent half and must not
be quoted as the fix), and **part 9's owed doubt** about whether the tracker was simply
broken for both players, which the solo arm answers with *no*.

### 3. The fix, and the pair of controls it ships with

`CZ_COOP_RESPONSE_PLAYER`, **ON by default** since this session. State 61 resolved the
acting player one call earlier in the same frame; remember it, and when the place
animation arrives claiming 0 while that remembered player is non-zero, answer its one
user-player lookup with the remembered player. No guest memory is written.

**Verified on two machines, twice** — a gas canister and a wheel, both showing
`[respfix] ... SUBSTITUTING` and **no `REMOVE player 0`**.

Default-on is safe as a property, not a hope: a host placement runs its own state 61 first
and records **0**, disarming the substitution before its state 34 can reach it, so a host
placement inside a guest placement's window is not a false positive. And single player
never has a non-zero acting player, so the gate cannot open there.

Both controls exist, which is what four previous attempts lacked: **positive** —
`CZ_COOP_RESPONSE_PLAYER_TEST=1` with the event harness opens the gate and answers the
lookup 1, clean exit; **null** — single player with nothing set reaches state 34, prints
it, and substitutes **0** times.

### 4. WHAT IS LEFT, AND THE SCREENSHOT THAT REFRAMED IT

With the fix on, the guest placed a **wheel** the host had never touched. Both machines:
correct actor, `WheelPawnPlaced` raised, the arm engaged, only the guest's item removed —
and the part still was not credited. The operator captured the bike-parts screen on both
machines (`~/.config/XenonLive/captures/20260927-062223-9f9d/screenshot.png`):

* **`Bidon d'essence` — GREEN.** The gas canister, which the host had accidentally picked
  up first.
* **`Roue` — RED**, with `Roue: MANQUANT(E/S)` and
  ***"Je n'ai pas encore TROUVÉ cette pièce"*** — *haven't **found** it yet*, not *not
  placed*.

**So the missing state is the FOUND registration at PICKUP, not the placement.** A part the
guest picks up is never marked found on either machine, and placing a part that is not
found does nothing. It is the same shape of bug one layer up — something in the
mission/HUD layer counting only player 0 — which is the operator's own reading: *"we should
make the call like the host picked it before but don't add it to his inventory."*

**What this retires:** the destroy/release path. The `RELEASE ... actor = 0` is command 17
doing what it is written to do, and it happens identically in the solo run that works.

**What is NOT established:** what sets the found flag. Leads are `hud_bikeparts` (the
screen in the capture), state 61's own tail `sub_821CF0E0(game->0xBC, 7)` +
`"BikePartPlaced"` (`0x8205B61C`), and the five part missions' decorative spawns — each
part mission starts on its `...Placed` event and half a second later spawns a
NonInteractableProp of that part AT the bike, so `WheelPilePawn2` appearing in the item
pool is the game's own statement that the wheel is on the bike. `CZ_COOP_POOL_CENSUS_MS`
reports APPEARED/GONE by name now, so that is a positive statement rather than the absence
a red row gives.

**Do not build a "fake a host pickup" fix before that flag is found by measurement.** Four
mechanisms died from being inferred, and both of tonight's wins came from watching a store.

### 11. THE FLAG HUNT'S FIRST ROUND — 31 mission-manager fields a HOST pickup writes and a GUEST pickup does not (2026-09-27)

`CZ_COOP_FLAGHUNT=1` with a 500 ms sweep, one session, host picks up a wheel and guest
picks up a gas canister, no placing:

```
host window  (player 0, WheelPawn):        51 distinct fields changed
guest window (player 1, GasolineCanister): 20 distinct fields changed
changed for the HOST and not the guest:    31
changed for the GUEST and not the host:     0
```

**The guest's writes are a strict SUBSET.** It is not doing something different; it is
doing strictly less. And the 31 are almost all in the mission manager:

```
missionMgr+15E8, +15F0, +17D4    the mission-event LISTENER LIST heads, which
                                 sub_821AFE48 walks (identified in part 9)
missionMgr+0AB8 .. +0AE8         13 consecutive dwords
missionMgr+1448, +144C
missionMgr+1648 .. +1664         8 consecutive dwords
missionMgr+17E8, +19C0, +19C8, +0FA0
game+0194
```

So **a host pickup advances mission state — registers listeners, fills arrays — and a
guest pickup touches none of it.** That is the missing write, with addresses.

**And the visible symptom was tied to it in the same session.** The operator then placed
both parts and captured the bike-parts screen
(`~/.config/XenonLive/captures/20260927-154010-f2e1/screenshot.png`): **`Roue:
RAPPORTÉ(E/S)`** in green — *"J'ai trouvé cette pièce et je l'ai rapportée à la
station-service !"* — with the wheel drawn in colour on the bike, and the gas canister
still a red row and a black silhouette. Both parts were placed, both raised the correct
event on both machines, and both spawned their decorative prop; only the one the HOST
picked up is credited.

**Store sites for the touched offsets**, as the next thread to pull: `missionMgr+0x19C0`
is written at `0x821ADE28`, `0x821AE094`, `0x821B0478` and `0x821FAFCC` — all inside the
mission system's own code — and `missionMgr+0x0FA0` at `0x82163714` / `0x821637B4`.
`0x1648` has **no** static store site, so it is written through a computed offset (an
array index), which fits the 8- and 13-dword runs.

**What is NOT the answer, measured this round:** the part MISSION starting. Both
`PrologueWheelPawn` and `PrologueGasCan` demonstrably started — each spawned its
decorative prop at the bike, on both machines — so the screen is not reading "is the part
mission active". `missions.txt` also shows `cMissionCondition Condition = "9"
ConditionParamater = "<MissionName>"` testing mission state (e.g. `HaveItem4` on
`PrologueWheelPawn` destroys the world spawn), which is a real mechanism but not this one.

### 12. THE FOUND FLAG IS NAMED: a prereq that looks in ONE inventory (2026-09-27, unattended)

Part 11 §1 asked for the found flag to be located **by measurement** and forbade building
the "fake a host pickup" fix before that. This is that measurement. Every link below is a
number from a log or a census; the one thing still owed is a positive control, and it is
named at the end.

#### 12.1 "Hook the store sites" could not have worked, and the census says why

The obvious next move after §11 was to hook the code that writes the 31 offsets and print
the caller. `tools/find_field_access.py` (new — a census of every D-form load/store in the
image that touches a given structure offset, with the enclosing function from the
recompiler's own function list) says that cannot work:

```
+0x15E8   9 D-form sites   only 4 in the mission system, all storing ZERO
+0x15F0  13                    "        "        "        "        "
+0x17D4   4                    all four          "        "        "
+0x19C0   4                    all four          "        "        "
+0x19C8   4                    all four          "        "        "
+0x1648   2 (both stfs, a different structure)   -- no mission-system site at all
```

and the four are always the same four: `sub_821ADD18`, `sub_821ADFB8`, `sub_821B0388`,
`sub_821FAF10` — the mission manager's reset and constructor, storing `0`. **Not one of the
increments we measured has a static store site.** The mission manager keeps an ARRAY OF
LISTS addressed by list number (`sub_82163708` does `stwx r11, (count + 0x28F)<<2, r3`), so
every mutator is indexed and carries no displacement. That is also the real reason
`+0x1648` "had no store site" — nothing special about that offset; the whole family is
invisible to a static grep. Hooking the four zeroing sites would have produced a log of
level loads.

**Transferable:** when a measured field has no store site, ask whether the WHOLE
STRUCTURE is indexed before concluding anything about that field. `find_field_access.py`
prints the note "indexed forms carry no displacement and CANNOT appear here" on every run
for this reason.

#### 12.2 The 31 offsets are one object moving between lists, and the guest's own code says so

Read as values instead of as addresses, §11's table is not a set of flags:

```
+0AB8..+0AE8   each slot takes the value of the NEXT slot      erase(front)
+0FA0          0x2D -> 0x2C                                    that list's count--
+1448/+144C    B97C9160 inserted at [0], old [0] shifted       push_front
+15E8/+15F0    0x11 -> 0x12                                    that list's count++
+1648..+1664   shift down one, last slot zeroed                erase(front)
+17D4          0x1D -> 0x1C                                    that list's count--
+17E8          0 -> B97C9160                                    stored singly
+19C0/+19C8    0x02 -> 0x03                                     that list's count++
```

One pointer, `B97C9160`, leaves two lists and joins two others. The mission manager's own
reset function states the layout that confirms it: three identical units of 0x1EC bytes at
`+0x1408`, `+0x15F4`, `+0x17E0`, each an array of 0x79 pointers followed by its counts.
`sub_821AEC40` names the members: a mission is 0x38 bytes with its DEFINITION at `+0x18`,
its owner at `+0x1C`, its STATE at `+0x08`, appended to **`missionMgr+0x4D8`** and counted
at **`missionMgr+0x10AC`**.

#### 12.3 `B97C9160` is `PrologueWheelObjective`

`CZ_COOP_MISSIONWATCH=MS` (new) diffs that table by name. The definition's name offset is
not guessed: the instrument censuses every 4-aligned offset in the definition that decodes
as one of the engine's small-string-optimisation strings and prints all of them, so the
offset is **measured** — `+0x1C`, 7 of 128 offsets decoding, 67 of the 68 distinct names in
`missions.txt` resolved. A headless DebugJump run reads:

```
[mw] baseline: 73 mission(s) in missionMgr AADAD250
[mw]   B97C9160 def A52B34F0 state 0  PrologueWheelObjective
[mw]   B97C9080 def A52B3270 state 0  PrologueGasolineCanisterObjective
```

The object the operator's host wheel pickup moved between lists **is the wheel's objective
mission.** And `missions.txt` says what gates it:

```
cMissionDefinition PrologueWheelObjective
    cMissionPrereq GetHandleBar5
        cMissionObjectiveGiveItemToNPC FindHandleBar4
            ITEM_NAME = "WheelPawn"
    cMissionObjective GetGenerator6
        cMissionObjectiveBringItem ... EventToWaitFor = "WheelPawnPlaced"
```

The objective that waits for the placement lives INSIDE the mission whose prerequisite is
an item check. If the prereq never passes, the mission never starts and `WheelPawnPlaced`
arrives with nobody listening — which is why placing an unfound part does nothing, and why
the screen says *"Je n'ai pas encore TROUVÉ cette pièce"* rather than "not placed".

#### 12.4 The prereq looks in exactly ONE inventory

`cMissionObjectiveGiveItemToNPC` is class 57 in the mission class table at `0x829DB2A0`;
factory `0x822F02E8`, constructor `0x823AF1B8` (0xAC bytes), vtable `0x8204E540`. Seven of
that vtable's slots point into the class's own code and **`CZ_COOP_OBJTRACE=1` hooks all
seven and counts them**, so which one evaluates the prereq is a measurement and not a
reading. Six fired; slot 18 (`sub_823AF228`) did not. The one that matters is slot 20:

```
823AF424  lbz  r11, 0xa8(r3)      ; a latch -- already satisfied, return 1
823AF44C  ...  hash the string at this+0x80 (the ITEM_NAME)
823AF4A4  bl   0x823A4768         ; -> the owning mission
823AF4AC  lwz  r31, 0x1c(r3)      ; mission->0x1C = the world
823AF4B4  lwz  r4,  0x80(r31)     ; world->0x80 = THE LOCAL PLAYER INDEX
823AF4B8  bl   0x82482AD8         ; GetUserPlayer(world->0x7C, that index)
```

`sub_82482AD8` is two instructions and a tail call to `sub_8247B020`, the same
`GetUserPlayer` the shipped `CZ_COOP_RESPONSE_PLAYER` fix substitutes into. Measured
headlessly, in single player:

```
[obj] A51D5650 ITEM_NAME "WheelPawn"        ... answer 0, asked GetUserPlayer for index 0
[obj] A51D5210 ITEM_NAME "GasolineCanister" ... answer 0, asked GetUserPlayer for index 0
[obj] A51D5430 ITEM_NAME "BikeForks"        ... answer 0, asked GetUserPlayer for index 0
[obj] A51D4FF0 ITEM_NAME "BikeEngine"       ... answer 0, asked GetUserPlayer for index 0
```

One objective object per bike part, each asking about **one player**, the local one.

#### 12.5 The operator's own log closes it, and it was recorded before this instrument existed

`A51D5650` is the object that tests `WheelPawn`. In the operator's flag-hunt log, inside
the HOST's pickup window:

```
41267: [item] Event subtype 9 (coop=1 isHost=1): player 0 f60 B97C9160 f64 A51D5650
```

— the wheel's objective MISSION and the `WheelPawn` GiveItemToNPC OBJECTIVE, in one
broadcast event, for **player 0**. Inside the GUEST's pickup window: **zero** subtype-9
events, and zero mission-manager writes. So the identification does not rest on heap
addresses being stable between runs: the two addresses appear together in one line of the
operator's log with the two names this session measured independently.

#### 12.6 The diagnosis, in one paragraph

**A bike part is marked FOUND when `cMissionObjectiveGiveItemToNPC` — the prerequisite of
`Prologue<Part>Objective` — sees the item in an inventory, and it looks in exactly one
inventory: `GetUserPlayer(world->0x7C, world->0x80)`, the LOCAL player. On the host that is
player 0. A part in the GUEST's hands is invisible to it, so the objective mission never
starts, the `<Part>Placed` event arrives with no listener, and both screens read NOT FOUND
because the host is authoritative.** That is the same class of defect as the placement half
this session already fixed — a single-player assumption spelled as "the local player" — one
layer up.

It also explains the four-data-point rule in one sentence: the flag is set at PICKUP and
only for player 0, so whoever places it, only a part the host once held is credited.

#### 12.7 A PREDICTION THAT IS NOT ABOUT BIKE PARTS, offered because it can refute all of this

`missions.txt` has ELEVEN `cMissionObjectiveGiveItemToNPC` instances, and only five are
bike parts. The others name `Zombrex`, `Key_MasterKey`, `Gems` and `BikeEngine` again, in
`PrologueKateyZombrex`, `PrologueMasterKey`, `PrologueMoMoneyMoProblems`,
`ProloguePawnshopHint01` and `ProloguePawnshopHint2`. If the mechanism above is right,
**every one of those is broken in co-op the same way**: a guest who picks up the Zombrex,
the shed key or the gems should fail to register it. If the operator finds that guests DO
get credit for the master key, this whole reading is wrong.

#### 12.8 WHAT IS OWED — the positive control, and it is one round

No run has yet printed `answer 1`. Headless single player never picks a part up, so the
test has only ever been observed returning 0. The round that closes it is two minutes and
produces BOTH arms in ONE log, one process, one binary — the shape that has won twice here:

**Host picks up one part, guest picks up a DIFFERENT part, nobody places anything.** Arms
on both machines: `CZ_COOP_OBJTRACE=1 CZ_COOP_MISSIONWATCH=500`.

Predicted, on the HOST's log:

```
[obj] ... ITEM_NAME "WheelPawn":        answer 0 -> 1, index 0     (the host's part)
[obj] ... ITEM_NAME "GasolineCanister": answer 0, index 0, forever (the guest's part)
[mw]  CHANGED ... PrologueWheelObjective : state(+08) 0 -> N
      and NO line for PrologueGasolineCanisterObjective
```

Any of these refutes it: the answer never reaching 1 for the host's own part; the index
being anything but 0; or `PrologueGasolineCanisterObjective` advancing anyway.

#### 12.9 The candidate fix, its controls, and a claim that had to be retracted the same day

`CZ_COOP_FOUND_ANY_PLAYER=1` — shipped **OFF**. When the item prerequisite answers NO for
the local player, re-run the guest's own test once per other user player and take a YES.
The repair is a re-run and not an argument substitution because the defect is not *"the
wrong player"* — substituting the index would ask about player 1 INSTEAD of player 0 and
break the host's own case — it is *"only one player"*. The test is side-effect free on the
path that returns 0 (`0x823AF518` falls straight to the return), so a retry after a NO costs
a read-only pass, and the run that says YES takes the success path for the player who
actually holds the item.

There is **no observe-only mode**, and that is a statement about this fix rather than an
omission: the measurement IS the side effect. A mode that ran the retry and discarded the
answer would already have completed the objective; a mode that did not run it could not know
the answer.

**The null control, and what it does NOT cover.** Armed, single player, DebugJump to the
bike: the banner prints, the trace still answers for all four objectives, and the fix makes
zero substitutions. But the counters say why, and the why matters — `[found] consulted`
never printed at all, because with no session layer the retry block is never entered. **So
the solo null proves the gate works and proves nothing whatever about the retry.** Left
there, the first co-op round with the fix armed would have been the first time that code
ever ran, on the operator's machine, where a mistake in the player-existence guard is a null
dereference and not a wrong answer.

**So there is a positive control**, `CZ_COOP_FOUND_ANY_PLAYER_TEST=1`, which drops the co-op
gate only and is bring-up only — the same shape as `CZ_COOP_RESPONSE_PLAYER_TEST`. It
exercised the machinery end to end in single player: no crash, the answer unchanged, and

```
[found] consulted 1 time(s): 0 had no second player to ask, 3 retries run, 0 RESCUED
[found] consulted 10 time(s): 0 had no second player to ask, 30 retries run, 0 RESCUED
```

**`3 retries run` in SINGLE PLAYER, which retracts a claim this session had already written
down twice.** Guard 4 — "only an index whose player object exists" — was described in the
code comment and printed in the arm's own banner as the thing that makes the fix *"inert in
single player by construction: one user player, so no other index has an object"*. That was
an inference about a container nobody had looked at, and it is false: `world->0x7C` holds
**four pre-allocated player slots** whether or not anyone is in them, so the loop body runs
three times per consult in a solo game. Both the comment and the banner are corrected in
place, and the real inertness guard is now the session's own is-co-op byte (`+0x98`, the same
one `Side()` reads and `coop_host.cpp` writes), which is 0 in a solo session.

What the same run established is worth more than the claim was: **30 retries over dormant
player slots changed the answer 0 times and crashed 0 times**, so the re-run is measurably
answer-preserving for a slot with nobody in it. That is the property the fix needs and it is
now measured rather than assumed.

**Transferable, and it is the third time this investigation has paid for the same thing:** an
arm's own banner is a claim, and a claim in a banner is read by every future session as
established. Guard 4's inertness sentence was written, compiled, printed and believed before
anything checked the container — and the control that caught it cost one run. The lesson is
not "check your guards"; it is that **a guard which asserts something about the GAME's data
needs a control that reaches it, and a guard which only asserts something about our own
configuration does not.** Guard 3 (the co-op byte) is the second kind. Guard 4 was the first
kind dressed as the second.

#### 12.10 BOTH ROUNDS RUN, AND THE FIX IS ON BY DEFAULT (2026-09-29, operator-verified)

The operator ran §12.8's round on two machines, and then the armed round. Every predicted
line appeared and nothing that would have refuted it did.

**Round 1 — the fix OFF. The host picked up the wheel, the guest the gas canister.**

```
[pickup] player 0 ... hash 878FC97B (WheelPawn)
[pickup] player 1 ... hash 5F8D0521 (GasolineCanister)

[obj] A51D5650 ITEM_NAME "WheelPawn":        answer 0 -> 1, index 0
[obj] A51D5210 ITEM_NAME "GasolineCanister": answer 0, index 0 -- ONE line, never changed
[mw]  CHANGED B97C9160  PrologueWheelObjective : state(+08) 0 -> 1
      no state line for PrologueGasolineCanisterObjective
```

`answer 1` is the positive control §12.8 said was owed. The two arms are the two parts, in
one log, one process, one binary.

**And the `LISTS` line reproduced §11's raw numbers exactly**, which is what ties the named
mission to the 31 offsets that started this:

```
[mw] LISTS  pending(+0FA0) 45 -> 44  A(+15E8) 17 -> 18  A(+15F0) 17 -> 18
            B(+17D4) 29 -> 28  C(+19C0) 2 -> 3  C(+19C8) 2 -> 3
```

**The joiner's log closed the last step of the chain, and by absence.** On the guest machine
`sub_823AF418` **never fires at all** — only slot 3's hook reports alive — while its mission
watch shows `PrologueWheelObjective state 0 -> 1` arriving anyway. So the guest does not
evaluate these prerequisites; it receives the host's mission state. That is *why* both
screens say NOT FOUND rather than just the host's, and it was measured rather than assumed.

**Round 2 — the fix ON, guest picks up a part, host picks up NOTHING**, so anything credited
is attributable to the arm:

```
[found] RESCUED: "GasolineCanister" — the local player (index 0) said NO, player 1 says YES.
        objective A51D5210, 1 rescue(s) so far.
[obj]   A51D5210 ITEM_NAME "GasolineCanister": answer 1
[mw]    CHANGED B97C9080  PrologueGasolineCanisterObjective : state(+08) 0 -> 1 -> ... -> 2
```

Exactly **one** rescue, for the right item, and the objective mission then ran to state 2.
The operator's own words: *"it worked it placed the bike part for both players"*. Zero
crashes on either machine.

**SO THE DEFAULT IS NOW ON** (`=0` is the control arm), the same call that was made for
`CZ_COOP_RESPONSE_PLAYER`, and on the same evidence: a negative control (round 1, the part
not credited), a positive control (round 2, credited once), a null (single player, zero
consults) and a bring-up control (the retry machinery, answer-preserving over empty slots).

**Its cost is not measurable and the method used cannot claim more than that.** The armed
session made **300,000 retries** — three per consult, because three of the four player slots
are dormant — which is about seven extra calls a frame. Binned by draw count against round 1
(`gotcha 237`'s method, medians not means): 9.04 vs 9.01 ms at 6,000-9,000 draws, 5.10 vs
5.99 below 3,000, with guest-main-thread time lower in the armed arm. **Two different play
routes with 3-23 windows a band cannot resolve a small effect** — this rules out a large one
and nothing finer. If it ever needs to be cheaper, the retry should skip dormant slots, and
the presence test that works is the one `WatchPlayer` uses (a player with no INVENTORY is not
in the game), not the pointer.

**§12.7's prediction is the thing to test next** — but see **§12.11, which corrects it**:
the shed key and Zombrex are KEY items granted by a type-0x13 network message rather than
picked up into an inventory, so they were never broken. **`Gems` is the only untested case
left**, and it is the interesting one, because its objective sits in a `cMissionObjective`
rather than a `cMissionPrereq`.

#### 12.11 §12.7's prediction is WRONG about the key items, and the repo already said so

Tested the same evening: the operator started a new game and picked the shed key up **as the
guest**. On the host,

```
[obj] A51E2030 ITEM_NAME "Key_MasterKey": answer 0 -> 1, index 0
```

with **no `[found] RESCUED` line and no `[pickup]` line anywhere**. The host answered YES on
its own, because the key never goes through an inventory at all:

```
[keyitem] ObtainItem(...) | type 13 <<< KEY ITEM GRANT, id 85038 (Key_MasterKey / shed key)
```

**So the shed key was never broken in co-op**, and §12.7's list is corrected in place. The
mistake was enumerating the eleven `cMissionObjectiveGiveItemToNPC` instances by class name
without asking which of their items are KEY items — and this repo already carried the fact
that settles it, in the dead-end list at §12.3: **`items.txt` declares exactly two
`KeyItemID`s, 85001 Zombrex and 85038 Key_MasterKey**, and the type-0x13 grant is a network
message, not a pickup. Both of those were on a different path the whole time.

The corrected prediction, which is smaller and therefore worth more:

| item | KeyItemID? | expected |
|---|---|---|
| the five bike parts | no | **was broken, now fixed** — measured, twice |
| `Key_MasterKey` | 85038 | **never broken** — measured |
| `Zombrex` | 85001 | never broken, same grant path. Not worth a run |
| `Gems` | no | **the only untested case left** |

`Gems` is also the most interesting of the eleven for a second reason: its
`cMissionObjectiveGiveItemToNPC` sits inside a `cMissionObjective` (with
`NPCName = "srv_jemi"`), **not inside a `cMissionPrereq`** the way every bike part's does. So
it tests the claim that the repair is in the CLASS rather than in the wiring. Reaching it
needs `PrologueWinSomeLoseSome` done, Jemi and Fausto both rescued, and 7:00; the gems then
spawn at `-104.856, 3.276, -127.693`.

**RUN, AND IT IS A NULL — see §12.12.** The gems short-circuit before the player lookup
(`+0x7C = 8`) and are satisfied through the `+0xA8` latch, so the repaired path is never
entered for them. The class claim narrows to PREREQ-position instances, i.e. the five bike
parts and nothing else.

**Transferable:** a prediction that enumerates instances of a CLASS is only as good as the
check that those instances take the same PATH. Three of the eleven here did not, and the
evidence was already written down two sections earlier in the same document.

Also from the same session, unremarked at the time: the armed round rescued **twice**, not
once — `GasolineCanister` and then `WheelPawn`, both guest pickups.

#### 12.12 The gems are a NULL, and `+0x7C` splits the eleven three ways (2026-09-29)

The operator reached `PrologueMoMoneyMoProblems` and did the gems, plus more bike parts.

**The gems never enter the repaired path at all.** The instrument says so on one line:

```
Gems:       answer 1, asked GetUserPlayer for index -1, latch(+A8)=1 skip(+7C)=8
WheelPawn:  answer 1, asked GetUserPlayer for index  0, latch(+A8)=0 skip(+7C)=0
```

`index -1` is the hook's *never called* sentinel — `GetUserPlayer` was not invoked during
that evaluation. `skip(+7C) = 8` is non-zero, which short-circuits at `0x823AF448` before the
lookup; `latch(+0xA8) = 1` returns 1 at `0x823AF424`, the top of the function. The mission
still completed (`PrologueMoMoneyMoProblems state 1 -> 2`), through whatever sets that latch.

**So the gems neither confirm nor refute the class-level claim** — the code path was never
entered. Recorded as a null rather than as a win, because an instrument that reports
`index -1` is the only reason the difference is visible at all; the answer was `1` either way
and a coarser trace would have read it as a success.

**What it does establish is a discriminator.** The eleven
`cMissionObjectiveGiveItemToNPC` instances split three ways, and only one group ever had the
defect:

| position in the data | `+0x7C` | consults a player? | verdict |
|---|---|---|---|
| inside `cMissionPrereq` (the 5 bike parts) | `0` | yes, the LOCAL player only | **was broken, now fixed** |
| inside `cMissionObjective` (`Gems`) | `8` | **never** — satisfied through the latch | never broken |
| key items (`Key_MasterKey`, `Zombrex`) | `0` | yes, and the type-0x13 grant lands on the host regardless | never broken (§12.11) |

**So §12.7's claim that "the repair is in the CLASS and not in the part" is narrowed, for the
second time today, to: prereq-position instances.** That is exactly the five bike parts, and
nothing else in the game was ever affected by this defect. Smaller than claimed twice, and it
is the claim the evidence supports.

**Transferable, and it is the same lesson as §12.11 one level finer:** enumerating instances
of a class is not enough, and neither is checking they take the same PATH — two instances can
enter the same FUNCTION and leave it by different branches. `+0x7C` and the `+0xA8` latch are
both printed by the trace only because a field that can short-circuit a decision was worth
printing next to the decision. That choice is what turned a false positive into a null.

**The bike-part evidence, cumulative across both games:** four `[found] RESCUED` lines —
`GasolineCanister`, `WheelPawn` (twice, in two different games and against two different
objective objects, `A51D5650` and `A51D5100`), and `BikeForks` — every one a guest pickup,
every one credited, with `GasCanPlaced`, `WheelPawnPlaced` and `BikeForksPlaced` all raised.
Zero crashes across the whole evening.

**ISSUE #9 IS CLOSED.** Both halves fixed, both on by default, both operator-verified, and
the blast radius is now bounded by measurement rather than by argument.

## Part 13: the guest arrives dressed and renders naked — the post-load clothing check (2026-09-29)

**The report (operator, relaying players):** *"when the guest arrives in a session they
are fully clothed but often they appear invisible to the host or with the torso
missing."* Fully clothed on his own screen, so his save and his own player are fine; it
is the COPY of him on the other machine that is wrong. And **intermittent**, which is
what separates it from part 4's `chest_NONE` — that one was deterministic, it is fixed as
data, and it is not this.

### The two symptoms are one defect at two magnitudes

In this engine a character IS his seven clothing pieces. The load requester's own prefix
table (`0x829D42B8`, seven pointers) names them and settles it without a guess:

| part | 0 | 1 | 2 | 3 | 4 | 5 | 6 |
|---|---|---|---|---|---|---|---|
| prefix | `headwear_` | `head_` | `facewear_` | `chest_` | `hands_` | `leg_` | `feet_` |

Nothing else of Chuck is drawn. So **"no torso" is exactly "piece 3 never arrived" and
"invisible" is exactly "none of the seven arrived"** — one measurable predicate, not two
bug reports.

### The chain, and the two addresses that decide it

1. **REPORT** — the joiner sends seven `tEventOutfit` messages; the receiver
   (`sub_82570E78`) hands each to `sub_82371978(clothing, part, name)`.
   **`clothing = *(player + 0xCE74)` — a LOAD**, not an offset (`lwzx r3, r27, 0xCE74`
   there; `lwz r3, 0(player + 0xCE74)` in the co-op flow at `0x82582A9C`).
   `coop_outfit.cpp`'s `IsWearing` hook had been printing `player + 0xCE74`, i.e. seven
   records out of the middle of the player object; **fixed in this part.**
2. **RECORD** — `clothing + part*0x30`: `+0x4AE8` the name (a 0x24-byte SSO string),
   `+0x4B0C` its hash, `+0x4B10` the clothingdatabase row. The co-op setter writes the
   name and the hash and nothing else; the single-player setter `sub_8238C6F8` also
   writes the tag and the db row.
3. **LOAD** — one change-part event per part; `sub_82271BB0` takes it and calls the
   requester `sub_82270290`, which builds `<prefix><name>` and streams.
4. **ARRIVE** — the piece lands in the clothing manager's per-player **load record**,
   `mgr + 0x10 + (mgrPlayerIdx*13 + part) * 0x128`.

### The record's layout was MEASURED, and part 2 paid for it

Chuck's default outfit has an **empty facewear**, so record 2 is a piece that was never
requested sitting in the same dump as six that were — a negative control that costs
nothing and is always there (`CZ_COOP_OUTFIT_DUMP=1` prints it again any time):

| field | a piece that arrived (rec 0,1,3,4,5,6) | the empty one (rec 2) |
|---|---|---|
| `+0x00` | the piece's own name, inline SSO (`young_chuck`, `naked`, `young_chuck_under`) | empty |
| `+0x38` | a model handle (`0x175`, `0x176`, …) | `FFFFFFFF` |
| `+0x78` | a texture handle (`0x166`, `0x167`, …) | `FFFFFFFF` |
| `+0xB8` | 1 — files landed | 0 |
| `+0xBC` | the part index it serves | 13 = idle |
| `+0x11C` | the streaming budget, in BYTES | the part's budget |

So **a piece is present iff its record's `+0x38` is a real handle**, and the record's own
name at `+0x00` is a free cross-check that the right record is being read at all.

**Two corrections to `coop_outfit.cpp`'s own comments** fall out of that dump:
`+0x110` is not the record's buffer size (it read 46 for a 1460 KB budget) — **`+0x11C`
is**, and `+0xBC` is the part the record serves with 13 meaning idle, not an expected
file count.

### RETRACTED IN PLACE, and it cost this part two runs

**`clothing + part*0x2C + 0x49A4` is a real seven-slot attached-model array** — written
by `sub_82371B88`, read by `sub_82371A70` — **and it is never written for the PLAYER**.
All seventeen of its call sites are inside `sub_82165DE8`, a different actor path. The
first cut of this work used it as the "is this piece attached" test and reported a
correctly dressed **single-player** Chuck as missing all six of his pieces, with the
right names beside every one of them. Do not re-buy it.

### What shipped: `runtime/kernel/coop_outfit_verify.cpp`

Every 2 s, once two dressed players exist, it reads all four `GetUserPlayer` slots
(`sub_82482AD8(world, idx)`, bounds-checked 0..3 by `sub_8247B020` itself), resolves each
player's index in the clothing manager the way the change-part handler does
(`0x82271EA4`: find the player pointer in `mgr+0x428C`), and compares, per part,
**reported → recorded → loaded**. `world = *(*(0x82A57428) + 0x2C)`, which is
`sub_82483230(mgr, 1)` — two instructions, `*(mgr + (idx+0xA)*4)` — so no guest call is
needed for it.

**Three controls, and they are why this is allowed to act on what it finds:**

* the **LOCAL player** is swept by the same code as the remote one and is visibly correct
  by definition, so if he reads BAD the repair refuses and says so;
* the **load record's own name** must match the clothing record's name, or we are reading
  the wrong record;
* **`CZ_COOP_OUTFIT_CHECK_SOLO=1`** runs the whole sweep in single player, where the
  answer must be "all seven". That arm is what caught the `+0x49A4` error above, before
  any operator session — a check that has only ever been silent has not been shown
  capable of reading a correct player (gotcha 30).

**The repair** is the title's own change-part event, rebuilt field for field from the
co-op flow's own per-part post (`0x82582A60..0x82582AD4`): vtable `0x8200AFD4`, type
`0x2F` at `+0x8`, `3` at `+0x10`, `-1` at `+0x18`/`+0x1C`, then
`sub_8247CAA8(evt, player, part, -1, name, 1)` and
`sub_82188488(eventMgr, evt, 0x8207EC20, 0x85B)` with
`eventMgr = *(*(world+0x78)+0x70)`. It is not a new mechanism — it is the same event the
join posts, posted again for the one part that did not come back. It fires only after
three consecutive missing sweeps (~6 s, past any honest async load), at most three times
per part, never while a control is bad. If the record itself lost the name, the one the
WIRE delivered is written back first (`sub_82371978`'s impl, called directly so our own
report hook does not re-enter and re-arm part 6's save-less dress).

Scratch for the event is the guest stack below the hook's own frame (`r1 - 0x200`), with
the callee's stack pointer pushed to `r1 - 0x400`. The sweep runs from `sub_824C0668`,
the per-frame game-session update that already carries part 6's deferred dress.

Arms: `CZ_COOP_OUTFIT_CHECK=0`, `CZ_COOP_OUTFIT_CHECK_MS=N` (2000),
`CZ_COOP_OUTFIT_REPAIR=0` (**the control**), `CZ_COOP_OUTFIT_VERBOSE=1`,
`CZ_COOP_OUTFIT_CHECK_SOLO=1`, `CZ_COOP_OUTFIT_DUMP=1`.

### The next suspect, and this part prints the number that decides it

If a piece fails because its streaming BUDGET is too small it will fail again the same
way, the repair will burn its three tries, and the log will say so. That is the honest
limit of this work, and the suspect behind it is already on the table (part 4 recorded it
as a non-defect **for `young_chuck`** and did not ask the general question):

**`0x829D42F0` is a KB budget table with two columns, solo and "more than one player",
the second EXACTLY HALF the first**, chosen on `mgr + 0x4374 > 1`:

| part | headwear | head | facewear | chest | hands | leg | feet |
|---|---|---|---|---|---|---|---|
| solo KB | 1460 | 1530 | 440 | 2006 | 1296 | 1900 | 716 |
| co-op KB | 730 | 765 | 220 | **1003** | 648 | 950 | 358 |

Confirmed live: in the single-player control run record 0's `+0x11C` read `0x16D000` =
1,495,040 bytes = exactly 1460 KB. **A chest over 1003 KB would be invisible in co-op and
fine solo, for either player** — which fits "often", fits "the torso specifically", and
fits "he is fully clothed on his own screen" (his own machine is two-player too, but the
host's Chuck loads while the manager still holds one player). The sweep prints the
record's LIVE budget beside every missing piece, so **one operator log decides it**. If
that is the mechanism the fix is upstream of this file — the halving itself, or the
two-player texture flag at `g_82AC4878 + 0xB54` the create path sets
(`0x822271F4..0x82227208`).

### OPERATOR-VERIFIED ON TWO MACHINES, 2026-09-29 — and the first run refuted the check itself

**Run 1 (`play_0929_1452.log`) printed NOT ONE line from the check.** The sweep required
two *dressed* players, and the guest's outfit report arrived with all seven names EMPTY —
which is precisely the "invisible" case this was written for. **A gate written from the
healthy case excluded the defect.** It counts players that EXIST now, and `PLAYER n IS
WEARING NOTHING` is its own reported state.

That same log carried a finding the check had nothing to do with: part 6's save-less
joiner dress fired correctly (`dressing slot 1 in row 17`, `SetOutfit player 1`,
`ApplyRow player 1`) and **every `SetPart` that followed landed on clothing `B926EA40` —
the HOST's own Chuck — while the guest's clothing object is `B9288350`.** A guest with no
save is still dressed onto the wrong player. That is open, it is the same shape as issue
#9's `CZ_COOP_RESPONSE_PLAYER`, and it is NOT what run 2 fixed.

**Run 2 (`play_0929_1455.log`), the guest with a save: the repair worked, and the log and
the operator's eye agree.**

```
player 0 (this machine) has all 6 of the pieces he is wearing          <- the control
PLAYER 1 (the other machine) IS MISSING CLOTHING — ... renders INVISIBLE here:
  headwear=young_chuck NOT LOADED  head=... NOT LOADED  chest=... NOT LOADED
  hands=naked NOT LOADED  leg=... NOT LOADED  feet=... NOT LOADED
  asking for player 1's headwear ('young_chuck') again — attempt 1 of 3   [x6]
player 1 (the other machine) has all 6 of the pieces he is wearing     <- after the re-post
```

Every re-post answered `LoadDone player 1 part N: 1 of 1 files`. **Attempts 2 and 3 never
fired**; 0 `[title:desync]`; no crash. Operator: *"this time he is visible"*.

**WHAT THAT SAYS THE DEFECT IS, and it is upstream of this file.** His names ARRIVED and
were RECORDED correctly — nothing was ever wrong with the report. **Nothing asked for the
FILES.** The title's own per-part change-part events at join do not take effect, and
re-posting the identical events does. That is a timing defect in the join, the same shape
part 6 found when a row applied at level start went nowhere because the player did not
exist yet. **This file is the backstop, not the cure.**

**TWO SUSPECTS THIS PART CARRIED AND RUN 2 KILLED — do not re-buy either:**

* *"the guest is not registered with the clothing manager"* (this file's own hypothesis
  after run 1). **He is.** `players: [B925ABE0 B92744F0 B928DE00 B92A7710]` matched
  `GetUserPlayer` entry for entry, all four. The engine preallocates four slots, so the
  `mgrIdx < 0` branch is effectively unreachable and is kept only as an assertion.
* *"the two-player budget halving starves the chest"* — the standing next suspect above.
  **`mgr+0x4374` read 1, not 2**, so the SOLO column was selected and the chest kept its
  full 2006 KB. The halving was not in play. The live budget is still printed beside every
  missing piece, because that is what made this answerable in one log instead of a round
  of experiments, and because a session where `+0x4374` does read 2 may yet exist.

**Shipped state: check and repair both ON by default, permanently** (operator's
instruction, 2026-09-29). `CZ_COOP_OUTFIT_REPAIR=0` remains the control arm.

### What is owed

* ~~A two-machine session~~ — **DONE, and it is the section above.**
* **The control is still owed**: `CZ_COOP_OUTFIT_REPAIR=0` on a join that ALSO reports him
  missing. He should then stay missing and stay invisible. Run 2 is a within-run causal
  chain (measured missing -> one repair round -> measured present -> seen present), which
  is stronger than "he arrived fine" and weaker than a controlled pair.
* **The real cure, now that the backstop works**: why does the join's own per-part post
  not take effect? Its names arrive and are recorded; nothing requests the files. Part 6
  already met this shape once (a row applied before the player existed).
* **And the save-less guest is still dressed onto the HOST** (run 1, above) — a separate,
  open defect that this repair does not touch, because a guest with no outfit has no name
  to re-post.
* **Two free nulls that were run here and must stay true:** single player produces no
  `[outfit]` line at all (fewer than two dressed players), and
  `CZ_COOP_OUTFIT_CHECK_SOLO=1` in single player must read all seven.

## Player issue #11: the guest's gas can is placed as BIKE FORKS — the decision half of #9 (OPEN, 2026-10-04)

`~/XenonLive/Player Issues/#11 - Dead Rising 2_ Case Zero - pokisal`, filed
2026-09-30 against `v1.1.1-70-gf2d8c9b` (so it carries `CZ_COOP_RESPONSE_PLAYER` and
`CZ_COOP_FOUND_ANY_PLAYER`, both on): *"Key Items still desynced — Gasoline placed on the
Bike by the coop partner was replaced by bike forks."* The reporter added, directly to the
operator:

> *"Host placed Handlebars, Client placed Wheel then Gasoline. We both placed handlebar and
> wheel at the same time which worked, then I quickly swapped items and placed Gasoline
> which became bike forks for both players. Could be something with swapping and placing."*

### What the report already establishes

- **The F9 log is the HOST's.** `[respfix] state 34 ... says player 0, but state 61
  resolved player 1 ... SUBSTITUTING` fires twice: the guest placed, and the response half
  of #9 (`CZ_COOP_RESPONSE_PLAYER`) engaged for him, as designed.
- **The HUD's three ticks are PLACEMENTS, not FOUND flags** (the 09-25 notebook already
  showed the tracker ticking exactly the host's placements). They read wheel, forks and
  handlebar, which is the reporter's sentence exactly: handlebar (host), wheel (guest),
  then the guest's gas can scored as forks.
- *"Forks for both players"* is #9 being fixed and not regressed: the host is
  authoritative, so both screens now agree, on the host's answer.
- **No `[itemsync]` line**: `CZ_COOP_ITEM_SYNC` was off in the shipped build.

### The mechanism, and it is the one #9 measured and left open

State 61 decides which part was placed by reading the acting player's SELECTED SLOT out
of **this machine's** copy of that player's inventory (§"ANSWERED on two real machines").
That copy drifts: in the 09-25 tables the same slot held different items on the two
machines four times in seven, and once the two machines disagreed about which slot was
even selected. `CZ_COOP_RESPONSE_PLAYER` fixed what happens AFTER the decision, and
`CZ_COOP_FOUND_ANY_PLAYER` fixed the found flag. **The decision itself was never
repaired**, so a guest placement is now scored correctly only when the host's copy of
his hands happens to agree. *"Quickly swapped and placed"* is the case where it is least
likely to: the host's copy of a selection change that only just happened.

What is NOT established: whether the host's copy was merely STALE (the guest also held
the forks and the selection had not crossed yet) or DRIFTED (the guest never held forks
and the host's copy names the gas can's slot as forks). The two are told apart by one
question to the reporter (did the guest also carry the forks?) or by the line below.

### The repair is the arm that already exists — and its publisher was dead at the bike

`CZ_COOP_ITEM_SYNC` (above, "THE FIX — `CZ_COOP_ITEM_SYNC`") is exactly the decision
repair: the machine a player is local to publishes what he holds, and when state 61 runs
for a REMOTE player the event is replaced with the owner's answer. It was switched off on
09-26 because row 1 of that run agreed without help and the wheel still did not attach.
That refutation stands **as a refutation of the arm being the WHOLE fix**: the missing
half was the response, since fixed.

**But that run could not have measured the substitution at all.** The publisher was
driven from `cMissionOnTrigger::Update` (`sub_823E79B8`), and part 10 §5 measured that
hook entered ONCE in the safehouse garage, which is where the bike is. Nobody moved the
publisher when the instruments were moved, so at the bike the far machine never had a
fresh value to substitute with. That is why the 09-26 host logged 6 `coop-link broadcast`
lines for a whole session, and why its transport verdict was "unknown". (Gotcha 30 again:
an arm whose driver never runs is indistinguishable from an arm that does nothing.)

Changed 2026-10-04:

1. **The driver** is the GetUserPlayer hook (`sub_8247B020`), the one the harness moved
   to in part 10. It declines inside the respfix window (its own sample asks for index 0,
   which is exactly the lookup that window is waiting to substitute) and inside a state-61
   placement.
2. **Sampled every 33 ms, sent on change**, with the 200 ms period kept as a heartbeat.
   With a 200 ms period, a swap followed quickly by a placement can reach the host as the
   previous item. The host's stale copy and the stale published value then AGREE, and
   guard 3 (the substitution must change the answer) lets the wrong part stand. That is
   exactly the reporter's *"quickly swapped and placed"*. `CZ_COOP_ITEM_SYNC_SAMPLE_MS`.
3. **The host/joiner side is no longer cached on first ask.** Driven from GetUserPlayer,
   the first ask happens at the frontend, where a joiner that has not joined yet
   truthfully answers "host" of its own solo session. Cached, that made it publish as
   player 0 for ever, and the real host refuses every such message as a same-side peer.
   The same-box pair showed it: the joiner printed `this machine is the HOST` about 340
   log lines before its join began. Now it is asked once per sample and printed on change.
4. `[itemsync] publisher running: ...` prints unconditionally once (gotcha 151).

### The prediction, so a run can refute it

On two machines, with `CZ_COOP_ITEM_SYNC=1` on BOTH: the guest carries the gas can AND
another part, swaps to the gas can and places it immediately. The host's log prints

```
[itemsync] placement by player 1 (remote here): this machine read 7D7806D9 (BikeForksPlaced)
           out of its own copy, player 1's own machine says 5F8D0521 (GasolineCanister)
           -> raising D4AF6D06 (GasCanPlaced)
```

and the HUD ticks the gas can. If that line reads `both machines agree`, the host's copy
was right and the defect is elsewhere. If it reads `NO FRESH held-item`, the channel is
not arriving. **One risk to watch, said in advance:** the substituted event only advances
the mission if the gas can's `cMissionObjectiveGiveItemToNPC` has STARTED on the host,
which needs the host to have seen it as FOUND. If the host's copy drifted (rather than
lagged), it may never have, and the substituted event lands with no listener. A
`[found] RESCUED: "GasolineCanister"` line earlier in the host's log is what says it did.
