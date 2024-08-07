/*
* SENDER NODE
*/
#include "painlessMesh.h"
#include "DHT.h"

#define MESH_PREFIX "whateverYouLike"
#define MESH_PASSWORD "somethingSneaky"
#define MESH_PORT 5555

#define DPIN 4        //Pin to connect DHT sensor (GPIO number) D2
#define DTYPE DHT11   // Define DHT 11 or DHT22 sensor type

// Q-Learning Parameters (only for node 1 to start the learning)
String q_alpha = "0.1";  // Learning rate
String q_gamma = "0.9";  // Discount factor
String q_epsilon = "0.1";  // Exploration rate
String q_epsilonDecay = "0.1";  // Exploration rate

Scheduler userScheduler; // to control your personal task
painlessMesh mesh;
DHT dht(DPIN,DTYPE);

// save the most recent q table for sending data
// with the latest learning data
JsonDocument qTable;
bool isFirstTime = true;

// User stub
void sendMessage(); // Prototype so PlatformIO doesn't complain
String getNetworkDataToSend();

Task taskSendMessage(TASK_SECOND * 5, TASK_FOREVER, &sendMessage);

void sendMessage() {
  mesh.subConnectionJson(true);
  StaticJsonDocument<1024> doc;

  JsonObject payload = doc.createNestedObject("payload");
  payload["tem"] = dht.readTemperature(false);  //Read temperature in C
  payload["hum"] = dht.readHumidity();          //Read Humidity

  doc["current_node_id"] = String(mesh.getNodeId());

  JsonObject q_parameters = doc.createNestedObject("q_parameters");
  q_parameters["alpha"] = q_alpha;
  q_parameters["gamma"] = q_gamma;
  q_parameters["epsilon"] = q_epsilon;
  q_parameters["epsilon_decay"] = q_epsilonDecay;

  doc["current_episode"] = 1;
  doc["accumulated_reward"] = 0.0;
  doc["total_time"] = 0.0;

  JsonArray episodesArray = doc.createNestedArray("episodes");
  JsonObject episode = episodesArray.createNestedObject();
  episode["episode_number"] = 1;
  episode["reward"] = 0.0;
  episode["time"] = 0.0;

  JsonArray stepsArray = episode.createNestedArray("steps");
  JsonObject step = stepsArray.createNestedObject();
  step["hop"] = 0;
  step["node_from"] = String(mesh.getNodeId());
  // step["node_to"] = String(mesh.getNodeId());

  JsonObject q_table = doc.createNestedObject("q_table");
  JsonObject action = q_table.createNestedObject(String(mesh.getNodeId()));

  int next_action = chooseAction(mesh.getNodeId(), doc, q_epsilon.toFloat());

  if (next_action == -1) {
    return;
  }

  String jsonString;
  serializeJsonPretty(doc, jsonString);

  Serial.println("Data to send : ");
  Serial.println(jsonString);

  Serial.print("To node with id: ");
  Serial.println(next_action);
  mesh.sendSingle(next_action, jsonString);
  //taskSendMessage.setInterval(TASK_SECOND * 1);
  return;
}


bool isHopMessage(StaticJsonDocument<1024> doc) {
  return doc.containsKey("current_node_id") && doc.containsKey("q_parameters")
    && doc.containsKey("current_episode") && doc.containsKey("episodes") 
    && doc.containsKey("q_table");
}

