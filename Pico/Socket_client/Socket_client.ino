#include <WiFi.h>
#include <ArduinoHttpClient.h>

const char* ssid = "WiFi SSID";
const char *password = "WiFi Passport";
const char *server = "Server IP";
const uint16_t port = "Server Port";
const int VIBRATION = 6;//<-- GPIO6 在左邊第二個GND下第一個PIN

WiFiClient wifi;

void setup() {
  Serial.begin(115200);
  delay(5000);

  Serial.println("正在連線Wi-Fi...");
  WiFi.begin(ssid, password);
  delay(5000);

  int i = 0;
  while(WiFi.status() != WL_CONNECTED){
    delay(200);
    Serial.print(".");
    i++;
    if(i%20 == 0){
      Serial.println();
      i = 0;
    }
  }

  Serial.println();
  Serial.println("Wi-Fi連線成功!");
  Serial.print("IP 位置: ");
  Serial.println(WiFi.localIP()); 

  while(!(wifi.connect(server, port))){
    Serial.println("Try reconnect to server!");
    delay(500);
  }
  Serial.println("Server connected successful");  
}

void loop() {
  while(wifi.available()) {
    String c = wifi.readStringUntil('\0');
    float val = c.toFloat();
    if (val >= 1000){
      Serial.print("Server sends alert value: ");
      Serial.println(val);
      Serial.println("LED 亮起來 示意 震動馬達震動");
      Serial.println("10秒後自動停止");
      analogWrite(VIBRATION, 255 / 5 * 3);
      delay(10000);
      analogWrite(VIBRATION, 0);
    }
  }
  if (!wifi.connected()) {
    Serial.println();
    Serial.println("disconnecting from server.");
    analogWrite(VIBRATION, 0);
    wifi.stop(); 
    delay(20);
    setup();
  }

  delay(5000);
}