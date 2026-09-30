#include <WiFi.h>
#include <WebServer.h>
#include <esp_system.h>

// ---------------------------------------------------------------------------
// Configuration: change these values before deploying the ESP32.
// ---------------------------------------------------------------------------
constexpr char AP_SSID[] = "SAFEROUTE-NET";
constexpr char AP_PASSWORD[] = "12345678";  // At least 8 characters.
constexpr char STAFF_API_KEY[] = "CHANGE-THIS-STAFF-KEY";

constexpr uint16_t HTTP_PORT = 80;
constexpr size_t MAX_REQUEST_BYTES = 2048;
constexpr size_t MAX_ZONE_LENGTH = 64;
constexpr size_t MAX_HAZARD_ID_LENGTH = 64;
constexpr size_t MAX_ACTIVE_HAZARDS = 16;
constexpr size_t MAX_SHOOTER_PATH_POINTS = 16;

WebServer server(HTTP_PORT);

struct EmergencyReport {
  String hazardId;
  String type;
  String zone;
  double x = 0;
  double y = 0;
  double radius = 0;
  String buildingId;
  int floor = 1;
  bool hasExactLocation = false;
  bool moving = false;
  double pathX[MAX_SHOOTER_PATH_POINTS] = {};
  double pathY[MAX_SHOOTER_PATH_POINTS] = {};
  uint8_t pathCount = 0;
  uint32_t updatedMs = 0;
};

EmergencyReport activeReports[MAX_ACTIVE_HAZARDS];
size_t activeReportCount = 0;
uint32_t statusRevision = 0;
uint32_t serverSessionId = 0;

const char *COLLECTED_HEADERS[] = {
  "X-SAFEROUTE-KEY",
};

void addCommonHeaders() {
  server.sendHeader("Access-Control-Allow-Origin", "*");
  server.sendHeader("Access-Control-Allow-Methods", "GET,POST,OPTIONS");
  server.sendHeader(
    "Access-Control-Allow-Headers",
    "Content-Type,X-SAFEROUTE-KEY"
  );
  server.sendHeader("Cache-Control", "no-store");
}

String jsonEscape(const String &value) {
  String escaped;
  escaped.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value.charAt(i);
    if (ch == '\\' || ch == '"') {
      escaped += '\\';
      escaped += ch;
    } else if (ch == '\n') {
      escaped += "\\n";
    } else if (ch == '\r') {
      escaped += "\\r";
    } else {
      escaped += ch;
    }
  }
  return escaped;
}

void sendJson(int statusCode, const String &body) {
  addCommonHeaders();
  server.send(statusCode, "application/json", body);
}

void sendError(int statusCode, const String &message) {
  Serial.printf("HTTP %d rejected: %s\n", statusCode, message.c_str());
  sendJson(
    statusCode,
    "{\"ok\":false,\"error\":\"" + jsonEscape(message) + "\"}"
  );
}

String statusJson() {
  String json;
  json.reserve(80 + activeReportCount * 700);
  json = "{\"ok\":true,\"server_id\":\"" + String(serverSessionId, HEX) +
         "\",\"revision\":" + String(statusRevision) + ",\"hazards\":[";
  for (size_t i = 0; i < activeReportCount; ++i) {
    if (i > 0) json += ',';
    const EmergencyReport &report = activeReports[i];
    json += "{\"active\":true";
    json += ",\"hazard_id\":\"" + jsonEscape(report.hazardId) + "\"";
    json += ",\"type\":\"" + jsonEscape(report.type) + "\"";
    json += ",\"zone\":\"" + jsonEscape(report.zone) + "\"";
    json += ",\"x\":" + String(report.x, 3);
    json += ",\"y\":" + String(report.y, 3);
    json += ",\"radius\":" + String(report.radius, 3);
    json += ",\"building_id\":\"" + jsonEscape(report.buildingId) + "\"";
    json += ",\"floor\":" + String(report.floor);
    json += ",\"has_exact_location\":";
    json += report.hasExactLocation ? "true" : "false";
    json += ",\"moving\":";
    json += report.moving ? "true" : "false";
    json += ",\"path\":[";
    for (uint8_t point = 0; point < report.pathCount; ++point) {
      if (point > 0) json += ',';
      json += '[';
      json += String(report.pathX[point], 3);
      json += ',';
      json += String(report.pathY[point], 3);
      json += ']';
    }
    json += ']';
    json += ",\"updated_ms\":" + String(report.updatedMs);
    json += '}';
  }
  json += "]}";
  return json;
}

