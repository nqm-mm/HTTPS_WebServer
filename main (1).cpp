/**
 * Example for the ESP32 HTTP(S) Webserver
 *
 * IMPORTANT NOTE:
 * This example is a bit more complex than the other ones, so be careful to 
 * follow all steps.
 * 
 * Make sure to check out the more basic examples like Static-Page to understand
 * the fundamental principles of the API before proceeding with this sketch.
 * 
 * To run this script, you need to
 *  1) Enter your WiFi SSID and PSK below this comment
 *  2) Install the SPIFFS File uploader into your Arduino IDE to be able to
 *     upload static data to the webserver.
 *     Follow the instructions at:
 *     https://github.com/me-no-dev/arduino-esp32fs-plugin
 *  3) Upload the static files from the data/ directory of the example to your
 *     module's SPIFFs by using "ESP32 Sketch Data Upload" from the tools menu.
 *     If you face any problems, read the description of the libraray mentioned
 *     above.
 *     Note: If mounting SPIFFS fails, the script will wait for a serial connection
 *     (open your serial monitor!) and ask if it should format the SPIFFS partition.
 *     You may need this before uploading the data
 *     Note: Make sure to select a partition layout that allows for SPIFFS in the
 *     boards menu
 *  4) Have the ArduinoJSON library installed and available. (Tested with Version 5.13.4)
 *     You'll find it at:
 *     https://arduinojson.org/
 *
 * This script will install an HTTPS Server on your ESP32 with the following
 * functionalities:
 *  - Serve static files from the SPIFFS's data/public directory
 *  - Provide a REST API at /api to receive the asynchronous http requests
 *    - /api/uptime provides access to the current system uptime
 *    - /api/events allows to register or delete events to turn PINs on/off
 *      at certain times.
 *  - Use Arduino JSON for body parsing and generation of responses.
 *  - The certificate is generated on first run and stored to the SPIFFS in
 *    the cert directory (so that the client cannot retrieve the private key)
 */

/**
 * Ví dụ sử dụng các function API:
 * 
 * 1. Lấy uptime:
 *    - Gửi GET tới /api/uptime
 *    - Kết quả trả về: {"uptime": 12345}
 * 
 * 2. Lấy danh sách sự kiện:
 *    - Gửi GET tới /api/events
 *    - Kết quả trả về: [{"gpio":25,"state":1,"time":1710000000,"id":0}, ...]
 * 
 * 3. Thêm sự kiện mới:
 * 
 *    - Gửi POST tới /api/events với body JSON:
 *      {"gpio":25,"state":1,"time":1710000000}
 *    - Kết quả trả về: {"gpio":25,"state":1,"time":1710000000,"id":0}
 * 
 * 4. Xóa sự kiện:
 *    - Gửi DELETE tới /api/events/0
 *    - Kết quả trả về: HTTP 204 No Content
 * 
 * 5. Upload file:
 *    - Gửi POST tới /api/upload với multipart/form-data, trường "file" là file cần upload.
 *    - Kết quả trả về: {"success":true,"filename":"tenfile.txt"}
 * 
 * 6. Lấy danh sách file:
 *    - Gửi GET tới /api/fs/list
 *    - Kết quả trả về: [{"name":"/public/abc.txt","size":123,"isDir":false}, ...]
 * 
 * 7. Xóa file:
 *    - Gửi DELETE tới /api/fs/file/abc.txt
 *    - Kết quả trả về: HTTP 204 No Content
 * 
 * 8. Xem trang upload:
 *    - Truy cập GET /api/upload-page trên trình duyệt để xem giao diện upload file.
 */
// TODO: Configure your WiFi here
#define WIFI_SSID "I-Soft"
#define WIFI_PSK  "i-soft@2023"

// We will use wifi
#include <WiFi.h>

// We will use SPIFFS and FS
#include <LittleFS.h>
#include <FS.h>

// We use JSON as data format. Make sure to have the lib available
#include <ArduinoJson-v5.13.4.h>

// Working with c++ strings
#include <string>

