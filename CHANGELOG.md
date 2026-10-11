# Changelog

English | [简体中文](CHANGELOG.zh-CN.md)

Changes made on top of upstream [OpenInputLagPatch](https://github.com/khang06/OpenInputLagPatch).

Only changes that have been tested in-game and confirmed working are listed here, so this file
is also the record of what has actually been verified rather than just what was attempted.

## 2026-10-11

### Added

- **Touhou 10 (Mountain of Faith) gets frame interpolation too** (`229342c`, `7036f2d`, `dcd812a`, `cf6f78d`, `b9c8811`)

  Mountain of Faith is the oldest of this batch (2007, a generation before Subterranean Animism),
  and it draws a frame the way th11 does with less of it: **one render block and one present**, both
  inside the frame function `CWindowManager__Update` (`0x439390`) - the render block calls the
  object render at `0x4394D8` and presents at `0x43920A` - so there are two addresses to take over
  rather than six, and since that one render block sits in the window update, the title screen and
  the menus get the extra frames as well. The device is `[0x491C30]`, the render context
  `[0x491FAC]`, the sprite queue's owner `[0x491C10]`, the byte the block sets to `0xFF` is
  `0x492378`; the object render is `FUN_00449D40` (its object in `EDI`) and the queue is drawn by
  `FUN_00442F50` (its object in `ESI`), so both go through the same `__declspec(naked)` trampolines
  th13 needed. Its present is laid out in pieces - the three pushes in front of the patched load
  belong to the same call - so the hook has to return with `ret 0xC` and put those 12 bytes of
  arguments back; getting that wrong is what made th10 fail its stack cookie and die on startup the
  first time it was tried.

  The bullets are the manager `[0x4776F0]`, whose pool is 2001 bullets of `0x7F0` bytes at
  `manager+0x60` and whose six column lists (heads at `manager+0x14`, next at `bullet+0x44C`) are
  both what the update rebuilds and what the draw walks, so they are exactly the bullets that are
  drawn. Position at `+0x3B4/+0x3B8/+0x3BC`, velocity at `+0x3C0/+0x3C4/+0x3C8`, state at `+0x446`
  (1 is a full step, 2 and 3 are half of one), global slowdown at `[0x476F78]`, and the drawn
  position the draw copies the position into at `+0x33C/+0x340/+0x344`. The update divisor
  `[0x491D66]` is read in the executable and written nowhere and is 0, so there is exactly one
  update per rendered frame.

  The player is `[0x477834]` (0x4478 bytes). Its update (`FUN_004250B0`) moves it as two ints in
  1/128 pixels at `+0x3CC/+0x3D0` and writes the two floats at `+0x3C0/+0x3C4` back out from them,
  and its draw callback (`FUN_00426360`) copies those into the animation object it carries at
  `+0x354/+0x358` every frame, so moving the logical position is what moves the body - th13's and
  th15's rule. Its options are 4 slots of `0x98` bytes at `+0x32A0` (in use when `+0x00` is
  non-zero, position at `+0x34/+0x38` as ints in 1/128 pixels, two animation objects at
  `+0x68/+0x6C`) and its shots are a pool of 128 slots of `0x5C` bytes at `+0x49C` (in use when
  `+0x40` is non-zero, position at `+0x14/+0x18/+0x1C`, two animation objects at `+0x44/+0x48`).
  The shots are filled in by `FUN_00427E90`, walked every frame by `FUN_00428280` and tested against
  the enemies by `FUN_00428630`, and all three of those work on that one pool, which is what says
  its shape is right (the offsets are each read out of those functions rather than carried over
  from th11, whose pool is at `+0x96C` with 0x6C byte slots). Both the options and the shots are
  positioned by the update rather than by a draw callback, so the objects are what is moved for
  them, and they are looked up by handle again every frame (walking the sprite manager's two lists
  at `[0x491C10]+0x72DAD4`/`+0x72DADC`; an object's drawn position is `+0x340/+0x344/+0x348`),
  which is what keeps the crash th12 had - a cached pointer to a freed sprite - from happening.

  The stage also builds **one single shot object** of its own (`FUN_004056B0`, 0x48 bytes, kept at
  `[0x4776EC]`), updated by `FUN_00405750` and fired by `FUN_00405860` from the player's own shot
  state machine (`FUN_00425730`). It is projected as well, so that a shot of that one's isn't left
  stepping while the pool's are smooth, but in the runs below it was never in flight: the pool is
  what the player's fire actually shows.

  The straight lasers are the manager `[0x47781C]` (one 0x45C byte block, the node the game works
  from at `+0x10`; list at `manager+0x18`, next at `laser+0x08`, state at `+0x0C`). th10 carries
  **no RTTI at all** - its only RTTI names are the eight MSVC standard library ones - so the classes
  are whitelisted by vtable instead of by name: the two of them (`0x46DA60` and `0x46DA10`, the
  classes `FUN_0041C5B0` and `FUN_0041C680` build) both keep the position at `+0x24/+0x28/+0x2C`,
  the angle at `+0x3C` and the length at `+0x40`, and each one's own draw (`FUN_0041D7C0` and the
  one at `0x41EA40`) reads the position and the angle at draw time as it writes them into the two
  animation objects the laser embeds, so moving them moves the beam. The velocity is read from each
  class (`+0x30/34/38` for the one that doesn't turn, whose length stops at `+0x434`; `+0x430/434/438`
  with the turn rate at `+0x440` and the length stopping at `+0x444` for the one that does). As in
  th11, a beam's length is not read at draw time (the update folds it into a sprite's pixel size
  once a frame), so a growing beam still extends in whole frames while its position and angle are
  smooth.

  The menus' text is the same problem the later games have, in an older shape: the text object is
  `[0x4776E0]`, its two record tables start at `+0x76C` (0x68 bytes a record, up to 0x100 of them)
  and `+0x6F6C`, and the counts of the ones the frame draws are at `+0x896C/+0x8970`, which the
  update clears once a frame; those two are kept where the game's own render runs and put back
  before every extra presentation, so an extra frame draws the same text the game's own frame did.
  th10 also has a **frame rate module of its own** (`\src\game\fps.cpp` is in the executable) whose
  task sits on the render chain, so an extra presentation would count three times the frames the
  game actually ran and, past 65, make the game resynchronise its clock. What that module keeps is
  saved where the game's own present runs and put back around every extra presentation (with its
  window's start pushed into the future so the extra frame doesn't settle the window itself), and
  its resynchronisation test is patched out the way thprac's th10 module does it (`EB 50` at
  `0x413516`), which is what keeps the game's own rate display honest once the rate is raised. It is
  also the first of the family to run part of the game on a **second thread** (the enemy thread
  `FUN_0041FAC0` starts with `__beginthreadex`), which is why what is projected is read and written
  at the one point in a frame where the game's own update walk has just run - the same point the
  game's own render reads those containers from.

  Confirmed in-game: the extras land evenly at 180 presents per second (`average: 3.000 presents per
  frame, 180.6 presentations per second`), with `1028 in play, 1028 projected` for the bullets in
  one run and `505 in play, 505 projected` in a later one, `1 body, 8 option and 42 shot sprite(s)
  projected` for the player, and `144 in play, 144 projected` for the lasers in a laser-heavy
  attack; nothing was left without a movement record and no projected position was written back
  during the render, which is what says the positions being moved are the ones the renderer reads.
  A replay recorded with the patch plays back on the unpatched game without desyncing, which is the
  outside check that the logic and the random numbers are untouched.

