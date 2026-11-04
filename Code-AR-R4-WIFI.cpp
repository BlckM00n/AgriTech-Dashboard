#include <WiFiS3.h>
#include <PubSubClient.h>
#include <LiquidCrystal_I2C.h>
#include <DHT.h>

// ---------- Configuración DHT22 ----------
#define DHTPIN 2          // Pin digital para DHT22
#define DHTTYPE DHT22

DHT dht(DHTPIN, DHTTYPE);

// ---------- Configuración LEDs (Semáforo) ----------
#define PIN_LED_VERDE    3
#define PIN_LED_AMARILLO 4
#define PIN_LED_ROJO     5

// ---------- Configuración Red ----------
const char* WIFI_SSID  = "Moon Wifi";
const char* WIFI_CLAVE = "BlackMoon";
const char* BROKER     = "broker.hivemq.com";
const char* TEMA       = "duoc/siy6122/fertirriego/datos";
const char* ID_CLIENTE = "nodo-fertirriego-grupo01";

// ---------- Configuración Sensores (5 variables) ----------
const char* NOMBRES[5] = {"temp_aire", "hum_aire", "hum_suelo", "ph", "ce"};
const char* UNID[5]    = {"C", "%", "%", "", "dS/m"};

// Pines analógicos
const int PIN_SUELO = A0; 
const int PIN_PH    = A1; 
const int PIN_CE    = A2; 

// Calibración Sensor de Suelo
const int VALOR_SECO   = 1023; // Raw en aire (0% humedad)
const int VALOR_HUMEDO = 350;  // Raw en agua (100% humedad)

// ---------- Tiempos (ms) ----------
const unsigned long T_LECTURA    = 2000;
const unsigned long T_PAGINA     = 5000;
const unsigned long T_MQTT       = 3000;
const unsigned long T_REINT_WIFI = 30000;
const unsigned long T_REINT_MQTT = 10000;

LiquidCrystal_I2C lcd(0x27, 16, 2);
WiFiClient red;
PubSubClient mqtt(red);

float valores[5];
const char* estados[5];

int pagina = 0;
unsigned long tLectura = 0, tPagina = 0, tMqtt = 0;
unsigned long tIntentoWiFi = 0, tIntentoMqtt = 0;
bool wifiIntentado = false;

// ---------- Estabilizador ADC ----------
int analogReadLimpio(int pin) {
  analogRead(pin);         // Lectura de descarte para vaciar el bus del ADC
  delayMicroseconds(50);   // Pausa breve de estabilización
  return analogRead(pin);  // Lectura real limpia
}

// ---------- Captura de datos ----------
float leer(int i) {
  switch (i) {
    case 0: { // Temperatura Aire (DHT22)
      float t = dht.readTemperature();
      return isnan(t) ? 0.0 : t;
    }
    case 1: { // Humedad Aire (DHT22)
      float h = dht.readHumidity();
      return isnan(h) ? 0.0 : h;
    }
    case 2: { // Humedad Suelo (Pin A0)
      int raw = analogReadLimpio(PIN_SUELO);
      float pct = map(raw, VALOR_SECO, VALOR_HUMEDO, 0, 100);
      return constrain(pct, 0.0, 100.0);
    }
    case 3: { // pH (Pin A1)
      int raw = analogReadLimpio(PIN_PH);
      return (raw / 1023.0) * 14.0;
    }
    default: { // CE (Pin A2)
      int raw = analogReadLimpio(PIN_CE);
      return (raw / 1023.0) * 3.0;
    }
  }
}

// ---------- Toma de decisiones ----------
const char* estado(int i, float v) {
  switch (i) {
    case 0: return v >= 35 ? "ALERTA TEMP ALTA" : "Estado: NORMAL";
    case 1: return v < 30 ? "! AIRE SECO" : v > 80 ? "! AIRE SATURADO" : "Hum Aire OK";
    case 2: return v < 30 ? "! REGAR SUELO" : v > 80 ? "! ENCHARCADO" : "Hum Suelo OK";
    case 3: return v < 5 ? "! ALCALINIZAR" : v > 7.2 ? "! ACIDIFICAR"
                 : (v >= 5.5 && v <= 6.5) ? "RANGO OPTIMO" : "FUERA DE OPTIMO";
    default: return v > 3 ? "ALERTA: Salino"
                 : (v >= 1.5 && v <= 2.5) ? "Rango: Optimo" : "Fuera de Optimo";
  }
}

