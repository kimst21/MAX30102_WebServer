#include <Wire.h>
#include "MAX30105.h"
#include "spo2_algorithm.h"
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
MAX30105 particleSensor;
#define MAX_BRIGHTNESS 255
// WiFi config
const char *SSID = " "; // 공유기 ID입력
const char *PWD = " ";  // 공유기 비번입력
// Web server running on port 80
AsyncWebServer server(80);
// Async Events
AsyncEventSource events("/events");
const char index_html[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">  <title>ESP32 Heart</title>
  <script type="text/javascript" src="https://www.gstatic.com/charts/loader.js"></script> 
  <script language="javascript">
      
    google.charts.load('current', {'packages':['gauge']});
    google.charts.setOnLoadCallback(drawChart);
  
    var chartHR;
    var chartSPOO2;
    var optionsHR;
    var optionsSPO2;
    var dataHR;
    var dataSPO2;
    function drawChart() {
        dataHR = google.visualization.arrayToDataTable([
          ['Label', 'Value'],
          ['HR', 0]
        ]);
    
        optionsHR = {
          min:40, max:230,
          width: 400, height: 120,
          greenColor: '#68A2DE',
          greenFrom: 40, greenTo: 90,
          yellowFrom: 91, yellowTo: 150,
          redFrom:151, redTo:230,
          minorTicks: 5
        };
    
        chartHR = new google.visualization.Gauge(document.getElementById('chart_div_hr'));
    
        
        dataSPO2 = google.visualization.arrayToDataTable([
          ['Label', 'Value'],
          ['SPO2', 0]
        ]);
    
        optionsSPO2 = {
          min:0, max:100,
          width: 400, height: 120,
          greenColor: '#68A2DE',
          greenFrom: 0, greenTo: 100,
          minorTicks: 5
        };
    
        chartSPO2 = new google.visualization.Gauge(document.getElementById('chart_div_spo2'));
    
    
        chartHR.draw(dataHR, optionsHR);
        chartSPO2.draw(dataSPO2, optionsSPO2);
        
   }
   
   if (!!window.EventSource) {
     var source = new EventSource('/events');
     source.addEventListener('open', function(e) {
        console.log("Events Connected");
     }, false);
     source.addEventListener('error', function(e) {
        if (e.target.readyState != EventSource.OPEN) {
          console.log("Events Disconnected");
        }
     }, false);
    source.addEventListener('message', function(e) {
        console.log("message", e.data);
    }, false);
    source.addEventListener('hr', function(e) {
        dataHR.setValue(0,1, e.data);
        chartHR.draw(dataHR, optionsHR);
    }, false);
 
   source.addEventListener('spo2', function(e) {
        dataSPO2.setValue(0,1, e.data);
        chartSPO2.draw(dataSPO2, optionsSPO2);
    }, false);
}
  </script>
  
  <style>
    .card {
      box-shadow: 0 4px 8px 0 rgba(0,0,0,0.2);
      transition: 0.3s;
      border-radius: 5px; /* 5px rounded corners */
   }
   /* On mouse-over, add a deeper shadow */
  .card:hover {
    box-shadow: 0 8px 16px 0 rgba(0,0,0,0.2);
  }
  /* Add some padding inside the card container */
  .container {
    padding: 2px 16px;
  }
  </style>
</head>
<body>
  <h2>ESP32 Heart</h2>
  <div class="content">
    <div class="card">
     <div class="container">
       <div id="chart_div_hr" style="width: 400px; height: 120px;"></div>
       <div id="chart_div_spo2" style="width: 400px; height: 120px;"></div>
  </div>
</div> 
  </div>
</body>
</html>
)rawliteral";
uint32_t irBuffer[100]; //적외선 LED 센서 데이터
uint32_t redBuffer[100];  //빨간색 LED 센서 데이터
int32_t bufferLength; //데이터 길이
int32_t spo2; //SPO2 값
int8_t validSPO2; //표시기를 통해 SPO2 계산이 유효한지 확인할 수 있습니다.
int32_t heartRate; //심박수 값
int8_t validHeartRate; //표시기를 통해 심박수 계산이 유효한지 확인할 수 있습니다.
byte pulseLED = 45; //PWM 핀에 있어야 합니다.
byte readLED = 48; //데이터를 읽을 때마다 깜박임

 // 지난번 SSE