- **Touhou 11 (Subterranean Animism) gets frame interpolation too** (`26ac150`, `7d08bd2`, `7695746`, `af30cfb`)

  Subterranean Animism is the oldest of this batch (2008, a generation before Undefined Fantastic
  Object), so its offsets are its own: its frame code has the same **three render blocks and three
  present sites** (renders at `0x446193`/`0x446571`/`0x446730`, presents at
  `0x446382`/`0x4465D6`/`0x4467F9`), and one of the render blocks is inside the window update
  `CWindowManager__UpdateFast` (`0x446420`) itself, which is why the menus and the title screen get
  the extra frames too. Those six are what the extra presentations take over; the device is at
  `[0x4C3288]`, the render context is the object the block puts in `EDI` (`0x4C3280`), the sprite
  queue's owner is the one it puts in `ESI` (`[0x4C3268]`), `0x44FCE0` empties the queue,
  `0x44FD10` draws it and `0x456E10` renders the objects.

  Each render block presents from a small frame-end helper (`FUN_004461F0`/`FUN_004465D0`/
  `FUN_00446790`), all of them a 5 byte load of the device followed by 16 bytes of arguments, so
  all three are patched and the code after them runs with the present result. The two the block
  calls with their object in a register (`0x42A300` wants `EDI`, `0x44FD10` wants `ESI`) go through
  the same kind of `__declspec(naked)` trampoline th13 needed, with the caller's register put back
  before returning. th11 inlines the sprite queue's reset into the frame function, so the extra
  presentation calls `0x44FCE0`, which is the same thing.

  An extra presentation also has to put the picture's own state back: the viewport, the view and
  projection transforms (the game's frame builds them from its 2D context in `FUN_0042A810`, and a
  tool drawing its overlay replaces them with its own), the 2D context the frame rendered with
  (`[0x4C37CC]`, which the sprite queue's clipping reads) and the two values `FUN_0042A810` leaves
  in the sprite manager at `+0xB0`/`+0xB4` that place the queue on screen. All of them are taken
  where the game's own render runs and written back before every extra presentation (the game's own
  `FUN_0042A810` is not called again - it draws the queue once on its way in).

  The bullets are the manager `[0x4A8D68]` and its own six column lists (heads at `manager+0x14`,
  next at `bullet+0x4B8`): position at `+0x43C/+0x440/+0x444`, velocity at `+0x448/+0x44C/+0x450`,
  state at `+0x4B2` (1 is a full step, 2 and 3 are half of one) and the global slowdown at
  `[0x4A7948]`. The update divisor `[0x4C3466]` is read in seven places across the executable and
  written nowhere, and is 0, so there is exactly one update per rendered frame. The player is
  `[0x4A8EB4]`, its own position at `+0x87C/+0x880/+0x884` (its draw callback `FUN_00431BC0` copies
  that into the body animation object's drawn position every frame). Its options are 8 slots of
  `0xE4` bytes at `+0x7570` (position at `+0x5C/+0x60` as ints in 1/128 pixels, two animation
  objects at `+0xB0`/`+0xB4`) and its shots are 256 slots of `0x6C` bytes at `+0x96C` (position at
  `+0x14/+0x18/+0x1C`, objects at `+0x4C`/`+0x50`). Those two have their positions written into the
  objects' drawn position at `+0x3E8/+0x3EC` by the update, so it is the objects that are moved for
  them; the player's own position is moved directly. The objects are looked up by handle again
  every frame (by walking the sprite manager's two lists at `[0x4C3268]+0x7B562C`/`+0x7B5634`),
  which is what keeps the crash th12 had - a cached pointer to a freed sprite - from happening.

  The straight lasers are the manager `[0x4A8E94]` (list at `manager+0x18`, next at `laser+0x08`,
  state at `+0x0C`). th11's RTTI carries only `LaserLineInf` (vtable `0x494A64`) and
  `LaserInfiniteInf` (`0x494ABC`) - there is no curve or beam laser class in this game - and both
  keep the position at `+0x3C/+0x40/+0x44`, the angle at `+0x54` and the length at `+0x58`, all of
  which their draw (at vtable `+0x0C`) reads as it writes them into the two animation objects the
  laser embeds, so moving them moves the beam. The velocity is read from each class (Line:
  `+0x48/4C/50`, and it does not turn; Infinite: `+0x44C/450/454` with the turn rate at `+0x45C`),
  the length grows by `+0x60` up to `+0x450` (Line) or `+0x460` (Infinite). One known difference:
  a laser's length is not read at draw time in this game (the update folds it into a sprite's pixel
  size once per frame), so a beam that is growing still extends in whole frames even though its
  position and angle are smooth.

  The menus' text list - the problem described under the Undefined Fantastic Object entry below -
  is handled here from the start: the text object is `[0x4A8D58]`, its records are `0x130` bytes
  each (th11 is a generation older, so they are smaller than the later games' `0x138`) at `+0x87C`
  up to `0x140` of them, and the count of the ones the frame draws is at `+0x1847C`; the render
  hook keeps it and every extra presentation puts it back before it draws.

  Confirmed in-game: the extras land evenly at 180 presents per second (`average: 3.000 presents
  per frame, 180.6 presentations per second`); `549 in play, 549 projected` for the bullets, `1
  body, 8 option and 57 shot sprite(s) projected` for the player, and `256 in play, 256 projected`
  for the lasers in a laser-heavy spell card (the manager's own limit of `0x100`), with the picture
  and the pause and save screens normal.

- **Touhou 12 (Undefined Fantastic Object) gets frame interpolation too** (`c4b7811`, `fe9146f`, `fd8eddc`)

  Undefined Fantastic Object is the last of the games the requirements name (the earlier ones already
  had reverse engineering or notes to work from, and the later three were worked backwards from
  Double Dealing Character and Legacy of Lunatic Kingdom to here). Its frame function has the same **three render blocks
  and three present sites** (renders at `0x45017B`/`0x450529`/`0x4506C8`, presents at
  `0x450355`/`0x450586`/`0x450789`), and those six are what the extra presentations take over.
  `0x45A380` empties the sprite queue and `0x45A3C0` draws it, and both want their object in `ESI`,
  so they are called through the same kind of `__declspec(naked)` trampoline th13 needed - putting
  the object where the routine wants it and putting the caller's register back before returning.

  The bullets are the manager `[0x4B43C8]` and its own list of the ones in play: position at
  `+0x4BC`, velocity at `+0x4C8`, moved exactly the way the game's own update moves them (the global
  slowdown and the half step a starting bullet gets included). The player is `[0x4B4514]`, its own
  position at `+0x97C`. The player, its options and its shots are moved smoothly by the input and by
  each other with **no velocity to read**, so they are *measured* the way th13, th14 and th15 do it:
  what each moved by since the frame before is taken at the point the game presents its own frame,
  written in for the fraction of a frame an extra presentation sits at, and put back right after the
  render. The straight lasers are the manager `[0x4B44F4]`, position at `+0x50`, and read their
  velocity from their own class (see the item below).

  Confirmed in-game: the extras land evenly at 180 presents per second, no sprite is left without a
  movement record, and the bullets, the player, its options and shots and the straight lasers all
  move with the extra frames; the pause and the save-replay screens are as they should be. The
  enemies are not interpolated (their animation scripts place them and there is nothing to read),
  and the curve lasers are left alone.

### Changed

- **The four games' straight lasers read their velocity instead of measuring what they moved**
  (`7fdd8fd`)

  All four measured the lasers: the position each was at the frame before was kept **per object
  address**, and an extra presentation wrote in the difference times the fraction of a frame. The
  problem is not precision but **object recycling** - lasers are short lived, the game frees and
  allocates them constantly, and a fresh laser that lands in a dead one's slot gets "old position
  minus new position" as if it were a velocity, which draws that laser offset. A screen with a few
  hundred of them, which these games reach, runs into it almost every time. Reading the velocity the
  laser carries cannot do that (a warning line, whose velocity really is zero, is left where it is),
  and it also gets a laser's first frame right, where a measurement has nothing to compare against.

  Two things fell out of reading the classes: in th13, th14 and th15 a line laser's angle is fixed
  (their own updates never write it - only the infinite class turns), and their beam class's update
  function (`0x437FF0`, `0x443C50`, `0x44B7D0`) is a stub that just returns 0, so it never moves and
  has no layout of its own. The per class layouts of all four games are in the notes under
  `_analysis`, each read out of that game's own class update rather than copied.

### Fixed

- **The menus' text is no longer drawn twice by the extra presentations** (`d387341`)

  A menu, the pause screen and the save/load replay screens do not draw their text a line at a time:
  the game's own printf-for-text call **appends a record** to a list inside its text object (0x138
  bytes each, up to 0x140 of them) and **the drawing then walks the records the frame's count of
  them covers**. That count **is started over once per frame**, and that happens in the **update
  walk** - the walk an extra presentation does not run, because it is the game's logic. So an extra
  frame drew the records the game's own frame had left behind a second time, appended its own behind
  them, and drew the lot; and the **second** extra presentation of a frame draws into the same back
  buffer the game's own frame was presented from, so both copies land on one picture.

  The fields and the look of each game (all read out of that game):

  | Game | Text object | Records | **The frame's record count** | Look |
  |---|---|---|---|---|
  | th12 | `[0x4B43B8]` | `+0x97C` | `+0x18F7C` | the layout does not move between the two, so the copies sit **exactly on top of each other** and only make the text look a little denser |
  | th13 | `[0x4C2160]` | `+0xB5C` | `+0x1915C` | two copies of the line, **offset more and more along it** |
  | th14 | `[0x4DB520]` | `+0xBAC` | `+0x191AC` | as th13 |
  | th15 | `[0x4E9A58]` | `+0xC20` | `+0x19220` | as th12 |

  > **A note for anyone chasing this**: the symptom **cannot be seen at 120Hz** (one extra
  > presentation per frame is one step of this, which is too small), it shows from 180Hz on; and
  > comparing the game's own frame with the *first* extra alone shows almost nothing and looks fine.
  > Capturing **every presentation of a frame** is what shows it.

  The fix is the same discipline the bullets and lasers have always used - save, draw, put back: the
  count is saved at the start of the game's own run of the render walk (th13 also saves the twelve
  fields of its text object that its own walk was measured to change: the pointer the next record is
  taken from at `+0x040`, the position the text was left off at `+0x050/+0x054`, eight floats at
  `+0x3CC` and the count) and put back before every extra presentation. Each extra then appends its
  own records from the start of the list and draws exactly what the game's own frame drew, and the
  state it leaves behind is what a single run of the game's walk would have left, so the game's next
  frame starts the list over where it always did.

  What settled it was **writing the back buffer out as a BMP right before every present** (through
  `GetBackBuffer` and `GetRenderTargetData`, every presentation of a frame, not one of them) and
  comparing them: the game's own frame against its second extra differed in 2911 pixels with a mean
  channel delta of **109** (the doubled line, visible to the eye), while against the first extra it
  differed in 2262 pixels with a mean of **13.75**, which looks right. With this in, all four games
  come out **pixel for pixel identical**, and the screens' text was confirmed in-game to be back to
  normal.

