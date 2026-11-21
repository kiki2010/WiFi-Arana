/*
  SpiderKiki - ESP32-C3: control del robot araña

  Controla los 8 servos (PCA9685), crea la red WiFi y sirve la
  página de control. El video lo transmite el ESP32-CAM, que es
  otra placa (ver spiderkiki_cam.ino).

  Uso:
    1. Conectar el celular a la red WiFi "spiderkiki" (sin contraseña).
    2. Abrir http://192.168.4.1
       El video aparece solo cuando el ESP32-CAM se conecta (192.168.4.100).

  Conexiones PCA9685 <-> ESP32-C3:
    SDA -> GPIO 8       SCL -> GPIO 9
    VCC -> 3V3          GND -> GND (común con la fuente de servos)
    V+  -> fuente 5V/6V aparte, solo para servos (nunca al 3V3)

  Servos (canal = número de servo):
    Horizontales: S0..S3     Verticales: S4..S7
    Pata i = horizontal Si + vertical S(i+4)

  Grupos de la marcha (se alternan):
    A: patas 0 y 3 (S0, S3, S4, S7)
    B: patas 1 y 2 (S1, S2, S5, S6)
*/

#include <Wire.h>
#include <Adafruit_PWMServoDriver.h>
#include <WiFi.h>
#include <WebServer.h>


// ---------------- WiFi ----------------

const char* AP_SSID = "spiderkiki";   // red abierta
// Para ponerle clave (mínimo 8 caracteres):
// const char* AP_PASS = "12345678";

WebServer server(80);   // página de control y comandos


// ---------------- PCA9685 ----------------

Adafruit_PWMServoDriver pwm = Adafruit_PWMServoDriver(0x40);

#define SERVOMIN   102   // pulso para 0°
#define SERVOMAX   512   // pulso para 180°
#define SERVO_FREQ 50    // Hz típicos de servo

#define SDA_PIN 8
#define SCL_PIN 9

#define NUM_SERVOS    18   // canales que se centran en "centro"
#define SERVOS_USADOS 8    // servos realmente conectados (S0..S7)


// ---------------- Velocidad de movimiento ----------------

// Milisegundos entre pasos de 1°: menor = más rápido
#define STEP_DELAY 12
#define STEP_SIZE  1

#define VEL_RAPIDA 6
#define VEL_NORMAL STEP_DELAY
#define VEL_LENTA  20


// ---------------- Ángulos ----------------

#define CENTRO 90

// Verticales: ángulo con la pata levantada / apoyada.
// S4 y S6 levantan hacia abajo de 90; S5 y S7 hacia arriba (están invertidos).
#define S4_LEVANTAR  60
#define S4_APOYAR    90
#define S5_LEVANTAR 120
#define S5_APOYAR    90
#define S6_LEVANTAR  60
#define S6_APOYAR    90
#define S7_LEVANTAR 120
#define S7_APOYAR    90

// Horizontales: extremos del paso (los lados están en espejo)
#define S0_ADELANTE  45
#define S0_ATRAS    135
#define S1_ADELANTE 135
#define S1_ATRAS     45
#define S2_ADELANTE  45
#define S2_ATRAS    135
#define S3_ADELANTE 135
#define S3_ATRAS     45

// Cuánto se separa del centro el servo vertical en cada acción
#define ALZAR_ACOSTAR 45
#define ALZAR_FLEXION 35
#define ALZAR_SALUDO  50
#define ALZAR_GOLPE   45
#define ALZAR_BAILE   35

// Extremos del movimiento de la pata que saluda (S3)
#define SALUDO_A 110
#define SALUDO_B 165


// ---------------- Modos de caminata ----------------

#define MODO_ADELANTE   0
#define MODO_ATRAS      1
#define MODO_DERECHA    2   // giro a la derecha
#define MODO_IZQUIERDA  3   // giro a la izquierda
#define MODO_LADO_IZQ   4   // desplazamiento lateral izquierdo
#define MODO_LADO_DER   5   // desplazamiento lateral derecho