// Define the name of the directory for public files in the SPIFFS parition
#define DIR_PUBLIC "/public"

// We need to specify some content-type mapping, so the resources get delivered with the
// right content type and are displayed correctly in the browser
char contentTypes[][2][32] = {
  {".html", "text/html"},
  {".css",  "text/css"},
  {".js",   "application/javascript"},
  {".json", "application/json"},
  {".png",  "image/png"},
  {".jpg",  "image/jpg"},
  {"", ""}
};

// Includes for the server
#include <HTTPSServer.hpp>
#include <SSLCert.hpp>
#include <HTTPRequest.hpp>
#include <HTTPResponse.hpp>
#include <util.hpp>

// The HTTPS Server comes in a separate namespace. For easier use, include it here.
using namespace httpsserver;


/**
 * Simple HTML page for uploading a file to the ESP32 server
 * Save this as /public/upload.html in your LittleFS data folder
 */
const char upload_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Upload File</title>
</head>
<body>
  <h2>Upload File to ESP32</h2>
  <form id="uploadForm" enctype="multipart/form-data" method="post" action="/api/upload">
    <input type="file" name="file" required>
    <button type="submit">Upload</button>
  </form>
  <div id="result"></div>
  <div>
    <label for="folderSelect">Select folder:</label>
    <select id="folderSelect" onchange="listFiles()">
      <option value="/public">/public</option>
    </select>
    <button onclick="listFiles()">Refresh File List</button>
  </div>
  <div id="listFiles">
    <h3>Files</h3>
    <ul id="fileList"></ul>
  </div>
  <div id="Usege">
    <h3>Usage</h3>
    <p id="memoryUsage"></p>
  </div>
  <script>
    document.addEventListener('DOMContentLoaded', function() {
      listFiles();
      getMemoryUsage();
    });

    document.getElementById('uploadForm').onsubmit = async function(e) {
      e.preventDefault();
      const form = e.target;
      const data = new FormData(form);
      const resultDiv = document.getElementById('result');
      resultDiv.textContent = "Uploading...";
      try {
        const res = await fetch(form.action, {
          method: 'POST',
          body: data
        });
        const text = await res.text();
        resultDiv.textContent = text;
        listFiles();
      } catch (err) {
        resultDiv.textContent = "Upload failed: " + err;
      }
    };

    async function listFiles() {
      const fileList = document.getElementById('fileList');
      fileList.innerHTML = '';
      const folder = document.getElementById('folderSelect').value;
      try {
        const res = await fetch('/api/fs/list?dir=' + folder);
        if (!res.ok) throw new Error('Network response was not ok');
        const files = await res.json();
        files.forEach(file => {
          const li = document.createElement('li');
          li.textContent = `${file.name} (${file.size} bytes)`;
          if (file.isDir) {
            const btn = document.createElement('button');
            btn.textContent = 'Open';
            btn.onclick = function() {
              setFolder(file.name);
            };
            li.appendChild(btn);
          } else {
            const link = document.createElement('a');
            link.href = file.name;
            link.textContent = ' [Download]';
            link.target = '_blank';
            li.appendChild(link);
          }
          fileList.appendChild(li);
        });
      } catch (err) {
        console.error('Error fetching file list:', err);
      }
    }
        // Always add /public if not present
        if (![...select.options].some(o => o.value === 'public')) {
          const opt = document.createElement('option');
          opt.value = 'public';
          opt.textContent = '/public';
          select.appendChild(opt);
          opt.value = '';
          opt.textContent = '/';
          select.appendChild(opt);
        }
      } catch (err) {
        console.error('Error fetching root folders:', err);
      }
    }

    function setFolder(folder) {
      const select = document.getElementById('folderSelect');
      select.value = folder;
      listFiles();
    }

    async function getMemoryUsage() {
      try {
        const res = await fetch('/api/fs/usage');
        if (!res.ok) throw new Error('Network response was not ok');
        const data = await res.json();
        document.getElementById('memoryUsage').textContent = 
          `Total: ${data.totalBytes} bytes, Used: ${data.usedBytes} bytes, Free: ${data.freeBytes} bytes`;
      } catch (err) {
        console.error('Error fetching memory usage:', err);
      }
    }
  </script>