## 2026-10-10

### Added

- **Touhou 13 (Ten Desires) gets frame interpolation too** (`43f9c66`, `94c39f1`, `740dd7d`)

  Ten Desires is the next one the requirements point at, and it runs on the same engine as th14, so
  this is the same mechanism with th13's own addresses - every one of them read out of the game
  itself. The frame function has the same **three render blocks and three present sites** (renders
  at `0x45CFFF`/`0x45D452`/`0x45D637`, presents at `0x45D207`/`0x45D4B6`/`0x45D709`), and those six
  are what the extra presentations take over. Two things are th13's own: two of the helpers the
  render block calls take their object in a register (`0x43B080` wants it in EDI, `0x4679A0` in
  ESI), which a function pointer cannot do, so naked trampolines put the object there and then
  call; and the middle present site zeroes esi among the arguments it pushes, so that site is
  hooked through a trampoline that does the same.

  The bullets are the bullet manager `[0x4C2174]` (built at a stage start, loading `bullet.anm`)
  and its own list of the ones in play (head at `+0x84`, node at bullet `+0x10`), with the position
  at `+0xB70`, the velocity at `+0xB7C` and the state at `+0xBBE`, moved exactly the way the game's
  own update `0x40DB10` moves them (including the half step a starting bullet gets and the global
  slowdown `[0x4C0A28]`). The player is `[0x4C22C4]`: its own position at `+0x5B8`, its options as
  8 slots of `0xE4` bytes at `+0xA2B8` (live dword at `+0x00`, sprite ids at `+0xB0/+0xB4`) and its
  shots as 256 slots of `0x9C` bytes at `+0x6A0` (live countdown at `+0x70`, sprite id at `+0x14`,
  position at `+0x2C`). The straight lasers are the manager `[0x4C22A0]` (head at `+0x18`,
  `obj+0x08` for the next one), and a vtable whitelist keeps only `LaserLineInf 0x4AB0CC`,
  `LaserInfiniteInf 0x4AB19C` and `LaserBeamInf 0x4AB204`; the curve `LaserCurveInf 0x4AB134` is
  left out. A line laser's own draw builds its picture out of the position at `+0x50`, the angle at
  `+0x68` and the length at `+0x6C`. The player, its options and its shots have no readable
  velocity and a laser turns and grows as it goes, so as in th14 and th15 these are measured: every
  game frame the fields are compared with the ones of the frame before, and the extra presentations
  move them by that difference times the fraction of a frame they stand for, putting them back
  right after the render.

  What took the work this time is which field has to move, because the game finishes its drawing at
  render time: `FUN_0046A700` bakes an animation object into the sprite manager's vertex queue as
  it draws (`0x467960` empties the queue, `0x4679A0` draws it in one call), so what matters is the
  field the bake reads. The bullets are copied from their position into their own animation objects
  by the draw pass (moving the position is what works); the options and the shots have their
  position written into the sprite's `+0x574` by the update, and the render only bakes the sprite
  (moving `+0x574` is what works); the player is the other way around - its draw callback
  `0x443D90` copies `player+0x5B8` into the body animation object's drawn position `+0x588` at
  render time, so the position itself is what moves it.

  Two leads the handover notes had were wrong, both of them things that were guessed from the shape
  of the code instead of read out of it, and both are corrected here: the "array of 0x44 byte shot
  objects" is the player's **bomb** (`BombReimuInf`, vtable `[0x4AA5BC]`, 8 objects of `0xD0`
  bytes that `0x40B4B0` builds) and has nothing to do with the player's shots, and the option slots
  start at `+0xA2B8`, not at `+0xA380` (that is `+0xC8` into each of them). The diagnostics now
  count the player's body, option and shot sprites apart and report the lasers, so a stage can be
  told apart from a menu at a glance.

  Confirmed in-game: the extra frames land evenly at 180.6 presentations per second on the 180hz
  display, no sprite is left without a movement record, and the player, its options, its shots and
  the straight lasers all follow the extra frames. The enemies are deliberately not interpolated
  (their animation scripts place them and there is no velocity to read) and the curve lasers are
  out of scope. Stage 1 of th13 is done with this.

