# Stage 1: build the panel (Vite + Solid + Tailwind) into dist/.
FROM node:20-alpine AS build
WORKDIR /w
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY . .
RUN npm run build

# Stage 2: Caddy serves the built panel and the firmware image on Railway's $PORT.
# The broker password and Spotify client ID come from service variables at start.
FROM caddy:2.8.4-alpine
COPY Caddyfile /etc/caddy/Caddyfile
COPY --from=build /w/dist /srv
CMD ["sh", "-c", "sed -i \"s|__MQTT_PASS__|$MQTT_PASS|; s|__SPOTIFY_CLIENT_ID__|$SPOTIFY_CLIENT_ID|\" /srv/config.js && exec caddy run --config /etc/caddy/Caddyfile --adapter caddyfile"]