int modoCaminata = MODO_ADELANTE;


// ---------------- Estado ----------------

int anguloActual[NUM_SERVOS];   // último ángulo enviado a cada canal

bool caminando = false;
int faseCaminata = 0;           // paso actual del ciclo de 6 fases

bool animando = false;          // hay una acción en curso
bool cancelar = false;          // pedido de interrumpir la acción


// ============================================================
// SERVOS
// ============================================================

// Mueve un servo directo al ángulo, sin suavizar.
void setServoAngleInstant(uint8_t channel, int angle) {
  angle = constrain(angle, 0, 180);
  int pulse = map(angle, 0, 180, SERVOMIN, SERVOMAX);
  pwm.setPWM(channel, 0, pulse);
  anguloActual[channel] = angle;
}

// Mueve un servo grado a grado hasta el destino.
void setServoAngleSuave(uint8_t channel, int destino) {

  destino = constrain(destino, 0, 180);
  int actual = anguloActual[channel];

  if (actual == destino)
    return;

  int paso = (destino > actual) ? STEP_SIZE : -STEP_SIZE;

  while (actual != destino) {
    actual += paso;
    if ((paso > 0 && actual > destino) || (paso < 0 && actual < destino))
      actual = destino;
    setServoAngleInstant(channel, actual);
    delay(STEP_DELAY);
  }
}

// Mueve dos servos a la vez; ambos terminan juntos aunque recorran
// distancias distintas.
void moverArticulaciones(uint8_t servo1, int destino1, uint8_t servo2, int destino2) {

  int actual1 = anguloActual[servo1];
  int actual2 = anguloActual[servo2];
  int distanciaMax = max(abs(destino1 - actual1), abs(destino2 - actual2));

  if (distanciaMax == 0)
    return;

  for (int i = 1; i <= distanciaMax; i++) {
    setServoAngleInstant(servo1, map(i, 0, distanciaMax, actual1, destino1));
    setServoAngleInstant(servo2, map(i, 0, distanciaMax, actual2, destino2));
    delay(STEP_DELAY);
  }
}

// Igual que la anterior, para los 4 servos horizontales (S0..S3).
void moverPatasHorizontales(int destinoS0, int destinoS1, int destinoS2, int destinoS3) {

  int actualS0 = anguloActual[0];
  int actualS1 = anguloActual[1];
  int actualS2 = anguloActual[2];
  int actualS3 = anguloActual[3];

  int d0 = abs(destinoS0 - actualS0);
  int d1 = abs(destinoS1 - actualS1);
  int d2 = abs(destinoS2 - actualS2);
  int d3 = abs(destinoS3 - actualS3);

  int distanciaMax = max(max(d0, d1), max(d2, d3));

  if (distanciaMax == 0)
    return;

  for (int i = 1; i <= distanciaMax; i++) {
    setServoAngleInstant(0, map(i, 0, distanciaMax, actualS0, destinoS0));
    setServoAngleInstant(1, map(i, 0, distanciaMax, actualS1, destinoS1));
    setServoAngleInstant(2, map(i, 0, distanciaMax, actualS2, destinoS2));
    setServoAngleInstant(3, map(i, 0, distanciaMax, actualS3, destinoS3));
    delay(STEP_DELAY);
  }
}


// ============================================================
// CAMINATA
// ============================================================

// Tablas por pata (índice 0..3): ángulo hacia adelante / atrás, y lado.
const int ADELANTE_VALS[4] = { S0_ADELANTE, S1_ADELANTE, S2_ADELANTE, S3_ADELANTE };
const int ATRAS_VALS[4]    = { S0_ATRAS, S1_ATRAS, S2_ATRAS, S3_ATRAS };
const bool ES_IZQUIERDA[4] = { true, false, true, false };