bool hasStaffAuthorization() {
  return server.hasHeader("X-SAFEROUTE-KEY") &&
         server.header("X-SAFEROUTE-KEY") == STAFF_API_KEY;
}

bool readJsonString(const String &json, const char *key, String &value) {
  const String token = "\"" + String(key) + "\"";
  int cursor = json.indexOf(token);
  if (cursor < 0) return false;

  cursor = json.indexOf(':', cursor + token.length());
  if (cursor < 0) return false;
  cursor = json.indexOf('"', cursor + 1);
  if (cursor < 0) return false;

  const int end = json.indexOf('"', cursor + 1);
  if (end < 0) return false;

  value = json.substring(cursor + 1, end);
  return true;
}

bool readJsonBool(const String &json, const char *key, bool &value) {
  const String token = "\"" + String(key) + "\"";
  int cursor = json.indexOf(token);
  if (cursor < 0) return false;

  cursor = json.indexOf(':', cursor + token.length());
  if (cursor < 0) return false;
  ++cursor;
  while (cursor < static_cast<int>(json.length()) &&
         isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
    ++cursor;
  }

  if (json.substring(cursor, cursor + 4) == "true") {
    value = true;
    return true;
  }
  if (json.substring(cursor, cursor + 5) == "false") {
    value = false;
    return true;
  }
  return false;
}

bool readJsonNumber(const String &json, const char *key, double &value) {
  const String token = "\"" + String(key) + "\"";
  int cursor = json.indexOf(token);
  if (cursor < 0) return false;

  cursor = json.indexOf(':', cursor + token.length());
  if (cursor < 0) return false;
  ++cursor;
  while (cursor < static_cast<int>(json.length()) &&
         isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
    ++cursor;
  }

  const int start = cursor;
  while (cursor < static_cast<int>(json.length())) {
    const char ch = json.charAt(cursor);
    if (!(isdigit(static_cast<unsigned char>(ch)) ||
          ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E')) {
      break;
    }
    ++cursor;
  }
  if (cursor == start) return false;

  const String numberText = json.substring(start, cursor);
  char *end = nullptr;
  value = strtod(numberText.c_str(), &end);
  return end != numberText.c_str() && *end == '\0' && isfinite(value);
}

bool parseJsonNumberAt(const String &json, int &cursor, double &value) {
  while (cursor < static_cast<int>(json.length()) &&
         isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
    ++cursor;
  }
  const int start = cursor;
  while (cursor < static_cast<int>(json.length())) {
    const char ch = json.charAt(cursor);
    if (!(isdigit(static_cast<unsigned char>(ch)) ||
          ch == '-' || ch == '+' || ch == '.' || ch == 'e' || ch == 'E')) {
      break;
    }
    ++cursor;
  }
  if (cursor == start) return false;

  const String numberText = json.substring(start, cursor);
  char *end = nullptr;
  value = strtod(numberText.c_str(), &end);
  return end != numberText.c_str() && *end == '\0' && isfinite(value);
}

// Reads a bounded JSON path in the form [[x,y], ...]. Missing path is
// accepted for older app versions and leaves an existing route unchanged.
bool readJsonPath(
  const String &json,
  bool &present,
  double *pathX,
  double *pathY,
  uint8_t &pathCount
) {
  const String token = "\"path\"";
  int cursor = json.indexOf(token);
  present = cursor >= 0;
  pathCount = 0;
  if (!present) return true;

  cursor = json.indexOf(':', cursor + token.length());
  if (cursor < 0) return false;
  ++cursor;
  while (cursor < static_cast<int>(json.length()) &&
         isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
    ++cursor;
  }
  if (cursor >= static_cast<int>(json.length()) || json.charAt(cursor) != '[') {
    return false;
  }
  ++cursor;

  while (cursor < static_cast<int>(json.length())) {
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
      ++cursor;
    }
    if (cursor >= static_cast<int>(json.length())) return false;
    if (json.charAt(cursor) == ']') return true;
    if (pathCount >= MAX_SHOOTER_PATH_POINTS || json.charAt(cursor) != '[') {
      return false;
    }
    ++cursor;
    if (!parseJsonNumberAt(json, cursor, pathX[pathCount])) return false;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
      ++cursor;
    }
    if (cursor >= static_cast<int>(json.length()) || json.charAt(cursor++) != ',') {
      return false;
    }
    if (!parseJsonNumberAt(json, cursor, pathY[pathCount])) return false;
    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
      ++cursor;
    }
    if (cursor >= static_cast<int>(json.length()) || json.charAt(cursor++) != ']') {
      return false;
    }
    ++pathCount;

    while (cursor < static_cast<int>(json.length()) &&
           isspace(static_cast<unsigned char>(json.charAt(cursor)))) {
      ++cursor;
    }
    if (cursor >= static_cast<int>(json.length())) return false;
    if (json.charAt(cursor) == ']') return true;
    if (json.charAt(cursor++) != ',') return false;
  }
  return false;
}