## 2026-10-09

### Added

- **Touhou 14 (Double Dealing Character) gets frame interpolation too** (`8efd72f`)

  With th15 done, the other game the requirements name as worth starting from is th14, and the two
  run on the same engine - so this is the same feature with th14's own addresses, every one of
  them read out of the game itself (the disassembly and the decompiler) rather than by matching
  sprites or `.anm` names. The requirement is explicit about that: what has to be taken is the
  game's own containers, because projecting everything that looks like a bullet both stutters when
  a screen is full of items and gets the wrong objects.

  The frame function has the same shape th15's has - three render blocks and three presents, one
  for the game scene and one per menu scene (renders at 0x46A45D / 0x46A86D / 0x46AA0C, presents at
  0x46A67A / 0x46A8D6 / 0x46AAEC) - and those six call sites are what the extra presentations take
  over. The bullets are walked through the manager that loads `bullet.anm` when a stage starts
  ([0x4DB530]) and through that manager's own in-play list (head +0x80, node at bullet+0x10), with
  the position at +0xBC0, the velocity at +0xBCC, the state at +0xC0E and the hold flag at +0x20,
  moved exactly the way the game's own update (0x416700) moves them, half step and slowdown factor
  included. The player is at [0x4DB67C] (position +0x5E0), with its 8 options at +0xD6EC (0xE4
  bytes each) and its 256 shots at +0x6C8 (0xD0 bytes each) - none of which has a velocity to read,
  so they are measured frame by frame, the same way th15's are. The straight lasers are the
  manager at [0x4DB664] (list head +0x18, links at +0x08), reached through a vtable white list:
  LaserLineInf (0x4BE434), LaserInfiniteInf (0x4BE3CC) and LaserBeamInf (0x4BE364) are handled and
  LaserCurveInf (0x4BE2FC) is left alone, with the position at +0x54, the angle at +0x6C and the
  length at +0x70.

  Confirmed in-game: every bullet in play is counted as projected, the player with its options and
  its shots moves with them and keeps doing so after a stage is restarted, and the frames land
  evenly on the display (about 3 presentations per game frame, ~179 per second, on the 180hz
  display it was tested on). **The enemies
  are not done** - the requirements list them, but neither game has a velocity to move an enemy
  body by, its animation script decides where it is drawn, and guessing is the one thing the
  requirements rule out - and the curve lasers stay out of scope. The ini and both READMEs were
  changed to say the same.

