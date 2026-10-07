# Release notes — next release (DRAFT, not published)

Lines owed to the next release since v1.1.2, collected as they land. Fold them into
`release-notes-vX.Y.Z.md` when the release is cut. The body format is v1.1.2's.

## Co-op

- **The ending is now playable in co-op.**
  - **The crash:** the host no longer crashes when the military arrives (after the
    leave-the-garage cutscene). The joiner's game was removing the host's helicopters out
    from under their pilots.
  - **The bikes:** each player now rides his own motorcycle to the end, and both see the
    ending cutscene. The guest used to be left on foot.
  - **Single player is unchanged.**
  - Verified on two machines on 2026-10-07 (`ed26c2c` and the guard commits before it).
- **A guest with no save is no longer invisible on the host** (co-op part 14's join window,
  operator-verified on 2026-10-07; after v1.1.2, so not yet shipped).

## Install note

- **The patched game data regenerates once on first launch** (overlay version 6 -> 7, for
  the co-op ending's mission data). This is expected and takes a few seconds.
