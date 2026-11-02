# Experimental Shell Launcher

Toolkit: GTK4/PyGObject (provisional). The launcher presents Files, Settings, and Terminal entry buttons; for now, a button only updates the status text and does not launch an application.

Run the headless integration with `bash desktop-container/launcher/headless-smoke.sh`. It uses a unique containerd namespace and an Ubuntu 26.04 base image. Temporary package-fetch tasks briefly use host networking to retrieve package indexes and `.deb` files from the Ubuntu archive; the separate Weston compositor and launcher client tasks install those packages offline and do not use host networking. Only the private runtime directory containing the Wayland socket is mounted into both final tasks. Each also gets its own role-specific package, log, or source mount. The final tasks do not use host D-Bus, host devices, privileged mode, or added capabilities; the compositor and client processes run as the invoking non-root UID/GID with capabilities dropped.

The test verifies client connection, window presentation, and a GTK button signal invoked by the toolkit. It does not validate physical display output, physical input, or interaction on hardware.