- **Touhou 15's frame interpolation covers the player, its options, its shots and the straight
  lasers** (`14e0551`)

  With only the bullets moving forward, the extra presentations showed a half frozen screen: the
  bullets travelled while the player, the options beside it, the bullets it fires and the straight
  lasers the enemies put up all repeated the frame the game's own presentation had shown. None of
  them has a velocity to read back - the player moves by what the input says, the options and shots
  come after it smoothly, and a laser turns and grows as it goes - so the patch measures instead:
  every game frame, at the point where the game presents, the fields the draw actually reads are
  compared with the frame before, and the extra presentations write those differences back times
  the fraction of a frame they stand for, then put them back right after the render. Something
  that didn't move - a held picture, a paused game - comes out as zero on its own, and something
  that only just appeared, or a pool slot handed to another object, has nothing to compare against
  and is left alone for that frame.

  What is moved is what the draw reads: the player's own position at +0x618, which the render
  copies into its animation object, and the animation objects themselves (+0x5EC) for the options
  and the player's shots, whose positions are written by the update. The lasers are walked through
  their own manager's list (0x4E9BA0), and only the straight kinds are handled (the line, infinite
  and beam vtables); those carry their own position (+0x54), angle (+0x6C) and length (+0x70),
  which is exactly what their draw builds the picture from. The curve laser is deliberately left
  alone, as the requirements ask, and the manager's own sentinel entry is skipped because its
  vtable isn't one of those.

  Two things went wrong on the way and are fixed: the first cut called the game's own sprite
  lookup through a function pointer, whose argument is passed on the stack, so it took back a
  pointer that wasn't an animation object and the game crashed as soon as a stage loaded; and the
  "position of the frame before" was kept in a table that is rebuilt every frame, so every
  difference came out as zero and the player never moved. Confirmed in-game: the player's, its
  options' and its shots' sprites are tracked and move with it, and the straight lasers in play
  (12 in the test) are found and projected. The enemies are still to come - they are the last
  piece of the stage 1 work - and the curve lasers stay out of scope.

