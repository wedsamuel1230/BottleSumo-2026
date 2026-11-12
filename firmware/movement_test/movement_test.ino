#include <Arduino.h>

#define LPWM 10
#define LDIR 11
#define RPWM 13
#define RDIR 14
#define LED1 48
#define LED2 47

const uint8_t Loffset = 6;

void forward(uint8_t speed){
  analogWrite(LPWM,(speed - Loffset));
  analogWrite(RPWM,speed);
  digitalWrite(LDIR, 1);
  digitalWrite(RDIR, 0);
}

void backward(uint8_t speed){
  analogWrite(LPWM,(speed - Loffset));
  analogWrite(RPWM,speed);
  digitalWrite(LDIR, 0);
  digitalWrite(RDIR, 1);
}

void Right_ward(uint8_t speed){
  analogWrite(LPWM,(speed - Loffset));
  analogWrite(RPWM,speed);
  digitalWrite(LDIR, 1);
  digitalWrite(RDIR, 1);
}

void Left_ward(uint8_t speed){
  analogWrite(LPWM,(speed - Loffset));
  analogWrite(RPWM,speed);
  digitalWrite(LDIR, 0);
  digitalWrite(RDIR, 0);
}

void stop(){
  digitalWrite(LPWM, LOW);
  digitalWrite(RPWM, LOW);
}

bool hasRun = false;

void setup() {
    Serial.begin(115200);
    pinMode(LPWM, OUTPUT);
    pinMode(LDIR, OUTPUT);
    pinMode(RPWM, OUTPUT);
    pinMode(RDIR, OUTPUT);
    pinMode(LED1, OUTPUT);
    pinMode(LED2, OUTPUT);
    digitalWrite(LED1, HIGH);
    digitalWrite(LED2, HIGH);

}

void loop() {
  Serial.println("Car is moving forward");
  forward(255);
  delay(1000);
  Serial.println("Car is stopping");
  stop();
  delay(2000);
  Serial.println("Car is moving leftward");
  Left_ward(255);
  delay(1000);
  Serial.println("Car is stopping");
  stop();
  delay(2000);
  Serial.println("Car is moving rightward");
  Right_ward(255);
  delay(1000);
  Serial.println("Car is stopping");
  stop();
  delay(2000);
  Serial.println("Car is moving backward");
  backward(255);
  delay(1000);
  Serial.println("Car is stopping");
  stop();
  delay(2000);
}
