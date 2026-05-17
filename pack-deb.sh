#!/bin/bash
set -e
NAME="drink-reminder"
VER="1.0"
ARCH="amd64"
DIR="${NAME}_${VER}_${ARCH}"

./build.sh

rm -rf "$DIR"
mkdir -p "$DIR/DEBIAN"
mkdir -p "$DIR/usr/bin"
mkdir -p "$DIR/usr/share/applications"
mkdir -p "$DIR/usr/share/icons/hicolor/256x256/apps"

cat > "$DIR/DEBIAN/control" <<EOF
Package: $NAME
Version: $VER
Section: utils
Priority: optional
Architecture: $ARCH
Maintainer: $(whoami) <$(whoami)@$(hostname)>
Description: Drink water reminder
 A minimalist desktop reminder that pops up periodically
 to remind you to drink water.
Depends: libx11-6, libdbus-1-3, libxft2, libpng16-16
EOF

cat > "$DIR/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache /usr/share/icons/hicolor/ >/dev/null 2>&1 || true
fi
EOF
chmod 755 "$DIR/DEBIAN/postinst"

cat > "$DIR/usr/share/applications/${NAME}.desktop" <<EOF
[Desktop Entry]
Name=Drink Reminder
Comment=Reminds you to drink water
# 开机自启时延迟 10 秒再运行程序
Exec=sh -c "sleep 10 && /usr/bin/$NAME"
Icon=$NAME
Terminal=false
Type=Application
Categories=Utility;
X-GNOME-Autostart-Delay=10
EOF

cp drink-reminder "$DIR/usr/bin/$NAME"
chmod 755 "$DIR/usr/bin/$NAME"
cp drink-reminder.png "$DIR/usr/share/icons/hicolor/256x256/apps/${NAME}.png"

dpkg-deb --build --root-owner-group "$DIR"
echo "Package built: ${DIR}.deb"
echo "Install: sudo dpkg -i ${DIR}.deb"
echo "Uninstall: sudo dpkg -r $NAME"
