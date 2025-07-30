// Includes
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// --------------------------------- Macros ---------------------------------
#define MAX_NODES 8
#define BAUD_LOW 115200
#define BAUD_HIGH 1000000
#define BAUD_RATE BAUD_LOW
#define MSG_SIZE 512
#define HS_CLEAR_INTERVAL 1000 // every K miliseconds hs_index <- 0
#define PING_DELAY 10 // delay response to simulate distance
#define HS_MAX_PERIOD 500 // max delay for waiting for accepted hs

// --------------------------------- Structures, Enums ---------------------------------
// ALONE = Not part of a grid
// CONNECTED = Connected to a grid
enum ESP_State {
  ALONE,
  CONNECTED
};

// The structure of a message (expected to receive from other esp's)
struct Msg {
  uint8_t payload[MSG_SIZE - 12];
  uint8_t from[6];
  uint8_t to[6];
};

// --------------------------------- Globals ---------------------------------
// Starts as either part of the grid or not
ESP_State esp_state = ALONE;

// What address we expect to hear from when adding a node into the grid
uint8_t expected_address[6] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// Just the address needed to broadcast together with the peer for broadcast
uint8_t broadcast_address[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

// Vector of addresses inside the grid together with the adjacency table
uint8_t addresses[MAX_NODES][6] = {0};
uint32_t grid[MAX_NODES][MAX_NODES] = {0};
uint32_t addr_index = 0;

// Handshake addresses
uint8_t hs_addresses[MAX_NODES][6] = {0};
uint32_t hs_index = 0;
unsigned long last_clear_time = 0;

// Tells us if the ESP is doing a handshake
bool doing_hs = false;

// Metric time
unsigned long metric_sent_time = 0;

// Debug mode (prints stuff)
bool debug_mode = false;

// True after alone receives a handshake signal
bool hs_received = false;

// --------------------------------- Functions ---------------------------------
void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status);
void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len);
void add_peer(const uint8_t peerMac[6]);
void send_msg(Msg &message);
Msg get_message(uint8_t to[6], const String& text);
void print_hs_table();
void print_command_table();
bool is_numeric(const String &s);
bool process_input(String input);
bool mac_equals(const uint8_t a[6], const uint8_t b[6]);
int32_t find_address(const uint8_t target[6]);
bool add_address(const uint8_t new_mac[6]);
bool remove_address(const uint8_t remMac[6]);
bool add_link_by_index(uint32_t fromIdx, uint32_t toIdx, uint32_t link);
bool remove_link_by_index(uint32_t fromIdx, uint32_t toIdx);
bool add_link_by_mac(const uint8_t macA[6], const uint8_t macB[6], uint32_t link);
bool remove_link_by_mac(const uint8_t macA[6], const uint8_t macB[6]);
Msg get_grid_message(const uint8_t destMac[6]);
void update_grid_from_payload(const uint8_t *payload);
void print_adjacency_table();

// --------------------------------- Setup ---------------------------------
void setup() {
  // Try to connect to serial comm
  Serial.begin(BAUD_RATE);
  // Set ESP-NOW
  WiFi.mode(WIFI_STA);
  delay(100);
  // Init esp now
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }
  esp_now_register_send_cb(on_data_sent);
  esp_now_register_recv_cb(on_data_recv);
  // If node is connected initially => it's a grid on its own
  if (esp_state == CONNECTED) {
    uint8_t mac[6];
    WiFi.macAddress(mac);
    add_address(mac);
  }
  // Send a message to serial that ESP is ready
  Serial.println("ESP32 Ready");
}

// --------------------------------- Loop ---------------------------------
void loop() {
  // We need this to do operations every some period of time
  unsigned long millis_now = millis();

  // Read input
  if (Serial.available()) {
    String input = Serial.readString();
    if (!process_input(input)) {
      Serial.println("Failed to parse command. Type HELP");
    }
  }

  // --------------------------------- Connected ---------------------------------
  if (esp_state == CONNECTED) {
    // Refresh handshake table
    if ( (millis_now - last_clear_time) >= HS_CLEAR_INTERVAL == 0 ) {
      last_clear_time += HS_CLEAR_INTERVAL;
      if(hs_index != 0) {
        memset(hs_addresses, 0, hs_index * sizeof(hs_addresses[0]));
        hs_index = 0;
      }
    }
  }
  // --------------------------------- Alone ---------------------------------
  else {
    if (!hs_received) {
      // Scream your name! (only if we dont receive hs)
      Msg msg = get_message(broadcast_address, "");
      send_msg(msg);
    }
  }
}

