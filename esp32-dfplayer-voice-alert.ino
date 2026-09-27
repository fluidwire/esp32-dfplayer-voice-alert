/*
 * ESP32 DFPlayer Mini Voice Alerts
 * --------------------------------
 * Listens on MQTT for named alert topics (flood / gate / queue / test) and
 * plays a pre-recorded voice clip from a DFPlayer Mini over UART2 -- the
 * "instead of a buzzer, say it out loud in Tagalog or English" building
 * block for flood-warning posts, gate/entry systems, or queue-number
 * announcers.
 *
 * How the topic-to-clip mapping works:
 *   This sketch subscribes to a wildcard topic (MQTT_TOPIC_ALERT_BASE"/#").
 *   Whatever comes after the last "/" is looked up in a small local table
 *   (ALERT_TABLE below) that maps a name straight to a numbered clip on the
 *   SD card -- e.g. ".../alert/flood" plays 0001.mp3. Add rows to the table
 *   to add clips; nothing else in the message payload matters (an empty
 *   payload is fine, the topic itself is the command).
 *
 * SD card layout (DFPlayer's own numbering rule, not a folder you choose):
 *   Root of the microSD card, files named 0001.mp3, 0002.mp3, 0003.mp3, ...
 *   up to 0255.mp3 (4-digit, zero-padded). play(1) plays 0001.mp3. This is
 *   the plain "numbered files in the root" mode -- if you instead use the
 *   "01/001.mp3" sub-folder mode you'd call playFolder() and the numbers
 *   below would mean something different. Don't mix the two card layouts.
 *
 * Busy-pin checks (so alerts don't cut each other off):
 *   The DFPlayer Mini breakout's BUSY pin drops LOW while a clip is
 *   playing on MOST breakout boards, and idles HIGH otherwise -- but a few
 *   clones wire it inverted. Flash this sketch, trigger one alert, and
 *   watch the Serial log: it prints "busy=..." on every state change. If
 *   the log says busy=1 while a clip is clearly playing, flip
 *   DFPLAYER_BUSY_ACTIVE_LOW to false in config.h and reflash. Getting the
 *   polarity right matters here because a new alert that arrives mid-clip
 *   is deliberately dropped (not queued) rather than talking over the one
 *   already playing -- with the polarity backwards you'd either always
 *   drop new alerts or never protect the current one.
 *
 * Required libraries (install via Library Manager):
 *   - "DFRobotDFPlayerMini" by DFRobot
 *   - "PubSubClient" by Nick O'Leary
 *
 * Wiring:
 *   DFPlayer VCC   -> ESP32 5V (or a clean 5V rail -- the amp stage wants it)
 *   DFPlayer GND   -> ESP32 GND (share ground with the speaker's PSU too)
 *   DFPlayer TX    -> ESP32 GPIO16 (RX2)
 *   DFPlayer RX    -> ESP32 GPIO17 (TX2) THROUGH A ~1kOhm SERIES RESISTOR --
 *                     the module's RX is a 5V logic input on most breakouts;
 *                     ESP32 GPIO is 3.3V-only. Most DFPlayer clones tolerate
 *                     3.3V TX directly, but the resistor is cheap insurance
 *                     and does no harm if it turns out you didn't need it.
 *   DFPlayer BUSY  -> ESP32 GPIO4 (see polarity note above)
 *   DFPlayer SPK_1/SPK_2 -> a small 3W 4-8 Ohm speaker (NOT a line-level
 *                     input -- these pins are a raw amplifier output)
 *   microSD card   -> DFPlayer's onboard card slot, FAT32, files named
 *                     0001.mp3 .. 0255.mp3 in the root
 *
 * Setup:
 *   1. Copy config.h.example to config.h. Fill in WiFi + MQTT + volume.
 *   2. Record or find short MP3 clips, name them 0001.mp3, 0002.mp3, ...
 *      per ALERT_TABLE below, drop them in the SD card root.
 *   3. Flash, open Serial Monitor at 115200, confirm "[dfplayer] ready".
 *   4. Publish to fluidwire/demo/dfplayer/alert/flood (any payload, even
 *      empty) and confirm 0001.mp3 plays and BUSY tracks correctly.
 *
 * Sample MQTT status payload:
 *   {"alert":"flood","track":1,"action":"playing"}
 *   {"alert":"gate","track":2,"action":"skipped_busy"}
 *
 * (c) 2026 Fluidwire. Released under the MIT License.
 * https://github.com/fluidwire
 */

#include <WiFi.h>
#include <PubSubClient.h>
#include <DFRobotDFPlayerMini.h>
#include "config.h"

// --- Pins --------------------------------------------------------------
const int DFPLAYER_RX2_PIN = 16; // ESP32 RX2 <- DFPlayer TX
const int DFPLAYER_TX2_PIN = 17; // ESP32 TX2 -> DFPlayer RX (via series resistor)
const int DFPLAYER_BUSY_PIN = 4; // DFPlayer BUSY -> ESP32 GPIO4

