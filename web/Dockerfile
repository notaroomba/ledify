# Stage 1: build the panel (Vite + Solid + Tailwind) into dist/.
FROM node:20-alpine AS build
WORKDIR /w
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY . .
RUN npm run build

# Stage 2: Caddy serves the built panel and the firmware image on Railway's $PORT.
# The broker password and Spotify client ID come from service variables at start.
# Variables: MQTT_USER/MQTT_PASS (the panel's broker login), SPOTIFY_CLIENT_ID, and PANEL_USER/PANEL_PASS to put a
# login in front of the page (leave PANEL_PASS unset for no login).
FROM caddy:2.8.4-alpine
COPY Caddyfile Caddyfile.auth /etc/caddy/
COPY start.sh /start.sh
COPY --from=build /w/dist /srv
CMD ["/start.sh"]
