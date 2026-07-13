FROM archlinux:base-devel

RUN pacman-key --init \
    && pacman-key --populate archlinux \
    && pacman -Syu --noconfirm --needed \
        cmake \
        dbus \
        glib2 \
        icu \
        libpipewire \
        libpulse \
        networkmanager \
        ninja \
        pam \
        parted \
        pkgconf \
        systemd \
    && rm -rf /var/cache/pacman/pkg/*

WORKDIR /work
