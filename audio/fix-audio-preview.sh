#!/usr/bin/env bash
# Fix audio previews on Omarchy (Arch + Hyprland/Wayland):
#  1) WAV space-preview in Nautilus (sushi) -> gst-plugins-good was missing (wavparse, pulsesink)
#  2) .s3m from the file manager -> schismtracker with autoplay (-p flag = plays immediately, like F5)
# Usage: bash fix-audio-preview.sh
set -euo pipefail

echo "== 1/2 GStreamer plugins (WAV space-preview) =="
if gst-inspect-1.0 wavparse >/dev/null 2>&1; then
  echo "wavparse already present — skipping install."
else
  echo "Installing gst-plugins-good (wavparse + pulsesink)..."
  sudo pacman -S --needed gst-plugins-good
  # optionally for other audio/video formats in sushi:
  # sudo pacman -S --needed gst-libav
fi

echo
echo "== 1b/2 GStreamer plugins for tracker modules (.s3m space-preview) =="
# modplug + openmptdec live in gst-plugins-bad (S3M/MOD/XM/IT decoders for sushi/decodebin)
if gst-inspect-1.0 openmptdec >/dev/null 2>&1 || gst-inspect-1.0 modplug >/dev/null 2>&1; then
  echo "Tracker plugin already present — skipping install."
else
  echo "Installing gst-plugins-bad (openmptdec/modplug)..."
  sudo pacman -S --needed gst-plugins-bad
fi

echo
echo "== 2/2 schismtracker with autoplay for .s3m/.mod/.it/.xm =="
APPDIR="$HOME/.local/share/applications"
DESKTOP="$APPDIR/schismtracker-play.desktop"
mkdir -p "$APPDIR"

cat > "$DESKTOP" <<'EOF'
[Desktop Entry]
Type=Application
Name=Schism Tracker (autoplay)
GenericName=Music Module Player
Comment=Opens a module and plays it immediately (F5 equivalent)
Exec=schismtracker -p %f
Terminal=false
Categories=AudioVideo;Audio;
MimeType=audio/x-s3m;audio/x-mod;audio/x-it;audio/x-xm;
Actions=Play;
[Desktop Action Play]
Name=Play
Exec=schismtracker -p %f
EOF
echo "Created: $DESKTOP"

for mime in audio/x-s3m audio/x-mod audio/x-it audio/x-xm; do
  xdg-mime default schismtracker-play.desktop "$mime"
  echo "  $mime -> schismtracker-play.desktop"
done

echo
echo "== Verification =="
gst-inspect-1.0 wavparse >/dev/null 2>&1 \
  && echo "OK: wavparse available (space-preview WAV will work)" \
  || echo "WARNING: wavparse still missing — check pacman output"
echo "audio/x-s3m -> $(xdg-mime query default audio/x-s3m)"
echo "audio/x-mod -> $(xdg-mime query default audio/x-mod)"

echo
echo "Done. If Nautilus was open, close and reopen it."