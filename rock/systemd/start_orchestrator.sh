#!/usr/bin/env bash
set -u

if [ -e /boot/disable-signblstm ]; then
  echo "[signblstm-orchestrator] disabled by /boot/disable-signblstm"
  exit 0
fi

cd /home/rock/signblstm_deploy_past || exit 1

for i in $(seq 1 30); do
  if [ -S /run/wifi_service.sock ]; then
    break
  fi
  sleep 1
done

exec /usr/bin/systemd-inhibit \
  --what=handle-power-key \
  --who=rtt_wifi_orchestrator \
  --why="use power key as gesture trigger" \
  /usr/bin/python3 -u /home/rock/signblstm_deploy_past/rtt_wifi_orchestrator.py
