#include "sd_file_server.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <ff.h>
#include <lwip/sockets.h>

#include "esphome/core/helpers.h"
#include "esphome/core/log.h"
#include "esphome/core/string_ref.h"

namespace esphome::sd_file_server {

static const char *const TAG = "sd_file_server";
static const char *const USERNAME = "samba";
static const char *const ROOT = "/sd";
static constexpr size_t NAME_MAX_LEN = 40;
static constexpr size_t YIELD_INTERVAL_BYTES = 16 * 1024;  // let the idle task run during a long stream

namespace {

// AsyncWebServer registers GET, POST and OPTIONS with the IDF server. HEAD goes through the same
// dispatcher, so it meets the same auth middleware and the same handlers.
struct HeadRegistrar : public AsyncWebServer {
  static void add(AsyncWebServer *server) {
    const httpd_uri_t head = {
        .uri = "",
        .method = HTTP_HEAD,
        .handler = &AsyncWebServer::request_handler,
        .user_ctx = server,
    };
    httpd_register_uri_handler(server->get_server(), &head);
  }
};

}  // namespace

SdFileServer::SdFileServer(web_server_base::WebServerBase *base, sd_spi_card::SdSpiCard *card)
    : base_(base), card_(card) {
  // A named user makes web_server_base wrap every add_handler() in its digest middleware: ours
  // and the captive portal's firmware upload. With a null password the middleware lets
  // everything through, so a unit without a pairing password behaves exactly as before.
  this->base_->set_auth_username(USERNAME);
}

void SdFileServer::setup() {
  this->base_->add_handler(this);
  this->setup_done_ = true;
  if (this->password_[0] != 0)
    this->start_();
}

void SdFileServer::dump_config() {
  ESP_LOGCONFIG(TAG,
                "SD File Server:\n"
                "  Listening: %s\n"
                "  Port: %u",
                YESNO(this->running_), this->base_->get_port());
}

void SdFileServer::set_password(const std::string &password) {
  snprintf(this->password_, sizeof(this->password_), "%s", password.c_str());
  this->base_->set_auth_password(this->password_[0] != 0 ? this->password_ : nullptr);
  if (!this->setup_done_)
    return;
  if (this->password_[0] != 0) {
    this->start_();
  } else {
    this->stop_();
  }
}

void SdFileServer::start_() {
  if (this->running_)
    return;
  this->base_->init();
  this->running_ = true;
  this->register_head_();
  ESP_LOGI(TAG, "Listening on port %u (free heap %u, largest block %u)", this->base_->get_port(),
           (unsigned) esp_get_free_heap_size(), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void SdFileServer::stop_() {
  if (!this->running_)
    return;
  this->running_ = false;
  this->base_->deinit();
  ESP_LOGI(TAG, "Stopped (free heap %u, largest block %u)", (unsigned) esp_get_free_heap_size(),
           (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

void SdFileServer::register_head_() {
  // init() started the IDF server, or the captive portal already had it up; either way HEAD is
  // missing until we add it. Re-done after every start because deinit() to zero tears it down.
  auto *server = this->base_->get_server();
  if (server != nullptr && server->get_server() != nullptr)
    HeadRegistrar::add(server);
}

bool SdFileServer::canHandle(AsyncWebServerRequest *request) const {
  if (!this->running_)
    return false;
  const auto method = request->method();
  if (method != HTTP_GET && method != HTTP_HEAD)
    return false;
  char url[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef u = request->url_to(url);
  return u == ROOT || (u.size() > 4 && memcmp(u.c_str(), "/sd/", 4) == 0);
}

void SdFileServer::handleRequest(AsyncWebServerRequest *request) {
  httpd_req_t *req = *request;
  // Small chunks (a listing, a file's tail) would otherwise each wait on the phone's delayed ACK.
  int nodelay = 1;
  setsockopt(httpd_req_to_sockfd(req), IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
  const bool head = request->method() == HTTP_HEAD;
  char url[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef u = request->url_to(url);
  if (!this->card_->is_mounted()) {
    this->send_status_(req, "503 Service Unavailable", "30");  // sd_spi_card retries the mount every 30 s
    return;
  }
  if (u == ROOT) {
    this->handle_list_(req, head);
    return;
  }
  const char *name = u.c_str() + 4;
  if (!valid_name_(name)) {
    this->send_status_(req, "404 Not Found");
    return;
  }
  this->handle_file_(req, name, head);
}

bool SdFileServer::valid_name_(const char *name) {
  const size_t len = strlen(name);
  if (len < 5 || len > NAME_MAX_LEN)
    return false;
  if (strcmp(name + len - 4, ".txt") != 0)
    return false;
  return strchr(name, '/') == nullptr && strstr(name, "..") == nullptr;
}

void SdFileServer::send_status_(httpd_req_t *req, const char *status, const char *retry_after) {
  httpd_resp_set_status(req, status);
  if (retry_after != nullptr)
    httpd_resp_set_hdr(req, "Retry-After", retry_after);
  httpd_resp_send(req, nullptr, 0);
}

void SdFileServer::handle_list_(httpd_req_t *req, bool head) {
  httpd_resp_set_type(req, "application/json");
  if (head) {
    httpd_resp_send(req, nullptr, 0);
    return;
  }
  // FatFs directly rather than readdir()+stat(): the VFS dirent carries no size, and a stat()
  // per file rescans the root directory each time, which made 90 files take seconds. FatFs is
  // built re-entrant, so this is safe beside the 5-minute append. Same drive as sd_spi_card's
  // f_getfree("0:").
  FF_DIR dir;
  if (f_opendir(&dir, "0:/") != FR_OK) {
    this->send_status_(req, "503 Service Unavailable", "30");
    return;
  }

  // Entries are packed into the buffer and sent a segment at a time: one chunk per entry costs a
  // round trip each (Nagle waiting on the phone's delayed ACK), seconds for a card of 90 logs.
  size_t used = 0;
  auto flush = [&]() -> bool {
    const bool ok = httpd_resp_send_chunk(req, this->buf_.data(), used) == ESP_OK;
    used = 0;
    return ok;
  };
  uint8_t mac[6];
  get_mac_address_raw(mac);
  used = snprintf(this->buf_.data(), this->buf_.size(), R"({"mac":"%02X%02X%02X%02X%02X%02X","files":[)", mac[0],
                  mac[1], mac[2], mac[3], mac[4], mac[5]);

  // Directory order, one pass. Sorting here needs either a name table or a scan per file; the
  // app sorts by name, which embeds the boot time.
  FILINFO fno;
  bool first = true;
  while (f_readdir(&dir, &fno) == FR_OK && fno.fname[0] != 0) {
    if ((fno.fattrib & AM_DIR) || !valid_name_(fno.fname))
      continue;
    char entry[NAME_MAX_LEN + 40];
    const size_t n = snprintf(entry, sizeof(entry), R"(%s{"name":"%s","size":%lu})", first ? "" : ",", fno.fname,
                              (unsigned long) fno.fsize);
    first = false;
    if (used + n > this->buf_.size() && !flush()) {
      f_closedir(&dir);
      return;
    }
    memcpy(this->buf_.data() + used, entry, n);
    used += n;
  }
  f_closedir(&dir);
  if (used + 2 > this->buf_.size() && !flush())
    return;
  memcpy(this->buf_.data() + used, "]}", 2);
  used += 2;
  if (flush())
    httpd_resp_send_chunk(req, nullptr, 0);
}

void SdFileServer::handle_file_(httpd_req_t *req, const char *name, bool head) {
  char path[sizeof("/sd/") + NAME_MAX_LEN];
  snprintf(path, sizeof(path), "%s/%s", ROOT, name);
  struct stat st;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
    this->send_status_(req, "404 Not Found");
    return;
  }
  const size_t size = st.st_size;
  // Header values are kept by pointer until the response goes out, so they live in this frame.
  char size_str[12];
  snprintf(size_str, sizeof(size_str), "%u", (unsigned) size);
  httpd_resp_set_type(req, "text/csv");
  httpd_resp_set_hdr(req, "X-File-Size", size_str);
  if (head) {
    httpd_resp_send(req, nullptr, 0);
    return;
  }

  size_t offset = 0;
  char range[40];
  char content_range[48];
  if (httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) == ESP_OK) {
    unsigned long start;
    char tail;
    // Open-ended ranges only. A start at or past the end is how the app learns the card changed.
    if (sscanf(range, "bytes=%lu-%c", &start, &tail) != 1 || start >= size) {
      snprintf(content_range, sizeof(content_range), "bytes */%u", (unsigned) size);
      httpd_resp_set_hdr(req, "Content-Range", content_range);
      this->send_status_(req, "416 Range Not Satisfiable");
      return;
    }
    offset = start;
    snprintf(content_range, sizeof(content_range), "bytes %u-%u/%u", (unsigned) offset, (unsigned) (size - 1),
             (unsigned) size);
    httpd_resp_set_status(req, "206 Partial Content");
    httpd_resp_set_hdr(req, "Content-Range", content_range);
  }

  FILE *f = fopen(path, "r");
  if (f == nullptr || (offset != 0 && fseek(f, offset, SEEK_SET) != 0)) {
    if (f != nullptr)
      fclose(f);
    this->send_status_(req, "503 Service Unavailable", "30");
    return;
  }
  // Stop at the size the listing saw, so a row appended mid-stream does not overrun Content-Range.
  size_t remaining = size - offset;
  size_t since_yield = 0;
  while (remaining > 0) {
    const size_t n = fread(this->buf_.data(), 1, std::min(remaining, this->buf_.size()), f);
    if (n == 0)
      break;
    if (httpd_resp_send_chunk(req, this->buf_.data(), n) != ESP_OK) {
      fclose(f);
      return;
    }
    remaining -= n;
    since_yield += n;
    if (since_yield >= YIELD_INTERVAL_BYTES) {
      vTaskDelay(1);
      since_yield = 0;
    }
  }
  fclose(f);
  httpd_resp_send_chunk(req, nullptr, 0);
}

}  // namespace esphome::sd_file_server
