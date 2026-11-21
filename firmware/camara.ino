/*
  SpiderKiki - ESP32-CAM (AI Thinker): solo video

  Placa independiente: no tiene ningún cable al PCA9685 ni a los servos.
  Se conecta por WiFi a la red "spiderkiki" que crea el ESP32-C3
  (ver spiderkiki_esp32c3.ino) y transmite video MJPEG.

  Video: http://192.168.4.100:81/stream
  (la página de control del ESP32-C3 lo muestra sola)

  Arduino IDE:
    Placa: AI Thinker ESP32-CAM
    Partition Scheme: Huge APP (3MB No OTA/1MB SPIFFS)
*/

#include <WiFi.h>
#include "esp_camera.h"
#include "esp_http_server.h"


// ---------------- WiFi ----------------

const char* WIFI_SSID = "spiderkiki";   // red abierta del ESP32-C3
// Si la red tiene clave, usar WiFi.begin(WIFI_SSID, WIFI_PASS):
// const char* WIFI_PASS = "12345678";

// IP fija, para que la página del robot siempre sepa dónde está el video
IPAddress IP_CAM(192, 168, 4, 100);
IPAddress IP_GATEWAY(192, 168, 4, 1);    // el ESP32-C3
IPAddress IP_MASCARA(255, 255, 255, 0);


// ---------------- Cámara ----------------

// Resoluciones de menor a mayor: QQVGA 160x120, HQVGA 240x176, QVGA 320x240.
// La más chica da el video más fluido.
#define TAMANO_VIDEO FRAMESIZE_QQVGA

// 10 = mejor calidad, 63 = peor calidad (más liviano)
#define CALIDAD_JPEG 15

// Poner en 1 si la imagen sale invertida
#define VOLTEAR_VERTICAL  0
#define ESPEJO_HORIZONTAL 0

// Pines del módulo AI Thinker
#define PWDN_GPIO_NUM  32
#define RESET_GPIO_NUM -1
#define XCLK_GPIO_NUM   0
#define SIOD_GPIO_NUM  26
#define SIOC_GPIO_NUM  27

#define Y9_GPIO_NUM    35
#define Y8_GPIO_NUM    34
#define Y7_GPIO_NUM    39
#define Y6_GPIO_NUM    36
#define Y5_GPIO_NUM    21
#define Y4_GPIO_NUM    19
#define Y3_GPIO_NUM    18
#define Y2_GPIO_NUM     5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM  23
#define PCLK_GPIO_NUM  22

httpd_handle_t servidorStream = NULL;


// ============================================================
// CÁMARA
// ============================================================

bool iniciarCamara() {

  camera_config_t config;

  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.frame_size   = TAMANO_VIDEO;
  config.jpeg_quality = CALIDAD_JPEG;
  config.fb_count     = 2;                      // doble buffer en PSRAM
  config.fb_location  = CAMERA_FB_IN_PSRAM;
  config.grab_mode    = CAMERA_GRAB_LATEST;     // siempre el cuadro más reciente

  esp_err_t err = esp_camera_init(&config);

  if (err != ESP_OK) {
    Serial.printf("Error al iniciar la camara: 0x%x\n", err);
    return false;
  }

  sensor_t* s = esp_camera_sensor_get();
  s->set_vflip(s, VOLTEAR_VERTICAL);
  s->set_hmirror(s, ESPEJO_HORIZONTAL);

  return true;
}


// ============================================================
// STREAM MJPEG
// ============================================================

// Envía cuadros JPEG sin parar hasta que el navegador cierra la conexión.
static esp_err_t manejarStream(httpd_req_t* req) {

  esp_err_t res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=frame");
  if (res != ESP_OK) return res;

  // Permite que la página del ESP32-C3 (otra IP) muestre este video
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

  char cabecera[80];

  while (true) {

    camera_fb_t* fb = esp_camera_fb_get();

    if (!fb) {
      res = ESP_FAIL;
    } else {
      size_t n = snprintf(cabecera, sizeof(cabecera),
                          "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n",
                          (unsigned)fb->len);

      res = httpd_resp_send_chunk(req, "\r\n--frame\r\n", 11);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, cabecera, n);
      if (res == ESP_OK) res = httpd_resp_send_chunk(req, (const char*)fb->buf, fb->len);

      esp_camera_fb_return(fb);
    }

    if (res != ESP_OK)
      break;
  }

  return res;
}

void iniciarServidorStream() {

  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 81;

  httpd_uri_t uriStream = {
    .uri      = "/stream",
    .method   = HTTP_GET,
    .handler  = manejarStream,
    .user_ctx = NULL
  };

  if (httpd_start(&servidorStream, &config) == ESP_OK) {
    httpd_register_uri_handler(servidorStream, &uriStream);
    Serial.println("Servidor de video listo en el puerto 81.");
  } else {
    Serial.println("No se pudo iniciar el servidor de video.");
  }
}


// ============================================================
// WIFI
// ============================================================

// Se conecta a la red del robot; espera hasta 10 s.
bool conectarWifi() {

  WiFi.mode(WIFI_STA);
  WiFi.config(IP_CAM, IP_GATEWAY, IP_MASCARA);
  WiFi.begin(WIFI_SSID);
  WiFi.setSleep(false);   // evita retrasos del WiFi en el video

  Serial.print("Conectando a ");
  Serial.print(WIFI_SSID);

  for (int i = 0; i < 20 && WiFi.status() != WL_CONNECTED; i++) {
    delay(500);
    Serial.print(".");
  }

  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Conectada. Video en http://");
    Serial.print(WiFi.localIP());
    Serial.println(":81/stream");
    return true;
  }

  Serial.println("Sin conexion (se reintenta solo).");
  return false;
}


// ============================================================
// SETUP Y LOOP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  if (!iniciarCamara()) {
    Serial.println("Camara no disponible. Revisar conexion y alimentacion.");
    return;
  }

  conectarWifi();
  iniciarServidorStream();
}

void loop() {

  // Si se pierde la red (por ejemplo, el robot se reinicia), reconecta.
  // El servidor de video sigue activo y vuelve a responder al reconectar.
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    conectarWifi();
  }

  delay(2000);
}