bool validHazardType(const String &type) {
  return type == "fire" ||
         type == "blocked_path" ||
         type == "active_threat";
}

bool validZone(const String &zone) {
  if (zone.isEmpty() || zone.length() > MAX_ZONE_LENGTH) return false;
  for (size_t i = 0; i < zone.length(); ++i) {
    const char ch = zone.charAt(i);
    if (!(isalnum(static_cast<unsigned char>(ch)) || ch == '_' || ch == '-')) {
      return false;
    }
  }
  return true;
}

bool validIdentifier(const String &value, size_t maxLength) {
  if (value.isEmpty() || value.length() > maxLength) return false;
  for (size_t i = 0; i < value.length(); ++i) {
    const char ch = value.charAt(i);
    if (!(isalnum(static_cast<unsigned char>(ch)) ||
          ch == '_' || ch == '-' || ch == ':' || ch == '.')) {
      return false;
    }
  }
  return true;
}

int findReportIndex(const String &hazardId) {
  for (size_t i = 0; i < activeReportCount; ++i) {
    if (activeReports[i].hazardId == hazardId) return static_cast<int>(i);
  }
  return -1;
}

bool removeReport(const String &hazardId) {
  const int index = findReportIndex(hazardId);
  if (index < 0) return false;
  for (size_t i = static_cast<size_t>(index); i + 1 < activeReportCount; ++i) {
    activeReports[i] = activeReports[i + 1];
  }
  activeReports[activeReportCount - 1] = EmergencyReport();
  --activeReportCount;
  ++statusRevision;
  return true;
}

void handleStatus() {
  sendJson(200, statusJson());
}

void handleReport() {
  if (!hasStaffAuthorization()) {
    sendError(401, "Missing or invalid staff API key");
    return;
  }

  const String body = server.arg("plain");
  if (body.isEmpty() || body.length() > MAX_REQUEST_BYTES) {
    sendError(400, "Request body is empty or too large");
    return;
  }

  bool active = false;
  bool hasActive = readJsonBool(body, "active", active);
  if (!hasActive) {
    String status;
    if (readJsonString(body, "status", status)) {
      status.toLowerCase();
      if (status == "active") {
        active = true;
        hasActive = true;
      } else if (status == "clear") {
        active = false;
        hasActive = true;
      }
    }
  }

  if (!hasActive) {
    sendError(400, "Provide active:true/false or status:active/clear");
    return;
  }

  String hazardId;
  if (!readJsonString(body, "hazard_id", hazardId) ||
      !validIdentifier(hazardId, MAX_HAZARD_ID_LENGTH)) {
    sendError(400, "hazard_id is missing or invalid");
    return;
  }

  if (!active) {
    removeReport(hazardId);
    Serial.printf("Emergency report cleared: id=%s\n", hazardId.c_str());
    sendJson(200, statusJson());
    return;
  }

  String type;
  String zone;
  if (!readJsonString(body, "type", type) || !validHazardType(type)) {
    sendError(400, "type must be fire, blocked_path, or active_threat");
    return;
  }

  const bool hasZone = readJsonString(body, "zone", zone) && validZone(zone);
  double x = 0;
  double y = 0;
  double radius = 0;
  double floorNumber = 1;
  const bool hasExactLocation =
    readJsonNumber(body, "x", x) &&
    readJsonNumber(body, "y", y) &&
    readJsonNumber(body, "radius", radius) &&
    readJsonNumber(body, "floor", floorNumber);

  if (!hasZone && !hasExactLocation) {
    sendError(400, "Provide a valid zone or exact x/y/radius/floor");
    return;
  }
  if (hasExactLocation &&
      (x < 0 || y < 0 || radius <= 0 || floorNumber < 1 || floorNumber > 999)) {
    sendError(400, "Exact hazard location values are out of range");
    return;
  }

  String buildingId;
  readJsonString(body, "building_id", buildingId);
  if (!buildingId.isEmpty() && !validIdentifier(buildingId, MAX_HAZARD_ID_LENGTH)) {
    sendError(400, "building_id is invalid");
    return;
  }

  bool moving = false;
  readJsonBool(body, "moving", moving);

  double incomingPathX[MAX_SHOOTER_PATH_POINTS] = {};
  double incomingPathY[MAX_SHOOTER_PATH_POINTS] = {};
  uint8_t incomingPathCount = 0;
  bool pathPresent = false;
  if (!readJsonPath(
        body,
        pathPresent,
        incomingPathX,
        incomingPathY,
        incomingPathCount
      )) {
    sendError(400, "path must contain up to 16 [x,y] points");
    return;
  }

  int index = findReportIndex(hazardId);
  if (index < 0) {
    if (activeReportCount >= MAX_ACTIVE_HAZARDS) {
      sendError(507, "Maximum active shared hazards reached");
      return;
    }
    index = static_cast<int>(activeReportCount++);
  }

  EmergencyReport &report = activeReports[index];
  report.hazardId = hazardId;
  report.type = type;
  report.zone = hasZone ? zone : "";
  report.x = hasExactLocation ? x : 0;
  report.y = hasExactLocation ? y : 0;
  report.radius = hasExactLocation ? radius : 0;
  report.buildingId = hasExactLocation ? buildingId : "";
  report.floor = hasExactLocation ? static_cast<int>(floorNumber) : 1;
  report.hasExactLocation = hasExactLocation;
  report.moving = moving;
  if (pathPresent) {
    report.pathCount = incomingPathCount;
    for (uint8_t point = 0; point < incomingPathCount; ++point) {
      report.pathX[point] = incomingPathX[point];
      report.pathY[point] = incomingPathY[point];
    }
  }
  report.updatedMs = millis();
  ++statusRevision;

  Serial.printf(
    "Emergency report: id=%s type=%s zone=%s revision=%lu\n",
    report.hazardId.c_str(),
    report.type.c_str(),
    report.zone.c_str(),
    static_cast<unsigned long>(statusRevision)
  );
  sendJson(200, statusJson());
}

