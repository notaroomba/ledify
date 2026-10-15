# Stage 1: compile the Tailwind stylesheet from the markup and script (purged, minified).
FROM node:20-alpine AS css
WORKDIR /w
COPY package.json ./
RUN npm install --no-audit --no-fund
COPY tailwind.config.js index.html app.js ./
COPY src ./src
RUN npx tailwindcss -i src/input.css -o dist/style.css --minify

# Stage 2: Caddy serves the panel and the firmware image on Railway's $PORT.
# The broker password and Spotify client ID come from service variables at start.
FROM caddy:2.8.4-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY index.html app.js /srv/
COPY --from=css /w/dist/style.css /srv/style.css
COPY firmware /srv/firmware
CMD ["sh", "-c", "sed -i \"s|__MQTT_PASS__|$MQTT_PASS|; s|__SPOTIFY_CLIENT_ID__|$SPOTIFY_CLIENT_ID|\" /srv/app.js && exec caddy run --config /etc/caddy/Caddyfile --adapter caddyfile"]
