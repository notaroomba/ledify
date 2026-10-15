# Static control panel plus the firmware image, served by Caddy on Railway's $PORT.
# The broker password comes from the MQTT_PASS variable (a reference to the mosquitto service) at start.
FROM caddy:2.8.4-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY index.html /srv/index.html
COPY firmware /srv/firmware
CMD ["sh", "-c", "sed -i \"s|__MQTT_PASS__|$MQTT_PASS|\" /srv/index.html && exec caddy run --config /etc/caddy/Caddyfile --adapter caddyfile"]
