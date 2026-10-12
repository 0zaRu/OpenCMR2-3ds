# Windows x64 validation and LAN regressions

> Historical validation from the Windows portable and LAN fix batches.
> Counts describe those snapshots; see [the current audit](../64bit-raw-access.md).
> Packaging instructions are in [WINDOWS_PORTABLE.md](../WINDOWS_PORTABLE.md).

## Validation and audit findings

The cross-build was tested with an isolated Wine x64 prefix. Its Finland
fixture matches the original plus SilentPatch for 500 updates at 30, 60, 120,
144 and 240 FPS and variable rendering cadence, with the same 25 Hz simulation.
The real window and both intro videos also start with SDL's Vulkan backend
under Wine on the desktop. Xvfb lacks DRI3 presentation support and cannot
validate that renderer. This does not replace testing on a physical Windows machine.

Actual stage startup exposed `CNetworkLeaderboards::Reset` converting a native
address to a 32-bit `int`. A Linux executable with low static addresses had
hidden the error; the Windows image starts above 4 GB. Reset now uses typed
records and still clears only each record's loaded flag. The raw-access plugin
also records pointer-to-integer widths, independently of pointee layout.

The observer now obtains matrices from native Car pointers before serializing
them, avoiding dereferencing pointer bits truncated into a Win32 capture.
The capture file format and original 32-bit recorder remain compatible.
The high-address regression fixtures reserve actual memory above 4 GB on
Windows; Wine's ordinary malloc can otherwise leave those tests below 4 GB.

The wider audit also recovered camera-offset and menu-sound cursors, a
pointer-sized stage archive cursor and the knockout round-entry pointer.
Keyboard repeat values are integer milliseconds rather than fake pointers;
result sorting uses the distance within its order array. Address arguments
to verified unused release-build parameters are passed as zero. Particle
appearance retains the original signed low-32-bit address hash explicitly;
those bits are a visual seed and are never converted back into a pointer.

The updated i386/x86-64 report contains 0 automatic bugs and 618 unresolved
accesses, down from 648 before this batch. Those remaining accesses still
need investigation. Two precise replay header/payload layout reviews retain
their automatic evidence and source guards. Pointer-width conversions are
checked even when the pointed-to record has an identical layout; merely
forming a pointer-sized one-past sentinel is not treated as dereferencing it.

The raw repository also recovers the transition callback-machine parameter.
MSVC6 emits an identical complete `.text` section, and its 11003-case original
comparison passes. Linux passes 30 x64 and 30 x86 tests, including real-data
scenarios and physics fixtures. This batch does not resolve the separate
Japan Rally Challenge rival stall also observed with the original installation.

## LAN host notification regression

The lobby was still indexing native system notifications as 32-bit words.
On a second player joining, `NetMsgCreatePlayer::lpData` lost its upper pointer
bits. Player-data updates read offset 12 instead of the native x64 offset 16;
session options started at offset 4 instead of the aligned offset 8. The
frontend now uses the provider's native message records. Race and results
departure handlers use the same typed player ID. Empty or incomplete custom
player descriptions are ignored until a complete description arrives.
The pointer-free game packets and physics remain unchanged.

`net_frontend` runs a real UDP host and guest, receives into actual memory
above 4 GB on x64, and calls the game's notification handlers. It covers
join, player updates, host options, departure and delayed/empty descriptions.
The old handler reproduces a segmentation fault in
`NetPlayers_AddOrUpdatePlayerInfo` with this fixture. All 16 Windows tests
pass under Wine.

`tests/lan_game.py` runs two independent game processes through the real
discovery, join and ready menus, then validates 500 updates, two cars and
movement of the remote car on both machines. It is registered in CTest when
original assets are supplied and the build is native. A cross-build can use
separate probe executables or a Wine wrapper:

```sh
python3 tests/lan_game.py --host-probe build/windows-probe \
  --guest-probe build/linux/tests/stage_probe --data /path/to/game \
  --output build/lan-windows-host
```

Two Windows processes and a Windows host with a Linux client pass locally
under Wine. These tests use loopback UDP and do not substitute for trying
the updated portable executable on two physical LAN computers. The five
additional unresolved audit entries are native notification casts; the
auditor cannot infer their receive-buffer layout from a raw function
parameter, so the report retains them for investigation.
