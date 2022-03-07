FROM docker.io/alpine:latest

RUN apk add --no-cache meson gnome-software-dev apk-polkit-rs-dev build-base curl \
    appstream-glib-dev gdk-pixbuf-dev libxmlb-dev glib-dev gtk+3.0-dev libhandy1-dev \
    json-glib-dev libsoup-dev gnome-desktop-dev gspell-dev polkit-dev gtk-doc \
    ostree-dev flatpak-dev libgudev-dev gnome-online-accounts-dev appstream-dev
RUN curl -L -O https://gitlab.gnome.org/pabloyoyoista/gnome-software/-/archive/alpine-patch/gnome-software-alpine-patch.tar.gz \
    && tar xf gnome-software-alpine-patch.tar.gz \
    && cd gnome-software-alpine-patch \
    && meson -Dvalgrind=false -Dmalcontent=false -Dpackagekit=false -Dfwupd=false build \
    && DESTDIR=/ ninja -C build install
RUN apk add --no-cache py3-dbusmock
LABEL org.opencontainers.image.source="https://github.com/Cogitri/gnome-software-plugin-apk"
