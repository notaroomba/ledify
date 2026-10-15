# Static control panel plus the firmware image, served by Caddy on Railway's $PORT.
FROM caddy:2.8.4-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY index.html /srv/index.html
COPY firmware /srv/firmware
