#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS 2
OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

int targetTemp = 30;  // 默认30°C
unsigned long boostStartTime = 0;
bool boostMode = false;

void setup() {
  Serial.begin(9600);
  delay(1000);
  
  Serial.println("=== Three-Level Temperature Control ===");
  sensors.begin();
  
  pinMode(3, OUTPUT);
  pinMode(5, OUTPUT);
  pinMode(9, OUTPUT);
  pinMode(10, OUTPUT);
  
  analogWrite(3, 0);
  analogWrite(5, 0);
  analogWrite(9, 0);
  analogWrite(10, 0);
  
  Serial.println("Commands: T30 (Low), T34 (Mid), T38 (High)");
}

void loop() {
  // 接收Serial命令
  if (Serial.available() > 0) {
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();
    
    if (cmd == "T30") {
      targetTemp = 30;
      boostMode = false;
      Serial.println("-> Low mode (30C)");
    } else if (cmd == "T34") {
      targetTemp = 34;
      boostMode = false;
      Serial.println("-> Mid mode (34C)");
    } else if (cmd == "T38") {
      targetTemp = 38;
      boostMode = true;
      boostStartTime = millis();
      Serial.println("-> High mode (38C, auto-downgrade in 15min)");
    }
  }
  
  // 读取温度
  sensors.requestTemperatures();
  float temp = sensors.getTempCByIndex(0);
  
  // High档15分钟后自动降到Mid档
  if (boostMode && (millis() - boostStartTime > 900000)) {  // 15分钟
    targetTemp = 34;
    boostMode = false;
    Serial.println("-> Auto-downgrade to Mid mode");
  }
  
  // 温度控制逻辑
  int pwm = 0;
  if (temp < targetTemp - 0.5) {
    // 温度过低，加热
    pwm = 255;
  } else if (temp >= targetTemp) {
    // 达到目标，停止
    pwm = 0;
  } else {
    // 在目标附近，细微调节
    pwm = map(int((targetTemp - temp) * 100), 0, 50, 0, 100);
  }
  
  // 输出PWM到4个MOS模块
  analogWrite(3, pwm);
  analogWrite(5, pwm);
  analogWrite(9, pwm);
  analogWrite(10, pwm);
  
  // 输出状态
  Serial.print("Temp: ");
  Serial.print(temp, 1);
  Serial.print("C | Target: ");
  Serial.print(targetTemp);
  Serial.print("C | PWM: ");
  Serial.println(pwm);
  
  delay(1000);
}
