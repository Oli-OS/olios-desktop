#!/usr/bin/env bash
set -Eeuo pipefail

readonly image_repository='localhost/lucia/gui-prototype'
readonly seatd_socket='/run/seatd.sock'
readonly wayland_display='wayland-lucia'
readonly compositor_timeout=180
readonly client_timeout=60
readonly -a linux_capabilities=(
  CAP_CHOWN CAP_DAC_OVERRIDE CAP_DAC_READ_SEARCH CAP_FOWNER CAP_FSETID
  CAP_KILL CAP_SETGID CAP_SETUID CAP_SETPCAP CAP_LINUX_IMMUTABLE
  CAP_NET_BIND_SERVICE CAP_NET_BROADCAST CAP_NET_ADMIN CAP_NET_RAW
  CAP_IPC_LOCK CAP_IPC_OWNER CAP_SYS_MODULE CAP_SYS_RAWIO CAP_SYS_CHROOT
  CAP_SYS_PTRACE CAP_SYS_PACCT CAP_SYS_ADMIN CAP_SYS_BOOT CAP_SYS_NICE
  CAP_SYS_RESOURCE CAP_SYS_TIME CAP_SYS_TTY_CONFIG CAP_MKNOD CAP_LEASE
  CAP_AUDIT_WRITE CAP_AUDIT_CONTROL CAP_SETFCAP CAP_MAC_OVERRIDE CAP_MAC_ADMIN
  CAP_SYSLOG CAP_WAKE_ALARM CAP_BLOCK_SUSPEND CAP_AUDIT_READ CAP_PERFMON
  CAP_BPF CAP_CHECKPOINT_RESTORE
)

usage() {
  printf 'Usage: %s [--preflight|--stay|--help]\n' "$0"
}

case "${1:-}" in
  '') ;;
  --preflight) preflight_only=1 ;;
  --stay) stay_mode=1 ;;
  --help)
    usage
    exit 0
    ;;
  *)
    usage >&2
    exit 2
    ;;
esac
preflight_only=${preflight_only:-0}
stay_mode=${stay_mode:-0}

require_command() {
  if ! command -v "$1" >/dev/null 2>&1; then
    printf 'Required command is unavailable: %s\n' "$1" >&2
    return 1
  fi
}

read_seatd_state() {
  seatd_load_state=$(sudo -n systemctl show -p LoadState --value seatd.service)
  seatd_active_state=$(sudo -n systemctl show -p ActiveState --value seatd.service)
  seatd_enabled_state=$(sudo -n systemctl show -p UnitFileState --value seatd.service)
  if [[ "$seatd_load_state" != loaded ]]; then
    printf 'seatd.service is not loaded (state: %s).\n' "$seatd_load_state" >&2
    return 1
  fi
  case "$seatd_active_state" in
    active|inactive) ;;
    *)
      printf 'Cannot safely restore seatd.service from state %s.\n' "$seatd_active_state" >&2
      return 1
      ;;
  esac
}

