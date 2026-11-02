#!/usr/bin/env bash
set -Eeuo pipefail

uid_number=$(id -u)
gid_number=$(id -g)
if [[ "$uid_number" -eq 0 ]]; then
  printf '%s\n' 'Run this test as a non-root user.' >&2
  exit 1
fi
if ! command -v ctr >/dev/null || ! command -v sudo >/dev/null; then
  printf '%s\n' 'The ctr and sudo commands are required.' >&2
  exit 1
fi
sudo -n true

script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lucia-launcher-headless.XXXXXXXX")
runtime_dir="$work_dir/runtime"
weston_logs="$work_dir/weston"
client_logs="$work_dir/client"
weston_packages="$work_dir/weston-packages"
client_packages="$work_dir/client-packages"
mkdir -m 700 "$runtime_dir" "$weston_logs" "$client_logs" "$weston_packages" "$client_packages"
chmod 700 "$runtime_dir"
runtime_owner=$(stat -c '%u:%g:%a' "$runtime_dir")
if [[ "$runtime_owner" != "$uid_number:$gid_number:700" ]]; then
  printf 'Unexpected private runtime directory ownership/mode: %s\n' "$runtime_owner" >&2
  exit 1
fi
namespace="lucia-launcher-${uid_number}-$(date +%s)-$$"
image="docker.io/library/ubuntu:26.04"
weston_id=weston
client_id=client
namespace_created=0

ctr() {
  sudo -n ctr -n "$namespace" "$@"
}

cleanup() {
  local exit_code=$?
  local container_id
  local attempt
  trap - EXIT
  set +e

  if [[ "$namespace_created" -eq 1 ]]; then
    for container_id in "$weston_id" "$client_id"; do
      ctr tasks kill --signal SIGKILL "$container_id" >/dev/null 2>&1
      ctr tasks delete "$container_id" >/dev/null 2>&1
      ctr containers delete "$container_id" >/dev/null 2>&1
    done
    ctr images remove "$image" >/dev/null 2>&1
    for attempt in {1..20}; do
      if sudo -n ctr namespaces remove "$namespace" >/dev/null 2>&1; then
        namespace_created=0
        break
      fi
      ctr snapshots list >/dev/null 2>&1
      ctr content list >/dev/null 2>&1
    done
  fi

  if [[ -d "$work_dir" && "$work_dir" == "${TMPDIR:-/tmp}"/lucia-launcher-headless.* ]]; then
    sudo -n rm -rf -- "$work_dir"
  fi
  if [[ "$namespace_created" -eq 1 ]]; then
    printf 'Temporary containerd namespace remains: %s\n' "$namespace" >&2
    exit_code=1
  fi
  exit "$exit_code"
}
trap cleanup EXIT

sudo -n ctr namespaces create "$namespace"
namespace_created=1
if ! ctr images pull --platform linux/amd64 "$image" >"$work_dir/image-pull.log" 2>&1; then
  cat "$work_dir/image-pull.log" >&2
  exit 1
fi

socket_mount="type=bind,src=$runtime_dir,dst=/run/lucia,options=rbind:rw"
weston_log_mount="type=bind,src=$weston_logs,dst=/var/log/lucia,options=rbind:rw"
client_log_mount="type=bind,src=$client_logs,dst=/var/log/lucia,options=rbind:rw"
weston_package_mount="type=bind,src=$weston_packages,dst=/mnt,options=rbind:ro"
client_package_mount="type=bind,src=$client_packages,dst=/mnt,options=rbind:ro"

