# Stage 1: build the panel (Vite + Solid + Tailwind) into dist/.
FROM node:20-alpine AS build
WORKDIR /w
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY . .
RUN npm run build

# Stage 2: a small Node server (server/index.mjs) serves the panel and the firmware image on Railway's $PORT and,
# once OWNER_PASS and the broker admin login are set, handles logins, board links and per-board broker accounts.
# Without those it works as before: MQTT_USER/MQTT_PASS go into config.js and PANEL_USER/PANEL_PASS put a login in
# front of the page. Accounts keep their state in DATA_DIR, which must be a Railway volume (mounted at /data).
FROM node:22-alpine
WORKDIR /app
COPY package.json package-lock.json ./
RUN npm ci --omit=dev --no-audit --no-fund
COPY server ./server
COPY --from=build /w/dist ./dist
ENV STATIC_DIR=/app/dist DATA_DIR=/data NODE_ENV=production
CMD ["node", "server/index.mjs"]
