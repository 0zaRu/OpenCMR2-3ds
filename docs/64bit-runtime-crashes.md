# Native runtime crash fixes

Synced through CMR2Decomp 9be32b5, retaining 2924 byte-exact MSVC6 functions.

- Ghost alpha setup uses Mesh::flags; its old +0x30 store corrupted the
  native triangle pointer before Mesh_Rebuild.
- Network sessions and all twenty enumeration entries use NetSessionDesc;
  password stores no longer overlap the native session-name pointer. The
  browser passes its password as char * without truncating it through int.
- Original replay/ghost files keep their 0x110 disk header. replay_disk converts
  between that representation and the native 0x138 header and owns its decoded
  copy, including when the source is a borrowed archive entry.
- Input replay snapshots pack original Car/CarPartSet scalar layouts and restore
  them while preserving live native pointers. The controller steering option
  remains numeric, including across disk round trips.
- Includes the preceding native shadow-buffer family and shared-vertex fix.

All 21 native CTests pass, including replay disk round trips and malformed
headers, original physics parity at six cadences, and runtime_scenarios:
500 ticks each for host and arcade time trial, 1000 for recording/playback.
Playback packet cursors advance and finish; they count compressed packets,
not simulation ticks. This does not yet reproduce the actual finish-menu loop.
Terrain visibility and the Italy light flash still require separate fixes.
The source recovery, layouts, original comparisons and native sanitizer
checks are documented in CMR2Decomp/docs/structure-recovery/64bit-crash-runtime.md.