- **The overlay shows the rate the frames are actually presented at** (`bad3516`)

  With the interpolation on, a game frame reaches the screen more than once, so the game's own
  frame rate stops describing what the display is getting, and nothing said whether the extra
  frames were landing evenly - the only way to tell was the diagnostic dump. The overlay now
  carries a second line with the rate the frames are really presented at, counted from the
  presentations themselves (the game's own one and every extra one), so it shows what the display
  gets rather than what the config asked for. It only appears when the overlay and the
  interpolation are both on, since with the interpolation off it would just repeat the rate the
  game already runs at, and it is measured over a 0.5 second window so it doesn't flicker.

  The line goes at the right edge of the play area rather than in the bottom right corner of the
  window, because that corner is where thprac draws its own frame rate and slowdown readout and
  the two ended up on top of each other. The game hands the overlay the anchor in its own
  coordinates (416 out of 640, the play field being 384 wide and starting at x = 32) and the
  overlay scales it to whatever the back buffer is, so it lands beside the picture whether it is
  presented 1:1 or scaled up. Confirmed in-game: it sits just to the right of the play field,
  clear of thprac's reading, and reports about 3.0 presentations per game frame at 180/s with
  `Interpolation = -1` on a 180hz display. The anchor is th15's own play field for now; another
  game that interpolates would need its own numbers.

- **Window mode: `CoverTaskbar` keeps the taskbar out of the way without holding the window on
  top** (`7104d0e`)

  Covering the taskbar means sitting in the topmost band, because the taskbar is a topmost window
  itself - which is why `AlwaysOnTop` has always come with the game covering every other window as
  well, more than someone who just wants a borderless screen-size window without the taskbar
  needs. `CoverTaskbar` splits the two apart and only keeps the taskbar out of the way while the
  window is the active one: 1 tells the shell the window is a fullscreen one
  (`ITaskbarList2::MarkFullscreenWindow`) so it steps the taskbar aside, with the window staying an
  ordinary one so that whatever is switched to still covers the game; 2 holds the window in the
  topmost band only while it is active, the same picture as `AlwaysOnTop` while playing but
  letting go the moment another window is activated. 1 falls back to 2 when the taskbar list can't
  be had.

  The first cut hung the game before it drew a single picture, for two reasons: it took the game's
  window procedure over to watch for activation, and changing the state sends messages back into
  that same procedure (SetWindowPos sends the position and activation changes, and the shell
  answers), so it re-entered itself and never came back; and it asked the shell for its taskbar
  object on the game's thread, a cross-process call that can block for as long as the shell is
  busy, which is fatal in the middle of starting up. The state is watched by a thread of its own
  now, which looks at which window is active every 50ms, waits until the window is actually up,
  and only calls anything when the answer changes; everything that can block happens on that
  thread, and the game's own thread only starts it. Confirmed in-game. The honest limit is that
  nothing can be done while the window isn't the active one - a window that isn't topmost is below
  the taskbar by definition - so this is a taskbar that stays out of the way while playing, not one
  that is gone for good.

### Fixed

- **Restarting a stage (ESC+R) could leave the player, its options and its shots unprojected**
  (`8efd72f`)

  A measured sprite keeps a record of where it was in the frame before, in a table keyed by the
  sprite's address, and those records were only ever added: nothing tells a sprite that is gone
  from a pool slot that is still up, so the shots of a stage fill the table up, and every sprite
  after them that isn't in it already stops being moved. The player itself is the first thing
  recorded in a frame, so a fresh stage always finds its own record - but restarting a stage
  builds the player at a new address, which needs a new record, finds none, and its projection
  quietly stops. That it only happened sometimes is the rest of the story: malloc puts the new
  player where the old one was often enough for the old record to be found and everything to work.
  The options and the shots go the same way as soon as their sprites move to addresses that aren't
  in the table.

  The records are recycled now: an empty slot is used first, and when there is none the record
  that was measured longest ago is taken, which has nothing to compare against anyway; a record
  that is in use by the frame being measured is never taken, so nothing that is on screen loses
  its place. The lasers get the same treatment, since they are handed out and freed one at a time
  and drift in the same way. th15's implementation had the same bug and gets the same fix. The
  diagnostics gained a `player: N sprite(s) this frame had no movement record` line, which stays 0.

### Changed

- **The reference config and the READMEs are bilingual and say what the interpolation actually
  does** (`bad3516`)

  The ini's comments were English only, which is the wrong way round for the people most likely to
  read them, so every entry is written in Chinese first and English after, with the default each
  one documents aligned to its value. The example blocks in README.md and README.en.md are
  generated from the ini so all three stay identical, which is what the READMEs claim they are.
  The `Interpolation` entry now spells out that the extra frames are made by *extrapolating* -
  position += velocity times the fraction of a frame, drawn, put back - rather than by keeping the
  previous frame and interpolating between the two, and it lists what that covers so far: th15's
  bullets, the player with its options and shots, and the straight lasers, with the enemies not
  done yet and the curve lasers left alone.

- **The interpolation's rate readout moved next to the render time line in the bottom left
  corner** (`8efd72f`)

  It sat at the right edge of the play field, which was only picked to keep it clear of thprac's
  own frame rate and slowdown readout in the bottom right corner of the window. The patch already
  draws a line of its own in the bottom left corner - how long the render took - and both of them
  are its own readouts, so having them side by side is what makes them useful together. The
  overlay can be asked to follow that line now (`overlay_set_present_rate_follow_text`), and both
  games ask for it. Confirmed in-game in both.

- **The reference config that ships has the interpolation and the spin wait on by default**
  (`cb68840`)

  The ini that ships is the one most people end up running, so its values are the defaults in
  practice, and the two that matter most for what this fork is for were still at the values from
  before the interpolation existed: `Interpolation` was `0` (off, so the motion smoothing had to be
  found and turned on by hand) and `Sleep` was `1` (the vpatch-style wait, which costs less CPU but
  is slightly less precise than the spin wait). Both are what the games are actually played with
  now - `Interpolation = -1`, which picks the largest multiple of the game's frame rate the display
  can show, and `Sleep = 0`, the spin wait - and the built-in defaults are moved to match, so the
  "默认 / Default" line the reference config documents keeps being the value the patch uses when
  the file is missing. Setting `Interpolation` back to `0` turns the interpolation off.

## 2026-10-08

### Added

- **Touhou 15 gets frame interpolation** (`641f5ac`, `7456449`)

  The game keeps running at its own frame rate, but every one of its frames is put on screen more
  than once - 60fps logic on a 180hz display is presented at 0, half a frame and one frame - which
  is what the new `Interpolation` option (off by default) sets up. The extra presentations draw the
  state the game is already in, so they need something inside them that moves to be worth anything:
  the bullets are projected forward by velocity * the fraction of a frame the presentation stands
  for, before each extra presentation and put back right after the render. That is the movement the
  game applies to a bullet itself between two of its frames, and the game's own logic never sees
  the projected positions.

  The extras are presented with the device's own Present, taken before any tool can replace it:
  thprac's Present hook measures the input latency assuming one present per game frame, and with
  the extras it would report a fraction of a frame instead. Presenting the scene again also erases
  the overlay a tool has already drawn for the frame - thprac draws its ImGui overlay from the
  return of the object render - so the value that says "the tool's frame is built" is found at
  runtime and set again before each extra presentation, which draws that same overlay into every
  one of them without running any of the tool's own code: running it again would advance ImGui's
  key repeat once per presentation, and holding a direction key would skip through its menus
  several times too fast.

  The bullets are found through the manager the game builds for them when a stage starts - the one
  that loads bullet.anm - rather than by looking at what their sprites look like, so this is
  exactly the game's bullets: an enemy bullet isn't one of the sprite objects the rest of the
  screen is made of, it is a larger object of its own on that manager's list, with its position and
  the movement it gets per frame next to each other at another set of offsets. Confirmed in-game:
  the presentations land evenly on a 180hz display with the game's own frame rate unchanged, the
  bullets move between them (the diagnostics report every bullet in play as projected), and the
  pause screens, the menus, thprac's overlay and its latency reading all behave. The enemies and
  the straight lasers are not projected yet - they come with their own later steps - and the curve
  lasers won't be done at all.

- **Touhou 19 v1.10c is supported** (`0c8a926`)

  th19 is a D3D9 game, so it gets the D3D9 side of the patch - the overlay, the D3D9Ex upgrade
  and the frame limiter - and, like the other D3D9 games, no question about the screen mode on
  boot. It has no replay system, so there is no replay speed control for it. Its executable is
  the first one with ASLR enabled, so every address is rebased onto wherever it got loaded
  instead of being used as it is.

  Unlike the older games, th19 doesn't run its logic once per rendered frame: each update
  advances the engine's clock by one frame at a time and only runs the game's logic once that
  clock says a frame has passed. How long a frame is comes from a hardcoded 60fps that's read
  out of a few instructions, so replacing the frame limiter alone left the game running at its
  original speed no matter what `GameFPS` was set to. Those instructions read the patch's frame
  rate now, which is what thprac's FPS option does for th11 and up.

- **Touhou 20 v1.00c is supported** (`18e1b2f`)

  th20 runs on the same engine as th19, so the same support applies to it. Unlike th19 it has a
  replay system, so it also gets replay speed control: holding the shoot key fast forwards a
  replay and holding the slow key slows it down. The shoot key is th20's own replay fast
  forward - holding ctrl does nothing in these games, which is how th17 and th18 behave too -
  and the frame rates used are `ReplaySkipFPS` and `ReplaySlowFPS`.

### Fixed

- **th19 and th20 had a whole frame of extra input lag, and the overlay reported the wrong
  time** (`18e1b2f`)

  Both games pace their frames with a clock of their own instead of calling their frame limiter
  from the gameplay path, and the limiter was ticked from the D3D9 EndScene hook, which runs
  before the frame is presented: the wait landed between the input being read and the picture
  being presented, so thprac's latency display showed a full frame (~16ms) instead of the ~1ms
  the game needs.

  The wait happens right where the game itself waits now - after the previous frame was
  presented, before the input is read - and it uses the time the game has already worked out:
  right before its own `Sleep(1)` the code has the time left until its clock's next deadline,
  so the wait ends exactly when that clock says the frame is due and the two can't drift apart.
  The wait is precise where the game's own `Sleep(1)` wasn't, which is also where the occasional
  2.5ms latency spikes came from, and the overlay reports how long the game's own work took -
  the same thing thprac shows - instead of the whole frame. `BltPrepareTime` doesn't apply to
  these two games any more: the wait is the game's own clock and not a schedule of the patch's.

- **Missing out on a multiple of 60hz display mode no longer quits the game** (`0c8a926`)

  With `FullscreenRefreshRate = 2` (the default) a fullscreen resolution that has no refresh
  rate that's a multiple of 60 ended in a panic message that killed the game. It lets the
  runtime pick a mode for it instead, the same way the D3D8 path already did.

### Changed

- **An external tool's frame rate wins over `GameFPS` on th19 and th20** (`18e1b2f`)

  Both games run their own clock off a frame rate, and thprac points that at a variable of its
  own for them: its modules for these two games don't look for the patch at all, so instead of
  handing its frame rate over through the patch's API it patches the engine's clock itself. The
  patch takes the operand back when that happens, because the replay speed control needs to own
  the clock, but it reads that variable and follows its value - so an external tool's frame rate
  still applies, and takes priority over the configured `GameFPS`.

  Replay speed control always goes by the patch's own `ReplaySkipFPS` and `ReplaySlowFPS` on
  these two games, since thprac's replay sliders can't reach the patch there; they can be set in
  the ini instead.

### Hotfix

- **th06 to th18 ran at high speed in v1.6** (`5ea9c18`)

  The deduplication the limiter got for th19 and th20 treated a tick that arrived sooner than a
  quarter of a frame as one belonging to the frame that was already being timed and skipped the
  wait for it. That holds for th19 and th20, which keep a clock of their own and are only ticked
  after their own wait - but not for th06 to th18, where the limiter is the one doing the
  waiting: there the tick measures the game's own work, which is usually only a millisecond or
  two, so most frames were not waited out at all, and those games ran at high speed with
  `GameFPS` doing nothing.

  th19 and th20 tick through `Limiter::TickUntil` and never had that check, so they were
  unaffected. The check is gone again, and the older games are back to their configured speed.

## 2026-10-06

### Added

- **The overlay works for D3D8 games that never go through D3D9** (`d0cc56a`, `ed756d7`)

  The overlay was D3D9 only - it is created in the D3D9 CreateDevice hook and drawn in the
  EndScene hook - so it worked with d3d8to9 and DXVK, which translate D3D8 to D3D9, but not
  with dgVoodoo2, which translates D3D8 straight to D3D11, or with no wrapper at all. A D3D8
  version of the overlay now draws with the D3D8 device the game already uses, and it is only
  used while there is no D3D9 device, so the wrappers that do go through D3D9 keep using the
  D3D9 overlay.

- **`FullscreenRefreshRate` is applied to the D3D8 games too** (`5d1d73f`)

  The option only existed in the D3D9 hook, so it did nothing for the D3D8 titles: they build
  their presentation parameters through IDirect3D8, and since the games ask for the current
  desktop rate, every value looked like it forced the maximum refresh rate. The D3D8
  CreateDevice and Reset hooks choose the rate the same way now, and let the runtime pick
  instead of failing when the display has no suitable mode.

- **vsync is forced off for the D3D8 games too** (`1dd1bb2`)

  Setting `D3DPRESENT_INTERVAL_IMMEDIATE` (together with `D3DSWAPEFFECT_DISCARD` and a back
  buffer count of 0) only happened in the D3D9 hook, so with dgVoodoo2 or without a wrapper
  the games kept presenting with the interval they asked for and replay skipping stayed capped
  by the display refresh rate. The D3D8 hook does the same now, for fullscreen devices - D3D8
  requires its two FullScreen_* fields to stay zero while windowed.

### Fixed

- **Alt-tabbing back into an exclusive fullscreen D3D8 game quit the game** (`d0cc56a`)

  The games release their `D3DPOOL_DEFAULT` surfaces and then call `Reset`, and they quit when
  that fails. D3D8's Reset fails while any default pool resources are still alive, and the
  overlay's own resources were released after the reset instead of before it, so a fullscreen
  D3D8 game exited as soon as the window was focused again.

- **The overlay moved to the corner of the pause screen on D3D8 games** (`ed756d7`)

  State blocks don't carry the viewport around, so the overlay was drawn with whatever
  viewport the game had set - and PCB's pause screen draws the picture into a smaller area. It
  sets its own viewport and transforms on every draw now, the same way thprac's overlays do.

### Changed

- **The missing D3D8 wrapper notice is a console message, and it recommends dgVoodoo2** (`d4296fc`, `5df484c`)

  A D3D8 game that never goes through D3D9 only loses the D3D9Ex input lag reduction now that
  the overlay and the other D3D8 settings work without it. That is worth a line in the debug
  console instead of a message box on every launch, and the recommendation is dgVoodoo2 first,
  since its flip model presentation measures best for input lag.

- **The reference config ships with default values** (`dee2a1e`)

  `openinputlagpatch.ini` had the values from testing (BltPrepareTime 0, Sleep 0, the window
  mode settings and so on) instead of the defaults its own comments document, which is
  confusing when comparing the two. Every entry is at its default now, so the file documents
  the options without turning anything on by itself, and the README recommends dgVoodoo2 for
  the D3D8 games as well.

## 2026-10-05

### Added

- **Window mode: the same `[Window]` configuration as vpatch, and a question about the screen
  mode on boot** (`26b310a`)

  `openinputlagpatch.ini` now has vpatch's `[Window]` section: `AskWindowMode`, `enabled`,
  `X`, `Y`, `Width`, `Height`, `TitleBar` and `AlwaysOnTop`, with the same meaning as in
  `vpatch.ini`. While a game runs in window mode its window is moved and resized to those
  values, and the title bar and always on top options are applied.

  With `AskWindowMode` turned on, Touhou 6 to 9.5 ask whether they should start in fullscreen
  before they create their window, so the screen mode no longer has to be changed in the
  games' own settings. The answer is written into the game's own "windowed" flag, the same way
  vpatch does it, instead of only forcing the D3D presentation parameters: with dgVoodoo2 set
  up to emulate fullscreen as a borderless window (`AppControlledScreenMode = false`,
  `FullscreenAttributes = fake`) that request is ignored, so a game whose own setting says
  windowed kept behaving like a windowed one.

  The question is asked right after the game creates its single instance mutex, which is the
  earliest point where tools like thprac can still detect the game, and changing the mode from
  inside the game afterwards keeps working. Only Touhou 6 to 9.5 are asked, because the later
  games have their own option for it.

## 2026-10-04

### Fixed

- **Spurious "couldn't hook Direct3DCreate9" warning when using dgVoodoo2** (`d20a521`)

  dgVoodoo2 translates D3D8 to D3D11, so its `d3d8.dll` has no `Direct3DCreate9` import to hook,
  and the patch showed the warning even though the game runs fine. The loaded `d3d8.dll` is now
  identified precisely by its version resource (`ProductName` contains `dgVoodoo`, checked across
  all language/codepage blocks) and the warning is skipped only in that case. An unknown wrapper
  still gets the warning, and d3d8to9 still takes the normal hook path.

- **Touhou 6 with dgVoodoo2: acceleration could not exceed the display refresh rate** (`667fe5e`)

  dgVoodoo2 turns the game's D3D8 present into a D3D11 flip-model swapchain, and that present
  blocks on vblank once the queue is full. Since the game logic presents once per frame, the game
  speed was pinned to the display refresh rate, so replay skip and thprac speedup could not go any
  faster. The D3D8 present is now hooked (vtable index 15) and submitted every Nth frame, with
  `N = ceil(target framerate / refresh rate)`, and only while the target framerate is above the
  refresh rate. At normal framerates `N` is 1, so every present is forwarded unchanged.

  Currently installed for Touhou 6 only, since the cap appears to be specific to that game -
  the later D3D8 titles are unaffected and the D3D9 titles use a different present path.

- **New `AlwaysBlt` option: keep rendering while the game window isn't active** (`12fe031`, `c41c53d`, `ea884df`)

  Switching away from the window froze the picture. Two things caused it: the games stop
  rendering on their own once they lose focus (`GameWindow::Render` returns immediately while
  the window isn't active), and oilp's window update hook stopped calling the game's window
  update while unfocused - which is also what drives rendering, so nothing was drawn at all.

  AlwaysBlt now bypasses the early return (turning the conditional jump at `0x004206F0` for
  th06 and `0x004346F0` for th07 into an unconditional one) and lets the window update keep
  running while unfocused, the same behaviour as vpatch's AlwaysBlt option. It is enabled by
  default and can be turned off with the `AlwaysBlt` entry in `openinputlagpatch.ini`; a
  reference copy of that config now ships alongside the build.

  Implemented for th06 and th07 only.
