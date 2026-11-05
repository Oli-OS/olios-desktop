# Oli Desktop GUI Prototype

**Status: provisional Oli Desktop interaction prototype, not a finished
desktop, framework, or selected product stack.** The physical presentation is
one fullscreen Wayland surface with a responsive workspace home screen and
bottom taskbar. The home screen has a clear workspace heading and an `Open Oli`
action; the taskbar has a centered Oli menu button and illustrative demo pins.
Pins do not launch applications. Either Oli entry point opens a conversation
panel where keyboard text entry,
Backspace, Enter, and Escape are supported. Oli is the LLM agent; this local
Qwen3/Ollama flow is only an assistant and broker prototype, not a complete
agent runtime. The physical runner sends prompts through a private Unix-domain
socket broker; the UI container has no host networking. The headless layout
check does not send prompts to a model. No MCP integration is present. The layout uses
the compositor-configured output dimensions. Weston is a
temporary presentation fixture in kiosk-shell mode; its desktop panel is not
presented as part of the Oli Desktop UI. Output detection and orientation use
Weston's neutral default unless the host supplies a private output-specific
configuration. Neither Weston nor Wayland is selected as Oli Desktop's
long-term compositor or display protocol.

The separate Cairo window client is retained only for the existing headless
regression check below. It is not part of the physical presentation or the
first visible milestone. This prototype does not modify or replace the
experimental GTK launcher in `desktop-container/launcher/` and does not launch
applications.

## Graphical Login

The host login screen uses the local GTK3 greeter inside the single-app Wayland
compositor `cage`, started by `greetd` on tty2. The greeter lists eligible human
accounts, shows a bundled neutral avatar, and passes PAM prompts through
greetd's documented IPC. After successful authentication it starts only the
existing `/usr/local/libexec/oli-desktop-session` command. No layer-shell mode
is used. The greeter runs as `_greetd` and connects to seatd through a per-user
ACL on `/run/seatd.sock`; it is not added to the `video` group. The greetd
service cgroup denies device access by default and allows tty2 plus only
`/dev/dri/card1` and `/dev/dri/renderD128`. On start, greetd grants `_greetd`
read/write ACLs on those two nodes; the ACLs are removed when the service stops.
Cage obtains active-seat devices through seatd and is started with VT switching
enabled so another TTY remains available for recovery.

The host provides `/etc/oli-desktop/display.conf` with `output` and `transform`
settings for the login screen. The greeter helper applies them with
`wlr-randr` before starting `gtkgreet`. Keep connector names and transforms in
this host-only file; this machine uses `output=DP-1` and `transform=270`, which
matches its post-login Weston configuration. A host may also provide
`/etc/oli-desktop/weston.ini`; the session launcher mounts that file read-only
for Weston, and changes take effect at the next Oli Desktop login.

The desktop taskbar provides a `Log out` action. It opens a confirmation dialog;
confirming exits the foreground desktop client with a dedicated status, after
which the session launcher removes the Weston task and returns to gtkgreet.
Cancel or Escape keeps the authenticated session running.

Install the runtime packages `cage`, `wlr-randr`, and `acl`, plus the GTK3 and
json-c development packages used to build the greeter. Build and exercise the
greetd IPC client before installing it:

```sh
sudo apt install cage wlr-randr acl libgtk-3-dev libjson-c-dev pkg-config
cc -std=c11 -Wall -Wextra -Werror \
	$(pkg-config --cflags gtk+-3.0 json-c) \
	desktop-container/gui-prototype/oli-login.c \
	-o /tmp/oli-login \
	$(pkg-config --libs gtk+-3.0 json-c)
/tmp/oli-login --self-test

backup_dir=$(sudo mktemp -d /var/backups/oli-greeter.XXXXXXXX)
sudo cp -a /etc/oli-desktop/greeter.css "$backup_dir/greeter.css"
sudo cp -a /usr/local/libexec/oli-greeter "$backup_dir/oli-greeter"
sudo install -D -m 0755 /tmp/oli-login /usr/local/libexec/oli-login
sudo install -D -m 0644 desktop-container/gui-prototype/avatar.svg /usr/local/share/oli-desktop/avatar.svg
sudo install -D -m 0644 desktop-container/gui-prototype/greeter.css /etc/oli-desktop/greeter.css
sudo install -D -m 0755 desktop-container/gui-prototype/oli-greeter /usr/local/libexec/oli-greeter
sudo systemctl restart greetd.service
```