</body>
</html>
)rawliteral";



SSLCert * getCertificate();
void handleLittleFS(HTTPRequest * req, HTTPResponse * res);
void handleGetUptime(HTTPRequest * req, HTTPResponse * res);
void handleGetEvents(HTTPRequest * req, HTTPResponse * res);
void handlePostEvent(HTTPRequest * req, HTTPResponse * res);
void handleDeleteEvent(HTTPRequest * req, HTTPResponse * res);
void handleGetHistory(HTTPRequest * req, HTTPResponse * res);
//handleUploadFile
void handleUploadFile(HTTPRequest * req, HTTPResponse * res);
// We use the following struct to store GPIO events:
#define MAX_EVENTS 20
struct {
  // is this event used (events that have been run will be set to false)
  bool active;
  // when should it be run?
  unsigned long time;
  // which GPIO should be changed?
  int gpio;
  // and to which state?
  int state;
} events[MAX_EVENTS];

// We just create a reference to the server here. We cannot call the constructor unless
// we have initialized the SPIFFS and read or created the certificate
HTTPSServer * secureServer;

#include "WebApi.h"

void setup() {
  // For logging
  Serial.begin(115200);

  // Set the pins that we will use as output pins
  pinMode(13, OUTPUT);

  // Try to mount SPIFFS without formatting on failure
  if (!LittleFS.begin(false)) {
    // If SPIFFS does not work, we wait for serial connection...
    while(!Serial);
    delay(1000);

    // Ask to format SPIFFS using serial interface
    Serial.print("Mounting LittleFS failed. Try formatting? (y/n): ");
    while(!Serial.available());
    Serial.println();

    // If the user did not accept to try formatting SPIFFS or formatting failed:
    if (Serial.read() != 'y' || !LittleFS.begin(true)) {
      Serial.println("LittleFS not available. Stop.");
      while(true);
    }
    Serial.println("LittleFS has been formated.");
  }
  Serial.println("LittleFS has been mounted.");

  // Now that SPIFFS is ready, we can create or load the certificate
  SSLCert *cert = getCertificate();
  if (cert == NULL) {
    Serial.println("Could not load certificate. Stop.");
    while(true);
  }

  // Initialize event structure:
  for(int i = 0; i < MAX_EVENTS; i++) {
    events[i].active = false;
    events[i].gpio = 0;
    events[i].state = LOW;
    events[i].time = 0;
  }

  // Connect to WiFi
  Serial.println("Setting up WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PSK);
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
  }
  Serial.println("Connected to WiFi");
  Serial.print(" 🌐   IP address: ");
  Serial.println(WiFi.localIP());

  // Create the server with the certificate we loaded before
  secureServer = new HTTPSServer(cert);
  WebAPI();
  Serial.println("Starting server...");
  secureServer->start();
  if (secureServer->isRunning()) {
    Serial.println("Server ready.");
  }

  
}

#include <EEPROM.h>
long resetcounter = 0;
byte DoorState = 0; // 0: Closed, 1: Open, 2: Stopped

void loop() {
  // This call will let the server do its work
  secureServer->loop();

  // Here we handle the events
  unsigned long now = millis() / 1000;
  for (int i = 0; i < MAX_EVENTS; i++) {
    // Only handle active events:
    if (events[i].active) {
      // Only if the counter has recently been exceeded
      if (events[i].time < now) {
      // Apply the state change
      digitalWrite(events[i].gpio, events[i].state);

      // Deactivate the event so it doesn't fire again
      events[i].active = false;
      }
    }
  }
    static long lastLoopTime = millis();
    if (millis() - lastLoopTime >= 10000) { // Run every second
        lastLoopTime = millis();
        Serial.println("\n=================================================");
        Serial.println("DoorLocker loop running...");
        Serial.println("  🖥️   Reset Counter: " + String(resetcounter));
        Serial.println("  🚪   Door state: " + String(DoorState ? "Open" : "Closed"));
        Serial.println("  💾   Free Heap: " + String(ESP.getFreeHeap() / 1024) + "Kb");
        Serial.println("  🎞   Free PSRAM: " + String(ESP.getFreePsram() / 1024) + "Kb");
        Serial.println("  🌡️  Chip : " + String(temperatureRead()) + " °C");
        Serial.println("=================================================\n");
        // DoorStates();
    } 
  // Other code would go here...
  delay(1);
}