void handleClear() {
  if (!hasStaffAuthorization()) {
    sendError(401, "Missing or invalid staff API key");
    return;
  }

  const String body = server.arg("plain");
  String requestedHazardId;
  if (body.isEmpty() || !readJsonString(body, "hazard_id", requestedHazardId) ||
      !validIdentifier(requestedHazardId, MAX_HAZARD_ID_LENGTH)) {
    sendError(400, "hazard_id is missing or invalid");
    return;
  }

  removeReport(requestedHazardId);
  Serial.printf("Emergency report cleared: id=%s\n", requestedHazardId.c_str());
  sendJson(200, statusJson());
}

void handleOptions() {
  addCommonHeaders();
  server.send(204, "text/plain", "");
}

void handleNotFound() {
  sendError(404, "Endpoint not found");
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println();
  Serial.println("Starting SAFEROUTE local emergency server...");
  serverSessionId = esp_random();

  WiFi.mode(WIFI_AP);
  const IPAddress localIp(192, 168, 4, 1);
  const IPAddress gateway(192, 168, 4, 1);
  const IPAddress subnet(255, 255, 255, 0);
  WiFi.softAPConfig(localIp, gateway, subnet);

  if (!WiFi.softAP(AP_SSID, AP_PASSWORD)) {
    Serial.println("ERROR: Could not start SAFEROUTE-NET");
    return;
  }

  server.collectHeaders(COLLECTED_HEADERS, 1);
  server.on("/status", HTTP_GET, handleStatus);
  server.on("/report", HTTP_POST, handleReport);
  server.on("/clear", HTTP_POST, handleClear);
  server.on("/status", HTTP_OPTIONS, handleOptions);
  server.on("/report", HTTP_OPTIONS, handleOptions);
  server.on("/clear", HTTP_OPTIONS, handleOptions);
  server.onNotFound(handleNotFound);
  server.begin();

  Serial.printf("Wi-Fi: %s\n", AP_SSID);
  Serial.print("Server: http://");
  Serial.println(WiFi.softAPIP());
  Serial.println("GET /status is ready for SAFEROUTE user apps");
  Serial.println("POST /report and /clear require X-SAFEROUTE-KEY");
}

void loop() {
  server.handleClient();
  delay(2);
}
