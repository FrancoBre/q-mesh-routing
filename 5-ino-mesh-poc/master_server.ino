#include <ArduinoJson.h>
#include <map>
#include <vector>
#include <random>
#include "painlessMesh.h"
#include <ESP8266WiFi.h>
#include <ESP8266WiFiMulti.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClient.h>

ESP8266WiFiMulti WiFiMulti;

#define   MESH_PREFIX     "whateverYouLike"
#define   MESH_PASSWORD   "somethingSneaky"
#define   MESH_PORT       5555
#define   SECRET_SSID     ""
#define   SECRET_PASS     ""

int status = WL_IDLE_STATUS;
long lastConnectionTime = 0; 
boolean lastConnected = false;
int failedCounter = 0;

Scheduler userScheduler; // to control your personal task
painlessMesh mesh;

void receivedCallback(uint32_t from, String &msg);
void updateQTable(String state_from, String state_to, float reward, float alpha, float gamma, JsonDocument& doc);
void sendMessageToServer(String &msg);

// ThingSpeak Settings
char thingSpeakAddress[] = "api.thingspeak.com";
String writeAPIKey = "";
const int updateThingSpeakInterval = 16 * 1000;      // Time interval in milliseconds to update ThingSpeak (number of seconds * 1000 = interval)

void newConnectionCallback(uint32_t nodeId) {
  Serial.print("--> startHere: New Connection, nodeId = ");
  Serial.println(nodeId);
}

void changedConnectionCallback() {
  Serial.println("Changed connections");
}

void nodeTimeAdjustedCallback(int32_t offset) {
  Serial.print("Adjusted time ");
  Serial.print(mesh.getNodeTime());
  Serial.print(". Offset = ");
  Serial.println(offset);
}

void receivedCallback(uint32_t from, String &msg) {
  Serial.print("startHere: Received from ");
  Serial.print(from);
  Serial.print(" msg=");
  Serial.println(msg);
  Serial.flush();

  // Deserialize the JSON message
  StaticJsonDocument<1024> doc;
  DeserializationError error = deserializeJson(doc, msg);

  if (error) {
    Serial.print(F("deserializeJson() failed: "));
    Serial.println(error.c_str());
    Serial.flush();
    return;
  }

  // Extract q-learning parameters and state information
  float q_alpha = doc["q_parameters"]["alpha"];
  float q_gamma = doc["q_parameters"]["gamma"];
  float q_epsilon = doc["q_parameters"]["epsilon"];
  float q_epsilonDecay = doc["q_parameters"]["epsilon_decay"];

  Serial.println("Extracted Q-learning parameters:");
  Serial.print("alpha: ");
  Serial.print(q_alpha);
  Serial.print(", gamma: ");
  Serial.print(q_gamma);
  Serial.print(", epsilon: ");
  Serial.print(q_epsilon);
  Serial.print(", epsilonDecay: ");
  Serial.println(q_epsilonDecay);
  Serial.flush();

  int current_episode = doc["current_episode"];
  float accumulated_reward = doc["accumulated_reward"];
  float total_time = doc["total_time"];

  Serial.println("Extracted episode information:");
  Serial.print("Current episode: ");
  Serial.print(current_episode);
  Serial.print(", Accumulated reward: ");
  Serial.print(accumulated_reward);
  Serial.print(", Total time: ");
  Serial.println(total_time);
  Serial.flush();

  JsonArray episodes = doc["episodes"];
  for (JsonObject episode : episodes) {
    int episode_number = episode["episode_number"];
    float reward = episode["reward"];
    float time = episode["time"];

    Serial.println("Processing episode:");
    Serial.print("Episode number: ");
    Serial.print(episode_number);
    Serial.print(", Reward: ");
    Serial.print(reward);
    Serial.print(", Time: ");
    Serial.println(time);
    Serial.flush();

    JsonArray steps = episode["steps"];
    for (JsonObject step : steps) {
      int hop = step["hop"];
      String node_from = step["node_from"];
      String node_to = String(mesh.getNodeId());

      Serial.println("Processing step:");
      Serial.print("Hop: ");
      Serial.print(hop);
      Serial.print(", Node from: ");
      Serial.print(node_from);
      Serial.print(", Node to: ");
      Serial.println(node_to);
      Serial.flush();

      // Add reward to episode
      episode["reward"] = episode["reward"] + 100; // Master node found!
      Serial.println("Master node found! Reward increased by 100");
      Serial.flush();

      // Update Q-Table
      updateQTable(node_from, node_to, episode["reward"], q_alpha, q_gamma, doc);

      // Send updated message to the next hop
      String updatedJsonString;
      serializeJson(doc, updatedJsonString);
      updateThingSpeak(updatedJsonString);
    }
  }
}

