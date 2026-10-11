# Runtime fixes and raw-access audit, 2026-10-11

## Corrected behaviour

- Arcade country selection now reads `Menu::cursor`. The old callback read
  byte +7 of an integer menu address; the native cursor is at +11. The old
  read returned zero from the title pointer and selected Italy. Actual menu
  navigation now loads the selected archive (`cha01` through `cha08`) for
  all eight countries in Single Race and Time Trial on both architectures.
- The Italy Arcade Championship crash came from reading the headlight
  collision-box centre with `param_2[0x25]`. That is the upper half of
  `pArray` on x64. The collision test now reads `CollisionBox::pVertex`
  and the other named fields. The runtime regression enables the headlight
  cheat seen in the user's core and drives six cars for 2500 ticks.
- Sector height/bounds queries use the actual Mesh geometry prefix and its
  native stride, rather than the incompatible SectorMesh view.
- Replay lane records, interior nodes/textures and stage archive cursors
  use their actual types. AI route counts are stored separately from native
  pointers. Shadow cylinders allocate the complete Mesh header. Camera
  heading calculations use a short local rather than overwriting a pointer.
- Replay event exhaustion now reads only the active format's event count.
  This also corrects the preservation source: the original at 0x0046d18b
  tests stream type before reading either input +0x1148 or state +0x58.
  The previous MSVC6 decompilation read state +0x58 unconditionally and
  faulted at 0x004dc0fa with a NULL state array during Japan Rally Challenge.

## Validation

The complete pre-event-fix CTest batteries passed (27 native, 28 i386 tests).
After the event correction, replay disk, original physics parity and all
runtime scenarios passed again on both targets. The new replay event test
also passes on both: input/state streams, NULL unused arrays, empty event
tables, compressed-repeat boundaries and final-event selection.

The physics fixture remains exact for its 2015 canonical values over 500
updates at six render cadences. This establishes the Finland excerpt;
it is not a claim of complete-game equivalence. See [PHYSICS_PARITY.md](PHYSICS_PARITY.md).

CMR2Decomp was rebuilt with MSVC6 and measured against the original. Its
2925 byte-exact functions remain byte-exact; none regressed. All 135
registered differential harnesses pass. The event function itself remains
non-exact despite correcting the demonstrated control-flow error.

## Audit coverage

The final audit reports 7194 accesses: 0 pending `bug`, 648 `unresolved`,
6546 `safe`. Two header-to-payload pointer formations are individually
reviewed false positives, with original automatic evidence retained and
SHA-256 guards in `tools/raw_access/reviewed.json`. No general header
exception is used. Changing guarded code or automatic evidence invalidates
the reviews. The generated report is [64bit-raw-access.md](64bit-raw-access.md).

The allocation inference now distinguishes homogeneous records from mixed
arenas, retains allocation bounds and requires AST evidence for sizeof-based
inference. Twelve audit regressions pass, including real Clang AST cases.
`safe` describes layout checks; `unresolved` still requires inspection.

## Rally Challenge remains open

The rival's Japan barrier stall occurs in both native and i386 ports and was
also observed in a 3500-tick original + verified SilentPatch capture with the
same assets. A native probe with the player parked also stalls. Correcting
the event-count crash does not establish that this trajectory problem is
fixed. No physics constants or recorded CPU controls have been tuned to
mask it. Local diagnostic recordings and executable provenance are under
`build/reference-challenge/`; the incomplete pre-fix decompilation capture
is retained as failure evidence and is not treated as a reference.

## Reproduce

```sh
cmake --build build/linux -j4
cmake --build build/linux-x86 -j4
ctest --test-dir build/linux --output-on-failure
ctest --test-dir build/linux-x86 --output-on-failure
python3 tests/raw_access_audit.py --plugin
python3 tools/audit_raw_access.py --json build/raw-access-reviewed.json \
  --markdown docs/64bit-raw-access.md --jobs 4
# Preserve the two automatic findings before individual review:
python3 tools/audit_raw_access.py --without-reviews \
  --json build/raw-access-unreviewed.json --jobs 4
```
