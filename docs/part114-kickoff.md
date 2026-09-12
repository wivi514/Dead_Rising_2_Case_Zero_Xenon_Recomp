# Part 114 kick-off — co-op part 5, where part 4 left it

**READ `docs/coop-plan.md` "Part 4: the chest piece, and the first content tests"
FIRST.** It is the execution record of the evening of 2026-09-11. This file says only
what to do next and what already exists so it is not rewritten. The work is on branch
**`xlive-integration`** (28 commits ahead of master); master has none of it.

## §0. Where co-op is

Two machines, two dressed Chucks, operator-played, three joins this part. The joining
Chuck's chest piece is FIXED (it was `OUTFIT_COOP_DEFAULT_UNDER`, a name with no csv
row → `chest_NONE`; data fix in `tools/patch_coop_outfit.py`, both archives, both
machines). Content tested and normal: a cinematic, a save on the host, items picked up
and dropped, one side quitting (the host re-hosts and continues). 0 `[title:desync]`.
The "incoming co-op call" HUD is compiled out of BOTH XBLA builds (string 11546 is
referenced by no instruction in either image) — closed, not owed.

## §1. What already exists — do not rewrite any of it

Everything in `part113-kickoff.md` §1, plus:

| piece | file | switch / control |
|---|---|---|
| the clothing pipeline, step by step | `kernel/coop_outfit.cpp` | `CZ_OUTFIT_TRACE=1` |
| both co-op outfit rows = the default rows, in the patched overlay | `tools/patch_coop_outfit.py` | idempotent; re-run after `gen_pc_options.py`; scp both `.big`s to the laptop with its game CLOSED (it holds `datafile.big` open) |
| the host launcher used this part | `TAG=coop_hostN tools/play_session.sh CZ_DEBUG_MENU=1 CZ_XLIVE_ONLINE=1 CZ_XLIVE_COOP=1 CZ_XLIVE_HOST=1 CZ_ONLINE_LOG=4 CZ_NET_LOG=1 CZ_OUTFIT_TRACE=1` | `CZ_ONLINE_LOG=4` is what prints the title's own *"received clothing report"* lines; 3 does not |
| the joiner, from here | `ssh czwin 'schtasks /run /tn cz_play'`; `taskkill /IM cz_runtime.exe /F` to stop it | — |

## §2. What to do next, in order

### 1. The two content tests still owed

A **mission transition** (start or finish a case with both in) and a **crowd** (stand
in a big group for a minute; the pass mark is `grep -c 'title:desync'` on the host log
staying 0). Both are operator play; the log records the network side by itself.

### 2. Only if co-op is to SHIP (the operator's decision; plan item 5 is the fallback)

Unchanged from `part113-kickoff.md` §2.4: panel rows for host/join and the privacy
setting, the joiner's screen from a menu row, `overlay_gen.cpp` gets the outfits.csv
rewrite (it is now REAL data, not a guess), a Windows bundle carrying
`libcurl-x64.dll`, and the libxlive token refresh before expiry.

### 3. If the join prompt is ever wanted

Drive the dialog path, not the HUD text: post string 11533 from the confirm-pending
site and feed the answer to `sub_82582188(session, accept)` — the map is in the plan's
part-4 section.

## §3. Gates

Unchanged and still owed before merging the branch: A5 with the co-op env vars unset,
`find_unlowered_switches.py`, and a solo play session. The new hooks are all
per-part/per-event and inert without `CZ_OUTFIT_TRACE`.