preflight() {
  local command_name
  local capability_ceiling
  local package
  local package_state
  local rotation_value

  if [[ "$(id -u)" -eq 0 ]]; then
    printf '%s\n' 'Run this development-only test as a non-root user.' >&2
    return 1
  fi
  for command_name in sudo ctr docker systemctl dpkg dpkg-query fgconsole chvt timeout stat awk grep tee; do
    require_command "$command_name" || return 1
  done
  if ! docker buildx version >/dev/null 2>&1 || ! docker buildx inspect >/dev/null 2>&1; then
    printf '%s\n' 'A usable Docker Buildx image builder is required; no host packages will be installed.' >&2
    return 1
  fi
  if ! sudo -n true; then
    printf '%s\n' 'Non-interactive sudo access is required.' >&2
    return 1
  fi
  if ! sudo -n ctr version >/dev/null; then
    printf '%s\n' 'The containerd daemon is not available through sudo.' >&2
    return 1
  fi
  if [[ "$(dpkg --print-architecture)" != amd64 ]]; then
    printf '%s\n' 'This runner currently requires an amd64 Ubuntu host.' >&2
    return 1
  fi
  capability_ceiling=$(</proc/sys/kernel/cap_last_cap)
  if [[ ! "$capability_ceiling" =~ ^[0-9]+$ || "$capability_ceiling" -ge "${#linux_capabilities[@]}" ]]; then
    printf 'Unsupported kernel capability ceiling: %s\n' "$capability_ceiling" >&2
    return 1
  fi
  for package in seatd libseat1; do
    package_state=$(dpkg-query -W -f='${db:Status-Status}' "$package" 2>/dev/null || true)
    if [[ "$package_state" != installed ]]; then
      printf 'Host package %s must already be installed; this script never installs host packages.\n' "$package" >&2
      return 1
    fi
  done
  if [[ ! -d /run/udev || ! -d /sys || ! -c /dev/tty2 ]]; then
    printf '%s\n' 'Expected /run/udev, /sys, and the tty2 device are required.' >&2
    return 1
  fi
  if ! saved_vt=$(sudo -n fgconsole) || [[ ! "$saved_vt" =~ ^[0-9]+$ ]]; then
    printf '%s\n' 'Unable to determine the current active virtual terminal.' >&2
    return 1
  fi
  read_seatd_state || return 1

  printf 'Preflight passed (read-only): uid=%s active_vt=%s seatd=%s/%s arch=amd64\n' \
    "$(id -u)" "$saved_vt" "$seatd_active_state" "$seatd_enabled_state"
  printf 'Host seatd and libseat1 are installed; no host packages will be installed.\n'
  printf 'Image builder: Docker Buildx; image will be built once before launch from Containerfile.\n'
  printf 'Rotation controls: '
  local found_rotation=0
  local rotation_path
  for rotation_path in /sys/class/graphics/fbcon/rotate /sys/class/graphics/fb[0-9]*/rotate; do
    if [[ -e "$rotation_path" ]]; then
      rotation_value=$(sudo -n cat "$rotation_path") || return 1
      printf '%s=%s ' "$rotation_path" "$rotation_value"
      found_rotation=1
    fi
  done
  if [[ "$found_rotation" -eq 0 ]]; then
    printf '%s' 'no fbcon rotation sysfs controls exposed'
  fi
  printf '\n'
}

preflight
if [[ "$preflight_only" -eq 1 ]]; then
  printf '%s\n' 'No service, display, container, or host configuration was changed.'
  exit 0
fi

if [[ "$stay_mode" -eq 1 ]]; then
  printf '%s\n' 'DEVELOPMENT ONLY: this test takes over physical tty2 until Ctrl+C.'
else
  printf '%s\n' 'DEVELOPMENT ONLY: this test takes over physical tty2 for up to 180 seconds.'
fi
printf '%s\n' 'It starts a DRM compositor and displays a LUCIA-owned background and taskbar, with no app content; save other console work first.'
if [[ ! -t 0 ]]; then
  printf '%s\n' 'An interactive terminal is required for the confirmation and client.' >&2
  exit 1
fi
read -r -p 'Type RUN PHYSICAL DISPLAY TEST to continue: ' confirmation
if [[ "$confirmation" != 'RUN PHYSICAL DISPLAY TEST' ]]; then
  printf '%s\n' 'Cancelled; no host state was changed.'
  exit 1
fi

uid_number=$(id -u)
gid_number=$(id -g)
saved_vt=$(sudo -n fgconsole)
read_seatd_state
saved_seatd_active=$seatd_active_state
saved_seatd_enabled=$seatd_enabled_state

run_token="${uid_number}-$(date +%s)-$$-${RANDOM}"
namespace="lucia-physical-${run_token}"
weston_id="weston-${run_token}"
client_id="client-${run_token}"
image="${image_repository}:${run_token}"
work_dir=$(mktemp -d "${TMPDIR:-/tmp}/lucia-physical-smoke.XXXXXXXX")
runtime_dir="$work_dir/runtime"
weston_logs="$work_dir/weston-logs"
client_log="$work_dir/client.log"
weston_config="$work_dir/weston.ini"
namespace_created=0
seatd_started=0
vt_switched=0
capability_drop_args=()
capability_ceiling=$(</proc/sys/kernel/cap_last_cap)
for ((capability_index = 0; capability_index <= capability_ceiling; capability_index++)); do
  capability_drop_args+=(--cap-drop "${linux_capabilities[$capability_index]}")
