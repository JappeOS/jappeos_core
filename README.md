<h1 align="center">
  <img src="https://raw.githubusercontent.com/JappeOS/JappeOS/dev/Icons/jappeos-logo-banner-white-512.png" width="120"><br>
  jappeos_core
</h1>

<p align="center">
  <strong>The core system daemon for JappeOS, manages sessions, power, and more.</strong>
</p>

<p align="center">
  <a href="./issues"><img src="https://img.shields.io/github/issues/JappeOS/jappeos_core?style=plastic&color=edda09"></a>
  <a href="./pulls"><img src="https://img.shields.io/github/issues-pr/JappeOS/jappeos_core?style=plastic&color=40a842"></a>
  <a href="./LICENSE"><img src="https://img.shields.io/github/license/JappeOS/jappeos_core?style=plastic&color=9d09ed"></a>
  <img src="https://img.shields.io/badge/arch-x86__64-blue?style=plastic">
  <img src="https://img.shields.io/badge/status-experimental-orange?style=plastic">
  <a href="https://discord.gg/dRtU4HR"><img src="https://img.shields.io/discord/716673375946407972?style=plastic&color=3250a8"></a>
</p>

---

## Overview

This is the core system daemon for JappeOS, which manages sessions, accounts, power, and more.
It exposes a **backend-agnostic D-Bus interface** intended to be consumed by higher-level system components such as the
desktop environment, which uses it through [`jappeos_services`](https://github.com/JappeOS/jappeos_services).

## Features

* Session management
* Account management
* Power management
* Network handling

## Role in the OS

Manages user sessions and everything system-related that is managed via a GUI.

## Building

> [!IMPORTANT]
>
> This daemon is a **core system component** of JappeOS. It is **not intended to be run on a general-purpose Linux distribution or a personal system**.
>
> You should only run it **inside a JappeOS environment**, such as:
> - A JappeOS development VM
> - A chroot or container matching the JappeOS root filesystem
> - A JappeOS system image used for debugging or testing
>
> Running this daemon on a host system outside JappeOS may interfere with system services, session management, networking, or power management.
> You can still build it on any system, given that the requirements below are met.

### Requirements

Building requires a Linux system with the following dependencies available:

- **CMake ≥ 3.31**
- **C++20-compatible compiler** (e.g. GCC or Clang)
- **pkg-config**
- **systemd** (runtime requirement)
- Development headers for:
    - `dbus-1`
    - `pam`
    - `libnm` (NetworkManager)

### Build Steps

From the project root directory:

```sh
mkdir -p build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

This will produce the `jappeos_core` executable in the `build/` directory.

For a debug build, use:

```sh
cmake -DCMAKE_BUILD_TYPE=Debug ..
cmake --build .
```

### Notes

- This project is **Linux-only** and relies on Linux-specific APIs and system services.
- The daemon is designed to run **exclusively as a systemd service** and should not be launched manually.
- CPU feature flags are intentionally restricted to ensure compatibility across supported hardware.

### Installation & Runtime

For now, the JappeOS filesystem has this service and its required systemd, PAM, D-Bus files *"hardcoded"* inside the
`/jappeos/` directory. See [`JappeOS-Build`](https://github.com/JappeOS/JappeOS-Build) for the latest filesystem structure.

Installation should be as easy as dropping the built `jappeos_core` binary inside the `/jappeos/` directory in the
filesystem, and building the *.iso.

Binaries will later be served from a proper package manager repository.

## Contributing

Contributions of all kinds are welcome and appreciated. You can help the project by:

- ⭐ Starring the repository to show your support
- 💖 Sponsoring the project (if available)
- 🐞 Reporting bugs via [GitHub Issues](./issues)
- 💡 Requesting or discussing new features

For code contributions, please see [`CONTRIBUTING.md`](./CONTRIBUTING.md) for guidelines.

## License

This repository is part of the JappeOS project and is licensed under the terms described in the [`LICENSE`](./LICENSE) file.
