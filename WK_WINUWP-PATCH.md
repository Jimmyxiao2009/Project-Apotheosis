# WK_WINUWP patch set

This repository includes `wk-winuwp.patch`, the current working-tree patch
for the WebKit source tree used by Project Apotheosis.

## Scope

- Base: `webkitgtk-2.52.4`
- Base commit: `e4ab5336695fe76b623682915737c3ee88f2e0ed`
- Patched working-tree commit: `7acdf5e11af175e98cf7dcfcbcfcde11a50bc6ce`
- Files changed: 65 under `Source/`
- Diff: 852 insertions and 72 deletions

The patch contains the ARM32 Windows 10 Mobile / UWP adaptations used by the
`gpu-path1` development line. It is intentionally a source diff rather than a
copy of the multi-gigabyte WebKit checkout.

## Apply

From the root of a WebKit checkout at the base tag:

```powershell
git checkout webkitgtk-2.52.4
git apply --3way path\to\wk-winuwp.patch
```

The patch is generated with paths relative to the WebKit root, so it should be
applied from that root. Review the resulting diff before building.

This is the source artifact requested in
[Issue #3](https://github.com/MoonlightLabCN/Project-Apotheosis/issues/3).