/**
 * This function will either read the certificate and private key from SPIFFS or
 * create a self-signed certificate and write it to SPIFFS for next boot
 */
SSLCert * getCertificate() {
  // Try to open key and cert file to see if they exist
  File keyFile = LittleFS.open("/key.der");
  File certFile = LittleFS.open("/cert.der");

  // If not, create them 
  if (!keyFile || !certFile || keyFile.size()==0 || certFile.size()==0) {
    Serial.println("No certificate found in LittleFS, generating a new one for you.");
    Serial.println("If you face a Guru Meditation, give the script another try (or two...).");
    Serial.println("This may take up to a minute, so please stand by :)");

    SSLCert * newCert = new SSLCert();
    // The part after the CN= is the domain that this certificate will match, in this
    // case, it's esp32.local.
    // However, as the certificate is self-signed, your browser won't trust the server
    // anyway.
    int res = createSelfSignedCert(*newCert, KEYSIZE_1024, "CN=esp32.local,O=acme,C=DE");
    if (res == 0) {
      // We now have a certificate. We store it on the SPIFFS to restore it on next boot.

      bool failure = false;
      // Private key
      keyFile = LittleFS.open("/key.der", FILE_WRITE);
      if (!keyFile || !keyFile.write(newCert->getPKData(), newCert->getPKLength())) {
        Serial.println("Could not write /key.der");
        failure = true;
      }
      if (keyFile) keyFile.close();

      // Certificate
      certFile = LittleFS.open("/cert.der", FILE_WRITE);
      if (!certFile || !certFile.write(newCert->getCertData(), newCert->getCertLength())) {
        Serial.println("Could not write /cert.der");
        failure = true;
      }
      if (certFile) certFile.close();

      if (failure) {
        Serial.println("Certificate could not be stored permanently, generating new certificate on reboot...");
      }

      return newCert;

    } else {
      // Certificate generation failed. Inform the user.
      Serial.println("An error occured during certificate generation.");
      Serial.print("Error code is 0x");
      Serial.println(res, HEX);
      Serial.println("You may have a look at SSLCert.h to find the reason for this error.");
      return NULL;
    }

	} else {
    Serial.println("Reading certificate from LittleFS.");

    // The files exist, so we can create a certificate based on them
    size_t keySize = keyFile.size();
    size_t certSize = certFile.size();

    uint8_t * keyBuffer = new uint8_t[keySize];
    if (keyBuffer == NULL) {
      Serial.println("Not enough memory to load privat key");
      return NULL;
    }
    uint8_t * certBuffer = new uint8_t[certSize];
    if (certBuffer == NULL) {
      delete[] keyBuffer;
      Serial.println("Not enough memory to load certificate");
      return NULL;
    }
    keyFile.read(keyBuffer, keySize);
    certFile.read(certBuffer, certSize);

    // Close the files
    keyFile.close();
    certFile.close();
    Serial.printf("Read %u bytes of certificate and %u bytes of key from LittleFS\n", certSize, keySize);
    return new SSLCert(certBuffer, certSize, keyBuffer, keySize);
  }
}

/**
 * This handler function will try to load the requested resource from SPIFFS's /public folder.
 * 
 * If the method is not GET, it will throw 405, if the file is not found, it will throw 404.
 */