// Posición "adentro" de cada pata para el desplazamiento lateral.
// "Afuera" es el centro.
const int LAT_ADENTRO[4] = { S0_ADELANTE, S1_ATRAS, S2_ATRAS, S3_ADELANTE };
#define LAT_AFUERA CENTRO

// Destino horizontal de una pata en el modo lateral.
// El lado hacia el que se camina apoya "adentro" y el otro lo recoge.
int calcularDestinoLateral(int idx, bool levantada, int modo) {

  bool apoyoAdentro = (modo == MODO_LADO_IZQ) ? ES_IZQUIERDA[idx] : !ES_IZQUIERDA[idx];

  if (levantada)
    return apoyoAdentro ? LAT_AFUERA : LAT_ADENTRO[idx];
  else
    return apoyoAdentro ? LAT_ADENTRO[idx] : LAT_AFUERA;
}

// Destino horizontal de la pata idx según el modo.
// Levantada: va hacia donde apoyará. Apoyada: empuja hacia el otro lado.
// Para girar, un lado camina hacia adelante y el otro hacia atrás.
int calcularDestino(int idx, bool levantada, int modo) {

  if (modo == MODO_LADO_IZQ || modo == MODO_LADO_DER)
    return calcularDestinoLateral(idx, levantada, modo);

  bool ladoIzquierdo = ES_IZQUIERDA[idx];
  bool patronAdelante;

  switch (modo) {
    case MODO_ADELANTE:  patronAdelante = true; break;
    case MODO_ATRAS:     patronAdelante = false; break;
    case MODO_DERECHA:   patronAdelante = ladoIzquierdo; break;
    case MODO_IZQUIERDA: patronAdelante = !ladoIzquierdo; break;
    default:             patronAdelante = true; break;
  }

  if (patronAdelante)
    return levantada ? ADELANTE_VALS[idx] : ATRAS_VALS[idx];
  else
    return levantada ? ATRAS_VALS[idx] : ADELANTE_VALS[idx];
}

// Levantar / apoyar cada grupo de patas (el delay deja asentar el servo)
void levantarGrupoA() { Serial.println("Grupo A: LEVANTAR"); moverArticulaciones(4, S4_LEVANTAR, 7, S7_LEVANTAR); delay(100); }
void bajarGrupoA()    { Serial.println("Grupo A: APOYAR");   moverArticulaciones(4, S4_APOYAR, 7, S7_APOYAR);     delay(100); }
void levantarGrupoB() { Serial.println("Grupo B: LEVANTAR"); moverArticulaciones(5, S5_LEVANTAR, 6, S6_LEVANTAR); delay(100); }
void bajarGrupoB()    { Serial.println("Grupo B: APOYAR");   moverArticulaciones(5, S5_APOYAR, 6, S6_APOYAR);     delay(100); }

// Ejecuta UNA fase del ciclo de marcha y avanza a la siguiente.
// Se llama en cada vuelta del loop mientras "caminando" sea true:
//   0-2: grupo A se levanta, se adelanta y apoya (B empuja)
//   3-5: lo mismo con el grupo B
void ejecutarCaminata() {

  switch (faseCaminata) {

    case 0:
      levantarGrupoA();
      faseCaminata = 1;
      break;

    case 1:
      moverPatasHorizontales(
        calcularDestino(0, true,  modoCaminata),
        calcularDestino(1, false, modoCaminata),
        calcularDestino(2, false, modoCaminata),
        calcularDestino(3, true,  modoCaminata)
      );
      delay(100);
      faseCaminata = 2;
      break;

    case 2:
      bajarGrupoA();
      faseCaminata = 3;
      break;

    case 3:
      levantarGrupoB();
      faseCaminata = 4;
      break;

    case 4:
      moverPatasHorizontales(
        calcularDestino(0, false, modoCaminata),
        calcularDestino(1, true,  modoCaminata),
        calcularDestino(2, true,  modoCaminata),
        calcularDestino(3, false, modoCaminata)
      );
      delay(100);
      faseCaminata = 5;
      break;

    case 5:
      bajarGrupoB();
      faseCaminata = 0;
      break;
  }
}


