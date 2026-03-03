#!/bin/bash
set -euo pipefail

# Usage: notarize.sh <file-to-notarize> <api-key-id> <api-issuer-id> <api-key-path>
FILE="$1"
API_KEY_ID="$2"
API_ISSUER_ID="$3"
API_KEY_PATH="$4"

echo "Submitting $(basename "$FILE") for notarization..."

set +e
SUBMIT_OUT=$(xcrun notarytool submit "$FILE" \
    --key "$API_KEY_PATH" \
    --key-id "$API_KEY_ID" \
    --issuer "$API_ISSUER_ID" \
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
            --key "$API_KEY_PATH" \
            --key-id "$API_KEY_ID" \
            --issuer "$API_ISSUER_ID" || true
    fi
    exit 1
fi

echo "Stapling $(basename "$FILE")..."
xcrun stapler staple "$FILE"
echo "Notarization complete for $(basename "$FILE")"
