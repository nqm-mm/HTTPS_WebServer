void WebAPI(){
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
    // Lấy tham số ?dir= từ query string nếu có
    std::string dir = DIR_PUBLIC;
    std::string reqStr = req->getRequestString();
    std::string query;
    size_t qpos = reqStr.find('?');
    if (qpos != std::string::npos) {
      query = reqStr.substr(qpos + 1);
    } else {
      query = "";
    }
    size_t pos = query.find("dir=");
    if (pos != std::string::npos) {
      size_t start = pos + 4;
      size_t end = query.find('&', start);
      std::string param = (end == std::string::npos) ? query.substr(start) : query.substr(start, end - start);
      if (!param.empty() && param[0] == '/') {
        dir = param;
      } else if (!param.empty()) {
        dir = std::string("/") + param;
      }
    }

    // Nếu folder không tồn tại, tạo mới
    if (!LittleFS.exists(dir.c_str())) {
      if (!LittleFS.mkdir(dir.c_str())) {
        res->setStatusCode(500);
        res->setStatusText("Internal Server Error");
        res->println("500 Internal Server Error: Cannot create directory");
        return;
      }
    }

    File root = LittleFS.open(dir.c_str());
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
  // Đăng ký endpoint GET /api/history để trả về lịch sử (tối đa 50 dòng mới nhất)
  ResourceNode * historyNode = new ResourceNode("/api/history", "GET", &handleGetHistory);
  secureServer->registerNode(historyNode);

  // Đăng ký endpoint GET /api/history-page để trả về trang HTML hiển thị lịch sử
  ResourceNode * historyPageNode = new ResourceNode("/api/history-page", "GET", [](HTTPRequest * req, HTTPResponse * res) {
    res->setHeader("Content-Type", "text/html; charset=UTF-8");
    res->print(R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <title>Lịch sử truy cập</title>
  <style>
    body { font-family: Arial; margin: 20px; }
    table { border-collapse: collapse; width: 100%; }
    th, td { border: 1px solid #ccc; padding: 6px 10px; }
    th { background: #eee; }
  </style>
</head>
<body>
  <h2>Lịch sử truy cập (tối đa 50 dòng mới nhất)</h2>
  <table>
    <thead>
      <tr>
        <th>#</th>
        <th>User</th>
        <th>State</th>
        <th>Epoch Time</th>
        <th>Time</th>
      </tr>
    </thead>
    <tbody id="historyBody"></tbody>
  </table>
  <script>
    async function loadHistory() {
      const res = await fetch('/api/history');
      const arr = await res.json();
      const body = document.getElementById('historyBody');
      body.innerHTML = '';
      arr.forEach((item, idx) => {
        const tr = document.createElement('tr');
        tr.innerHTML = `<td>${idx+1}</td>
          <td>${item.user}</td>
          <td>${item.state == 1 ? "Mở" : (item.state == 2 ? "Đóng" : item.state)}</td>
          <td>${item.epochtime}</td>
          <td>${new Date(item.epochtime*1000).toLocaleString()}</td>`;
        body.appendChild(tr);
      });
    }
    loadHistory();
  </script>
</body>
</html>
    )rawliteral");
  });
  secureServer->registerNode(historyPageNode);
}