// ============================================================
// HERRAMIENTAS PARA LAS ACCIONES
// ============================================================

// Sentido de levantar de cada servo vertical (S4..S7): -1 baja, +1 sube
const int DIR_V[4] = { -1, 1, -1, 1 };

// Ángulo del servo vertical de la pata "pata" separado "alzar" grados del centro
int angV(int pata, int alzar) {
  return CENTRO + DIR_V[pata] * alzar;
}

// Si llega algo por el puerto serie durante una acción, la interrumpe.
bool cancelado() {

  if (animando && Serial.available()) {
    delay(30);
    while (Serial.available()) Serial.read();   // descarta lo recibido
    cancelar = true;
  }

  return cancelar;
}

void copiarActual(int p[8]) {
  for (int i = 0; i < SERVOS_USADOS; i++) p[i] = anguloActual[i];
}

void poseBase(int p[8]) {
  for (int i = 0; i < SERVOS_USADOS; i++) p[i] = CENTRO;
}

// Lleva los 8 servos a la pose "destino" a la vez, con "retardo" ms por grado.
// Se corta si se cancela la acción.
void moverA(const int destino[8], int retardo) {

  int inicio[8];
  int fin[8];
  int distanciaMax = 0;

  for (int i = 0; i < SERVOS_USADOS; i++) {
    inicio[i] = anguloActual[i];
    fin[i] = constrain(destino[i], 0, 180);
    int d = abs(fin[i] - inicio[i]);
    if (d > distanciaMax) distanciaMax = d;
  }

  if (distanciaMax == 0)
    return;

  for (int paso = 1; paso <= distanciaMax; paso++) {

    if (cancelado())
      return;

    for (int i = 0; i < SERVOS_USADOS; i++) {
      setServoAngleInstant(i, inicio[i] + (fin[i] - inicio[i]) * paso / distanciaMax);
    }

    delay(retardo);
  }
}

// Espera que además permite cancelar
void pausa(int ms) {
  cancelado();
  delay(ms);
}

void irAEspera(int retardo) {
  int p[8];
  poseBase(p);
  moverA(p, retardo);
}

// Corta la señal de los servos (quedan sin fuerza, no consumen)
void soltarServos() {
  for (uint8_t ch = 0; ch < SERVOS_USADOS; ch++) pwm.setPWM(ch, 0, 4096);
}


// ============================================================
// ACCIONES
// ============================================================

void accionAcostar() {
  int p[8];
  for (int i = 0; i < 4; i++) { p[i] = CENTRO; p[4 + i] = angV(i, ALZAR_ACOSTAR); }
  moverA(p, VEL_LENTA);
}

void accionDormir() {
  accionAcostar();
  if (!cancelado()) { delay(500); soltarServos(); }
}

void accionFlexiones() {

  int arriba[8], abajo[8];
  poseBase(arriba);
  poseBase(abajo);

  for (int i = 0; i < 4; i++) abajo[4 + i] = angV(i, ALZAR_FLEXION);

  moverA(arriba, VEL_NORMAL);

  for (int r = 0; r < 5 && !cancelado(); r++) {
    moverA(abajo, VEL_NORMAL);
    pausa(150);
    moverA(arriba, VEL_NORMAL);
    pausa(150);
  }
}

// Levanta la pata 3 y la agita
void accionSaludar() {

  int p[8];
  poseBase(p);
  moverA(p, VEL_NORMAL);

  p[7] = angV(3, ALZAR_SALUDO);
  moverA(p, VEL_NORMAL);

  p[3] = SALUDO_A;
  moverA(p, VEL_RAPIDA);

  for (int k = 0; k < 4 && !cancelado(); k++) {
    p[3] = SALUDO_B;
    moverA(p, VEL_RAPIDA);
    p[3] = SALUDO_A;
    moverA(p, VEL_RAPIDA);
  }
}

