#!/bin/bash
set -e
TOOLS="$(cd "$(dirname "$0")" && pwd)"
python3 "$TOOLS/memdump.py" && code /tmp/picospeccycy_dump.log
