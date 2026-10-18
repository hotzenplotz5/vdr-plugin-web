# Seahawk vdr-plugin-web — dual packaging modes (experimental)

The VDR-Suite HbbTV implementation is **already included in the current Git
`main` tree**. There are two intentionally different source modes:

1. **Direct Git checkout** (future Daily branch source): `debian/patches/series`
   applies only Seahawk's `enable_fastscale.patch` and
   `fix_thrift-services_ftbs.patch`. The historical HbbTV patches **must
   not** be applied again, because all HbbTV files are present already.
2. **Pinned Seahawk source reconstruction**: `prepare-source.sh` downloads
   the **unchanged**, SHA-256-pinned historical Seahawk upstream and Debian
   tarballs, then applies the four HbbTV patches
   `debian/patches/0001`–`0004` to that original source only. It also adds
   `${vdr:Depends}` and opt-in CEF helper tools. This historical workflow
   is independently tested, but is **not** the source model of the new Daily
   Git-tree build.

Standalone reconstruction as an ordinary user:

```sh
bash packaging/seahawk/prepare-source.sh ./quilt-output-noble noble
bash packaging/seahawk/prepare-source.sh ./quilt-output-resolute resolute
```

The generated source is unsigned, is not uploaded, and does not touch the
productive yaVDR. `cefbrowser` and `remotetranscode` are separate HbbTV
runtime requirements. The direct Git-tree binary build and a Launchpad source
recipe have different gates: before turning on `Built daily`, confirm the
`3.0 (quilt)` upstream tarball provenance, monotonic package versions for
Noble/Resolute, ABI dependencies and end-to-end installation from the actual
published PPA. A green build from a pinned source alone is insufficient.
