#!/usr/bin/env bash
# Prove the running build service is the sandbox the design says (docs/DEV_LOOP.md, "Trust"), from the
# live containers. Run on the build host after deploy.sh, and after a docker or kernel upgrade:
#
#   ./verify.sh     # every check; exit 0 only if all pass
# shellcheck disable=SC2015  # `ok` and `bad` only echo: A && ok || bad is a safe if-else here
set -euo pipefail
cd "$(dirname "$0")"
# shellcheck disable=SC1091
. ./build.env
A=blinky-build-api-1
W=blinky-build-worker-1
fail=0
ok() { echo "ok   $*"; }
bad() { echo "FAIL $*"; fail=1; }
is() { [ "$2" = "$3" ] && ok "$1: $2" || bad "$1: got '$2', want '$3'"; }

for c in $A $W; do
  echo "== $c"
  is "health" "$(docker inspect -f '{{.State.Health.Status}}' "$c")" healthy
  is "user" "$(docker inspect -f '{{.Config.User}}' "$c")" 10001:10001
  is "cap_drop" "$(docker inspect -f '{{json .HostConfig.CapDrop}}' "$c")" '["ALL"]'
  is "security_opt" "$(docker inspect -f '{{json .HostConfig.SecurityOpt}}' "$c")" '["no-new-privileges:true"]'
  is "privileged" "$(docker inspect -f '{{.HostConfig.Privileged}}' "$c")" false
  is "mount types" "$(docker inspect -f '{{range .Mounts}}{{.Type}} {{end}}' "$c" | tr ' ' '\n' | sort -u | xargs)" volume
  is "docker socket" "$(docker inspect -f '{{range .Mounts}}{{.Source}} {{end}}' "$c" | grep -c docker.sock || true)" 0
  is "restart policy" "$(docker inspect -f '{{.HostConfig.RestartPolicy.Name}}' "$c")" unless-stopped
  is "compose project dir" "$(docker inspect -f '{{index .Config.Labels "com.docker.compose.project.working_dir"}}' "$c")" "$PWD"
  read -r cpus mem pids < <(docker exec "$c" cat /sys/fs/cgroup/cpuset.cpus.effective /sys/fs/cgroup/memory.max /sys/fs/cgroup/pids.max | xargs)
  is "cpuset" "$cpus" "$BLINKY_BUILD_CPUSET"
  [ "$mem" != max ] && [ "$pids" != max ] && ok "memory and pid caps: $mem, $pids" || bad "uncapped: memory $mem, pids $pids"
done

echo "== the worker"
is "network" "$(docker inspect -f '{{.HostConfig.NetworkMode}}' $W)" none
is "interfaces" "$(docker exec $W python -c 'import socket; print(" ".join(sorted(n for _, n in socket.if_nameindex())))')" lo
is "memory == memory+swap" "$(docker inspect -f '{{.HostConfig.Memory}}' $W)" "$(docker inspect -f '{{.HostConfig.MemorySwap}}' $W)"
docker exec $W sh -c 'env | grep -qiE "token|secret|password" && exit 1 || exit 0' \
  && ok "no credential in its environment" || bad "a credential-looking variable in the worker's environment"
docker exec $W sh -c 'touch /opt/blinky/src/.arduino/x 2>/dev/null && exit 1 || exit 0' \
  && ok "toolchain not writable" || bad "the toolchain is writable"
docker exec $W sh -c 'mv /opt/blinky/src/.arduino /opt/blinky/src/.arduino.x 2>/dev/null && { mv /opt/blinky/src/.arduino.x /opt/blinky/src/.arduino; exit 1; } || exit 0' \
  && ok "toolchain can't be renamed" || bad "the toolchain directory can be renamed (and replaced)"
docker exec $W sh -c 'rm -rf /opt/blinky/src/.arduino 2>/dev/null; test -d /opt/blinky/src/.arduino/data' \
  && ok "toolchain can't be removed" || bad "the toolchain can be removed"
is "checkout directory owner and mode" "$(docker exec $W stat -c '%u:%g %a' /opt/blinky/src)" "0:10001 1775"

echo "== the api"
is "read-only root filesystem" "$(docker inspect -f '{{.HostConfig.ReadonlyRootfs}}' $A)" true
is "published" "$(docker port $A | sort -u | xargs)" "$BLINKY_BUILD_PORT/tcp -> $BLINKY_BUILD_BIND:$BLINKY_BUILD_PORT"
url="http://$BLINKY_BUILD_BIND:$BLINKY_BUILD_PORT"
curl -fsS --max-time 10 "$url/v1/healthz" >/dev/null && ok "healthz answers on its address" || bad "healthz on its address"
is "a request without the token" "$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' "$url/v1/builds/x" || true)" 401
is "a request with a wrong token" "$(curl -sS --max-time 10 -o /dev/null -w '%{http_code}' -H 'Authorization: Bearer wrong' "$url/v1/builds/x" || true)" 401
for ip in 127.0.0.1 $(ip -o -4 addr show scope global | awk '{print $4}' | cut -d/ -f1 | grep -vx "$BLINKY_BUILD_BIND"); do
  curl -sS --max-time 3 -o /dev/null "http://$ip:$BLINKY_BUILD_PORT/v1/healthz" 2>/dev/null \
    && bad "the api answers on $ip too" || ok "not on $ip"
done

echo
[ "$fail" = 0 ] && echo "ALL CHECKS PASSED" || echo "SOME CHECKS FAILED"
exit "$fail"
