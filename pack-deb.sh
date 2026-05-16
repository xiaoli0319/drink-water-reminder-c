#!/bin/bash
set -e
NAME="drink-reminder"
VER="1.0"
ARCH="amd64"
DIR="${NAME}_${VER}-1_${ARCH}"

rm -rf "$DIR"
mkdir -p "$DIR/DEBIAN"
mkdir -p "$DIR/usr/bin"
mkdir -p "$DIR/usr/share/applications"

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
Depends: libx11-6, libdbus-1-3
EOF

cat > "$DIR/usr/share/applications/${NAME}.desktop" <<EOF
[Desktop Entry]
Name=Drink Reminder
Comment=Reminds you to drink water
Exec=/usr/bin/$NAME
Icon=weather-none-available
Terminal=false
Type=Application
Categories=Utility;
EOF

cp drink-reminder "$DIR/usr/bin/$NAME"
chmod 755 "$DIR/usr/bin/$NAME"

dpkg-deb --build --root-owner-group "$DIR"
echo "Package built: ${DIR}.deb"
echo "Install: sudo dpkg -i ${DIR}.deb"
echo "Uninstall: sudo dpkg -r $NAME"