// Una pata se levanta y da dos golpes hacia adelante
void golpear(int pata) {

  int p[8];
  copiarActual(p);

  int h = pata;       // servo horizontal
  int v = 4 + pata;   // servo vertical

  p[v] = angV(pata, ALZAR_GOLPE);
  moverA(p, VEL_RAPIDA);

  for (int k = 0; k < 2 && !cancelado(); k++) {
    p[h] = ADELANTE_VALS[pata];
    moverA(p, VEL_RAPIDA);
    p[h] = CENTRO;
    moverA(p, VEL_RAPIDA);
  }

  p[v] = CENTRO;
  moverA(p, VEL_RAPIDA);
}

void accionPelea() {
  int p[8];
  poseBase(p);
  moverA(p, VEL_NORMAL);
  for (int r = 0; r < 3 && !cancelado(); r++) { golpear(2); golpear(3); }
}

// Alterna dos pares de patas en diagonal (A: 0 y 3, B: 1 y 2)
void accionBaile1() {

  int base[8], A[8], B[8];
  poseBase(base); poseBase(A); poseBase(B);

  A[4] = angV(0, ALZAR_BAILE);
  A[7] = angV(3, ALZAR_BAILE);
  B[5] = angV(1, ALZAR_BAILE);
  B[6] = angV(2, ALZAR_BAILE);

  moverA(base, VEL_NORMAL);

  for (int r = 0; r < 8 && !cancelado(); r++) {
    moverA(A, VEL_RAPIDA);
    moverA(B, VEL_RAPIDA);
  }
}

// Se balancea de lado a lado y termina con rebotes
void accionBaile2() {

  int base[8], izq[8], der[8], rebote[8];
  poseBase(base); poseBase(izq); poseBase(der); poseBase(rebote);

  izq[4] = angV(0, ALZAR_BAILE);
  izq[6] = angV(2, ALZAR_BAILE);
  der[5] = angV(1, ALZAR_BAILE);
  der[7] = angV(3, ALZAR_BAILE);

  for (int i = 0; i < 4; i++) rebote[4 + i] = angV(i, ALZAR_BAILE / 2);

  moverA(base, VEL_NORMAL);

  for (int r = 0; r < 6 && !cancelado(); r++) {
    moverA(izq, 8);
    pausa(80);
    moverA(der, 8);
    pausa(80);
  }

  moverA(base, VEL_NORMAL);

  for (int r = 0; r < 4 && !cancelado(); r++) {
    moverA(rebote, 8);
    moverA(base, 8);
  }
}

// Gira un poco a la izquierda y luego a la derecha
void accionBaile3() {

  int p[8];
  poseBase(p);
  moverA(p, VEL_NORMAL);

  for (int giro = 0; giro < 2 && !cancelado(); giro++) {

    modoCaminata = (giro == 0) ? MODO_IZQUIERDA : MODO_DERECHA;
    faseCaminata = 0;

    for (int f = 0; f < 12 && !cancelado(); f++)
      ejecutarCaminata();
  }

  faseCaminata = 0;
}

// Ejecuta una acción con control de estado.
// volverAEspera: al terminar, vuelve a la pose de espera (90°).
void ejecutarAnimacion(const char* nombre, void (*accion)(), bool volverAEspera) {

  Serial.println();
  Serial.print("ACCION: ");
  Serial.println(nombre);

  cancelar = false;
  animando = true;

  accion();

  animando = false;

  if (cancelar) {
    Serial.println("Accion interrumpida.");
    cancelar = false;
  }

  if (volverAEspera)
    irAEspera(VEL_NORMAL);

  Serial.println("Listo.");
}


