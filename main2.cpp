

// TODO: Configure your WiFi here
#define WIFI_SSID "I-Soft"
#define WIFI_PSK  "i-soft@2023"

// Include certificate data (see note above)
#include "cert.h"
#include "private_key.h"


#include <WiFi.h>
#include <SPIFFS.h>
#include <ArduinoJson.h>
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <HTTPBodyParser.hpp>
#include <HTTPMultipartBodyParser.hpp>
#include <HTTPURLEncodedBodyParser.hpp>

// The HTTPS Server comes in a separate namespace. For easier use, include it here.
using namespace httpsserver;

// Create an SSL certificate object from the files included above
SSLCert cert = SSLCert(
  cert_der, cert_der_len,
  key_der, key_der_len
);

// First, we create the HTTPSServer with the certificate created above
HTTPSServer secureServer = HTTPSServer(&cert);

// Additionally, we create an HTTPServer for unencrypted traffic
HTTPServer insecureServer = HTTPServer();

const char* adminUser = "admin";
const char* adminPass = "123456";
const char* sessionFile = "/sessions.json";
const unsigned long SESSION_TIMEOUT = 5 * 60 * 1000; // 5 phút


// Hàm tạo token ngẫu nhiên
String generateToken() {
  char buf[33];
  for (int i = 0; i < 32; i++) buf[i] = "0123456789ABCDEF"[random(16)];
  buf[32] = 0;
  return String(buf);
}

// Hàm tạo deviceID dựa trên MAC
String getDeviceID() {
  uint64_t chipid = ESP.getEfuseMac();
  char id[13];
  sprintf(id, "%04X%08X", (uint16_t)(chipid>>32), (uint32_t)chipid);
  return String(id);
}

// Đọc file session
DynamicJsonDocument readSessions() {
  DynamicJsonDocument doc(2048);
  File file = SPIFFS.open(sessionFile, "r");
  if (!file) return doc;
  DeserializationError err = deserializeJson(doc, file);
  file.close();
  if (err) doc.clear();
  return doc;
}

// Ghi file session
void writeSessions(DynamicJsonDocument& doc) {
  File file = SPIFFS.open(sessionFile, "w");
  serializeJson(doc, file);
  file.close();
}

// Kiểm tra session token
bool checkSession(String deviceID, String token) {
  DynamicJsonDocument doc = readSessions();
  if (!doc.containsKey(deviceID)) return false;
  JsonObject session = doc[deviceID];
  if (session["token"] != token) return false;
  unsigned long now = millis();
  unsigned long last = session["last"];
  if (now - last > SESSION_TIMEOUT) return false;
  // Cập nhật lại thời gian
  session["last"] = now;
  writeSessions(doc);
  return true;
}

// Lưu session mới
void saveSession(String deviceID, String token) {
  DynamicJsonDocument doc = readSessions();
  JsonObject session = doc.createNestedObject(deviceID);
  session["token"] = token;
  session["last"] = millis();
  writeSessions(doc);
}

// Trang login
const char* loginPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head><title>Đăng nhập Admin</title></head>
<body>
  <form id="loginForm">
    <label>Username: <input name="username" id="username"></label><br>
    <label>Password: <input name="password" id="password" type="password"></label><br>
    <input type="submit" value="Đăng nhập">
  </form>
  <script>
  document.getElementById('loginForm').onsubmit = function(e) {
    e.preventDefault();
    fetch('/login', {
      method: 'POST',
      headers: {'Content-Type': 'application/json'},
      body: JSON.stringify({
        username: document.getElementById('username').value,
        password: document.getElementById('password').value
      })
    }).then(resp => {
      if (resp.redirected) {
        window.location = resp.url;
      } else {
        resp.text().then(html => document.body.innerHTML = html);
      }
    });
  };
  </script>
</body>
</html>
)rawliteral";

// Trang chính
const char* mainPage = R"rawliteral(
<!DOCTYPE html>
<html>
<head><title>Trang quản trị</title></head>
<body>
  <h1>Chào mừng, admin!</h1>
  <p>DeviceID: %DEVICEID%</p>
