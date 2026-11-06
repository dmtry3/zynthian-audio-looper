#!/bin/bash
# Запускается в WSL после git push: на Pi делает git clone или git pull, собирает и ставит,
# перезапускает Zynthian. Адрес берётся из scripts/deploy.env (образец: scripts/deploy.env.example).
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
ENV="$ROOT/scripts/deploy.env"
if [ ! -f "$ENV" ]; then
    echo "Нет $ENV. Скопируй scripts/deploy.env.example в scripts/deploy.env и впиши адрес Pi."
    exit 1
fi
source "$ENV"
: "${PI_HOST:?PI_HOST не задан}" "${PI_USER:?PI_USER не задан}" "${PI_DIR:?PI_DIR не задан}"
REPO_URL="${REPO_URL:-https://github.com/dmtry3/zynthian-audio-looper.git}"
DEST="$PI_USER@$PI_HOST"
SUDO=""
[ "$PI_USER" != "root" ] && SUDO="sudo"

# На Pi попадёт только то, что лежит на GitHub: проверяем, что всё закоммичено и запушено.
cd "$ROOT"
if [ -n "$(git status --porcelain)" ]; then
    echo "Есть незакоммиченные изменения. Сначала git commit и git push."
    exit 1
fi
git fetch -q origin
if [ "$(git rev-parse HEAD)" != "$(git rev-parse origin/main)" ]; then
    echo "Локальная ветка не совпадает с origin/main. Сначала git push."
    exit 1
fi

echo "== Обновляю код на $DEST:$PI_DIR"
ssh "$DEST" "if [ -d '$PI_DIR/.git' ]; then cd '$PI_DIR' && git pull --ff-only; else git clone '$REPO_URL' '$PI_DIR'; fi"

echo "== Собираю и ставлю на Pi"
ssh "$DEST" "cd '$PI_DIR' && $SUDO ./scripts/install.sh"

echo "== Перезапускаю Zynthian"
ssh "$DEST" "$SUDO systemctl restart zynthian"
echo "Готово. Логи: ssh $DEST journalctl -u zynthian -f"
