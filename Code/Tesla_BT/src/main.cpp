#include <Arduino.h>

/*
 * ESP32-C3-WROOM-02 Bluetooth A2DP přijímač
 *
 * Závislosti (nainstaluj přes Library Manager):
 *   - ESP32-A2DP by Phil Schatzmann (https://github.com/pschatzmann/ESP32-A2DP)
 *   - LiquidCrystal_I2C (nebo HD44780 I2C dle tvého LCD modulu)
 *
 * Board: ESP32C3 Dev Module
 *
 * Piny dle schématu:
 *   I2S_BCK   = IO3
 *   I2S_DIN   = IO4   (DATA OUT do DAC/zesilovače)
 *   I2S_LRCK  = IO5
 *   I2C_SDA   = IO6
 *   I2C_SCL   = IO8
 *   RADIO_VOL = IO18  (ADC - hlasitost)
 *   RADIO_HI  = IO17  (ADC - výšky / treble)
 *   RADIO_LOW = IO15  (ADC - basy / bass)
 *   IO2       = IO16  (ADC - volný / balance nebo jiný parametr)
 *   AUDIO_SEL = IO11  (digitální výstup - výběr zdroje audia)
 *   AMP_SD    = IO12  (digitální výstup - shutdown zesilovače, HIGH = zapnuto)
 */

#include "BluetoothA2DPSink.h"
#include <Wire.h>
#include <LiquidCrystal_I2C.h>

// ── Piny ────────────────────────────────────────────────────────────────────
#define I2S_BCK_PIN 3
#define I2S_DATA_PIN 4
#define I2S_LRCK_PIN 5

#define I2C_SDA_PIN 6
#define I2C_SCL_PIN 8

#define PIN_VOL 18     // ADC – hlasitost
#define PIN_TREBLE 17  // ADC – výšky
#define PIN_BASS 15    // ADC – basy
#define PIN_BALANCE 16 // ADC – balance (IO2 ve schématu)

#define PIN_AUDIO_SEL 11 // HIGH = Bluetooth vstup vybrán
#define PIN_AMP_SD 12    // HIGH = zesilovač aktivní, LOW = shutdown

// ── LCD (I2C adresa typicky 0x27 nebo 0x3F, 16x2) ──────────────────────────
LiquidCrystal_I2C lcd(0x27, 16, 2);

// ── A2DP ────────────────────────────────────────────────────────────────────
BluetoothA2DPSink a2dp_sink;

// ── Globální stav ────────────────────────────────────────────────────────────
String bt_device_name = "";
String bt_track_artist = "";
String bt_track_title = "";
bool connected = false;

int vol_raw = 0;         // 0–4095
uint8_t volume_pct = 80; // 0–100

unsigned long last_adc_read = 0;
unsigned long last_lcd_update = 0;
const unsigned long ADC_INTERVAL = 80;  // ms
const unsigned long LCD_INTERVAL = 500; // ms

// ── Callback: spojení / odpojení ─────────────────────────────────────────────
void on_connection_state_changed(esp_a2d_connection_state_t state, void *)
{
  if (state == ESP_A2D_CONNECTION_STATE_CONNECTED)
  {
    connected = true;
    // Zapni zesilovač
    digitalWrite(PIN_AMP_SD, HIGH);
    Serial.println("[BT] Připojeno");
  }
  else if (state == ESP_A2D_CONNECTION_STATE_DISCONNECTED)
  {
    connected = false;
    bt_device_name = "";
    bt_track_artist = "";
    bt_track_title = "";
    // Ztlum zesilovač při odpojení
    digitalWrite(PIN_AMP_SD, LOW);
    Serial.println("[BT] Odpojeno");
  }
}

// ── Callback: AVRCP metadata (název skladby, interpret) ──────────────────────
void avrc_metadata_callback(uint8_t id, const uint8_t *text)
{
  String s = String((char *)text);
  switch (id)
  {
  case ESP_AVRC_MD_ATTR_TITLE:
    bt_track_title = s;
    Serial.print("[META] Název: ");
    Serial.println(s);
    break;
  case ESP_AVRC_MD_ATTR_ARTIST:
    bt_track_artist = s;
    Serial.print("[META] Interpret: ");
    Serial.println(s);
    break;
  default:
    break;
  }
}

