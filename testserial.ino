void setup() {
  Serial.begin(115200);
  delay(3000);

  Serial.println("TEST SERIAL C3");
}

void loop() {
  Serial.println("RUNNING...");
  delay(1000);
}
