#!/usr/bin/env bash
set -u

DEV="/dev/hpad-pad"
state="unknown"

while true; do
  if [[ -e "$DEV" ]]; then
    if [[ "$state" != "connected" ]]; then
      echo "=== connected to $DEV ==="
      state="connected"
    fi

    sudo socat - "$DEV",b115200,raw,echo=0,crnl

    echo
    echo "=== disconnected from $DEV ==="
    state="disconnected"
  else
    if [[ "$state" != "waiting" ]]; then
      echo "=== waiting for $DEV ==="
      state="waiting"
    fi
  fi

  sleep 0.5
done