fetch_packages() {
  local task_id=$1
  local output_dir=$2
  shift 2
  if ! ctr run --rm --net-host \
    --mount "type=bind,src=$output_dir,dst=/out,options=rbind:rw" \
    "$image" "$task_id" bash -ec '
      apt-get update -qq > /out/fetch.log 2>&1 || { cat /out/fetch.log; exit 1; }
      DEBIAN_FRONTEND=noninteractive apt-get install --download-only -y --no-install-recommends "$@" >> /out/fetch.log 2>&1 || { cat /out/fetch.log; exit 1; }
      mkdir -p /out/debs /out/lists
      cp /var/cache/apt/archives/*.deb /out/debs/
      cp -a /var/lib/apt/lists/. /out/lists/
    ' bash "$@"; then
    cat "$output_dir/fetch.log" >&2
    return 1
  fi
}

fetch_packages fetch-weston "$weston_packages" weston passwd util-linux
fetch_packages fetch-client "$client_packages" python3 python3-gi gir1.2-gtk-4.0 passwd util-linux

ctr run --detach \
  --mount "$socket_mount" \
  --mount "$weston_log_mount" \
  --mount "$weston_package_mount" \
  --env "LUCIA_UID=$uid_number" \
  --env "LUCIA_GID=$gid_number" \
  --env "HOME=/tmp/lucia-home-$uid_number" \
  --env XDG_RUNTIME_DIR=/run/lucia \
  --env WAYLAND_DISPLAY=wayland-lucia \
  "$image" "$weston_id" bash -ec '
    cp -a /mnt/lists/. /var/lib/apt/lists/
    cp /mnt/debs/*.deb /var/cache/apt/archives/
    DEBIAN_FRONTEND=noninteractive apt-get --no-download install -y --no-install-recommends weston passwd util-linux > /var/log/lucia/install.log 2>&1 || { tail -40 /var/log/lucia/install.log; exit 1; }
    getent group "$LUCIA_GID" >/dev/null || groupadd --gid "$LUCIA_GID" lucia
    dpkg-query --show weston > /var/log/lucia/packages.txt
    getent passwd "$LUCIA_UID" >/dev/null || useradd --uid "$LUCIA_UID" --gid "$LUCIA_GID" --home-dir "$HOME" --no-create-home --shell /usr/sbin/nologin lucia
    install -d -o "$LUCIA_UID" -g "$LUCIA_GID" "$HOME"
    exec setpriv --reuid="$LUCIA_UID" --regid="$LUCIA_GID" --clear-groups --bounding-set=-all --inh-caps=-all --ambient-caps=-all -- weston --backend=headless-backend.so --socket=wayland-lucia --idle-time=0 --no-config --log=/var/log/lucia/weston.log
  '

client_output=
if ! client_output=$(ctr run --rm \
  --mount "$socket_mount" \
  --mount "$client_log_mount" \
  --mount "$client_package_mount" \
  --mount "type=bind,src=$script_dir,dst=/opt,options=rbind:ro" \
  --env "LUCIA_UID=$uid_number" \
  --env "LUCIA_GID=$gid_number" \
  --env "HOME=/tmp/lucia-home-$uid_number" \
  --env XDG_RUNTIME_DIR=/run/lucia \
  --env WAYLAND_DISPLAY=wayland-lucia \
  --env GDK_BACKEND=wayland \
  --env GSK_RENDERER=cairo \
  --env GSETTINGS_BACKEND=memory \
  --env NO_AT_BRIDGE=1 \
  --env GTK_A11Y=none \
  "$image" "$client_id" bash -ec '
    cp -a /mnt/lists/. /var/lib/apt/lists/
    cp /mnt/debs/*.deb /var/cache/apt/archives/
    DEBIAN_FRONTEND=noninteractive apt-get --no-download install -y --no-install-recommends python3 python3-gi gir1.2-gtk-4.0 passwd util-linux > /var/log/lucia/install.log 2>&1 || { tail -40 /var/log/lucia/install.log; exit 1; }
    getent group "$LUCIA_GID" >/dev/null || groupadd --gid "$LUCIA_GID" lucia
    getent passwd "$LUCIA_UID" >/dev/null || useradd --uid "$LUCIA_UID" --gid "$LUCIA_GID" --home-dir "$HOME" --no-create-home --shell /usr/sbin/nologin lucia
    install -d -o "$LUCIA_UID" -g "$LUCIA_GID" "$HOME"
    dpkg-query --show python3 python3-gi gir1.2-gtk-4.0 libgtk-4-1 > /var/log/lucia/packages.txt
    python3 -c "import os, time; p = \"/run/lucia/wayland-lucia\"; end = time.monotonic() + 20; exec(\"while time.monotonic() < end:\\n if os.path.exists(p): break\\n time.sleep(0.05)\"); raise SystemExit(0 if os.path.exists(p) else \"Wayland socket did not appear\")"
    exec setpriv --reuid="$LUCIA_UID" --regid="$LUCIA_GID" --clear-groups --bounding-set=-all --inh-caps=-all --ambient-caps=-all -- python3 /opt/launcher.py --smoke-click Files
  ' 2>&1); then
  printf '%s\n' "$client_output" >&2
  [[ -f "$weston_logs/weston.log" ]] && tail -40 "$weston_logs/weston.log" >&2
  [[ -f "$client_logs/install.log" ]] && tail -40 "$client_logs/install.log" >&2
  exit 1
fi

printf '%s\n' "$client_output"
for marker in \
  "CLIENT_IDENTITY: uid=$uid_number gid=$gid_number capeff=0000000000000000" \
  WAYLAND_CONNECTED \
  WINDOW_PRESENTED \
  'BUTTON_SIGNAL_OK: Files' \
  'STATUS_UPDATED: Selected: Files'; do
  if ! grep -Fq "$marker" <<<"$client_output"; then
    printf 'Missing smoke-test marker: %s\n' "$marker" >&2
    exit 1
  fi
done

printf '\nUbuntu archive packages:\n'
cat "$weston_logs/packages.txt" "$client_logs/packages.txt"
printf '\nHeadless client integration passed; no physical display/input was used.\n'