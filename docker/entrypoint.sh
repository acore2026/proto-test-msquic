#!/usr/bin/env bash
set -euo pipefail

if [[ "${1:-}" == "help" || "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
    exec msquic-loadtest --help
fi

exec msquic-loadtest "$@"
