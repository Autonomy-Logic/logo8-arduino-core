/*
  SmokeTest — the sketch CI compiles to prove the core is usable.

  Deliberately in-tree rather than picked from libraries/examples: most of
  those are inherited MSP430 examples that cannot build for a TM4C target, so
  choosing one at random is how a green CI job stops meaning anything.

  Exercises what the LOGO! actually needs: the core's Arduino API, and the
  Ethernet library against the on-chip MAC.
*/
#include <Ethernet.h>

byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xFE, 0xED };
IPAddress ip(192, 168, 2, 5);
EthernetServer server(502);

void setup() {
  Serial.begin(115200);
  Ethernet.begin(mac, ip);
  server.begin();
  pinMode(RED_LED, OUTPUT);
}

void loop() {
  EthernetClient client = server.available();
  if (client) {
    while (client.connected() && client.available() > 0) {
      client.write((uint8_t)client.read());
    }
    client.stop();
  }
  digitalWrite(RED_LED, (millis() / 500) % 2);
}