</body>
</html>
)rawliteral";


void handleRoot(HTTPRequest *req, HTTPResponse *res) {
  String deviceID = getDeviceID();
  String token = "";
  if (req->getHeader("Cookie").length() > 0) {
    String cookie = String(req->getHeader("Cookie").c_str());
    int idx = cookie.indexOf("token=");
    if (idx >= 0) token = cookie.substring(idx + 6, idx + 38);
  }
  if (checkSession(deviceID, token)) {
    String page = mainPage;
    page.replace("%DEVICEID%", deviceID);
    res->setHeader("Content-Type", "text/html");
    res->println(page);
  } else {
    res->setHeader("Content-Type", "text/html");
    res->println(loginPage);
  }
}

void handleLogin(HTTPRequest *req, HTTPResponse *res) {
  HTTPBodyParser *parser;
  std::string contentType = req->getHeader("Content-Type");
  size_t semicolonPos = contentType.find(";");
  if (semicolonPos != std::string::npos) {
    contentType = contentType.substr(0, semicolonPos);
  }
  if (contentType == "application/json") {
    parser = new HTTPMultipartBodyParser(req);
  } else {
    Serial.printf("Unknown POST Content-Type: %s\n", contentType.c_str());
    return;
  }

  // We expect a JSON body with "username" and "password"
  String username, password;
  while (parser->nextField()) {
    std::string name = parser->getFieldName();
    if (name == "username") {
      username = parser->getFieldFilename();
    } else if (name == "password") {
      password = parser->getFieldFilename();
    }
  }


  String deviceID = getDeviceID();
  if (username == adminUser && password == adminPass) {
    String token = generateToken();
    saveSession(deviceID, token);
    res->setHeader("Set-Cookie", std::string("token=") + token.c_str() + "; Path=/; HttpOnly");
    res->setStatusCode(302);
    res->setHeader("Location", "/");
    res->println("");
  } else {
    res->setHeader("Content-Type", "text/html");
    res->println(loginPage);
  }
}

void handle404(HTTPRequest * req, HTTPResponse * res) {
  req->discardRequestBody();
  res->setStatusCode(404);
  res->setStatusText("Not Found");
  res->setHeader("Content-Type", "text/html");
  res->println("<!DOCTYPE html>");
  res->println("<html>");
  res->println("<head><title>Not Found</title></head>");
  res->println("<body><h1>404 Not Found</h1><p>The requested resource was not found on this server.</p></body>");
  res->println("</html>");
}


void setup() {
  // For logging
  Serial.begin(115200);

  // Connect to WiFinhưng
  Serial.println("Setting up WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PSK);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.print("Connected. IP=");
  Serial.println(WiFi.localIP());

  if (!SPIFFS.begin(true)) {
    Serial.println("SPIFFS lỗi!");
    return;
  }

  // For every resource available on the server, we need to create a ResourceNode
  // The ResourceNode links URL and HTTP method to a handler function
  ResourceNode * nodeRoot = new ResourceNode("/", "GET", &handleRoot);
  ResourceNode * node404  = new ResourceNode("", "GET", &handle404);
  ResourceNode *nodeLogin = new ResourceNode("/login", "POST", &handleLogin);

  // Add the root node to the servers. We can use the same ResourceNode on multiple
  // servers (you could also run multiple HTTPS servers)
  secureServer.registerNode(nodeRoot);
  insecureServer.registerNode(nodeRoot);

  secureServer.registerNode(nodeLogin);
  insecureServer.registerNode(nodeLogin);
  
  secureServer.setDefaultNode(node404);
  insecureServer.setDefaultNode(node404);

  Serial.println("Starting HTTPS server...");
  secureServer.start();
  Serial.println("Starting HTTP server...");
  insecureServer.start();
  if (secureServer.isRunning() && insecureServer.isRunning()) {
    Serial.println("Servers ready.");
  }
}

void loop() {
  // We need to call both loop functions here
  secureServer.loop();
  insecureServer.loop();

  // Other code would go here...
  delay(1);
}