// ============================================================
// CONTROL DE MOVIMIENTO
// ============================================================

void iniciarMovimiento(int modo, String nombre) {

  if (caminando) {
    Serial.println("El robot ya esta caminando.");
    return;
  }

  caminando = true;
  modoCaminata = modo;
  faseCaminata = 0;

  Serial.print("MOVIMIENTO INICIADO: ");
  Serial.println(nombre);
}

void detenerCaminata() {
  caminando = false;
  Serial.println("CAMINATA DETENIDA.");
}

// Las acciones no se pueden lanzar mientras camina
bool ocupado() {

  if (caminando) {
    Serial.println("El robot esta caminando. Escribi 'parar' antes.");
    return true;
  }

  return false;
}

// Centra todos los canales a 90°
void centro() {

  caminando = false;
  Serial.println("Centrando...");

  for (uint8_t ch = 0; ch < NUM_SERVOS; ch++)
    setServoAngleSuave(ch, CENTRO);

  Serial.println("Todos los servos en 90°.");
}


// ============================================================
// COMANDOS
// ============================================================
//
// Lo usan tanto el Monitor Serie como la página web.
// Devuelve un texto corto que la web muestra como estado.
//
// Comandos:
//   Marcha:   caminar, atras, izquierda, derecha, girarizq, girarder, parar
//   Posturas: espera (standby), centro, acostar, dormir
//   Acciones: saludar, flexiones, pelea, baile1, baile2, baile3
//   Manual:   "sN angulo" (ej: s0 90)

String procesarComando(String cmd) {

  cmd.trim();
  cmd.toLowerCase();

  if (cmd.length() == 0)
    return "vacio";

  if (cmd == "centro")    { centro(); return "centrado"; }

  if (cmd == "caminar")   { iniciarMovimiento(MODO_ADELANTE, "ADELANTE"); return "caminando adelante"; }
  if (cmd == "atras")     { iniciarMovimiento(MODO_ATRAS, "ATRAS"); return "caminando atras"; }
  if (cmd == "izquierda") { iniciarMovimiento(MODO_LADO_IZQ, "LATERAL IZQUIERDA"); return "lateral izquierda"; }
  if (cmd == "derecha")   { iniciarMovimiento(MODO_LADO_DER, "LATERAL DERECHA"); return "lateral derecha"; }
  if (cmd == "girarizq")  { iniciarMovimiento(MODO_IZQUIERDA, "GIRO IZQUIERDA"); return "girando izquierda"; }
  if (cmd == "girarder")  { iniciarMovimiento(MODO_DERECHA, "GIRO DERECHA"); return "girando derecha"; }
  if (cmd == "parar")     { detenerCaminata(); return "detenido"; }

  if (cmd == "espera" || cmd == "standby") {
    caminando = false;
    irAEspera(VEL_NORMAL);
    return "en espera";
  }

  if (cmd == "acostar")   { if (ocupado()) return "ocupado"; ejecutarAnimacion("ACOSTAR", accionAcostar, false); return "acostado"; }
  if (cmd == "dormir")    { if (ocupado()) return "ocupado"; ejecutarAnimacion("DORMIR", accionDormir, false); return "durmiendo"; }
  if (cmd == "saludar")   { if (ocupado()) return "ocupado"; ejecutarAnimacion("SALUDAR", accionSaludar, true); return "saludo"; }
  if (cmd == "flexiones") { if (ocupado()) return "ocupado"; ejecutarAnimacion("FLEXIONES", accionFlexiones, true); return "flexiones"; }
  if (cmd == "pelea")     { if (ocupado()) return "ocupado"; ejecutarAnimacion("PELEA", accionPelea, true); return "pelea"; }
  if (cmd == "baile1")    { if (ocupado()) return "ocupado"; ejecutarAnimacion("BAILE 1", accionBaile1, true); return "baile1"; }
  if (cmd == "baile2")    { if (ocupado()) return "ocupado"; ejecutarAnimacion("BAILE 2", accionBaile2, true); return "baile2"; }
  if (cmd == "baile3")    { if (ocupado()) return "ocupado"; ejecutarAnimacion("BAILE 3", accionBaile3, true); return "baile3"; }

  // Control manual: "sN angulo"
  if (cmd.length() >= 2 && cmd.charAt(0) == 's' && isDigit(cmd.charAt(1))) {

    int espacio = cmd.indexOf(' ');

    if (espacio == -1)
      return "ERROR: falta el angulo (ej: s0 90)";

    int canal  = cmd.substring(1, espacio).toInt();
    int angulo = cmd.substring(espacio + 1).toInt();

    if (canal < 0 || canal >= NUM_SERVOS)
      return "ERROR: canal invalido";

    if (angulo < 0 || angulo > 180)
      return "ERROR: angulo invalido";

    setServoAngleSuave(canal, angulo);

    return "S" + String(canal) + " -> " + String(angulo) + " grados";
  }

  return "comando no reconocido";
}