done

containerd() {
  sudo -n ctr -n "$namespace" "$@"
}

print_failure_logs() {
  local log_file
  for log_file in \
    "$work_dir/image-build.log" \
    "$weston_logs/weston-startup.log" \
    "$weston_logs/weston.log" \
    "$client_log"; do
    if [[ -f "$log_file" ]]; then
      printf '\n--- %s ---\n' "$log_file" >&2
      cat "$log_file" >&2 || true
    fi
  done
}

cleanup() {
  local exit_code=$?
  local cleanup_failed=0
  local container_id
  local attempt
  local current_value
  local image_reference
  local snapshot_key
  local snapshot_listing
  trap - EXIT INT TERM
  set +e

  if [[ "$namespace_created" -eq 1 ]]; then
    for container_id in "$client_id" "$weston_id"; do
      containerd tasks kill --all --signal SIGKILL "$container_id" >/dev/null 2>&1
    done
    if [[ "$exit_code" -ne 0 ]]; then
      print_failure_logs
    fi
    for container_id in "$client_id" "$weston_id"; do
      containerd tasks delete --force "$container_id" >/dev/null 2>&1
      containerd containers delete "$container_id" >/dev/null 2>&1
    done
    while IFS= read -r image_reference; do
      [[ -n "$image_reference" ]] && containerd images remove "$image_reference" >/dev/null 2>&1
    done < <(containerd images list -q)
    for attempt in {1..20}; do
      snapshot_listing=$(containerd snapshots list)
      while IFS=$' \t' read -r snapshot_key _; do
        [[ -z "$snapshot_key" || "$snapshot_key" == KEY ]] && continue
        containerd snapshots remove "$snapshot_key" >/dev/null 2>&1 || true
      done <<<"$snapshot_listing"
      [[ -z "$(awk 'NR > 1 && NF { print $1 }' <<<"$snapshot_listing")" ]] && break
    done
    for attempt in {1..20}; do
      if sudo -n ctr namespaces remove "$namespace" >/dev/null 2>&1; then
        namespace_created=0
        break
      fi
      containerd snapshots list >/dev/null 2>&1
      containerd content list >/dev/null 2>&1
      sleep 0.25
    done
    if [[ "$namespace_created" -eq 1 ]]; then
      printf 'Temporary containerd namespace remains: %s\n' "$namespace" >&2
      cleanup_failed=1
    fi
  fi
  if [[ "$exit_code" -ne 0 && "$namespace_created" -eq 0 ]]; then
    print_failure_logs
  fi

  if [[ "$vt_switched" -eq 1 ]]; then
    if ! sudo -n chvt "$saved_vt"; then
      printf 'Could not restore active VT %s.\n' "$saved_vt" >&2
      cleanup_failed=1
    elif [[ "$(sudo -n fgconsole 2>/dev/null)" != "$saved_vt" ]]; then
      printf 'Active VT did not return to %s.\n' "$saved_vt" >&2
      cleanup_failed=1
    fi
  fi

  if [[ "$seatd_started" -eq 1 ]]; then
    if ! sudo -n systemctl stop seatd.service; then
      printf '%s\n' 'Could not stop the temporary seatd.service instance.' >&2
      cleanup_failed=1
    fi
  fi
  current_value=$(sudo -n systemctl show -p ActiveState --value seatd.service 2>/dev/null)
  if [[ "$current_value" != "$saved_seatd_active" ]]; then
    if [[ "$saved_seatd_active" == active && "$current_value" == inactive ]]; then
      sudo -n systemctl start seatd.service || cleanup_failed=1
    elif [[ "$saved_seatd_active" == inactive && "$current_value" == active ]]; then
      sudo -n systemctl stop seatd.service || cleanup_failed=1
    fi
    current_value=$(sudo -n systemctl show -p ActiveState --value seatd.service 2>/dev/null)
    if [[ "$current_value" != "$saved_seatd_active" ]]; then
      printf 'seatd active state changed (expected %s, found %s).\n' "$saved_seatd_active" "$current_value" >&2
      cleanup_failed=1
    fi
  fi
  current_value=$(sudo -n systemctl show -p UnitFileState --value seatd.service 2>/dev/null)
  if [[ "$current_value" != "$saved_seatd_enabled" ]]; then
    printf 'seatd enabled state changed (expected %s, found %s).\n' "$saved_seatd_enabled" "$current_value" >&2
    cleanup_failed=1
  fi

  if [[ -d "$work_dir" && "$work_dir" == "${TMPDIR:-/tmp}"/lucia-physical-smoke.* ]]; then
    sudo -n rm -rf -- "$work_dir" || cleanup_failed=1
  fi
  if [[ "$cleanup_failed" -eq 1 ]]; then
    exit_code=1
  fi
  exit "$exit_code"
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

chmod 700 "$work_dir"
sudo -n install -d -o "$uid_number" -g "$gid_number" -m 0700 "$runtime_dir"
sudo -n install -d -o "$uid_number" -g "$gid_number" -m 0750 "$weston_logs"
printf '[output]\nname=DP-1\ntransform=rotate-270\n' >"$weston_config"
chmod 0444 "$weston_config"

prototype_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
if ! docker buildx build \
  --platform linux/amd64 \
  --file "$prototype_dir/Containerfile" \
  --tag "$image" \
  --output "type=oci,dest=$work_dir/lucia-image.oci.tar,name=$image" \
  "$prototype_dir" >"$work_dir/image-build.log" 2>&1; then
  cat "$work_dir/image-build.log" >&2 || true
  exit 1
fi

sudo -n ctr namespaces create "$namespace"
namespace_created=1
if ! containerd images import --local --platform linux/amd64 \
  --base-name "$image_repository" --index-name "$image" \
  "$work_dir/lucia-image.oci.tar" >"$work_dir/image-import.log" 2>&1; then
  cat "$work_dir/image-import.log" >&2 || true
  exit 1
fi
if ! containerd images list | awk -v image="$image" '$1 == image { found = 1 } END { exit !found }'; then
  printf 'Imported OCI image was not found in containerd namespace %s: %s\n' "$namespace" "$image" >&2
  exit 1
fi

if [[ "$saved_seatd_active" == inactive ]]; then
  seatd_started=1
  sudo -n systemctl start seatd.service
fi
for attempt in {1..50}; do
  [[ -S "$seatd_socket" ]] && break
  sleep 0.1
done
if [[ ! -S "$seatd_socket" ]]; then
  printf 'seatd socket did not appear at %s.\n' "$seatd_socket" >&2
  exit 1
fi
seatd_socket_gid=$(sudo -n stat -c '%g' "$seatd_socket")

runtime_mount="type=bind,src=$runtime_dir,dst=/run/lucia,options=rbind:rw"
weston_config_mount="type=bind,src=$weston_config,dst=/run/lucia/weston.ini,options=rbind:ro"
seatd_mount="type=bind,src=$seatd_socket,dst=$seatd_socket,options=rbind:rw"
udev_mount='type=bind,src=/run/udev,dst=/run/udev,options=rbind:ro'
sys_mount='type=bind,src=/sys,dst=/sys,options=rbind:ro'

vt_switched=1
sudo -n chvt 2
sudo -n ctr -n "$namespace" run --detach \
  --device /dev/tty2 \
  --mount "$runtime_mount" \
  --mount "$weston_config_mount" \
  --mount "$seatd_mount" \
  --mount "$udev_mount" \
  --mount "$sys_mount" \
  --mount "type=bind,src=$weston_logs,dst=/var/log/lucia,options=rbind:rw" \
  --env "LUCIA_UID=$uid_number" \
  --env "LUCIA_GID=$gid_number" \
  --env "SEATD_SOCKET_GID=$seatd_socket_gid" \
  --env LIBSEAT_BACKEND=seatd \
  --env HOME=/tmp/lucia-home \
  --env XDG_RUNTIME_DIR=/run/lucia \
  --env "WAYLAND_DISPLAY=$wayland_display" \
  --env "LUCIA_STAY=$stay_mode" \
  "$image" "$weston_id" bash -ec '
    exec > /var/log/lucia/weston-startup.log 2>&1
    weston_help=$(weston --help 2>&1) || { printf "%s\n" "$weston_help"; exit 1; }
    [[ "$weston_help" == *--shell* ]] || { printf "%s\n" "Weston does not advertise --shell"; exit 1; }
    kiosk_shell=$(find /usr/lib -type f -name kiosk-shell.so -print -quit)
    [[ -n "$kiosk_shell" ]] || { printf "%s\n" "Weston kiosk-shell.so is unavailable"; exit 1; }
    printf "WESTON_KIOSK_SHELL_AVAILABLE: %s\n" "$kiosk_shell"
    exec setpriv --reuid="$LUCIA_UID" --regid="$SEATD_SOCKET_GID" --clear-groups --bounding-set=-all --inh-caps=-all --ambient-caps=-all --no-new-privs -- bash -ec "
      while read -r cap_field cap_value extra_field; do
        case \$cap_field in
          CapInh:|CapPrm:|CapEff:|CapBnd:|CapAmb:)
            [[ \$cap_value == 0000000000000000 ]] || { printf \"Unexpected %s capability mask: %s\\n\" \$cap_field \$cap_value >&2; exit 1; }
            ;;
        esac
      done < /proc/self/status
        if [[ "\$LUCIA_STAY" == 1 ]]; then
          exec weston --backend=drm-backend.so --shell=kiosk-shell.so --socket="\$WAYLAND_DISPLAY" --idle-time=0 --config=/run/lucia/weston.ini --log=/var/log/lucia/weston.log
        else
          exec timeout --signal=TERM --kill-after=5s 180s weston --backend=drm-backend.so --shell=kiosk-shell.so --socket="\$WAYLAND_DISPLAY" --idle-time=0 --config=/run/lucia/weston.ini --log=/var/log/lucia/weston.log
        fi
    "
  '

socket_ready=0
for attempt in {1..120}; do
  if [[ -S "$runtime_dir/$wayland_display" ]]; then
    socket_ready=1
    break
  fi
  if ! sudo -n ctr -n "$namespace" tasks list | awk -v id="$weston_id" '$1 == id { found = 1 } END { exit !found }'; then
    break
  fi
  sleep 0.25
done
if [[ "$socket_ready" -ne 1 ]]; then
  printf '%s\n' 'Weston did not create its Wayland socket.' >&2
  exit 1
fi

printf '%s\n' 'Weston kiosk shell is on tty2; LUCIA will present the fullscreen background and taskbar with no app content.'
printf 'WAYLAND_SOCKET_READY: %s\n' "$runtime_dir/$wayland_display"
grep -F 'WESTON_KIOSK_SHELL_AVAILABLE:' "$weston_logs/weston-startup.log"
if [[ "$stay_mode" -eq 1 ]]; then
  printf '%s\n' 'Stay mode is active: compositor and client have no timeout. Press Ctrl+C in this terminal to restore host state and clean up this run.'
else
  printf 'The compositor watchdog is %s seconds; the background client is limited to %s seconds. Press Ctrl+C here to finish early.\n' \
    "$compositor_timeout" "$client_timeout"
fi
client_status=0
client_command=(sudo -n ctr -n "$namespace" run --rm \
  --user "$uid_number:$gid_number" \
  "${capability_drop_args[@]}" \
  --mount "$runtime_mount" \
  --env "LUCIA_UID=$uid_number" \
  --env HOME=/tmp \
  --env XDG_RUNTIME_DIR=/run/lucia \
  --env "WAYLAND_DISPLAY=$wayland_display" \
  "$image" "$client_id" bash -ec '
    [[ "$(id -u)" == "$LUCIA_UID" ]] || { printf "%s\n" "Client is not running as the invoking UID" >&2; exit 1; }
    while read -r cap_field cap_value extra_field; do
      case "$cap_field" in
        CapInh:|CapPrm:|CapEff:|CapBnd:|CapAmb:)
          [[ "$cap_value" == 0000000000000000 ]] || { printf "Unexpected %s capability mask: %s\n" "$cap_field" "$cap_value" >&2; exit 1; }
          ;;
      esac
    done < /proc/self/status
    exec /usr/local/bin/lucia-background-prototype
  ')
if [[ "$stay_mode" -eq 1 ]]; then
  "${client_command[@]}" 2>&1 | tee "$client_log"
  client_status=${PIPESTATUS[0]}
else
  timeout --signal=INT --kill-after=10s "$client_timeout" "${client_command[@]}" \
    >"$client_log" 2>&1 || client_status=$?
fi
case "$client_status" in
  0)
    printf '%s\n' 'Desktop client exited cleanly (status 0).'
    ;;
  124)
    printf 'Background client remained active through the %s-second display limit (timeout status 124).\n' \
      "$client_timeout"
    ;;
  *)
    printf 'Background client failed (status %s).\n' "$client_status" >&2
    cat "$client_log" >&2 || true
    exit "$client_status"
    ;;
