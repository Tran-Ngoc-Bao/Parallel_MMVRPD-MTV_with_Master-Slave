#!/usr/bin/env bash
set -euo pipefail

if [[ "${EUID:-$(id -u)}" -eq 0 ]]; then
    SUDO=""
else
    if ! command -v sudo >/dev/null 2>&1; then
        echo "Error: this script needs root privileges, but 'sudo' is not installed." >&2
        exit 1
    fi
    SUDO="sudo"
fi

if ! command -v apt-get >/dev/null 2>&1; then
    echo "Error: this script is intended for Debian/Ubuntu systems with 'apt-get'." >&2
    exit 1
fi

${SUDO} apt-get update
${SUDO} DEBIAN_FRONTEND=noninteractive apt-get install -y \
    vim \
    build-essential \
    cmake \
    git \
    openmpi-bin \
    libopenmpi-dev \
    curl \
    ca-certificates \
    python3 \
    python3-pip

pip3 install matplotlib

if ! command -v cargo >/dev/null 2>&1; then
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --no-modify-path
    echo "Rust installed. Run: source \"\$HOME/.cargo/env\" to activate in current shell."
else
    echo "Rust already installed: $(cargo --version)"
fi

if ! command -v claude >/dev/null 2>&1; then
    curl -fsSL https://claude.ai/install.sh | bash
    echo "Claude Code installed. Run: export PATH=\"\$HOME/.local/bin:\$PATH\" to activate in current shell."
else
    echo "Claude Code already installed: $(claude --version)"
fi

echo "Done."