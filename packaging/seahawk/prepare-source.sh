#!/usr/bin/env bash
# Rebuild Seahawk's source with versioned VDR-Suite quilt patches.
# No package install, service restart, root execution, or system modification.
set -Eeuo pipefail
if [[ "$EUID" -eq 0 ]]; then
  echo 'REFUSING_ROOT=1' >&2
  exit 2
fi
if [[ $# -ne 1 ]]; then
  echo "Usage: $0 NEW_OUTPUT_DIRECTORY" >&2
  exit 2
fi
repo_root="$(cd -- "$(dirname -- "$0")/../.." && pwd -P)"
out="$1"
if [[ -e "$out" ]]; then
  echo 'OUTPUT_ALREADY_EXISTS=1' >&2
  exit 2
fi
mkdir -p -- "$out"
out="$(cd "$out" && pwd -P)"
original_name='0.0.1+git20260827-111-34ded50'
archive_version="$original_name-1yavdr1~resolute"
version="$original_name-1yavdr1+vdrsuite1~noble"
url_root="https://launchpad.net/~seahawk1986-hotmail/+archive/ubuntu/vdr-2.8.3/+sourcefiles/vdr-plugin-web/$archive_version"
orig="vdr-plugin-web_$original_name.orig.tar.gz"
deb="vdr-plugin-web_$archive_version.debian.tar.bz2"
cd "$out"
curl --fail --location --silent --show-error --retry 3 --output "$orig" "$url_root/$orig"
curl --fail --location --silent --show-error --retry 3 --output "$deb" "$url_root/$deb"
printf '%s  %s\n%s  %s\n' \
  '7396b7a3c6c97d1de50beb42ff4f8aa54a3e06ae4c8046cddad000f930f550a9' "$orig" \
  '7e49fc1f653db0673edb8ef50e29b2bcb4d71de99944a6dacc87164a2850ec9c' "$deb" | sha256sum --check --status
printf 'ORIGINAL_SHA256=PASS\n'
src="$out/vdr-plugin-web-$original_name"
tar -xzf "$orig" -C "$out"
test -d "$src"
tar -xjf "$deb" -C "$src"
test "$(tr -d '\r' < "$src/debian/patches/series" | sed '/^$/d')" = 'enable_fastscale.patch'
test -s "$src/debian/patches/fix_thrift-services_ftbs.patch"
# Seahawk ships this libthrift compatibility fix but does not enable it;
# Noble's installed libthrift requires it (TOutput::instance no longer exists).
printf '%s\n' 'fix_thrift-services_ftbs.patch' >> "$src/debian/patches/series"
for patch in \
  0001-vdrsuite-hbbtv-discovery.patch \
  0002-vdrsuite-hbbtv-session-runtime.patch \
  0003-vdrsuite-hbbtv-presentation-media.patch \
  0004-vdrsuite-hbbtv-tests-and-cef-tools.patch; do
  test -s "$repo_root/debian/patches/$patch"
  cp -- "$repo_root/debian/patches/$patch" "$src/debian/patches/$patch"
  printf '%s\n' "$patch" >> "$src/debian/patches/series"
done
python3 - "$src" "$version" <<'PY'
import datetime
import pathlib
import sys
root=pathlib.Path(sys.argv[1])
version=sys.argv[2]
control=root/'debian/control'
txt=control.read_text()
old='Depends: ${shlibs:Depends}, ${misc:Depends}'
assert txt.count(old)==1, 'Unexpected Seahawk dependency line'
control.write_text(txt.replace(old,old+', ${vdr:Depends}',1))
install=root/'debian/install'
assert install.read_text().strip()=='config/sockets.ini etc/vdr/', 'Unexpected Seahawk installation layout'
install.write_text(install.read_text().rstrip('\n')+'\n'+
  'tools/apply_cefbrowser_zdf_controls_fix.py usr/share/vdr-plugin-web/vdr-suite-tools/\n'+
  'tools/detect_cefbrowser_static_root.py usr/share/vdr-plugin-web/vdr-suite-tools/\n'+
  'tools/install_cefbrowser_zdf_controls_fix.sh usr/share/vdr-plugin-web/vdr-suite-tools/\n')
changelog=root/'debian/changelog'
stamp=datetime.datetime.now(datetime.timezone.utc).strftime('%a, %d %b %Y %H:%M:%S +0000')
entry=(f'vdr-plugin-web ({version}) noble; urgency=medium\n\n'
       '  * Add VDR-Suite HbbTV services as quilt patches.\n'
       '  * Preserve Seahawk fastscale patch and original Debian packaging.\n'
       '  * Add VDR ABI dependency and opt-in CEF correction tools.\n\n'
       f' -- VDR-Suite contributors <noreply@github.com>  {stamp}\n\n')
changelog.write_text(entry+changelog.read_text())
(root/'debian/README.vdrsuite').write_text(
  'Experimental standalone source packaging on Seahawk original.\n'
  'cefbrowser and remotetranscode remain separate components.\n'
  'No CEF patching, VDR restart, or service enable is automatic.\n')
PY
dpkg-source -b "$src"
dsc="$out/vdr-plugin-web_$version.dsc"
test -f "$dsc"
dpkg-source -x --no-check "$dsc" "$out/verified"
test -f "$out/verified/service/vdrsuite_hbbtv_media_service.cpp"
test -f "$out/verified/service/vdrsuite_hbbtv_discovery_service.h"
test -f "$out/verified/tools/apply_cefbrowser_zdf_controls_fix.py"
grep -Fq 'ENABLE_FAST_SCALE ?= 1' "$out/verified/Makefile"
grep -Fq '${vdr:Depends}' "$out/verified/debian/control"
echo 'QUILT_DSC_ROUNDTRIP=PASS'
echo "SOURCE_PACKAGE=$dsc"
echo 'NOT_INSTALLED_OR_UPLOADED=1'
