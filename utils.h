// Loop and blink forever
void errorLoop(int pin) 
{
  while (true) {
    for (int i=0; i<3; i++) {
      digitalWrite(pin, LOW);
      delay(100);
      digitalWrite(pin, HIGH);
      delay(100);
    }
    delay(1000);
  }
}

String getContentType(String filename)
{
  if (filename.endsWith(".html") || filename.endsWith(".htm")) return "text/html";
  else if (filename.endsWith(".css")) return "text/css";
  else if (filename.endsWith(".js"))  return "application/javascript";
  else if (filename.endsWith(".ico")) return "image/x-icon";
  else if (filename.endsWith(".png")) return "image/png";
  return "text/plain";
}

String leadingZero(int num)
{
  if (num < 10) return "0" + String(num);
  return String(num);
}

double clamp(double value, double min, double max)
{
  if (value > max) return max;
  if (value < min) return min;
  return value;
}