The `oli-greeter` helper applies the host-only output transform before launching
the GTK app. Keep the existing greetd command, seatd ACL, and closed DRM device
policy in place. No auto-login, host D-Bus access, or app access to seat devices
is configured.

## Headless Regression Coverage

This table describes the separate headless window regression only; it is not
evidence for the physical desktop layout or a selected framework design.

| Proposed behavior | Prototype evidence | Limit |
| --- | --- | --- |
| One top-level window | xdg-shell toplevel, configured at 480 by 320 logical units | Fixed dimensions; no resize workflow |
| Vertical content arrangement | Heading, status label, and button are placed top-to-bottom | Layout is hand-positioned, not a reusable layout engine |
| Text label and button | Cairo draws status text and an `Activate` button into `wl_shm` buffers | Basic Cairo toy text only; no shaping, localization, or broad widget model |
| Pointer and keyboard activation | `wl_pointer` press/release and `wl_keyboard` Tab/Enter/Space handlers share the activation path | Weston headless advertises no `wl_seat`; smoke invokes the same event handlers in-process and does not inject compositor input or prove physical input |
| Predictable focus and visible focus | Tab focuses the sole button; keyboard focus is rendered as a high-contrast outline | Only one focusable control; no focus traversal between multiple controls |
| Status update | Both activation paths update the displayed status and report a marker | No application-launch behavior |
| Session logout | Taskbar `Log out` opens a confirmation; cancel keeps the session active, and confirm exits through the session launcher | Requires the installed greetd/gtkgreet session configuration; physical return to the greeter is not exercised by the headless check |
| Accessible label, role, and name | Client model records and reports metadata for the status label and button | Core Wayland has no accessibility semantic channel; metadata is not exposed to assistive technology |
| Scaling-friendly dimensions | Window geometry and drawing use 480 by 320 logical units with buffer scale 2 | Headless test uses one output; fractional and dynamic output scaling are not tested |
| Lifecycle and errors | Startup, running after the first presented frame, and shutdown are reported; Wayland/build setup errors are written to stderr | Recovery policy and structured error API are open |
| Container boundary | Unprivileged client task shares the private Weston runtime directory and has a read-only source mount | No host D-Bus, host devices, privileged mode, or added capabilities; physical display is not exercised |

## Headless Check

Run as a non-root user with `sudo -n`, `ctr`, and a working `containerd` service:

```sh
bash desktop-container/gui-prototype/headless-smoke.sh
```

The script pulls a temporary Ubuntu 26.04 image and obtains compiler, Wayland,
Cairo, Weston, and DejaVu font packages from Ubuntu archives in temporary package-fetch
containers. The final Weston and client tasks install the downloaded packages
offline, run as the invoking UID/GID with process capabilities dropped, and do
not use host networking. Their only shared mount is a mode-0700 private runtime
directory for the Wayland socket; logs, packages, and read-only client source
are role-specific mounts. No host packages, configuration, display, seat, D-Bus,
or devices are changed or mounted. The temporary tasks, namespace, image
reference, and test files are cleaned up on exit.

This verifies the separate window regression, then compiles and runs the
background renderer against a 1024 by 768 headless output transformed
clockwise to 768 by 1024. It checks the reported output transform, fullscreen
configure, taskbar/menu/pin bounds and markers, workspace home and assistant
action bounds, logout control and confirmation bounds, cancellation, keyboard
confirmation, the logout exit status, an opened assistant panel and prompt
bounds, and a presented frame. The headless backend advertises no pointer
or keyboard seat, so this does not validate physical input. The separate window
path exercises its own keyboard and pointer event-handler paths with
deterministic in-process events. This is not a
screenshot/pixel test, compositor-generated input test, physical input test,
accessibility integration test, or evidence that any provisional technology is
the final product choice.

## Physical Display Check

**Development only:** this test takes over physical `tty2` for up to 180 seconds
to display the Oli Desktop-owned background, demo pins, and local Qwen3 assistant. It requires a non-root user with
non-interactive `sudo`, `containerd`, Docker Buildx with a usable builder, and the
host `seatd` and `libseat1` packages already installed. It never installs host
packages. The read-only preflight checks the builder and host prerequisites but
does not build the image or change display state.