esac
printf 'CLIENT_TASK_EXIT_STATUS: %s\n' "$client_status"
if [[ "$stay_mode" -ne 1 ]]; then
  cat "$client_log"
fi
grep -Fq 'LUCIA_BACKGROUND_FULLSCREEN_CONFIGURED:' "$client_log" || { printf '%s\n' 'Fullscreen configure marker missing.' >&2; exit 1; }
grep -Fq 'LUCIA_BACKGROUND_FRAME_PRESENTED' "$client_log" || { printf '%s\n' 'Presented background frame marker missing.' >&2; exit 1; }
grep -Fq 'LUCIA_OUTPUT_TRANSFORM: transform=rotate-270' "$client_log" || { printf '%s\n' 'Clockwise Weston output transform marker missing.' >&2; exit 1; }
grep -Fq 'LUCIA_POINTER_CURSOR_SET: width=32 height=44 hotspot=2,2' "$client_log" || { printf '%s\n' 'LUCIA pointer cursor marker missing.' >&2; exit 1; }
grep -Fq 'LUCIA_SHELL_ELEMENTS: identity=LUCIA workspace=1 apps=none status=none' "$client_log" || { printf '%s\n' 'LUCIA shell element marker missing.' >&2; exit 1; }
if [[ ! "$client_log" =~ LUCIA_BACKGROUND_FULLSCREEN_CONFIGURED:\ ([0-9]+)x([0-9]+) ]]; then
  printf '%s\n' 'Configured output dimensions are missing or invalid.' >&2
  exit 1
