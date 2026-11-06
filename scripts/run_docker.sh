#!/usr/bin/env bash
# Build the Docker image (cached after the first time), run the placer on a
# netlist and render the GIF. Paths must be inside the current directory,
# which is mounted into the container.
#
#   scripts/run_docker.sh testcases/netlist_L.txt                 # -> output/netlist_L/
#   scripts/run_docker.sh testcases/netlist_L.txt out/L --bmp     # custom dir + extra options
set -euo pipefail

if [[ $# -lt 1 || $1 == -h || $1 == --help ]]; then
    sed -n '2,8p' "$0" | sed 's/^# \{0,1\}//'
    exit 1
fi

repo=$(cd "$(dirname "$0")/.." && pwd)
image=${GORDIAN_IMAGE:-gordian}
netlist=$1
shift
name=$(basename "${netlist%.*}")
out=output/$name
if [[ $# -gt 0 && $1 != -* ]]; then
    out=$1
    shift
fi

docker build -q -t "$image" "$repo" >/dev/null
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD":/work -w /work "$image" \
    bash -c 'gordian "$1" -o "$2" "${@:3}" && gordian-visualize "$2"' _ "$netlist" "$out" "$@"
