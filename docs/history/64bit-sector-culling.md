# Terrain bounds preserved in native culling

> Historical validation at the commits named below. Counts and open issues
> describe that snapshot; see [the roadmap](../PLAN.md) and
> [the generated audit](../64bit-raw-access.md) for current status.

Synced through CMR2Decomp 096b134. Sector culling's old stores at +0x7c,
+0x80 and +0x84 overlapped the native bounding rectangle. Those writes now use
viewDistanceSquared and two conservatively named integer state fields. The
bounds survive every frame. The visible-sector index list is named and all
terrain, object, node, shadow and reflection consumers use the same declaration.
The original disk record and the existing native stage loader are preserved.

The original/rebuilt comparison covers 1950 cases and complete grid memory;
all 3364 MSVC6 functions preserve their scores and 2924 remain exact.
Root sanitizer tests and sector_culling CTest cover 384 cases, three passes
per case, with high pointers and full-record/mask/list models. Restoring the
old offsets fails the bounds check. All 22 native CTests pass, including the
six-cadence original physics comparison and host/time-trial/replay fixtures.

This fixes demonstrated corruption that matches the late-terrain symptom.
Visual acceptance on all stages and the Italy flash remain unverified, as does
the exact post-finish replay menu path. Full recovery evidence is in
CMR2Decomp/docs/structure-recovery/64bit-sector-culling.md.
