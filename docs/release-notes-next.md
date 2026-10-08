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

- **Saves made before this update load again** with the new co-op ending data. Adding the
  co-op motorcycle to the mission data had made every older save read as "damaged"; the
  co-op bike is now kept out of the save file, so saves are laid out exactly as in the
  original game (`a3ade42`, operator-verified 2026-10-07). This MUST ship with the overlay
  v7 change above — without it, updating breaks every player's existing saves.

## Visuals

- **Soldier zombies keep the same head and helmet at every distance.** The game's far
  soldier model had two parts misnamed, so every soldier wore a helmet from afar and many
  lost it when they came close (the same happens on a real 360 / Xenia). Far soldiers now
  show their real variant: some are bare-headed or in a balaclava at any distance
  (`78632f1`, operator-verified 2026-10-08).

- **New setting: ZOMBIE DETAIL** (Settings panel, last row). Zombies now switch to their
  detailed model from twice as far away, and twice as many get it at once. Before, you had
  to stand right next to one. The original game's value is 1.0; the default is now 2.0,
  and it goes up to 5.0. A higher value costs more GPU time in big crowds. The Steam Deck
  build keeps the original 1.0 for performance. It applies immediately.

- **Chuck's hair no longer flickers at the back of the neck** (player issue #2). The
  hair is drawn in several passes, and two of them computed the head's position in
  slightly different ways, so the layers traded places from frame to frame. Other
  skinned characters drawn the same way are fixed too (operator-verified 2026-10-08).

## Install note

- **Some shaders are rebuilt once on first launch** (the shader cache's recipe goes
  `clip=1` -> `clip=1 vid=c`). This is the hair fix reaching existing installs; it shows
  the same "preparing shaders" progress as a first install.

- **The patched game data regenerates once on first launch** (overlay version 6 -> 7, for
  the co-op ending's mission data). This is expected and takes a few seconds.
