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
