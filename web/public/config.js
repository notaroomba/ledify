// Filled in when the container starts (see start.sh); a "__X__" value means a local copy without it.
window.LEDIFY_CONFIG = {
  mqttUrl: 'wss://mosquitto-production-595e.up.railway.app',
  mqttUser: '__MQTT_USER__',
  mqttPass: '__MQTT_PASS__',
  boardMqtt: 'mqtt://zephyr.proxy.rlwy.net:52505',     // what a board should be told to connect to (plain MQTT)
  spotifyClientId: '__SPOTIFY_CLIENT_ID__',
};