// ── Přečti ADC a aktualizuj hlasitost ────────────────────────────────────────
void read_adc()
{
  vol_raw = analogRead(PIN_VOL); // 0–4095
  int treble = analogRead(PIN_TREBLE);
  int bass = analogRead(PIN_BASS);
  // int balance = analogRead(PIN_BALANCE); // případné využití

  // Mapuj hlasitost 0–100
  volume_pct = map(vol_raw, 0, 4095, 0, 100);

  // Nastav hlasitost A2DP (0–127)
  uint8_t a2dp_vol = map(volume_pct, 0, 100, 0, 127);
  a2dp_sink.set_volume(a2dp_vol);

  // Servisní výpis pro ladění
  Serial.printf("[ADC] VOL=%d%% | TREBLE=%d | BASS=%d\n",
                volume_pct, treble, bass);
}

// ── Obnov LCD ────────────────────────────────────────────────────────────────
void update_lcd()
{
  lcd.clear();

  if (!connected)
  {
    // Hledáme zařízení
    lcd.setCursor(0, 0);
    lcd.print("BT Radio");
    lcd.setCursor(0, 1);
    lcd.print("Ceka na spojeni.");
    return;
  }

  // Řádek 0: interpret (max 16 znaků)
  String artist = bt_track_artist.length() > 0 ? bt_track_artist : "Neznamy";
  if (artist.length() > 16)
    artist = artist.substring(0, 16);
  lcd.setCursor(0, 0);
  lcd.print(artist);

  // Řádek 1: název skladby + hlasitost
  String title = bt_track_title.length() > 0 ? bt_track_title : "---";
  if (title.length() > 12)
    title = title.substring(0, 12);
  String line2 = title;
  // Doplň hlasitost vpravo (formát "xxx%")
  String vol_str = String(volume_pct) + "%";
  while ((line2.length() + vol_str.length()) < 16)
    line2 += " ";
  line2 += vol_str;
  lcd.setCursor(0, 1);
  lcd.print(line2);
}

// ── Setup ────────────────────────────────────────────────────────────────────
void setup()
{
  Serial.begin(115200);
  Serial.println("\n[BOOT] ESP32-C3 BT Radio");

  // Digitální výstupy
  pinMode(PIN_AUDIO_SEL, OUTPUT);
  pinMode(PIN_AMP_SD, OUTPUT);
  digitalWrite(PIN_AUDIO_SEL, HIGH); // Vyber Bluetooth vstup
  digitalWrite(PIN_AMP_SD, LOW);     // Zesilovač zatím vypnut

  // ADC vstupy (výchozí, není třeba pinMode pro analogRead)
  // Volitelně nastav atenuaci pro rozsah 0–3,3 V
  analogSetAttenuation(ADC_11db);

  // I2C + LCD
  Wire.begin(I2C_SDA_PIN, I2C_SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("BT Radio");
  lcd.setCursor(0, 1);
  lcd.print("Inicializace...");

  // I2S konfigurace pro A2DP
  i2s_pin_config_t pin_cfg = {
      .mck_io_num = I2S_PIN_NO_CHANGE,
      .bck_io_num = I2S_BCK_PIN,
      .ws_io_num = I2S_LRCK_PIN,
      .data_out_num = I2S_DATA_PIN,
      .data_in_num = I2S_PIN_NO_CHANGE};
  a2dp_sink.set_pin_config(pin_cfg);

  // Callbacks
  a2dp_sink.set_on_connection_state_changed(on_connection_state_changed);
  a2dp_sink.set_avrc_metadata_callback(avrc_metadata_callback);

  // Spusť Bluetooth – název viditelný pro telefon
  a2dp_sink.start("ESP32 Radio");

  Serial.println("[BT] Bluetooth spusten, ceka na spojeni...");
}

// ── Loop ─────────────────────────────────────────────────────────────────────
void loop()
{
  unsigned long now = millis();

  if (now - last_adc_read >= ADC_INTERVAL)
  {
    last_adc_read = now;
    read_adc();
  }

  if (now - last_lcd_update >= LCD_INTERVAL)
  {
    last_lcd_update = now;
    update_lcd();
  }
}