// ---------- Control del Semáforo LED ----------
void actualizarLEDs() {
  bool alertaCritica = (valores[0] >= 35.0) || (valores[2] < 30.0) || (valores[2] > 80.0) || (valores[4] > 3.0);
  bool advertencia   = (valores[1] < 30.0) || (valores[1] > 80.0) || (valores[3] < 5.5) || (valores[3] > 6.5) || (valores[4] < 1.5) || (valores[4] > 2.5);

  if (alertaCritica) {
    digitalWrite(PIN_LED_ROJO, HIGH);
    digitalWrite(PIN_LED_AMARILLO, LOW);
    digitalWrite(PIN_LED_VERDE, LOW);
  } else if (advertencia) {
    digitalWrite(PIN_LED_ROJO, LOW);
    digitalWrite(PIN_LED_AMARILLO, HIGH);
    digitalWrite(PIN_LED_VERDE, LOW);
  } else {
    digitalWrite(PIN_LED_ROJO, LOW);
    digitalWrite(PIN_LED_AMARILLO, LOW);
    digitalWrite(PIN_LED_VERDE, HIGH);
  }
}

// ---------- LCD sin parpadeo ----------
String ajustar(String s, int largo) {
  while ((int)s.length() < largo) s += ' ';
  return s.substring(0, largo);
}

void dibujarLCD() {
  int i = pagina;
  String l0 = String(NOMBRES[i]) + ": " + String(valores[i], 1) + UNID[i];
  lcd.setCursor(0, 0);
  lcd.print(ajustar(l0, 15));
  lcd.print(mqtt.connected() ? '*' : '-');
  lcd.setCursor(0, 1);
  lcd.print(ajustar(estados[i], 16));
}

// ---------- Red no bloqueante ----------
void gestionarRed() {
  if (WiFi.status() != WL_CONNECTED) {
    if (!wifiIntentado || millis() - tIntentoWiFi > T_REINT_WIFI) {
      tIntentoWiFi = millis();
      wifiIntentado = true;
      WiFi.begin(WIFI_SSID, WIFI_CLAVE);
    }
    return;
  }
  if (!mqtt.connected()) {
    if (millis() - tIntentoMqtt > T_REINT_MQTT) {
      tIntentoMqtt = millis();
      mqtt.connect(ID_CLIENTE);
    }
  } else {
    mqtt.loop();
  }
}

// ---------- Publicación MQTT unificada ----------
void publicarMQTT() {
  if (!mqtt.connected()) return;

  String msg = "{";
  msg += "\"tempAire\":";     msg += String(valores[0], 2);
  msg += ",\"humedadAire\":";  msg += String(valores[1], 2);
  msg += ",\"humedadSuelo\":"; msg += String(valores[2], 2);
  msg += ",\"pH\":";           msg += String(valores[3], 2);
  msg += ",\"CE\":";           msg += String(valores[4], 2);
  msg += ",\"nivelAgua\":80";
  msg += "}";

  mqtt.publish(TEMA, msg.c_str());
}

void setup() {
  Serial.begin(115200);

  pinMode(PIN_LED_VERDE, OUTPUT);
  pinMode(PIN_LED_AMARILLO, OUTPUT);
  pinMode(PIN_LED_ROJO, OUTPUT);

  dht.begin();
  
  lcd.init();
  lcd.backlight();
  lcd.print("Fertirriego IoT");
  lcd.setCursor(0, 1);
  lcd.print("Modo Hibrido");

  mqtt.setServer(BROKER, 1883);

  delay(1500);
  for (int i = 0; i < 5; i++) { 
    valores[i] = leer(i); 
    estados[i] = estado(i, valores[i]); 
  }
  
  actualizarLEDs();
  tLectura = tPagina = tMqtt = millis();
  dibujarLCD();
}

void loop() {
  gestionarRed();
  unsigned long ahora = millis();

  // 1) Lectura de sensores cada 2 s
  if (ahora - tLectura >= T_LECTURA) {
    tLectura = ahora;
    for (int i = 0; i < 5; i++) {
      valores[i] = leer(i);
      estados[i] = estado(i, valores[i]);
      Serial.print(NOMBRES[i]); Serial.print(": "); Serial.print(valores[i], 2);
      Serial.print(UNID[i]);    Serial.print(" | "); Serial.println(estados[i]);
    }
    Serial.println("---");
    actualizarLEDs();
    dibujarLCD();
  }

  // 2) Rotación de página LCD cada 5 s
  if (ahora - tPagina >= T_PAGINA) {
    tPagina = ahora;
    pagina = (pagina + 1) % 5;
    dibujarLCD();
  }

  // 3) Publicación MQTT cada 3 s
  if (ahora - tMqtt >= T_MQTT) {
    tMqtt = ahora;
    if (mqtt.connected()) publicarMQTT();
  }
}
