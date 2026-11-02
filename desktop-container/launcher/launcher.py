#!/usr/bin/env python3

import argparse
import os
import sys

import gi

gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, GLib, Gtk


APP_ENTRIES = ("Files", "Settings", "Terminal")


def effective_capabilities():
    with open("/proc/self/status", encoding="ascii") as status_file:
        for line in status_file:
            if line.startswith("CapEff:"):
                return line.split(":", 1)[1].strip()
    return "unknown"


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--smoke-click", choices=APP_ENTRIES)
    args = parser.parse_args()

    capabilities = effective_capabilities()
    if os.geteuid() == 0 or capabilities != "0000000000000000":
        raise SystemExit("launcher must run non-root with no effective capabilities")

    print(
        f"CLIENT_IDENTITY: uid={os.getuid()} gid={os.getgid()} capeff={capabilities}",
        flush=True,
    )

    Gtk.init()
    display = Gdk.Display.get_default()
    if display is None:
        raise SystemExit("no Wayland display is available")
    print("WAYLAND_CONNECTED", flush=True)

    window = Gtk.Window(title="LUCIA Launcher (Experimental)")
    window.set_default_size(420, 260)

    layout = Gtk.Box(orientation=Gtk.Orientation.VERTICAL, spacing=12)
    layout.set_margin_top(20)
    layout.set_margin_bottom(20)
    layout.set_margin_start(20)
    layout.set_margin_end(20)

    heading = Gtk.Label(label="LUCIA Launcher")
    heading.set_xalign(0)
    layout.append(heading)

    status = Gtk.Label(label="Choose an app")
    status.set_xalign(0)
    layout.append(status)

    buttons = {}
    for entry in APP_ENTRIES:
        button = Gtk.Button(label=entry)
        button.connect(
            "clicked",
            lambda clicked_button: status.set_text(
                f"Selected: {clicked_button.get_label()}"
            ),
        )
        buttons[entry] = button
        layout.append(button)

    window.set_child(layout)
    loop = GLib.MainLoop()
    window.connect("close-request", lambda *_: (loop.quit(), False)[1])
    window.present()
    print("WINDOW_PRESENTED", flush=True)

    if args.smoke_click:
        click_succeeded = []

        def invoke_button():
            buttons[args.smoke_click].emit("clicked")
            expected_status = f"Selected: {args.smoke_click}"
            click_succeeded.append(status.get_text() == expected_status)
            if click_succeeded[-1]:
                print(f"BUTTON_SIGNAL_OK: {args.smoke_click}", flush=True)
                print(f"STATUS_UPDATED: {expected_status}", flush=True)
            loop.quit()
            return GLib.SOURCE_REMOVE

        GLib.timeout_add(250, invoke_button)

    loop.run()
    window.set_visible(False)
    return 0 if not args.smoke_click or click_succeeded == [True] else 1


if __name__ == "__main__":
    sys.exit(main())