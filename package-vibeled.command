#!/bin/bash
# Double-click in Finder, or run: bash package-vibeled.command --qt /path/to/Qt/macos
cd -- "$(dirname -- "$0")" || exit 1
export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"
if python3 -c 'import sys; sys.exit(sys.version_info < (3,10))' >/dev/null 2>&1; then
    python3 tools/build_vibeled.py "$@"
    result=$?
else
    echo "Python 3.10 or newer is required. Install Python and try again."
    result=1
fi
pause_on_exit=1
for argument in "$@"; do
    [[ "$argument" == "--no-pause" ]] && pause_on_exit=0
done
if [[ -t 0 && "$pause_on_exit" == 1 ]]; then
    read -r -p "Press Enter to close..." _
fi
exit "$result"
