#!/bin/bash
# Запускается в WSL: копирует исходники на Pi, там собирает и ставит, перезапускает Zynthian.
# Адрес берётся из scripts/deploy.env (образец: scripts/deploy.env.example).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV="$ROOT/scripts/deploy.env"
if [ ! -f "$ENV" ]; then
    echo "Нет $ENV. Скопируй scripts/deploy.env.example в scripts/deploy.env и впиши адрес Pi."
    exit 1
fi
source "$ENV"
: "${PI_HOST:?PI_HOST не задан}" "${PI_USER:?PI_USER не задан}" "${PI_DIR:?PI_DIR не задан}"
DEST="$PI_USER@$PI_HOST"
SUDO=""
[ "$PI_USER" != "root" ] && SUDO="sudo"

echo "== Копирую исходники в $DEST:$PI_DIR"
ssh "$DEST" "mkdir -p '$PI_DIR'"
rsync -az --delete --exclude .git/ --exclude build/ --exclude build_test/ --exclude tests/out/ \
      --exclude __pycache__/ --exclude scripts/deploy.env "$ROOT/" "$DEST:$PI_DIR/"

echo "== Собираю и ставлю на Pi"
ssh "$DEST" "cd '$PI_DIR' && $SUDO ./scripts/install.sh"

echo "== Перезапускаю Zynthian"
ssh "$DEST" "$SUDO systemctl restart zynthian"
echo "Готово. Логи: ssh $DEST journalctl -u zynthian -f"