fi
configured_width=${BASH_REMATCH[1]}
configured_height=${BASH_REMATCH[2]}
if ((configured_height <= configured_width)); then
  printf 'Clockwise rotated output is not portrait: output=%sx%s\n' \
    "$configured_width" "$configured_height" >&2
  exit 1
fi
if [[ ! "$client_log" =~ LUCIA_TASKBAR_LAYOUT:\ edge=bottom\ x=([0-9]+)\ y=([0-9]+)\ width=([0-9]+)\ height=([0-9]+) ]]; then
  printf '%s\n' 'Responsive bottom taskbar layout marker is missing or invalid.' >&2
  exit 1
fi
taskbar_x=${BASH_REMATCH[1]}
taskbar_y=${BASH_REMATCH[2]}
taskbar_width=${BASH_REMATCH[3]}
taskbar_height=${BASH_REMATCH[4]}
if ((taskbar_x != 0 || taskbar_y + taskbar_height != configured_height || taskbar_width != configured_width || taskbar_height < 1)); then
  printf 'Taskbar bounds do not match configured output: output=%sx%s taskbar=%s,%s %sx%s\n' \
    "$configured_width" "$configured_height" "$taskbar_x" "$taskbar_y" "$taskbar_width" "$taskbar_height" >&2
  exit 1
fi
grep -Fq 'WESTON_KIOSK_SHELL_AVAILABLE:' "$weston_logs/weston-startup.log" || { printf '%s\n' 'Weston kiosk-shell runtime probe marker missing.' >&2; exit 1; }
printf '\nPhysical LUCIA background-and-taskbar prototype finished; restoring the saved VT and seatd state.\n'