The reusable [`Containerfile`](Containerfile) uses Ubuntu 26.04 build and runtime
stages. It compiles both C programs and includes Weston, kiosk-shell, libseat,
and the client runtime libraries. To build a local image for inspection:

```sh
docker buildx build --platform linux/amd64 \
	--file desktop-container/gui-prototype/Containerfile \
	--tag localhost/olios/oli-desktop:local \
	desktop-container/gui-prototype
```

For a physical run, the runner builds one uniquely tagged OCI image with Buildx,
exports it as an OCI archive, and imports it into that run's temporary
containerd namespace before taking over the display. Weston and the Oli
background client run as separate tasks from that same image; runtime tasks do
not install packages, compile code, or bind-mount the source directory. The
image remains unchanged for both tasks and is removed with the temporary
namespace during cleanup.

Run the read-only preflight, then launch the interactive test:

By default, the Weston compositor watchdog is 180 seconds and the Oli Desktop
display client is capped at 60 seconds. To keep the physical presentation running until
you stop it, use stay mode instead:

```sh
bash desktop-container/gui-prototype/physical-smoke.sh --preflight
bash desktop-container/gui-prototype/physical-smoke.sh
bash desktop-container/gui-prototype/physical-smoke.sh --stay
```

Stay mode bypasses both timeout wrappers. The compositor and fullscreen Oli
Desktop remain active until you press Ctrl+C in the terminal running the
script. Click the centered Oli button to open the conversation panel, type a
message, and press Enter or Send to query the real `qwen3:0.6b` model. Escape or
Cancel stops an in-flight request; Clear removes the conversation. The three
demo pins are illustrative and do not launch apps. The client log is captured
and streamed to that terminal while it runs. When the
pointer enters the fullscreen surface, the Oli Desktop client installs its
high-contrast 32 by 44 pixel cursor with `wl_pointer.set_cursor`; normal pointer motion remains
compositor-managed. Ctrl+C triggers cleanup, restoring the
saved VT and seatd active/enabled state, then removing this run's containerd
namespace and temporary work directory. Host fbcon and GRUB rotation settings
are neither changed nor restored. While stay mode is active, the test owns tty2
and its temporary runtime resources remain present; it does not show app windows
or app content.

The runner asks for an explicit confirmation, builds the host-side broker as
the invoking user, verifies the installed model through the host's loopback-only
Ollama listener, and verifies the built image's Weston runtime advertises
`--shell` and contains `kiosk-shell.so`, temporarily starts
`seatd` only when it was inactive, and restores the original active VT and
seatd active/enabled states on exit. Weston uses the image's generic config and
autodetects available outputs; the smoke check accepts the detected output
dimensions and does not require a particular orientation. Weston runs with the
kiosk shell, so its desktop panel is not part of the presentation.
Weston receives only tty2, the seatd socket, and read-only `/run/udev` and `/sys`
mounts. The client runs as the invoking non-root UID with all capabilities
dropped, shares the private Wayland runtime directory and a separate read-only
0700 provider-socket directory, and has no device mounts. The provider broker
runs as the invoking UID and connects only to `127.0.0.1:11434`; neither runtime
task uses host networking or receives access to Ollama's TCP listener. No host D-Bus, privileged
mode, GRUB changes, reboot, or host package changes are used.

The separate [`background.c`](background.c) client requests a fullscreen
xdg-shell surface, sizes its Cairo buffer from the compositor configure, and
draws the Oli Desktop-owned workspace home screen, `Open Oli` action, centered
menu, illustrative pins, and conversation panel. It handles Wayland pointer and keyboard capabilities when
available; xkbcommon decodes text for the prompt. Prompt traffic uses bounded
length frames over the private local-model socket, and generated text or
provider errors are rendered in the panel. The assistant panel is closed at
physical startup. It records transform, cursor, fullscreen, taskbar/menu/pin, assistant-layout, and
presented-frame markers; the client task exit status and log are captured
before cleanup removes the temporary work directory. Pins contain no launched
app windows or app-provided pixels. This is an Oli Desktop-owned shell-rendering
prototype, not a framework or compositor implementation.

The test removes its temporary namespace, image reference, tasks, and files. It
is an exploratory hardware check, not a phase-gate result. The prototype does
not test multiple outputs, hotplug, scaling policy, app composition, or a
production shell/session lifecycle.