// --- Alert table: last path segment of the MQTT topic -> SD card track --
struct AlertEntry {
  const char *name;
  uint8_t track;
};
const AlertEntry ALERT_TABLE[] = {
  {"flood", 1}, // 0001.mp3
  {"gate",  2}, // 0002.mp3
  {"queue", 3}, // 0003.mp3
  {"test",  4}, // 0004.mp3
};
const size_t ALERT_TABLE_LEN = sizeof(ALERT_TABLE) / sizeof(ALERT_TABLE[0]);

HardwareSerial dfSerial(2); // UART2
DFRobotDFPlayerMini dfPlayer;
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

bool lastBusyRaw = true; // last raw digitalRead() value, for change-logging only

bool isBusyNow() {
  int raw = digitalRead(DFPLAYER_BUSY_PIN);
  if (raw != (lastBusyRaw ? HIGH : LOW)) {
    Serial.print(F("[dfplayer] BUSY pin raw="));
    Serial.println(raw);
  }
  lastBusyRaw = (raw == HIGH);
  bool activeLow = DFPLAYER_BUSY_ACTIVE_LOW;
  return activeLow ? (raw == LOW) : (raw == HIGH);
}

void publishStatus(const char *alert, int track, const char *action) {
  char buf[128];
  snprintf(buf, sizeof(buf), "{\"alert\":\"%s\",\"track\":%d,\"action\":\"%s\"}",
           alert, track, action);
  Serial.print(F("[mqtt] "));
  Serial.println(buf);
  mqtt.publish(MQTT_TOPIC_STATUS, buf);
}

int lookupTrack(const char *alertName) {
  for (size_t i = 0; i < ALERT_TABLE_LEN; i++) {
    if (strcmp(ALERT_TABLE[i].name, alertName) == 0) {
      return ALERT_TABLE[i].track;
    }
  }
  return -1;
}

void onMqttMessage(char *topic, uint8_t *payload, unsigned int length) {
  (void)payload;
  (void)length;

  // Pull the last "/"-separated segment off the topic as the alert name.
  String t(topic);
  int lastSlash = t.lastIndexOf('/');
  String alertName = (lastSlash >= 0) ? t.substring(lastSlash + 1) : t;

  int track = lookupTrack(alertName.c_str());
  if (track < 0) {
    Serial.print(F("[dfplayer] unknown alert: "));
    Serial.println(alertName);
    publishStatus(alertName.c_str(), -1, "unknown_alert");
    return;
  }

  if (isBusyNow()) {
    Serial.print(F("[dfplayer] busy, dropping alert: "));
    Serial.println(alertName);
    publishStatus(alertName.c_str(), track, "skipped_busy");
    return;
  }

  Serial.print(F("[dfplayer] playing track "));
  Serial.print(track);
  Serial.print(F(" for alert "));
  Serial.println(alertName);
  dfPlayer.play(track);
  publishStatus(alertName.c_str(), track, "playing");
}

void connectWiFi() {
  Serial.print(F("[wifi] connecting to "));
  Serial.println(WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print('.');
  }
  Serial.println();
  Serial.print(F("[wifi] IP: "));
  Serial.println(WiFi.localIP());
}

void connectMQTT() {
  mqtt.setServer(MQTT_HOST, MQTT_PORT);
  mqtt.setCallback(onMqttMessage);
  while (!mqtt.connected()) {
    Serial.print(F("[mqtt] connecting... "));
    bool ok = strlen(MQTT_USER) > 0
      ? mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)
      : mqtt.connect(MQTT_CLIENT_ID);
    if (ok) {
      Serial.println(F("ok"));
      char sub[96];
      snprintf(sub, sizeof(sub), "%s/#", MQTT_TOPIC_ALERT_BASE);
      mqtt.subscribe(sub);
      Serial.print(F("[mqtt] subscribed to "));
      Serial.println(sub);
    } else {
      Serial.print(F("failed state="));
      Serial.println(mqtt.state());
      delay(2000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(200);

  pinMode(DFPLAYER_BUSY_PIN, INPUT_PULLUP);

  dfSerial.begin(9600, SERIAL_8N1, DFPLAYER_RX2_PIN, DFPLAYER_TX2_PIN);
  Serial.println(F("[dfplayer] initializing..."));
  if (!dfPlayer.begin(dfSerial, /*isACK=*/true, /*doReset=*/true)) {
    Serial.println(F("[dfplayer] FAILED to init -- check wiring + SD card"));
    while (true) delay(1000);
  }
  dfPlayer.volume(DFPLAYER_VOLUME);
  Serial.println(F("[dfplayer] ready"));

  connectWiFi();
  connectMQTT();
}

void loop() {
  if (!mqtt.connected()) connectMQTT();
  mqtt.loop();

  static uint32_t lastHeartbeat = 0;
  uint32_t now = millis();
  if (now - lastHeartbeat >= 30000UL || lastHeartbeat == 0) {
    Serial.print(F("[dfplayer] heartbeat, busy="));
    Serial.println(isBusyNow() ? "1" : "0");
    lastHeartbeat = now;
  }

  delay(10);
}