// --------------------------------- Other functions ---------------------------------
void on_data_recv(const esp_now_recv_info_t *recv_info, const uint8_t *incomingData, int len) {
  if (len != sizeof(Msg)) {
    return;
  }

  Msg message = {0};
  memcpy(&message, incomingData, sizeof(Msg));

  if (mac_equals(message.from, message.to)) {
    return;
  }

  if (debug_mode) {
    char fromMacStr[18];
    snprintf(fromMacStr, sizeof(fromMacStr),
              "%02X:%02X:%02X:%02X:%02X:%02X",
              message.from[0], message.from[1], message.from[2],
              message.from[3], message.from[4], message.from[5]);
    Serial.print("DEBUG: Received from ");
    Serial.println(fromMacStr);

    Serial.print("DEBUG: Payload = ");
    Serial.println((char *)message.payload);
  }

  // ---------------------------- Connected ----------------------------
  if (esp_state == CONNECTED) {
    if (memcmp(message.payload, "HANDSHAKE", strlen("HANDSHAKE")) == 0) {
      Serial.println("Hey! A new friend! Added to the list...");
      update_grid_from_payload(message.payload);
    }

    if (!doing_hs) {
      bool already_present = false;
      for (uint32_t i = 0; i < hs_index; i++) {
        if (memcmp(hs_addresses[i], message.from, 6) == 0) {
          already_present = true;
          break;
        }
      }

      if (!already_present && hs_index < MAX_NODES) {
        memcpy(hs_addresses[hs_index], message.from, 6);
        hs_index ++;
      }
    } else {
      if (millis() - metric_sent_time >= HS_MAX_PERIOD) {
        doing_hs = false;
      }

      if (memcmp(message.payload, "ACCEPTED", strlen("ACCEPTED")) == 0) {
        metric_sent_time = millis() - metric_sent_time;
        doing_hs = false;
        Serial.print("Handshake successful. Ping time: ");
        Serial.println(metric_sent_time);
        Serial.println("Sending back the adjacency table. Updating self table.");
        // Update self table
        add_address(message.from);
        add_link_by_mac(message.to, message.from, metric_sent_time);
        add_link_by_mac(message.from, message.to, metric_sent_time);
        // Send back the grid
        Msg msg = get_grid_message(message.from);
        send_msg(msg);
        // Tell others about your new friend
        int32_t localIdx = find_address(message.to);
        int32_t otherIdx = find_address(message.from);
        for (uint8_t i = 0; i < addr_index; i++) {
          if ((int32_t)i == localIdx) continue;
          if ((int32_t)i == otherIdx) continue;

          msg = get_grid_message(addresses[i]);
          send_msg(msg);
        }
      }
    }
  }
  // ---------------------------- Alone ----------------------------
  else {
    if (memcmp(message.payload, "HANDSHAKE", strlen("HANDSHAKE")) == 0) {
      Serial.println("Detected HANDSHAKE");
      if (hs_received) {
        Serial.println("Grid received. Will now update table.");
        update_grid_from_payload(message.payload);
        esp_state = CONNECTED;
        hs_received = false;
      }
      else {
        hs_received = true;
        Msg msg = get_message(message.from, "ACCEPTED");
        delay(PING_DELAY);
        send_msg(msg);
        Serial.println("Handshake received from grid node. Sent back accepting message");
      }
    }
  }
}

bool is_numeric(const String &s) {
  if (s.length() == 0) return false;
  for (size_t i = 0; i < s.length(); i++) {
    if (!isDigit(s.charAt(i))) {
      return false;
    }
  }
  return true;
}

bool process_input(String input) {
  input.trim();
  input.toUpperCase();

  // 1) LST GRID
  if (input == "LS GRID") {
    print_adjacency_table();
    return true;
  }

  // 2) LST_HS
  else if (input == "LS HS") {
    print_hs_table();
    return true;
  }

  // 3) ADD [index]
  else if (input.startsWith("ADD ")) {
    String idxStr = input.substring(4);
    idxStr.trim();

    if (!is_numeric(idxStr)) {
      return false;
    }

    int index = idxStr.toInt();

    if (index < 0 || index >= hs_index) {
      Serial.println("Error: Index out of range");
      return true;
    }

    String payload = String("HANDSHAKE");
    Msg msg = get_message(hs_addresses[index], payload);
    doing_hs = true;
    metric_sent_time = millis();
    send_msg(msg);

    return true;
  }

  // 4) DEBUG — enable debug mode
  else if (input == "DEBUG") {
    debug_mode = !debug_mode;
    if (debug_mode) Serial.println("Debug mode enabled");
    else Serial.println("Debug mode disabled");
    return true;
  }

  // 5) HELP
  else if (input == "HELP") {
    print_command_table();
    return true;
  }

  // No matching command
  return false;
}

