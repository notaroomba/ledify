#!/bin/sh
# Container start: inject the runtime settings into config.js, protect the page when a panel password is set.
set -e
sed -i "s|__MQTT_USER__|${MQTT_USER:-ledify}|; s|__MQTT_PASS__|$MQTT_PASS|; s|__SPOTIFY_CLIENT_ID__|$SPOTIFY_CLIENT_ID|" /srv/config.js
CFG=/etc/caddy/Caddyfile
if [ -n "$PANEL_PASS" ]; then
  export PANEL_USER="${PANEL_USER:-ledify}"
  export PANEL_HASH="$(caddy hash-password --plaintext "$PANEL_PASS")"
  CFG=/etc/caddy/Caddyfile.auth
fi
exec caddy run --config "$CFG" --adapter caddyfile
