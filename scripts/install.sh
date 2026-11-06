#!/bin/bash
# Запускается на Zynthian: собирает плагин и ставит две ссылки (как у прошлых плагинов).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
"$ROOT/scripts/build.sh"
LV2_DIR="${LV2_DIR:-/usr/local/lib/lv2}"
UI_DIR="${ZYNTHIAN_UI_DIR:-/zynthian/zynthian-ui}"
mkdir -p "$LV2_DIR" "$UI_DIR/zyngui/widgets_user"
ln -sfn "$ROOT/build/audio_looper.lv2" "$LV2_DIR/audio_looper.lv2"
# Обновление Zynthian может стереть ссылку на виджет: install.sh ставит её заново.
ln -sfn "$ROOT/widget/zynthian_widget_audio_looper.py" "$UI_DIR/zyngui/widgets_user/zynthian_widget_audio_looper.py"
echo "Installed. Restart Zynthian: systemctl restart zynthian"
