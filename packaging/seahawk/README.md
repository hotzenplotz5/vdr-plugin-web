# Seahawk vdr-plugin-web — experimental VDR-Suite quilt package

Uses the **unchanged**, checksum-pinned Seahawk upstream tarball and existing
Debian metadata as the canonical packaging base, including original changelog,
copyright, debhelper VDR plugin support and enabled fastscale patch.

The four real quilt patches under `debian/patches/` reproduce the
24-file difference between Zabrimus master
`34ded5090fbad021338c491355566dbdb4d98f9d` and
hotzenplotz5 main `35f50dd8a3a611d56039d190c093a489e4a0193e`.

To assemble an **unsigned** Noble Debian source package as an ordinary user:

```sh
bash packaging/seahawk/prepare-source.sh ./quilt-output-noble
```

Downloads are verified against the maintainer-provided .dsc SHA256.
The original `debian/patches/enable_fastscale.patch` is retained.
The prepared package adds `${vdr:Depends}` for runtime ABI checking,
ships CEF ZDF helper scripts (opt-in only), and verifies `dpkg-source -b/-x`.
This source builder does NOT install `.deb`, start VDR, upload to PPA or
modify administrator-owned browser content.

`cefbrowser` and `remotetranscode` are separate HbbTV runtime requirements.
Binary compilation, clean-install/upgrade and actual HbbTV hardware acceptance
remain required; do not install on the productive yaVDR.
