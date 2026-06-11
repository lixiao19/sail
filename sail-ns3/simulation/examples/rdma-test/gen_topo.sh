#!/usr/bin/env bash
# gen_topo.sh — Generate topology file variants for E1 (loss-rate-sweep) and E4 (latency-sweep).
#
# Usage:
#   ./gen_topo.sh [e1|e4|all]   (default: all)
#
# Output: experiments/topos/ relative to this script's directory.
# The generated paths follow the convention used by E1/E4 run.sh.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BASE_TOPO="${SCRIPT_DIR}/ft-ft-topo.txt"
OUT_DIR="${SCRIPT_DIR}/experiments/topos"

mkdir -p "${OUT_DIR}"

MODE="${1:-all}"

# ---------------------------------------------------------------------------
# E1: loss-rate variants (WAN latency fixed at 10ms)
# ---------------------------------------------------------------------------
gen_e1() {
  for loss_rate in 0 0.0001 0.0005 0.001 0.005 0.01 0.05; do
    out="${OUT_DIR}/ft-ft-loss${loss_rate}.txt"
    awk -v loss="${loss_rate}" '
      /^0 1 800Gbps / { print "0 1 800Gbps 10ms " loss; next }
      { print }
    ' "${BASE_TOPO}" > "${out}"
    echo "E1  ${out}"
  done
}

# ---------------------------------------------------------------------------
# E4: latency variants (WAN loss fixed at 0.001)
# ---------------------------------------------------------------------------
gen_e4() {
  for latency in 0.1ms 0.4ms 1ms 5ms 10ms 20ms; do
    out="${OUT_DIR}/ft-ft-lat${latency}.txt"
    awk -v lat="${latency}" '
      /^0 1 800Gbps / { print "0 1 800Gbps " lat " 0.001"; next }
      { print }
    ' "${BASE_TOPO}" > "${out}"
    echo "E4  ${out}"
  done
}

case "${MODE}" in
  e1)  gen_e1 ;;
  e4)  gen_e4 ;;
  all) gen_e1; gen_e4 ;;
  *)
    echo "Usage: $0 [e1|e4|all]" >&2
    exit 1
    ;;
esac

echo "Done. Topology variants written to ${OUT_DIR}/"
