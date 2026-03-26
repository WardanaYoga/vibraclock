#include <ThreeWire.h>
#include <RtcDS1302.h>

ThreeWire myWire(3, 2, 4);
RtcDS1302<ThreeWire> Rtc(myWire);

void setup() {
  Serial.begin(115200);
  delay(2000);

  Serial.println("RTC TEST");

  Rtc.Begin();

  if (!Rtc.GetIsRunning()) {
    Serial.println("RTC tidak jalan");
    Rtc.SetIsRunning(true);
  }

  // SET SEKALI SAJA
  RtcDateTime now = RtcDateTime(__DATE__, __TIME__);
  Rtc.SetDateTime(now);
}

void loop() {
  RtcDateTime now = Rtc.GetDateTime();

  Serial.printf("%02d:%02d:%02d\n",
    now.Hour(), now.Minute(), now.Second());

  delay(1000);
}