// ============================================================
// PÁGINA WEB
// ============================================================
//
// Los botones llaman a /cmd?c=<comando> (mismos comandos del
// puerto serie). El video lo transmite el ESP32-CAM aparte
// (192.168.4.100:81) y se reintenta solo si se corta.

const char PAGINA_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>SpiderKiki</title>
<style>
  :root { color-scheme: dark; }
  body {
    font-family: system-ui, sans-serif;
    background: #14141c;
    color: #f2f2f2;
    margin: 0;
    padding: 16px;
  }
  h1 { font-size: 1.3em; text-align: center; margin: 8px 0 16px; }
  #estado {
    text-align: center;
    min-height: 1.4em;
    color: #8be28b;
    margin-bottom: 8px;
    font-size: 0.9em;
  }
  #video { position: sticky; top: 0; z-index: 5; background: #14141c; padding-bottom: 8px; }
  #cam { display: block; width: 100%; max-width: 480px; margin: 0 auto; border-radius: 10px; background: #000; aspect-ratio: 4/3; }
  .grid {
    display: grid;
    grid-template-columns: repeat(3, 1fr);
    gap: 8px;
    margin-bottom: 20px;
  }
  button {
    padding: 14px 4px;
    border: none;
    border-radius: 10px;
    background: #2a2a3a;
    color: #f2f2f2;
    font-size: 0.95em;
    font-weight: 600;
  }
  button:active { background: #4a4a6a; }
  .parar { background: #7a2a2a; grid-column: span 3; }
  .parar:active { background: #a33; }
  h2 { font-size: 1em; margin: 12px 0 8px; color: #aaa; }
  .manual { display: flex; gap: 8px; align-items: center; margin-bottom: 10px; }
  select, input[type=range] { flex: 1; }
  .angulo { width: 2.5em; text-align: right; }
</style>
</head>
<body>

<h1>🕷️ SpiderKiki</h1>
<div id="estado">Listo</div>
<div id="video"><img id="cam" alt="Cámara"></div>

<h2>Marcha</h2>
<div class="grid">
  <button onclick="enviar('caminar')">Adelante</button>
  <button onclick="enviar('atras')">Atrás</button>
  <button onclick="enviar('girarizq')">Girar Izq</button>
  <button onclick="enviar('izquierda')">Lateral Izq</button>
  <button onclick="enviar('girarder')">Girar Der</button>
  <button onclick="enviar('derecha')">Lateral Der</button>
  <button class="parar" onclick="enviar('parar')">PARAR</button>
</div>

<h2>Posturas y acciones</h2>
<div class="grid">
  <button onclick="enviar('espera')">Espera</button>
  <button onclick="enviar('centro')">Centro</button>
  <button onclick="enviar('acostar')">Acostar</button>
  <button onclick="enviar('dormir')">Dormir</button>
  <button onclick="enviar('saludar')">Saludar</button>
  <button onclick="enviar('flexiones')">Flexiones</button>
  <button onclick="enviar('pelea')">Pelea</button>
  <button onclick="enviar('baile1')">Baile 1</button>
  <button onclick="enviar('baile2')">Baile 2</button>
  <button onclick="enviar('baile3')">Baile 3</button>
</div>

<h2>Control manual</h2>
<div class="manual">
  <select id="servo">
    <option value="0">S0</option>
    <option value="1">S1</option>
    <option value="2">S2</option>
    <option value="3">S3</option>
    <option value="4">S4</option>
    <option value="5">S5</option>
    <option value="6">S6</option>
    <option value="7">S7</option>
  </select>
  <input type="range" id="angulo" min="0" max="180" value="90"
         oninput="document.getElementById('valorAngulo').textContent=this.value"
         onchange="enviarServo()">
  <span id="valorAngulo" class="angulo">90</span>
</div>

<script>
// Envía un comando al robot y muestra la respuesta
function enviar(cmd) {
  document.getElementById('estado').textContent = 'Enviando: ' + cmd + '...';
  fetch('/cmd?c=' + encodeURIComponent(cmd))
    .then(r => r.text())
    .then(t => document.getElementById('estado').textContent = t)
    .catch(() => document.getElementById('estado').textContent = 'Error de conexion');
}

// Slider: manda "sN angulo" al soltar
function enviarServo() {
  const s = document.getElementById('servo').value;
  const a = document.getElementById('angulo').value;
  enviar('s' + s + ' ' + a);
}

// Conecta el video; si se corta, reintenta a los 3 s
function iniciarCam() {
  const img = document.getElementById('cam');
  img.onerror = () => setTimeout(iniciarCam, 3000);
  img.src = 'http://192.168.4.100:81/stream?t=' + Date.now();
}
window.addEventListener('load', iniciarCam);
</script>

</body>
</html>
)HTML";

void manejarRaiz() {
  server.send_P(200, "text/html", PAGINA_HTML);
}

void manejarComando() {

  if (!server.hasArg("c")) {
    server.send(400, "text/plain", "falta parametro c");
    return;
  }

  String resultado = procesarComando(server.arg("c"));

  server.send(200, "text/plain", resultado);
}

void manejarNoEncontrado() {
  server.send(404, "text/plain", "no encontrado");
}

void iniciarWifi() {

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  WiFi.setSleep(false);   // menos latencia en los comandos

  IPAddress ip = WiFi.softAPIP();

  server.on("/", manejarRaiz);
  server.on("/cmd", manejarComando);
  server.onNotFound(manejarNoEncontrado);

  server.begin();

  Serial.println();
  Serial.print("WiFi: ");
  Serial.print(AP_SSID);
  Serial.print(" (sin contraseña) -> http://");
  Serial.println(ip);
}


// ============================================================
// SETUP Y LOOP
// ============================================================

void setup() {

  Serial.begin(115200);
  delay(1000);

  Wire.begin(SDA_PIN, SCL_PIN);

  pwm.begin();
  pwm.setPWMFreq(SERVO_FREQ);
  delay(10);

  // Arranca con todos los servos centrados
  for (uint8_t ch = 0; ch < NUM_SERVOS; ch++) {
    anguloActual[ch] = CENTRO;
    setServoAngleInstant(ch, CENTRO);
  }

  iniciarWifi();

  Serial.println("ROBOT ARAÑA - ESP32-C3 listo. Servos en 90°.");
}

void loop() {

  server.handleClient();   // atiende la página y los comandos web

  if (Serial.available()) {
    String entrada = Serial.readStringUntil('\n');
    Serial.println(procesarComando(entrada));
  }

  if (caminando) {
    ejecutarCaminata();   // una fase por vuelta, así "parar" responde rápido
  }
}