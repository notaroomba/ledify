// For "npm run dev" only: the hosted server (server/index.mjs) writes its own config.js; a "__X__" value means unset.
window.LEDIFY_CONFIG = {
  mqttUrl: 'wss://mosquitto-production-595e.up.railway.app',
  mqttUser: '__MQTT_USER__',
  mqttPass: '__MQTT_PASS__',
  boardMqtt: 'mqtt://zephyr.proxy.rlwy.net:52505',     // what a board should be told to connect to (plain MQTT)
  spotifyClientId: '__SPOTIFY_CLIENT_ID__',
};