void handleLittleFS(HTTPRequest * req, HTTPResponse * res) {
	
  // We only handle GET here
  if (req->getMethod() == "GET") {
    // Redirect / to /index.html
    std::string reqFile = req->getRequestString()=="/" ? "/index.html" : req->getRequestString();

    // Try to open the file
    std::string filename = std::string(DIR_PUBLIC) + reqFile;

    // Check if the file exists
    if (!LittleFS.exists(filename.c_str())) {
      // Send "404 Not Found" as response, as the file doesn't seem to exist
      res->setStatusCode(404);
      res->setStatusText("Not found");
      res->println("404 Not Found");
      return;
    }

    File file = LittleFS.open(filename.c_str());

    // Set length
    res->setHeader("Content-Length", httpsserver::intToString(file.size()));

    // Content-Type is guessed using the definition of the contentTypes-table defined above
    int cTypeIdx = 0;
    do {
      if(reqFile.rfind(contentTypes[cTypeIdx][0])!=std::string::npos) {
        res->setHeader("Content-Type", contentTypes[cTypeIdx][1]);
        break;
      }
      cTypeIdx+=1;
    } while(strlen(contentTypes[cTypeIdx][0])>0);

    // Read the file and write it to the response
    uint8_t buffer[256];
    size_t length = 0;
    do {
      length = file.read(buffer, 256);
      res->write(buffer, length);
    } while (length > 0);

    file.close();
  } else {
    // If there's any body, discard it
    req->discardRequestBody();
    // Send "405 Method not allowed" as response
    res->setStatusCode(405);
    res->setStatusText("Method not allowed");
    res->println("405 Method not allowed");
  }
}

/**
 * This function will return the uptime in seconds as JSON object:
 * {"uptime": 42}
 */
void handleGetUptime(HTTPRequest * req, HTTPResponse * res) {
  // Create a buffer of size 1 (pretty simple, we have just one key here)
  StaticJsonBuffer<JSON_OBJECT_SIZE(1)> jsonBuffer;
  // Create an object at the root
  JsonObject& obj = jsonBuffer.createObject();
  // Set the uptime key to the uptime in seconds
  obj["uptime"] = millis()/1000;
  // Set the content type of the response
  res->setHeader("Content-Type", "application/json");
  // As HTTPResponse implements the Print interface, this works fine. Just remember
  // to use *, as we only have a pointer to the HTTPResponse here:
  obj.printTo(*res);
}

/**
 * This handler will return a JSON array of currently active events for GET /api/events
 */
