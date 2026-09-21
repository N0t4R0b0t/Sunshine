#!/usr/bin/env bash
# Build the end-to-end host: the real browser session layer plus a small ffmpeg-backed stream.
#
# It needs only libdatachannel, which the main build already produces, so it does not require (or
# risk) a full Sunshine link. Point BUILD_DIR at a build tree where `datachannel` has been built:
#   ninja -C cmake-build-webrtc datachannel
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="${BUILD_DIR:-$root/cmake-build-webrtc}"
ldc="$build/third-party/libdatachannel"
out="${OUT:-$here/build}"

for lib in "$ldc/libdatachannel.a" "$ldc/deps/libjuice/libjuice.a" "$ldc/deps/libsrtp/libsrtp2.a" "$ldc/deps/usrsctp/usrsctplib/libusrsctp.a"; do
  [ -f "$lib" ] || { echo "missing $lib - run: ninja -C $build datachannel" >&2; exit 1; }
done

mkdir -p "$out"
flags=(-std=gnu++23 -O1 -g -DRTC_STATIC -DRTC_ENABLE_MEDIA=1 -DRTC_ENABLE_WEBSOCKET=0
       -I"$here/shim" -I"$root" -I"$root/src/webrtc" -I"$root/third-party/libdatachannel/include")

# Objects are built in parallel but kept to three jobs, since this machine runs out of memory easily.
compile() { c++ "${flags[@]}" -c "$1" -o "$2"; }
pids=()
compile "$here/e2e_host.cpp" "$out/e2e_host.o" & pids+=($!)
compile "$root/src/webrtc/session.cpp" "$out/session.o" & pids+=($!)
# MEDIA_SRC lets a test build a deliberately broken copy without touching the real source.
compile "${MEDIA_SRC:-$root/src/webrtc/media.cpp}" "$out/media.o" & pids+=($!)
for pid in "${pids[@]}"; do
  wait "$pid" || { echo "compile failed" >&2; exit 1; }
done

c++ -o "$out/e2e_host" "$out/e2e_host.o" "$out/session.o" "$out/media.o" \
  "$ldc/libdatachannel.a" "$ldc/deps/libjuice/libjuice.a" "$ldc/deps/libsrtp/libsrtp2.a" \
  "$ldc/deps/usrsctp/usrsctplib/libusrsctp.a" -lssl -lcrypto -lpthread
echo "built $out/e2e_host"