long last_sse = 0;
void connectToWiFi() {
  Serial.print("Connecting to ");
  Serial.println(SSID);
  
  WiFi.begin(SSID, PWD);
  
  while (WiFi.status() != WL_CONNECTED) {
    Serial.print(".");
    delay(500);
    //ESP32를 절전 모드로 설정할 수도 있습니다.
  }
 
  Serial.print("Connected. IP: ");
  Serial.println(WiFi.localIP());
}
void configureEvents() {
  events.onConnect([](AsyncEventSourceClient *client){
    if(client->lastId()){
      Serial.printf("Client connections. Id: %u\n", client->lastId());
    }
    //  재연결 지연을 1초로 설정
    client->send("hello from ESP32",NULL,millis(),1000);
  });
  server.addHandler(&events);
}
void setup()
{
  Serial.begin(115200); // 초당 115200비트로 직렬 통신을 초기화합니다:
  pinMode(pulseLED, OUTPUT);
  pinMode(readLED, OUTPUT);
  // Initialize sensor
  if (!particleSensor.begin(Wire, I2C_SPEED_FAST)) //기본 I2C 포트, 400kHz 속도 사용
  {
    Serial.println(F("MAX30105 was not found. Please check wiring/power."));
    while (1);
  }
 
  byte ledBrightness = 60; //Options: 0=Off to 255=50mA
  byte sampleAverage = 4; //Options: 1, 2, 4, 8, 16, 32
  byte ledMode = 2; //Options: 1 = Red only, 2 = Red + IR, 3 = Red + IR + Green
  byte sampleRate = 100; //Options: 50, 100, 200, 400, 800, 1000, 1600, 3200
  int pulseWidth = 411; //Options: 69, 118, 215, 411
  int adcRange = 4096; //Options: 2048, 4096, 8192, 16384
 
  particleSensor.setup(ledBrightness, sampleAverage, ledMode, sampleRate, pulseWidth, adcRange); //Configure sensor with these settings
  connectToWiFi();
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send_P(200, "text/html", index_html, NULL);
  });
  configureEvents();
  server.begin();
}
void loop()
{
  bufferLength = 100; //버퍼 길이 100은 25sps로 실행되는 4초 분량의 샘플을 저장
  //처음 100개의 샘플을 읽고 신호 범위를 결정합니다.
  for (byte i = 0 ; i < bufferLength ; i++)
  {
    while (particleSensor.available() == false) //새로운 데이터가 있나요?
      particleSensor.check(); //센서에 새로운 데이터가 있는지 확인
    redBuffer[i] = particleSensor.getRed();
    irBuffer[i] = particleSensor.getIR();
    particleSensor.nextSample(); //이 샘플은 끝났으므로 다음 샘플로 이동합니다.
   //Serial.print(F("red="));
   // Serial.print(redBuffer[i], DEC);
   // Serial.print(F(", ir="));
   // Serial.println(irBuffer[i], DEC);
  }
  //처음 100개의 샘플(샘플의 첫 4초) 후 심박수 및 SpO2를 계산합니다.
  maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);
  //MAX30102에서 지속적으로 샘플을 채취합니다.  심박수 및 SpO2는 1초마다 계산됩니다.
  while (1)
  {
    //메모리에 처음 25개의 샘플 세트를 덤프하고 마지막 75개의 샘플 세트를 맨 위로 이동합니다.
    for (byte i = 25; i < 100; i++)
    {
      redBuffer[i - 25] = redBuffer[i];
      irBuffer[i - 25] = irBuffer[i];
    }
    //심박수를 계산하기 전에 25세트 샘플을 채취합니다.
    for (byte i = 75; i < 100; i++)
    {
      while (particleSensor.available() == false) //do we have new data?
        particleSensor.check(); //센서에 새로운 데이터가 있는지 확인
      digitalWrite(readLED, !digitalRead(readLED)); //데이터를 읽을 때마다 온보드 LED 깜박임
      redBuffer[i] = particleSensor.getRed();
      irBuffer[i] = particleSensor.getIR();
     
      particleSensor.nextSample(); //이 샘플은 끝났으므로 다음 샘플로 이동합니다.
    }
    if (millis() - last_sse > 2000) {
      if (validSPO2 == 1) {
        events.send(String(spo2).c_str(), "spo2", millis());
        Serial.println("Send event SPO2");
        Serial.print(F("SPO2="));
        Serial.println(spo2, DEC);
      }
      if (validHeartRate == 1) {
         events.send(String(heartRate).c_str(), "hr", millis());
         Serial.println("Send event HR");
         Serial.print(F("HR="));
         Serial.println(heartRate, DEC);
      }
      last_sse = millis();
    }
    //25개의 새로운 샘플을 수집한 후 HR 및 SP02를 다시 계산합니다.
    maxim_heart_rate_and_oxygen_saturation(irBuffer, bufferLength, redBuffer, &spo2, &validSPO2, &heartRate, &validHeartRate);
    
  }
}