// Needed for painless library
void receivedCallback(uint32_t from, String &msg) {
    Serial.print("startHere: Received from ");
    Serial.print(from);
    Serial.print(" msg=");
    Serial.println(msg.c_str());

    // Deserialize the JSON message
    StaticJsonDocument<1024> doc;
    DeserializationError error = deserializeJson(doc, msg);

    if (error) {
        Serial.print(F("deserializeJson() failed: "));
        Serial.println(error.c_str());
        Serial.flush();
        return;
    }

    // Check if the message has all the learning data
    if (isHopMessage(doc)) {
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
                episode["reward"] = reward - 1; // This node is not master!
                Serial.println("This node is not master! Reduce episode reward in 1");
                Serial.flush();

                // Update Q-Table
                updateQTable(node_from, node_to, episode["reward"], q_alpha, q_gamma, doc);

                // Choose next action using epsilon-greedy policy
                int next_action = chooseAction(mesh.getNodeId(), doc, q_epsilon);

                Serial.print("Chosen next action: ");
                Serial.println(next_action);
                Serial.flush();

                // Send updated message to the next hop
                String updatedJsonString;
                serializeJson(doc, updatedJsonString);
                qTable = doc["q_table"];
                sendMessageToNextHop(next_action, updatedJsonString);
            }
        }
    } 
    // Message is a q_table update broadcast
    else if (doc.is<JsonObject>()) {
        Serial.println("Received Q-Table update:");

        for (JsonPair kv : doc.as<JsonObject>()) {
            const char* node_id = kv.key().c_str();
            JsonObject q_values = kv.value().as<JsonObject>();

            Serial.print("Node ID: ");
            Serial.println(node_id);

            for (JsonPair q_value : q_values) {
                const char* neighbor_id = q_value.key().c_str();
                float value = q_value.value().as<float>();

                Serial.print("Neighbor ID: ");
                Serial.print(neighbor_id);
                Serial.print(" Value: ");
                Serial.println(value);
            }
        }
        Serial.flush();

        qTable = doc;
    } else {
      Serial.println("Unknown message structure");
      Serial.flush();
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
    auto nodes = mesh.getNodeList(false);
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

    if (isFirstTime) {
        Serial.println("Populating QTable for the first time");
        StaticJsonDocument<1024> doc;
        JsonObject q_table = doc.createNestedObject("q_table");

        // Asegurarse de que cada estado tiene un objeto dentro de la Q-table
        for (auto &&id : nodes) {
            JsonObject stateObj = q_table.createNestedObject(String(state)); // Cambio aquí
            for (auto &&action : nodes) {
                stateObj[String(action)] = 0.0; // Inicializar valores Q para cada acción
            }
        }

        String jsonString;
        serializeJsonPretty(doc, jsonString);
        Serial.println(jsonString);
        qTable = doc;
        isFirstTime = false;
    } else {
        Serial.println("");
        Serial.println("Updating QTable with missing neighbors");
        JsonObject q_table = qTable["q_table"];

        // Asegurarse de que el estado actual tiene un objeto dentro de la Q-table
        if (!q_table.containsKey(String(state))) {
            JsonObject stateObj = q_table.createNestedObject(String(state));
            for (auto &&action : nodes) {
                stateObj[String(action)] = 0.0; // Inicializar valores Q para cada acción
            }
        }

        // Verificar si los vecinos están en la Q-table para el estado actual
        JsonObject stateObj = q_table[String(state)];
        for (auto &&id : nodes) {
            if (!stateObj.containsKey(String(id))) {
                Serial.print("Adding missing action for neighbor ");
                Serial.println(String(id));
                stateObj[String(id)] = 0.0; // Inicializar valor Q para la acción faltante
            }
        }

        String jsonString;
        serializeJsonPretty(qTable, jsonString);
        Serial.println(jsonString);
    }

    Serial.flush();

    Serial.print("Neighbors for state ");
    Serial.print(String(state));
    Serial.print(" are ");
    Serial.println(nodesStr);
    Serial.flush();

    if (random() < epsilon) {
        // Explorar
        int action_index = random(0, num_neighbors - 1);
        int action = neighbors[action_index];
        Serial.println("Exploring action");
        Serial.println(action);
        Serial.flush();
        return action;
    } else {
        // Explotar: elegir la acción con el valor Q más alto
        JsonObject q_table = qTable["q_table"];
        if (!q_table.containsKey(String(state))) {
            Serial.print("No Q-values found for state ");
            Serial.println(String(state));
            Serial.flush();
            return -1;
        }

        // Asegurarse de que todos los vecinos están en la Q-table para este estado
        JsonObject actions = q_table[String(state)];
        for (auto &&neighbor : neighbors) {
            if (!actions.containsKey(String(neighbor))) {
                Serial.print("Adding missing action for neighbor ");
                Serial.println(neighbor);
                actions[String(neighbor)] = 0.0; // Inicializar Q-value para la acción faltante
            }
        }

        Serial.println("Available actions are:");
        String jsonActions;
        serializeJsonPretty(actions, jsonActions);
        Serial.println(jsonActions);

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

void sendMessageToNextHop(uint32_t next_hop, String &msg) {
  Serial.print("Sending message to next hop ");
  Serial.print(next_hop);
  Serial.print(": ");
  Serial.println(msg);
  Serial.flush();
  mesh.sendSingle(next_hop, msg);
}

void newConnectionCallback(uint32_t nodeId) {
  Serial.print("--> startHere: New Connection, nodeId = ");
  Serial.println(nodeId);
  sendMessage();
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

void setup() {
  Serial.begin(9600);
  Serial.println();
  Serial.println();
  Serial.println();
  Serial.println();
  Serial.println("Initializing SENDER NODE");

  for (uint8_t t = 4; t > 0; t--) {
    Serial.printf("[SETUP] WAIT %d...\n", t);
    Serial.flush();
    delay(1000);
  }

  dht.begin();
  mesh.setDebugMsgTypes(ERROR | STARTUP);  // set before init() so that you can see startup messages

  mesh.init(MESH_PREFIX, MESH_PASSWORD, &userScheduler, MESH_PORT);
  //mesh.init(MESH_PREFIX, MESH_PASSWORD, MESH_PORT);
  //mesh.init( MESH_PREFIX, MESH_PASSWORD, MESH_PORT, WIFI_AP_STA, 6 );

  mesh.onReceive(&receivedCallback);
  mesh.onNewConnection(&newConnectionCallback);
  mesh.onChangedConnections(&changedConnectionCallback);
  mesh.onNodeTimeAdjusted(&nodeTimeAdjustedCallback);

  // This node and all other nodes should ideally know the mesh contains a root, so call this on all nodes
  mesh.setContainsRoot(true);

  Serial.println("Mesh initialized successfully");

  userScheduler.addTask(taskSendMessage);
  taskSendMessage.enable();
}

void loop() {
  // it will run the user scheduler as well
  mesh.update();
}