void print_hs_table() {
  Serial.println(F("\n\nIndex\tMAC Address"));
  Serial.println(F("-----\t-----------------"));

  for (uint32_t i = 0; i < hs_index; i++) {
    Serial.print(i);
    Serial.print('\t');

    Serial.printf(
      "%02X:%02X:%02X:%02X:%02X:%02X\n",
      hs_addresses[i][0],
      hs_addresses[i][1],
      hs_addresses[i][2],
      hs_addresses[i][3],
      hs_addresses[i][4],
      hs_addresses[i][5]
    );
  }

  if (hs_index == 0) {
    Serial.println(F("(no MACs stored)"));
  }
}

void print_adjacency_table() {
  Serial.println(F("\nIndex → MAC:"));
  for (uint32_t i = 0; i < addr_index; i++) {
    Serial.print(F("  ["));
    Serial.print(i);
    Serial.print(F("] "));
    for (uint8_t b = 0; b < 6; b++) {
      if (addresses[i][b] < 0x10) Serial.print('0');
      Serial.print(addresses[i][b], HEX);
      if (b < 5) Serial.print(':');
    }
    Serial.println();
  }

  // 2) Print the adjacency matrix header
  Serial.println(F("\nAdjacency table:"));
  Serial.print(F("     "));
  for (uint32_t j = 0; j < addr_index; j++) {
    Serial.print(j);
    Serial.print(F("     "));
  }
  Serial.println();

  for (uint32_t i = 0; i < addr_index; i++) {
    Serial.print(i);
    Serial.print(F(":  "));

    for (uint32_t j = 0; j < addr_index; j++) {
      Serial.print(F(" "));
      Serial.print(grid[i][j]);
      Serial.print(F("     "));
    }
    Serial.println();
  }

  Serial.println();
}

Msg get_message(uint8_t to[8], const String& text) {
  Msg message = {0};
  memset(&message, 0, sizeof(Msg));

  WiFi.macAddress(message.from);

  size_t maxCopy = sizeof(message.payload) - 1;
  size_t len = text.length();
  if (len > maxCopy) len = maxCopy;

  memcpy(message.payload, text.c_str(), len);
  message.payload[len] = '\0';
  memcpy(message.to, to, 6);

  return message;
}

void send_msg(Msg &message) {
  add_peer(message.to);
  WiFi.macAddress(message.from);

  esp_err_t err = esp_now_send(message.to, (uint8_t *)&message, sizeof(Msg));
  if (err != ESP_OK) {
    Serial.printf("esp_now_send failed (err=%d)\n", err);
  }

  delay(100);
}

void on_data_sent(const uint8_t *mac_addr, esp_now_send_status_t status) {
  if (status != ESP_NOW_SEND_SUCCESS) {
    Serial.println("on_data_sent() failed");
  }
}

void add_peer(const uint8_t peerMac[6]) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, peerMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_now_del_peer(peerMac);

  esp_err_t res = esp_now_add_peer(&peerInfo);
  if (res != ESP_OK) {
    Serial.printf("Failed to add peer (err=%d)\n", res);
  }
}

void print_command_table() {
  Serial.println(F("COMMAND     | SYNTAX       | DESCRIPTION"));
  Serial.println(F("------------+--------------+--------------------------------"));
  Serial.println(F("LS GRID     | —            | List the entire grid"));
  Serial.println(F("LS HS       | —            | List all HS MAC addresses"));
  Serial.println(F("ADD [index] | index = 0..N | Add the HS entry at position “index”"));
  Serial.println(F("HELP        | —            | Show this help table"));
  Serial.println(F("DEBUG       | —            | Toggles debug mode"));
}

bool mac_equals(const uint8_t a[6], const uint8_t b[6]) {
  return (memcmp(a, b, 6) == 0);
}

int32_t find_address(const uint8_t target[6]) {
  for (uint32_t i = 0; i < addr_index; i++) {
    if (mac_equals(addresses[i], target)) {
      return (int32_t)i;
    }
  }
  return -1;
}

bool add_address(const uint8_t new_mac[6]) {
  if (addr_index >= MAX_NODES) {
    return false;
  }
  if (find_address(new_mac) >= 0) {
    return false;
  }
  memcpy(addresses[addr_index], new_mac, 6);
  addr_index++;
  return true;
}

