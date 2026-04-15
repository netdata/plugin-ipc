#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

fail() {
    printf 'FAIL: %s\n' "$*" >&2
    exit 1
}

TMP_DIR="$(mktemp -d "${ROOT_DIR}/build/compare-policy-retry.XXXXXX")"
trap 'rm -rf "$TMP_DIR"' EXIT

STATE_DIR="${TMP_DIR}/state"
OUT_DIR="${TMP_DIR}/out"
STUB_RUNNER="${TMP_DIR}/targeted-runner.sh"
mkdir -p "$STATE_DIR"

cat > "$STUB_RUNNER" <<'STUB'
#!/usr/bin/env bash
set -euo pipefail

out_dir=""
row=""

while [ "$#" -gt 0 ]; do
    case "$1" in
        --out-dir)
            out_dir="$2"
            shift 2
            ;;
        --duration)
            shift 2
            ;;
        --row)
            row="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

[ -n "$out_dir" ] || { printf 'missing --out-dir\n' >&2; exit 1; }
[ -n "$row" ] || { printf 'missing --row\n' >&2; exit 1; }
[ -n "${NIPC_COMPARE_TEST_STATE:-}" ] || { printf 'missing NIPC_COMPARE_TEST_STATE\n' >&2; exit 1; }
[ -n "${NIPC_WINDOWS_TOOLCHAIN:-}" ] || { printf 'missing NIPC_WINDOWS_TOOLCHAIN\n' >&2; exit 1; }

IFS=',' read -r scenario client server target_rps <<< "$row"
file_name="$(printf '%s' "${scenario}-${client}-${server}-${target_rps}" | tr -c 'A-Za-z0-9._-' '_').csv"
counter_file="${NIPC_COMPARE_TEST_STATE}/${NIPC_WINDOWS_TOOLCHAIN}-${file_name}.count"

count=0
if [ -f "$counter_file" ]; then
    read -r count < "$counter_file"
fi
count=$((count + 1))
printf '%s\n' "$count" > "$counter_file"

throughput=100
if [ "$scenario,$client,$server,$target_rps" = "np-ping-pong,c,c,0" ] &&
   [ "$NIPC_WINDOWS_TOOLCHAIN" = "msys" ] &&
   [ "$count" -eq 1 ]; then
    throughput=40
fi

mkdir -p "$out_dir"
cat > "${out_dir}/${file_name}" <<CSV
scenario,client,server,target_rps,throughput,p50_us,p95_us,p99_us,client_cpu_pct,server_cpu_pct,total_cpu_pct
${scenario},${client},${server},${target_rps},${throughput},1,1,1,1,1,2
CSV
STUB

chmod +x "$STUB_RUNNER"

env \
    NIPC_COMPARE_TEST_STATE="$STATE_DIR" \
    NIPC_BENCH_COMPARE_TARGETED_RUNNER="$STUB_RUNNER" \
    NIPC_BENCH_COMPARE_REPETITIONS=1 \
    NIPC_BENCH_COMPARE_POLICY_ATTEMPTS=2 \
    bash "${ROOT_DIR}/tests/compare-windows-bench-toolchains.sh" "$OUT_DIR" 1 >/dev/null

grep -q '^np-max,np-ping-pong,c,c,0,both,70.0,40.0,fail$' \
    "${OUT_DIR}/policy.attempt-1.csv" ||
    fail "first policy attempt did not record the expected np-max failure"

grep -q '^np-max,np-ping-pong,c,c,0,both,70.0,100.0,pass$' \
    "${OUT_DIR}/policy.csv" ||
    fail "final policy did not pass after retrying np-max"

msys_np_count_file="${STATE_DIR}/msys-np-ping-pong-c-c-0.csv.count"
[ "$(cat "$msys_np_count_file")" = "2" ] ||
    fail "np-max MSYS row was not retried exactly once"

printf 'PASS: Windows compare policy retry\n'
