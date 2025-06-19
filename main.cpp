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
  <div id="result"></div>\
  <div id="listFiles">
    <h3>Files in /public</h3>
    <ul id="fileList"></ul>
  </div>
  <div>
    <button onclick="listFiles()">Refresh File List</button>
  </div>
  <div id="Usege">
    <h3>Usage</h3>
    <p>Upload files to the ESP32 server. The files will be stored in the /public directory.</p>
    <p>After uploading, you can view and download the files from the list below.</p>
    <p id="memoryUsage"></p>
  </div>
  <script>
    document.addEventListener('DOMContentLoaded', function() {
      listFiles();
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
      } catch (err) {
        resultDiv.textContent = "Upload failed: " + err;
      }
    };
    async function listFiles() {
      const fileList = document.getElementById('fileList');
      fileList.innerHTML = '';
      try {
        const res = await fetch('/api/fs/list');
        if (!res.ok) throw new Error('Network response was not ok');
        const files = await res.json();
        files.forEach(file => {
          const li = document.createElement('li');
          li.textContent = `${file.name} (${file.size} bytes)`;
          if (!file.isDir) {
            const link = document.createElement('a');
            link.href = `/public/${file.name}`;
            link.textContent = ' [Download]';
            li.appendChild(link);
          }
          fileList.appendChild(li);
        });
      } catch (err) {
        console.error('Error fetching file list:', err);
      }
    }
    async function getMemoryUsage() {
      try {
        const res = await fetch('/api/fs/usage');
        if (!res.ok) throw new Error('Network response was not ok');
        const data = await res.json();
        document.getElementById('memoryUsage').textContent = `Free Memory: ${data.freeMemory} bytes`;
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
  Serial.print("Connected. IP=");
  Serial.println(WiFi.localIP());

  // Create the server with the certificate we loaded before
  secureServer = new HTTPSServer(cert);

  // We register the SPIFFS handler as the default node, so every request that does
  // not hit any other node will be redirected to the file system.
  ResourceNode * LittleFSNode = new ResourceNode("", "", &handleLittleFS);
  secureServer->setDefaultNode(LittleFSNode);

  // Add a handler that serves the current system uptime at GET /api/uptime
  ResourceNode * uptimeNode = new ResourceNode("/api/uptime", "GET", &handleGetUptime);
  secureServer->registerNode(uptimeNode);

  // Add the handler nodes that deal with modifying the events:
  ResourceNode * getEventsNode = new ResourceNode("/api/events", "GET", &handleGetEvents);
  secureServer->registerNode(getEventsNode);
  ResourceNode * postEventNode = new ResourceNode("/api/events", "POST", &handlePostEvent);
  secureServer->registerNode(postEventNode);
  ResourceNode * deleteEventNode = new ResourceNode("/api/events/*", "DELETE", &handleDeleteEvent);
  secureServer->registerNode(deleteEventNode);
  // Register the upload API endpoint in setup()
  ResourceNode * uploadNode = new ResourceNode("/api/upload", "POST", &handleUploadFile);
  secureServer->registerNode(uploadNode);
  // API: GET /api/fs/list - List files in /public
  ResourceNode * fsListNode = new ResourceNode("/api/fs/list", "GET", [](HTTPRequest * req, HTTPResponse * res) {
    File root = LittleFS.open(DIR_PUBLIC);
    if (!root || !root.isDirectory()) {
      res->setStatusCode(500);
      res->setStatusText("Internal Server Error");
      res->println("500 Internal Server Error: Cannot open directory");
      return;
    }
    DynamicJsonBuffer jsonBuffer(2048);
    JsonArray& arr = jsonBuffer.createArray();
    File file = root.openNextFile();
    while (file) {
      JsonObject& obj = arr.createNestedObject();
      obj["name"] = file.name();
      obj["size"] = file.size();
      obj["isDir"] = file.isDirectory();
      file = root.openNextFile();
    }
    res->setHeader("Content-Type", "application/json");
    arr.printTo(*res);
  });
  secureServer->registerNode(fsListNode);

  // API: DELETE /api/fs/file/* - Delete file in /public
  ResourceNode * fsDeleteNode = new ResourceNode("/api/fs/file/*", "DELETE", [](HTTPRequest * req, HTTPResponse * res) {
    ResourceParameters * params = req->getParams();
    std::string fname = params->getPathParameter(0);
    if (fname.empty() || fname.find("..") != std::string::npos) {
      res->setStatusCode(400);
      res->setStatusText("Bad Request");
      res->println("400 Bad Request");
      return;
    }
    std::string path = std::string(DIR_PUBLIC) + "/" + fname;
    if (!LittleFS.exists(path.c_str())) {
      res->setStatusCode(404);
      res->setStatusText("Not Found");
      res->println("404 Not Found");
      return;
    }
    if (LittleFS.remove(path.c_str())) {
      res->setStatusCode(204);
      res->setStatusText("No Content");
    } else {
      res->setStatusCode(500);
      res->setStatusText("Internal Server Error");
      res->println("500 Internal Server Error: Cannot delete file");
    }
  });
  secureServer->registerNode(fsDeleteNode);

  // API: GET /api/fs/usage - Get FS usage info
  ResourceNode * fsUsageNode = new ResourceNode("/api/fs/usage", "GET", [](HTTPRequest * req, HTTPResponse * res) {
    StaticJsonBuffer<JSON_OBJECT_SIZE(3)> jsonBuffer;
    JsonObject& obj = jsonBuffer.createObject();
    obj["totalBytes"] = LittleFS.totalBytes();
    obj["usedBytes"] = LittleFS.usedBytes();
    obj["freeBytes"] = LittleFS.totalBytes() - LittleFS.usedBytes();
    res->setHeader("Content-Type", "application/json");
    obj.printTo(*res);
  });
  secureServer->registerNode(fsUsageNode);
  // Đăng ký endpoint GET /api/upload-page để trả về trang upload_html
  ResourceNode * uploadPageNode = new ResourceNode("/api/upload-page", "GET", [](HTTPRequest * req, HTTPResponse * res) {
    res->setHeader("Content-Type", "text/html; charset=UTF-8");
    res->print(upload_html);
  });
  secureServer->registerNode(uploadPageNode);
  Serial.println("Starting server...");
  secureServer->start();
  if (secureServer->isRunning()) {
    Serial.println("Server ready.");
  }
}

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

void handlePostEvent(HTTPRequest * req, HTTPResponse * res) {
  // We expect an object with 4 elements and add some buffer
  const size_t capacity = JSON_OBJECT_SIZE(4) + 180;
  DynamicJsonBuffer jsonBuffer(capacity);

  // Create buffer to read request
  char * buffer = new char[capacity + 1];
  memset(buffer, 0, capacity+1);

  // Try to read request into buffer
  size_t idx = 0;
  // while "not everything read" or "buffer is full"
  while (!req->requestComplete() && idx < capacity) {
    idx += req->readChars(buffer + idx, capacity-idx);
  }

  // If the request is still not read completely, we cannot process it.
  if (!req->requestComplete()) {
    res->setStatusCode(413);
    res->setStatusText("Request entity too large");
    res->println("413 Request entity too large");
    // Clean up
    delete[] buffer;
    return;
  }

  // Parse the object
  JsonObject& reqObj = jsonBuffer.parseObject(buffer);

  // Check input data types
  bool dataValid = true;
  if (!reqObj.is<long>("time") || !reqObj.is<int>("gpio") || !reqObj.is<int>("state")) {
    dataValid = false;
  }
	
  // Check actual values
  unsigned long eTime = 0;
  int eGpio = 0;
  int eState = LOW;
  if (dataValid) {
    eTime = reqObj["time"];
    if (eTime < millis()/1000) dataValid = false;

    eGpio = reqObj["gpio"];
    if (!(eGpio == 25 || eGpio == 26 || eGpio == 27 || eGpio == 32 || eGpio == 33)) dataValid = false;

    eState = reqObj["state"];
    if (eState != HIGH && eState != LOW) dataValid = false;
  }

  // Clean up, we don't need the buffer any longer
  delete[] buffer;

  // If something failed: 400
  if (!dataValid) {
    res->setStatusCode(400);
    res->setStatusText("Bad Request");
    res->println("400 Bad Request");
    return;
  }

  // Try to find an inactive event in the list to write the data to
  int eventID = -1;
  for(int i = 0; i < MAX_EVENTS && eventID==-1; i++) {
    if (!events[i].active) {
      eventID = i;
      events[i].gpio = eGpio;
      events[i].time = eTime;
      events[i].state = eState;
      events[i].active = true;
    }
  }

  // Check if we could store the event
  if (eventID>-1) {
    // Create a buffer for the response
    StaticJsonBuffer<JSON_OBJECT_SIZE(4)> resBuffer;

    // Create an object at the root
    JsonObject& resObj = resBuffer.createObject();

    // Set the uptime key to the uptime in seconds
    resObj["gpio"] = events[eventID].gpio;
    resObj["state"] = events[eventID].state;
    resObj["time"] = events[eventID].time;
    resObj["id"] = eventID;

    // Write the response
    res->setHeader("Content-Type", "application/json");
    resObj.printTo(*res);

  } else {
    // We could not store the event, no free slot.
    res->setStatusCode(507);
    res->setStatusText("Insufficient storage");
    res->println("507 Insufficient storage");

  }
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