bool remove_address(const uint8_t remMac[6]) {
  int32_t pos = find_address(remMac);
  if (pos < 0) {
    return false;
  }

  for (uint32_t col = 0; col < MAX_NODES; col++) {
    grid[pos][col] = 0;
  }
  for (uint32_t row = 0; row < MAX_NODES; row++) {
    grid[row][pos] = 0;
  }

  for (uint32_t i = (uint32_t)pos; i < addr_index - 1; i++) {
    memcpy(addresses[i], addresses[i + 1], 6);
  }
  memset(addresses[addr_index - 1], 0, 6);
  addr_index--;
  return true;
}

bool add_link_by_index(uint32_t fromIdx, uint32_t toIdx, uint32_t value) {
  if (fromIdx >= addr_index || toIdx >= addr_index) {
    return false;
  }
  grid[fromIdx][toIdx] = value;
  return true;
}

bool remove_link_by_index(uint32_t fromIdx, uint32_t toIdx) {
  if (fromIdx >= addr_index || toIdx >= addr_index) {
    return false;
  }
  grid[fromIdx][toIdx] = 0;
  return true;
}

bool add_link_by_mac(const uint8_t macA[6], const uint8_t macB[6], uint32_t value) {
  int32_t idxA = find_address(macA);
  int32_t idxB = find_address(macB);
  if (idxA < 0 || idxB < 0) {
    return false;
  }
  return add_link_by_index((uint32_t)idxA, (uint32_t)idxB, value);
}

bool remove_link_by_mac(const uint8_t macA[6], const uint8_t macB[6]) {
  int32_t idxA = find_address(macA);
  int32_t idxB = find_address(macB);
  if (idxA < 0 || idxB < 0) {
    return false;
  }
  return remove_link_by_index((uint32_t)idxA, (uint32_t)idxB);
}

Msg get_grid_message(const uint8_t destMac[6]) {
  Msg message;
  memset(&message, 0, sizeof(Msg));

  WiFi.macAddress(message.from);
  memcpy(message.to, destMac, 6);

  uint8_t *p = message.payload;
  size_t maxPayload = sizeof(message.payload);

  const char handshakeStr[] = "HANDSHAKE";
  size_t hsLen = strlen(handshakeStr);
  if (hsLen + 1 > maxPayload) {
    return message;
  }
  memcpy(p, handshakeStr, hsLen);
  p += hsLen;
  *p++ = '\0';

  uint8_t N = (addr_index > MAX_NODES ? (uint8_t)MAX_NODES : (uint8_t)addr_index);
  if ((size_t)(p - message.payload) + 1 > maxPayload) {
    return message;
  }
  *p++ = N;

  for (uint8_t i = 0; i < N; i++) {
    if ((size_t)(p - message.payload) + 6 > maxPayload) {
      return message;
    }
    memcpy(p, addresses[i], 6);
    p += 6;
  }

  for (uint8_t i = 0; i < N; i++) {
    for (uint8_t j = 0; j < N; j++) {
      if ((size_t)(p - message.payload) + sizeof(uint32_t) > maxPayload) {
        return message;
      }
      memcpy(p, &grid[i][j], sizeof(uint32_t));
      p += sizeof(uint32_t);
    }
  }

  return message;
}


void update_grid_from_payload(const uint8_t *payload) {
  const char handshakeStr[] = "HANDSHAKE";
  size_t hsLen = strlen(handshakeStr);
  if (memcmp(payload, handshakeStr, hsLen) != 0) return;
  if (payload[hsLen] != '\0') return;
  const uint8_t *p = payload + hsLen + 1;
  uint8_t N = *p++;
  if (N == 0 || N > MAX_NODES) return;
  size_t bytesLeft = sizeof(((Msg*)0)->payload) - (p - payload);
  size_t needed = (size_t)6 * N + (size_t)4 * N * N;
  if (needed > bytesLeft) return;
  addr_index = N;
  for (uint8_t i = 0; i < N; i++) {
    memcpy(addresses[i], p, 6);
    p += 6;
  }
  for (uint8_t i = N; i < MAX_NODES; i++) {
    memset(addresses[i], 0, 6);
  }
  for (uint8_t i = 0; i < MAX_NODES; i++) {
    for (uint8_t j = 0; j < MAX_NODES; j++) {
      grid[i][j] = 0;
    }
  }
  for (uint8_t i = 0; i < N; i++) {
    for (uint8_t j = 0; j < N; j++) {
      uint32_t v;
      memcpy(&v, p, sizeof(uint32_t));
      grid[i][j] = v;
      p += sizeof(uint32_t);
    }
  }
}
