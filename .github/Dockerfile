FROM docker.io/alpine:latest

RUN apk add meson gnome-software-dev apk-polkit-rs-dev build-base curl \
    appstream-glib-dev gdk-pixbuf-dev libxmlb-dev glib-dev gtk+3.0-dev libhandy1-dev \
    json-glib-dev libsoup-dev gnome-desktop-dev gspell-dev polkit-dev gtk-doc \
    ostree-dev flatpak-dev libgudev-dev gnome-online-accounts-dev appstream-dev
RUN curl -L -O https://download.gnome.org/sources/gnome-software/41/gnome-software-41.2.tar.xz \
    && tar xf gnome-software-41.2.tar.xz \
    && cd gnome-software-41.2 \
    && meson -Dvalgrind=false -Dmalcontent=false -Dpackagekit=false -Dfwupd=false build \
    && DESTDIR=/ ninja -C build install
LABEL org.opencontainers.image.source="https://github.com/Cogitri/gnome-software-plugin-apk"
