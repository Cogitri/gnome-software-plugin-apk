FROM docker.io/alpine:latest

RUN apk add --no-cache meson gnome-software-dev apk-polkit-rs-dev build-base \
    curl gdk-pixbuf-dev libxmlb-dev glib-dev gtk+3.0-dev libhandy1-dev \
    json-glib-dev libsoup-dev gspell-dev polkit-dev libgudev-dev appstream-dev \
    desktop-file-utils gsettings-desktop-schemas-dev
RUN curl -L -O https://gitlab.gnome.org/pabloyoyoista/gnome-software/-/archive/alpine-patch/gnome-software-alpine-patch.tar.gz \
    && tar xf gnome-software-alpine-patch.tar.gz \
    && cd gnome-software-alpine-patch \
    && meson \
       --prefix /usr \
       -Dvalgrind=false \
       -Dmalcontent=false \
       -Dpackagekit=false \
       -Dfwupd=false \
       -Dflatpak=false \
       -Dgtk_doc=false \
       -Dman=false \
       -Dtests=false \
        build \
    && meson install -C build
RUN apk add --no-cache py3-dbusmock
LABEL org.opencontainers.image.source="https://github.com/Cogitri/gnome-software-plugin-apk"