void handleGetEvents(HTTPRequest * req, HTTPResponse * res) {
  // We need to calculate the capacity of the json buffer
  int activeEvents = 0;
  for(int i = 0; i < MAX_EVENTS; i++) {
    if (events[i].active) activeEvents++;
  }

  // For each active event, we need 1 array element with 4 objects
  const size_t capacity = JSON_ARRAY_SIZE(activeEvents) + activeEvents * JSON_OBJECT_SIZE(4);

  // DynamicJsonBuffer is created on the heap instead of the stack
  DynamicJsonBuffer jsonBuffer(capacity);
  JsonArray& arr = jsonBuffer.createArray();
  for(int i = 0; i < MAX_EVENTS; i++) {
    if (events[i].active) {
      JsonObject& eventObj = arr.createNestedObject();
      eventObj["gpio"] = events[i].gpio;
      eventObj["state"] = events[i].state;
      eventObj["time"] = events[i].time;
      // Add the index to allow delete and post to identify the element
      eventObj["id"] = i;
    }
  }

  // Print to response
  res->setHeader("Content-Type", "application/json");
  arr.printTo(*res);
}
  unsigned long eTime = 0;
  int eGpio = 0;
  int eState = LOW;
  /**
   * Ghi lịch sử vào file nhị phân (binary) để tiết kiệm bộ nhớ
   * Mỗi bản ghi: userCode (uint32_t), state (uint8_t), time (uint32_t)
   */
  #define HISTORY_FILE "/history.bin"
  struct HistoryRecord {
    uint32_t userCode;
    uint8_t state;
    uint32_t epochtime;
  };

  void saveHistory(uint32_t userCode, uint8_t state, uint32_t epochtime) {
    File f = LittleFS.open(HISTORY_FILE, FILE_APPEND);
    if (!f) return;
    HistoryRecord rec;
    rec.userCode = userCode;
    rec.state = state;
    rec.epochtime = epochtime;
    f.write((const uint8_t*)&rec, sizeof(rec));
    f.close();
  }

  /**
   * API: GET /api/history?start=epochtime&end=epochtime
   * Trả về danh sách lịch sử trong khoảng thời gian
   */
  void handleGetHistory(HTTPRequest * req, HTTPResponse * res) {
    // Lấy tham số start, end
    std::string reqStr = req->getRequestString();
    std::string query;
    size_t qpos = reqStr.find('?');
    if (qpos != std::string::npos) {
      query = reqStr.substr(qpos + 1);
    } else {
      query = "";
    }
    uint32_t start = 0, end = 0xFFFFFFFF;
    size_t spos = query.find("start=");
    if (spos != std::string::npos) {
      size_t st = spos + 6;
      size_t en = query.find('&', st);
      std::string param = (en == std::string::npos) ? query.substr(st) : query.substr(st, en - st);
      start = strtoul(param.c_str(), nullptr, 10);
    }
    size_t epos = query.find("end=");
    if (epos != std::string::npos) {
      size_t st = epos + 4;
      size_t en = query.find('&', st);
      std::string param = (en == std::string::npos) ? query.substr(st) : query.substr(st, en - st);
      end = strtoul(param.c_str(), nullptr, 10);
    }

    File f = LittleFS.open(HISTORY_FILE, FILE_READ);
    if (!f) {
      res->setHeader("Content-Type", "application/json");
      res->print("[]");
      return;
    }
    DynamicJsonBuffer jsonBuffer(2048);
    JsonArray& arr = jsonBuffer.createArray();
    HistoryRecord rec;
    while (f.read((uint8_t*)&rec, sizeof(rec)) == sizeof(rec)) {
      if (rec.epochtime >= start && rec.epochtime <= end) {
        JsonObject& obj = arr.createNestedObject();
        obj["user"] = rec.userCode;
        obj["state"] = rec.state;
        obj["epochtime"] = rec.epochtime;
      }
    }
    f.close();
    res->setHeader("Content-Type", "application/json");
    arr.printTo(*res);
  }

void handlePostEvent(HTTPRequest * req, HTTPResponse * res) {
  // Đọc body JSON

  // DynamicJsonBuffer jsonBuffer(256);
  // JsonObject& obj = jsonBuffer.parseObject(*req);
  // if (!obj.success()) {
  //   res->setStatusCode(400);
  //   res->setStatusText("Bad Request");
  //   res->println("400 Bad Request: Invalid JSON");
  //   return;
  // }

  // // Lấy giá trị state và user
  // int state = obj.containsKey("state") ? obj["state"] : -1;
  // const char* user = obj.containsKey("user") ? obj["user"] : "";

  // // Lấy thời gian thực và epochtime
  // unsigned long now = millis() / 1000;
  // time_t epochtime = now;

  // // In ra log
  // Serial.print("User: ");
  // Serial.print(user);
  // Serial.print(", State: ");
  // Serial.print(state);
  // Serial.print(" (");
  // if (state == 1) Serial.print("mở");
  // else if (state == 2) Serial.print("đóng");
  // else Serial.print("lỗi");
  // Serial.print("), Thời gian thực: ");
  // Serial.print(now);
  // Serial.print(", Epoch: ");
  // Serial.println(epochtime);

  // // Trả về kết quả (có thể trả lại thông tin vừa nhận)
  // StaticJsonBuffer<128> outBuffer;
  // JsonObject& out = outBuffer.createObject();
  // out["user"] = user;
  // out["state"] = state;
  // out["time"] = now;
  // out["epochtime"] = epochtime;
  // res->setHeader("Content-Type", "application/json");
  // out.printTo(*res);
}

/**
 * This handler will delete an event (meaning: deactive the event)
 */
