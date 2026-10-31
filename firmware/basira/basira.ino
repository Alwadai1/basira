/*
  بصيرة | Basira — نسخة الجوال (بدون لابتوب)
  ------------------------------------------------
  اللوحة: AI Thinker ESP32-CAM
  الفكرة:
   1) يضغط الكفيف الزر (GPIO13) فتلتقط الكاميرا صورة.
   2) ترسل الكاميرا الصورة بنفسها إلى Gemini عبر إنترنت نقطة اتصال الجوال.
   3) يعود الوصف العربي، فتنطقه صفحة بصيرة المفتوحة في متصفح الجوال
      عبر سماعة البلوتوث المقترنة بالجوال.

  قبل الرفع: انسخ config.example.h إلى config.h وضع فيه بياناتك.
  إعدادات Arduino IDE:
   Board: "AI Thinker ESP32-CAM" · PSRAM: Enabled · Partition: Huge APP (3MB No OTA)
*/

#include "esp_camera.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include "mbedtls/base64.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "config.h"
#include "page.h"

// ---------- أطراف لوحة AI Thinker ----------
#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27
#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#define BUTTON_PIN 13   // الزر بين GPIO13 و GND
#define FLASH_LED 4     // فلاش الكاميرا (نبقيه مطفأ)
#define STATUS_LED 33   // المصباح الأحمر الصغير على اللوحة (يعمل عند LOW)

// التعليمات التي تُرسل إلى Gemini مع كل صورة
const char* PROMPT =
  "أنت مساعد لشخص كفيف يرتدي نظارة بها كاميرا. صف ما أمامه بالعربية البسيطة القريبة من اللهجة السعودية "
  "في جملتين قصيرتين فقط. ابدأ بأهم شيء أمامه، واذكر أي عائق أو خطر واتجاهه (يمين، يسار، أمامك) ومسافته التقريبية. "
  "إن وُجد نص واضح فاقرأه. لا تستخدم رموزًا أو تنسيقًا.";

WebServer server(80);
SemaphoreHandle_t lock;
volatile bool busy = false;
uint32_t lastId = 0;
String lastText = "بصيرة جاهزة";
unsigned long lastPress = 0;

// ---------- أدوات صغيرة ----------
void appendUtf8(String& o, uint32_t cp) {
  if (cp < 0x80) o += (char)cp;
  else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
  else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F)); o += (char)(0x80 | (cp & 0x3F)); }
}

// يستخرج أول قيمة "key" نصية من رد JSON (يكفي لرد Gemini)
String jsonString(const String& r, const char* key, int from) {
  int p = r.indexOf(String("\"") + key + "\"", from);
  if (p < 0) return "";
  p = r.indexOf(':', p);
  p = r.indexOf('"', p) + 1;
  String o;
  for (int i = p; i < (int)r.length(); i++) {
    char c = r[i];
    if (c == '\\' && i + 1 < (int)r.length()) {
      char n = r[++i];
      if (n == 'n' || n == 'r' || n == 't') o += ' ';
      else if (n == 'u' && i + 4 < (int)r.length()) { appendUtf8(o, strtoul(r.substring(i + 1, i + 5).c_str(), nullptr, 16)); i += 4; }
      else o += n;
    } else if (c == '"') break;
    else o += c;
  }
  o.trim();
  return o;
}

String jsonEscape(const String& s) {
  String o;
  for (unsigned i = 0; i < s.length(); i++) {
    char c = s[i];
    if (c == '"' || c == '\\') { o += '\\'; o += c; }
    else if (c == '\n' || c == '\r') o += ' ';
    else o += c;
  }
  return o;
}

// ---------- إرسال الصورة إلى Gemini ----------
String askGemini(camera_fb_t* fb) {
  String head = String("{\"contents\":[{\"parts\":[{\"text\":\"") + PROMPT +
                "\"},{\"inline_data\":{\"mime_type\":\"image/jpeg\",\"data\":\"";
  const char* tail = "\"}}]}]}";
  size_t b64len = 4 * ((fb->len + 2) / 3);
  size_t total = head.length() + b64len + strlen(tail);

  WiFiClientSecure c;
  c.setInsecure();          // نسخة تجريبية: يُستحسن لاحقًا إضافة شهادة Google الجذرية
  c.setTimeout(25000);
  if (!c.connect("generativelanguage.googleapis.com", 443)) {
    Serial.println("فشل الاتصال بخادم Gemini");
    return "";
  }

  c.print(String("POST /v1beta/models/") + GEMINI_MODEL + ":generateContent HTTP/1.0\r\n" +
          "Host: generativelanguage.googleapis.com\r\n" +
          "x-goog-api-key: " + GEMINI_KEY + "\r\n" +
          "Content-Type: application/json\r\n" +
          "Content-Length: " + String(total) + "\r\n" +
          "Connection: close\r\n\r\n");
  c.print(head);

  // ترميز الصورة Base64 على دفعات صغيرة لتوفير الذاكرة
  const size_t IN = 3 * 256;
  unsigned char out[4 * 256 + 8];
  for (size_t i = 0; i < fb->len; i += IN) {
    size_t n = min(IN, fb->len - i), olen = 0;
    mbedtls_base64_encode(out, sizeof(out), &olen, fb->buf + i, n);
    c.write(out, olen);
  }
  c.print(tail);

  String resp;
  uint8_t buf[512];
  unsigned long t = millis();
  while ((c.connected() || c.available()) && millis() - t < 30000) {
    int n = c.read(buf, sizeof(buf));
    if (n > 0) { resp.concat((const char*)buf, n); t = millis(); }
    else delay(5);
  }
  c.stop();

  int body = resp.indexOf("\r\n\r\n");
  if (resp.indexOf(" 200 ") < 0 || body < 0) {
    Serial.println("رد غير متوقع من Gemini:");
    Serial.println(resp.substring(0, 600));
    String msg = jsonString(resp, "message", body < 0 ? 0 : body);
    return msg.length() ? "خطأ من الخدمة" : "";
  }
  return jsonString(resp, "text", body);
}

