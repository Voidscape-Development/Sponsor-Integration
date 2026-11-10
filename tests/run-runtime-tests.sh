#!/usr/bin/env bash
#
# Runs the runtime tests: every scenario of runtime-test in its own process,
# headless under Xvfb, against real libobs and the stock OBS plugins.
#
#   tests/run-runtime-tests.sh <build dir> [scenario ...]
#
# Needs: xvfb-run, python3, and OBS installed (libobs plus the image-source,
# obs-transitions and obs-ffmpeg plugins). ffmpeg is used to make the stinger
# video; without it the stinger scenario is skipped.
#
# Configure the build with -DENABLE_RUNTIME_TESTS=ON first. Set
# RUNTIME_TEST_VERBOSE=1 to print every event and pixel change.

set -uo pipefail

build_dir=$(cd "${1:?usage: $0 <build dir> [scenario ...]}" && pwd)
shift
repo_dir=$(cd "$(dirname "$0")/.." && pwd)

plugin="${build_dir}/sponsor-integration.so"
test_bin="${build_dir}/runtime-test"

for f in "${plugin}" "${test_bin}"; do
  if [[ ! -x "${f}" && ! -f "${f}" ]]; then
    echo "missing ${f} - configure with -DENABLE_RUNTIME_TESTS=ON and build first" >&2
    exit 2
  fi
done

work=$(mktemp -d)
trap 'rm -rf "${work}"' EXIT

# three solid 64x36 images: red, green, blue
mkdir -p "${work}/images"
python3 - "${work}/images" <<'EOF'
import struct, sys, zlib

def png(path, rgb, w=64, h=36):
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

png(sys.argv[1] + '/1_red.png', (255, 0, 0))
png(sys.argv[1] + '/2_green.png', (0, 255, 0))
png(sys.argv[1] + '/3_blue.png', (0, 0, 255))
EOF

# a 13 second white stinger, longer than any transition's grace period
stinger=""
if command -v ffmpeg >/dev/null; then
  if ffmpeg -loglevel error -f lavfi -i color=c=white:s=64x36:r=30:d=13 -pix_fmt yuv420p \
    "${work}/stinger.mp4"; then
    stinger="${work}/stinger.mp4"
  fi
fi

scenarios=("$@")
if [[ ${#scenarios[@]} -eq 0 ]]; then
  scenarios=(nogap gap wrap end controls hidden_restart retype hammer scene stinger ad_playlist missing_files)
fi

failed=()
skipped=()

for scenario in "${scenarios[@]}"; do
  if [[ "${scenario}" == stinger && -z "${stinger}" ]]; then
    skipped+=("${scenario} (no ffmpeg)")
    continue
  fi

  log="${work}/${scenario}.log"
  xvfb-run -a -s "-screen 0 1280x720x24" \
    "${test_bin}" "${scenario}" "${work}/images" "${plugin}" "${repo_dir}/data" ${stinger:+"${stinger}"} \
    >"${log}" 2>&1
  status=$?

  # the scenario's own report, without libobs' logging
  grep -E '^(==|  (ok  |FAIL)|[ 0-9]+\.[0-9]{2}  )' "${log}"

  if [[ ${status} -ne 0 ]]; then
    failed+=("${scenario}")
    if ! grep -q '^== .*: FAILED' "${log}"; then
      echo "  ${scenario} exited with status ${status}; last lines of its log:"
      tail -n 30 "${log}" | sed 's/^/    /'
    fi
  fi
done

echo
for s in "${skipped[@]+"${skipped[@]}"}"; do
  echo "skipped: ${s}"
done

if [[ ${#failed[@]} -ne 0 ]]; then
  echo "FAILED: ${failed[*]}"
  exit 1
fi

echo "all runtime tests passed"
