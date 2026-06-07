#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  Serial.println("=== DS18B20 Test ===");
  sensors.begin();
  
  int count = sensors.getDeviceCount();
  Serial.print("Found sensors: ");
  Serial.println(count);
  
  if (count == 0) {
    Serial.println("ERROR: No sensor found!");
  } else {
    Serial.println("Sensor OK!");
  }
}

void loop() {
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);
  
  Serial.print("Temperature: ");
  Serial.print(temp);
  Serial.println(" C");
  
  delay(1000);
}
