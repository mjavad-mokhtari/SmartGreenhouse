#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT="SmartGreenhouse Server"
TIME="$(date '+%Y-%m-%d %H:%M:%S %Z')"
VERSION="unknown"
STATUS="✅ موفق"

# Optional persistent env file for notifications:
# TELEGRAM_BOT_TOKEN=<bot token>
# TELEGRAM_CHAT_ID=<chat id>
# N8N_WEBHOOK_URL=<n8n webhook>
if [ -f "$REPO_ROOT/.deploy.env" ]; then
  # shellcheck disable=SC1090
  set -a
  . "$REPO_ROOT/.deploy.env"
  set +a
fi

notify_telegram() {
  if [ -z "${TELEGRAM_BOT_TOKEN:-}" ] || [ -z "${TELEGRAM_CHAT_ID:-}" ]; then
    return 0
  fi

  local message="$1"
  curl -sS -X POST "https://api.telegram.org/bot${TELEGRAM_BOT_TOKEN}/sendMessage" \
    -H 'Content-Type: application/json' \
    -d "{\"chat_id\":\"${TELEGRAM_CHAT_ID}\",\"text\":\"${message}\"}" >/dev/null 2>&1 || true
}

notify_n8n() {
  if [ -z "${N8N_WEBHOOK_URL:-}" ]; then
    return 0
  fi

  local payload="$1"
  curl -sS -X POST "${N8N_WEBHOOK_URL}" \
    -H 'Content-Type: application/json' \
    -d "${payload}" >/dev/null 2>&1 || true
}

send_notifications() {
  local msg
  msg="🔄 SmartGreenhouse Deploy\n📦 پروژه: ${PROJECT}\n🧩 نسخه: ${VERSION}\n⏱ زمان: ${TIME}\n✅ وضعیت: ${STATUS}"

  notify_telegram "${msg}"
  notify_n8n "$(cat <<JSON
{
  "project": "${PROJECT}",
  "version": "${VERSION}",
  "time": "${TIME}",
  "status": "${STATUS}"
}
JSON
)"
}

trap 'code=$?; if [ ${code:-1} -ne 0 ]; then STATUS="❌ ناموفق"; fi; send_notifications' EXIT

cd "$REPO_ROOT"

echo "[deploy] pull"
git pull origin main

action="Deploy"
if [ "$(git status --porcelain --untracked-files=no | wc -l | tr -d ' ')" -gt 0 ]; then
  action="Deploy-After-Local-Changes"
fi

cd "$REPO_ROOT/server"
echo "[deploy] install"
npm install --omit=dev

echo "[deploy] restart"
pm2 restart greenhouse --update-env

git fetch --all --tags --prune >/dev/null 2>&1 || true
VERSION="$(git rev-parse --short HEAD 2>/dev/null || echo unknown)"

send_notifications

echo "[deploy] ${action} complete"
