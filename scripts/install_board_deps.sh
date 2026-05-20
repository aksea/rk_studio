#!/usr/bin/env bash
set -euo pipefail

if [[ ${EUID} -eq 0 ]]; then
  sudo_cmd=()
else
  sudo_cmd=(sudo)
fi

echo "[deps] installing preview/record build dependencies"
"${sudo_cmd[@]}" apt-get update
"${sudo_cmd[@]}" apt-get install -y \
  cmake g++ pkg-config \
  qtbase5-dev \
  libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
  libgstreamer-plugins-good1.0-dev libgstreamer-allocators1.0-0 \
  gstreamer1.0-tools gstreamer1.0-plugins-base gstreamer1.0-plugins-good

echo "[deps] board dependencies are ready"
