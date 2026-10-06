#!/usr/bin/env bash
# Host-side entrypoint. Containers receive source read-only and an isolated work volume.
set -euo pipefail
format=${1:?package format required}
root=$(cd "$(dirname "$0")/../../.." && pwd)
destination=$(realpath -m "${2:-$root/release}")
case "$format" in
  deb) image=ubuntu:24.04; asset=dolphin-dap-ubuntu24.04-amd64.deb ;;
  appimage) image=ubuntu:24.04; asset=dolphin-dap-linux-x86_64.AppImage ;;
  rpm) image=fedora:44; asset=dolphin-dap-fedora44-x86_64.rpm ;;
  arch) image=archlinux:base-devel; asset=dolphin-dap-archlinux-x86_64.pkg.tar.zst ;;
  gpkg) image=gentoo/stage3:amd64-desktop-systemd; asset=dolphin-dap-gentoo-amd64.gpkg.tar ;;
  *) echo "Unsupported format: $format" >&2; exit 2 ;;
esac
mkdir -p "$destination"
if [[ -e "$destination/$asset" ]]; then
  echo "Package already exists: $destination/$asset" >&2
  exit 1
fi
# Use the numeric part of a release tag plus its commit to distinguish upstream snapshots.
tag=${GITHUB_REF_NAME:-$(git -C "$root" describe --tags --abbrev=0)}
base=$(sed -E 's/^v//; s/\+.*//; s/-.*//' <<< "$tag")
if [[ ! "$base" =~ ^[0-9]+(\.[0-9]+)*$ ]]; then base=0.0.0; fi
version="$base.$(git -C "$root" show -s --format=%ct)"
docker run --rm --platform linux/amd64 --shm-size=2g \
  --mount "type=bind,source=$root,target=/source,readonly" \
  --mount "type=bind,source=$destination,target=/release" \
  --mount type=volume,target=/work \
  -e PACKAGE_VERSION="$version" -e BUILD_JOBS="${BUILD_JOBS:-2}" \
  "$image" bash -c "$(cat "$root/Tools/ci/linux-packages/build-native.sh")" -- "$format"
if [[ "$format" != gpkg ]]; then
  docker run --rm --platform linux/amd64 \
    --mount "type=bind,source=$root,target=/source,readonly" \
    --mount "type=bind,source=$destination,target=/release,readonly" \
    "$image" bash /source/Tools/ci/linux-packages/smoke-installed.sh "$format"
fi
test -s "$destination/$asset"
(cd "$destination" && sha256sum "$asset" > "$asset.sha256")
