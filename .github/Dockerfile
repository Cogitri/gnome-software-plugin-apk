FROM ghcr.io/distroless/alpine-base:latest

ENV BRANCH=gnome-43
# distroless/alpine-base only has main by default
RUN echo "" >> /etc/apk/repositories \
    && echo https://dl-cdn.alpinelinux.org/alpine/edge/community >> /etc/apk/repositories
RUN apk add --no-cache meson apk-polkit-rs-dev build-base \
    curl gdk-pixbuf-dev libxmlb-dev glib-dev gtk4.0-dev libadwaita-dev \
    json-glib-dev libsoup3-dev gspell-dev polkit-dev libgudev-dev appstream-dev \
    desktop-file-utils gsettings-desktop-schemas-dev dbus
RUN curl -L -O https://gitlab.gnome.org/GNOME/gnome-software/-/archive/${BRANCH}/gnome-software-${BRANCH}.tar.gz \
    && tar xf gnome-software-${BRANCH}.tar.gz \
    && cd gnome-software-${BRANCH} \
    && meson \
       --prefix /usr \
       -Dmalcontent=false \
       -Dpackagekit=false \
       -Dfwupd=false \
       -Dflatpak=false \
       -Dwebapps=false \
       -Dhardcoded_foss_webapps=false \
       -Dhardcoded_proprietary_webapps=false \
       -Dgtk_doc=false \
       -Dman=false \
       -Dtests=false \
        build \
    && meson install -C build
RUN apk add --no-cache py3-dbusmock
LABEL org.opencontainers.image.source="https://github.com/Cogitri/gnome-software-plugin-apk"
