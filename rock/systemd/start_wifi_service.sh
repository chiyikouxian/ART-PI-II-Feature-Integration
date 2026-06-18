#!/usr/bin/env bash
set -u

if [ -e /boot/disable-signblstm ]; then
  echo "[signblstm-wifi] disabled by /boot/disable-signblstm"
  exit 0
fi

cd /home/rock/signblstm_deploy_past || exit 1

rm -f /run/wifi_service.sock /run/wifi_service.lock

exec /usr/bin/python3 -u /home/rock/signblstm_deploy_past/run_wifi_service.py