void handleDeleteEvent(HTTPRequest * req, HTTPResponse * res) {
  // Access the parameter from the URL. See Parameters example for more details on this
  ResourceParameters * params = req->getParams();
  size_t eid = std::atoi(params->getPathParameter(0).c_str());

  if (eid < MAX_EVENTS) {
    // Set the inactive flag
    events[eid].active = false;
    // And return a successful response without body
    res->setStatusCode(204);
    res->setStatusText("No Content");
  } else {
    // Send error message
    res->setStatusCode(400);
    res->setStatusText("Bad Request");
    res->println("400 Bad Request");
  }
}

/**
 * Helper function to read a line from HTTPRequest (since readBytesUntil is not available)
 */
size_t readLineFromRequest(HTTPRequest *req, char *buffer, size_t maxLen) {
  size_t i = 0;
  while (i < maxLen - 1) {
    char c;
    size_t n = req->readBytes((uint8_t*)&c, 1);
    if (n == 0) break;
    buffer[i++] = c;
    if (c == '\n') break;
  }
  buffer[i] = 0;
  return i;
}

/**
 * API endpoint to upload a file to LittleFS via POST /api/upload
 * Expects multipart/form-data with a file field named "file"
 */
void handleUploadFile(HTTPRequest * req, HTTPResponse * res) {
  // Only allow POST
  if (req->getMethod() != "POST") {
    req->discardRequestBody();
    res->setStatusCode(405);
    res->setStatusText("Method Not Allowed");
    res->println("405 Method Not Allowed");
    return;
  }

  // Parse Content-Type header for boundary
  std::string contentType = req->getHeader("Content-Type");
  size_t boundaryPos = contentType.find("boundary=");
  if (boundaryPos == std::string::npos) {
    res->setStatusCode(400);
    res->setStatusText("Bad Request");
    res->println("400 Bad Request: No boundary in Content-Type");
    return;
  }
  std::string boundary = "--" + contentType.substr(boundaryPos + 9);

  // Read until filename
  std::string line;
  std::string filename;
  char lineBuf[256];

  // Read lines until we find the filename
  while (true) {
    size_t len = readLineFromRequest(req, lineBuf, sizeof(lineBuf));
    if (len == 0) break;
    line = lineBuf;
    if (line.find("filename=") != std::string::npos) {
      size_t start = line.find("filename=\"");
      if (start != std::string::npos) {
        start += 10;
        size_t end = line.find("\"", start);
        if (end != std::string::npos) {
          filename = line.substr(start, end - start);
        }
      }
      break;
    }
  }
  // Skip headers until empty line
  while (true) {
    size_t len = readLineFromRequest(req, lineBuf, sizeof(lineBuf));
    if (len == 0) break;
    line = lineBuf;
    if (line == "\r" || line == "\n" || line == "\r\n") break;
  }

  if (filename.empty()) {
    res->setStatusCode(400);
    res->setStatusText("Bad Request");
    res->println("400 Bad Request: No filename");
    return;
  }

  // Save file to /public
  std::string filepath = std::string(DIR_PUBLIC) + "/" + filename;
  File file = LittleFS.open(filepath.c_str(), FILE_WRITE);
  if (!file) {
    res->setStatusCode(500);
    res->setStatusText("Internal Server Error");
    res->println("500 Internal Server Error: Cannot open file");
    return;
  }

  // Read file data until boundary
  bool fileDone = false;
  char buf[256];
  size_t n;
  std::string lastLine;
  while (!fileDone) {
    n = readLineFromRequest(req, buf, sizeof(buf));
    if (n == 0) break;
    std::string s(buf);
    if (s.find(boundary) != std::string::npos) {
      fileDone = true;
      // Remove trailing \r\n from previous line
      if (!lastLine.empty() && lastLine.back() == '\r') lastLine.pop_back();
      file.write((const uint8_t*)lastLine.c_str(), lastLine.length());
      break;
    }
    if (!lastLine.empty()) {
      file.write((const uint8_t*)lastLine.c_str(), lastLine.length());
    }
    lastLine = s;
  }
  file.close();

  res->setHeader("Content-Type", "application/json");
  res->print((std::string("{\"success\":true,\"filename\":\"") + filename + "\"}").c_str());
}