// ---------- مهمة الوصف (تعمل في الخلفية حتى تبقى صفحة الجوال متجاوبة) ----------
void describeTask(void*) {
  camera_fb_t* fb = esp_camera_fb_get();   // نتخلص من الإطار القديم
  if (fb) esp_camera_fb_return(fb);
  fb = esp_camera_fb_get();

  String txt;
  if (!fb) txt = "الكاميرا لا تستجيب، أعد تشغيل النظارة";
  else {
    Serial.printf("صورة بحجم %u بايت، جارٍ الإرسال...\n", fb->len);
    txt = askGemini(fb);
    esp_camera_fb_return(fb);
    if (txt.length() == 0) txt = "تعذر الوصف، تأكد من إنترنت الجوال وحاول مرة أخرى";
  }
  Serial.println("الوصف: " + txt);

  xSemaphoreTake(lock, portMAX_DELAY);
  lastText = txt;
  lastId++;
  xSemaphoreGive(lock);

  busy = false;
  digitalWrite(STATUS_LED, HIGH);
  vTaskDelete(NULL);
}

void startDescribe() {
  if (busy) return;
  busy = true;
  digitalWrite(STATUS_LED, LOW);
  xTaskCreate(describeTask, "describe", 16384, NULL, 1, NULL);
}

// ---------- صفحات الويب للجوال ----------
void handleLast() {
  xSemaphoreTake(lock, portMAX_DELAY);
  String j = String("{\"id\":") + lastId + ",\"busy\":" + (busy ? "true" : "false") +
             ",\"text\":\"" + jsonEscape(lastText) + "\"}";
  xSemaphoreGive(lock);
  server.sendHeader("Cache-Control", "no-store");
  server.send(200, "application/json; charset=utf-8", j);
}

void handlePhoto() {   // لمعاينة ما تراه الكاميرا أثناء التركيب
  if (busy) { server.send(503, "text/plain", "busy"); return; }
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) { server.send(500, "text/plain", "camera error"); return; }
  server.sendHeader("Cache-Control", "no-store");
  server.send_P(200, "image/jpeg", (const char*)fb->buf, fb->len);
  esp_camera_fb_return(fb);
}

bool initCamera() {
  camera_config_t c = {};
  c.ledc_channel = LEDC_CHANNEL_0; c.ledc_timer = LEDC_TIMER_0;
  c.pin_d0 = Y2_GPIO_NUM; c.pin_d1 = Y3_GPIO_NUM; c.pin_d2 = Y4_GPIO_NUM; c.pin_d3 = Y5_GPIO_NUM;
  c.pin_d4 = Y6_GPIO_NUM; c.pin_d5 = Y7_GPIO_NUM; c.pin_d6 = Y8_GPIO_NUM; c.pin_d7 = Y9_GPIO_NUM;
  c.pin_xclk = XCLK_GPIO_NUM; c.pin_pclk = PCLK_GPIO_NUM; c.pin_vsync = VSYNC_GPIO_NUM; c.pin_href = HREF_GPIO_NUM;
  c.pin_sccb_sda = SIOD_GPIO_NUM; c.pin_sccb_scl = SIOC_GPIO_NUM;
  c.pin_pwdn = PWDN_GPIO_NUM; c.pin_reset = RESET_GPIO_NUM;
  c.xclk_freq_hz = 20000000;
  c.pixel_format = PIXFORMAT_JPEG;
  c.frame_size = FRAMESIZE_VGA;      // 640×480 يكفي للوصف ويبقي الإرسال سريعًا
  c.jpeg_quality = 12;
  c.fb_count = 1;
  c.fb_location = CAMERA_FB_IN_PSRAM;
  c.grab_mode = CAMERA_GRAB_LATEST;
  return esp_camera_init(&c) == ESP_OK;
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);   // يمنع إعادة التشغيل عند هبوط الجهد لحظة الإرسال
  Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);
  pinMode(FLASH_LED, OUTPUT); digitalWrite(FLASH_LED, LOW);
  pinMode(STATUS_LED, OUTPUT); digitalWrite(STATUS_LED, HIGH);
  lock = xSemaphoreCreateMutex();

  if (!initCamera()) { Serial.println("فشل تشغيل الكاميرا"); delay(3000); ESP.restart(); }

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("الاتصال بنقطة الجوال");
  while (WiFi.status() != WL_CONNECTED) { delay(400); Serial.print("."); }
  Serial.println();
  Serial.print("افتح في متصفح الجوال: http://"); Serial.println(WiFi.localIP());
  if (MDNS.begin("basira")) Serial.println("أو جرّب: http://basira.local");

  server.on("/", []() { server.send_P(200, "text/html; charset=utf-8", PAGE); });
  server.on("/last", handleLast);
  server.on("/snap", []() { startDescribe(); server.send(200, "text/plain", "ok"); });
  server.on("/photo", handlePhoto);
  server.begin();
}

void loop() {
  server.handleClient();
  if (digitalRead(BUTTON_PIN) == LOW && millis() - lastPress > 800) {
    lastPress = millis();
    startDescribe();
  }
  delay(2);
}
