#!/bin/bash
set -euo pipefail

# Usage: notarize.sh <file-to-notarize> <apple-id> <password> <team-id>
FILE="$1"
APPLE_ID="$2"
APPLE_ID_PASSWORD="$3"
APPLE_TEAM_ID="$4"

echo "Submitting $(basename "$FILE") for notarization..."

set +e
SUBMIT_OUT=$(xcrun notarytool submit "$FILE" \
    --apple-id "$APPLE_ID" \
    --password "$APPLE_ID_PASSWORD" \
    --team-id "$APPLE_TEAM_ID" \
    --output-format json \
    --timeout 15m \
    --wait 2>&1)
EXIT_CODE=$?
set -e

echo "$SUBMIT_OUT"

if [ $EXIT_CODE -ne 0 ] && [ -z "$SUBMIT_OUT" ]; then
    echo "::error::notarytool failed to submit (exit $EXIT_CODE)"
    exit 1
fi

STATUS=$(echo "$SUBMIT_OUT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('status',''))" 2>/dev/null || echo "")
SUBMISSION_ID=$(echo "$SUBMIT_OUT" | python3 -c "import sys,json; print(json.load(sys.stdin).get('id',''))" 2>/dev/null || echo "")

if [ "$STATUS" != "Accepted" ]; then
    echo "::error::Notarization of $(basename "$FILE") failed with status: ${STATUS:-unknown}"
    if [ -n "$SUBMISSION_ID" ]; then
        echo "=== Notarization Log ==="
        xcrun notarytool log "$SUBMISSION_ID" \
            --apple-id "$APPLE_ID" \
            --password "$APPLE_ID_PASSWORD" \
            --team-id "$APPLE_TEAM_ID" || true
    fi
    exit 1
fi

echo "Stapling $(basename "$FILE")..."
xcrun stapler staple "$FILE"
echo "Notarization complete for $(basename "$FILE")"