void updateQTable(String state_from, String state_to, float reward, float alpha, float gamma, JsonDocument& doc) {
  auto nodes = mesh.getNodeList(true);
  std::vector<int> neighbors;
  String nodesStr;
  int num_neighbors = 0;

  JsonObject q_table = doc["q_table"];
  for (auto &&id : nodes) {
    String node_from = String(id);
    for (auto &&id_2 : nodes) {
      String node_to = String(id_2);

      if (id_2 != id) {
        if (!q_table.containsKey(node_from)) {
          q_table.createNestedObject(node_from);
        }

        if (!q_table[node_to].containsKey(node_to)) {
          q_table[node_from][node_to] = 0.0; // Initialize Q-value if not present
        }
      }
    }
  }

  Serial.println("Q-table:");
  serializeJsonPretty(q_table, Serial);
  Serial.flush();

  // Ensure state_from exists in q_table
  if (!q_table.containsKey(state_from)) {
    q_table.createNestedObject(state_from);
  }

  // Ensure state_to exists in q_table[state_from]
  if (!q_table[state_from].containsKey(state_to)) {
    q_table[state_from][state_to] = 0.0; // Initialize Q-value if not present
  }

  // Retrieve Q-value to update
  float currentQ = q_table[state_from][state_to].as<float>();

  // Calculate maxQ for state_to
  float maxQ = -9999.0; // Start with a very low value
  if (q_table.containsKey(state_to)) {
    JsonObject actions = q_table[state_to];
    for (JsonPair kv : actions) {
      float value = kv.value().as<float>();
      if (value > maxQ) {
        maxQ = value;
      }
    }
  }

  // Apply Bellman equation to update Q-value
  float updatedQ = currentQ + alpha * (reward + gamma * maxQ - currentQ);

  // Update Q-value in q_table
  q_table[state_from][state_to] = updatedQ;

  // Log updated Q-table
  Serial.println("Updated Q-table:");
  serializeJsonPretty(q_table, Serial);
  Serial.println();
  Serial.flush();
}

int chooseAction(int state, JsonDocument& doc, float epsilon) {
  auto nodes = mesh.getNodeList(true);
  std::vector<int> neighbors;
  String nodesStr;
  int num_neighbors = 0;

  Serial.print("iterating over node list ");
  for (auto &&id : nodes) {
    neighbors.push_back(id);
    nodesStr += String(id) + String(" ");
    Serial.print(nodesStr);
    num_neighbors++;
  }
  Serial.flush();

  if (num_neighbors == 0) {
    Serial.println("No neighbors found");
    return -1;
  }
  Serial.flush();

  Serial.print("Neighbors for state ");
  Serial.print(String(state));
  Serial.print(" are ");
  Serial.println(nodesStr);
  Serial.flush();

  if (random() < epsilon) {
    // Explore
    int action_index = random(0, num_neighbors - 1);
    int action = neighbors[action_index];
    Serial.println("Exploring action");
    Serial.println(action);
    Serial.flush();
    return action;
  } else {
    // Exploit: Choose the action with the highest Q-value
    JsonObject q_table = doc["q_table"];
    if (!q_table.containsKey(String(state))) {
      Serial.print("No Q-values found for state");
      Serial.println(String(state));
      Serial.flush();
      return -1;
    }

    JsonObject actions = q_table[String(state)];
    float best_value = -1.0;
    String best_action = "";
    Serial.println("Starting exploitation phase...");
    Serial.flush();
    for (JsonPair kv : actions) {
      String action = kv.key().c_str();
      float value = kv.value().as<float>();
      uint32_t action_int = action.toInt();
      Serial.print("Checking action: ");
      Serial.print(action.c_str());
      Serial.print(" with value ");
      Serial.println(value);
      Serial.flush();
      if (value > best_value && std::find(neighbors.begin(), neighbors.end(), action_int) != neighbors.end()) {
        Serial.print("Action ");
        Serial.print(action.c_str());
        Serial.print(" is a valid neighbor and has a better value ");
        Serial.println(value);
        Serial.flush();
        best_value = value;
        best_action = action;
      } else {
        Serial.print("Action ");
        Serial.print(action.c_str());
        Serial.println(" is not valid or does not have a better value");
        Serial.flush();
      }
    }
    Serial.print("Exploiting best action: ");
    Serial.print(best_action.c_str());
    Serial.print(" with value ");
    Serial.println(best_value);
    Serial.flush();
    return best_action.toInt();
  }
}

void updateThingSpeak(String tsData) {
  Serial.print("Sending final q-learning information to server ");
  Serial.print(": ");
  Serial.println(tsData);
  Serial.flush();

  if ((WiFi.status() == WL_CONNECTED)) {

    WiFiClient client;
    HTTPClient http;

    Serial.print("[HTTP] begin...\n");
    // configure traged server and url
    if (http.begin(client, "api.thingspeak.com/update?api_key=" + writeAPIKey + "&field1=" + tsData)) {  // HTTP
      Serial.print("[HTTP] GET...\n");
      // start connection and send HTTP header
      int httpCode = http.GET();
      // httpCode will be negative on error
      if (httpCode > 0) {
        // HTTP header has been send and Server response header has been handled
        Serial.printf("[HTTP] GET... code: %d\n", httpCode);

        // file found at server
        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          String payload = http.getString();
          Serial.println(payload);
        }
      } else {
        Serial.printf("[HTTP] GET... failed, error: %s\n", http.errorToString(httpCode).c_str());
      }

      http.end();
    } else {
      Serial.println("[HTTP] Unable to connect");
    }
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.begin(SECRET_SSID, SECRET_PASS);

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.print("Connected! IP address: ");
  Serial.println(WiFi.localIP());
  
  // print the SSID of the network you're attached to:
  Serial.print("SSID: ");
  Serial.println(WiFi.SSID());

  //mesh.setDebugMsgTypes( ERROR | MESH_STATUS | CONNECTION | SYNC | COMMUNICATION | GENERAL | MSG_TYPES | REMOTE ); // all types on
  mesh.setDebugMsgTypes( ERROR | STARTUP );  // set before init() so that you can see startup messages

  mesh.init( MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT );
  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);
  mesh.setRoot(true);
}

void loop() {
  // it will run the user scheduler as well
  mesh.update();
}
