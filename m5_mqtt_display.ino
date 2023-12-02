#include "M5CoreInk.h"
#include  <WiFi.h>
#include "esp_adc_cal.h"
#include "esp32-hal-gpio.h"
#include "time.h"
#include <MqttClient.h>
#include "m5_mqtt_display.h"

//#define VERBOSE 2

Ink_Sprite InkPageSprite(&M5.M5Ink);

WiFiClient wifiClient;
MqttClient mqttClient(wifiClient);

// constants
const int   line_len     = 25;
const int   line_count   = 12;
const char  line_sep     = '\n';
const int   wakeStart    =  6*60*60;
const int   wakeEnd      = 22*60*60;
const int   timestr_size = 20;
const int   batstr_size  = 8;
const float volt_factor  = 510.0/2.51; // <--  1000 Milli * 5.1 V / 25.1 V
const float bat_max      = volt_factor*4.1;
const float bat_min      = volt_factor*3.0;
const float bat_step     = 20.0/(bat_max-bat_min); // 5% Steps

// global variables
char lines[line_count][line_len+1];
char timestr[timestr_size];
char sleepstr[timestr_size];
char batstr[batstr_size];
bool bat_low;

void connect2Wifi() {
  WiFi.begin(ssid, pass);
#ifdef VERBOSE
  Serial.printf("Attempting to connect to WPA SSID: ");
  Serial.println(ssid);
#endif
}

void wait4Wifi() {
  int n=0;
  while (!WiFi.isConnected()) {
#ifdef VERBOSE
    Serial.print(".");
#endif
    n++;
    if (n>20) {
      stopWifi();
      display_error_and_shutdown("Can't connect to WiFi",600);
    }
    delay(1000);
  }
#ifdef VERBOSE
  Serial.println("connected");
#endif
}

void stopWifi() { WiFi.disconnect(true); WiFi.mode(WIFI_OFF); }

void connect2MqttBroker() {
#ifdef VERBOSE
  Serial.print("Attempting to connect to the MQTT broker: ");
  Serial.println(broker);
#endif
  mqttClient.setId(b_user);
  mqttClient.setUsernamePassword(b_user,b_pass);
  int n=0;
  while (!mqttClient.connect(broker, b_port)) {
    n++;
    if( n>20 ) {
#ifdef VERBOSE
      Serial.print("MQTT connection failed! Error code = ");
      Serial.println(mqttClient.connectError());
#endif
      display_error_and_shutdown("Can't connect to MQTT server",60);
    }
    delay(100);
  }
#ifdef VERBOSE
  Serial.println("connected");
  Serial.print("Subscribing to topic: ");
  Serial.println(b_topic);
#endif
  mqttClient.subscribe(b_topic);
#ifdef VERBOSE
  Serial.print("Waiting for messages on topic: ");
  Serial.println(b_topic);
#endif
}

void getMqttMessage() {
  int messageSize;
  uint8_t c=0;
  while( (messageSize=mqttClient.parseMessage())<1 && c++<10) delay(1000);
#if VERBOSE > 1
  Serial.print("Received a message with topic '");
  Serial.print(mqttClient.messageTopic());
  Serial.print("', length ");
  Serial.print(messageSize);
  Serial.println(" bytes.");
#endif
  for( int l=0; l < line_count; l++ ) {
    int i=0;
    char b;
    while( mqttClient.available() && (b=mqttClient.read())!=line_sep ) {
      if( i<line_len) lines[l][i++]=b;
    }
    lines[l][i]=0;
  }
  // Prüfe ob wir was vom MQTT-Server erhalten haben
  bool empty=true;
  for( int l=0; l < line_count; l++ ) if( lines[l][0]!=0 ) empty=false;
  // Wenn wir nichts erhalten haben, dann zeige das an
  if( empty ) {
    strcpy(lines[0+0],"  MQTT");  // linksbündig
    strcpy(lines[4+1],"MESSAGE"); // rechtsbündig
    strcpy(lines[0+2]," EMPTY");  // linksbündig
  }
#if VERBOSE > 2
  for( int l=0; l < line_count; l++ ) Serial.println(lines[l]);
#endif
}

void getBatVoltage() {
  esp_adc_cal_characteristics_t *adc_chars=(esp_adc_cal_characteristics_t*)calloc(1,sizeof(esp_adc_cal_characteristics_t));
  esp_adc_cal_characterize(ADC_UNIT_1,ADC_ATTEN_DB_11,ADC_WIDTH_BIT_12,3600,adc_chars);
  analogSetPinAttenuation(35,ADC_11db);
  const uint16_t raw=analogRead(35);
  const uint32_t voltage=esp_adc_cal_raw_to_voltage(raw,adc_chars);
  free(adc_chars);
  const int percent = 5*int((float(voltage)-bat_min)*bat_step);
  sprintf(batstr,"%d%%",percent);
#ifdef VERBOSE
  Serial.println(batstr);
#endif
  if( percent > 9 ) bat_low=false;
  else if( percent > 0 ) {
    bat_low=false;
    strcpy(lines[3],"  _C_H_A_R_G_E_  _M_E_");
    for( int l=7; l < line_count; l+=4 ) lines[l][0]=0;
  } else {
    for( int l=0; l < line_count; l++ ) lines[l][0]=0;
    strcpy(lines[1],"BATTERY");
    strcpy(lines[6],"LOW");
    sleepstr[0]=0;
    bat_low=true;
  }
}

void lines2Sprite() {
  if( InkPageSprite.creatSprite(0,0,200,200,true) != 0) {
#ifdef VERBOSE
    Serial.println("Ink Sprite create faild");
#endif
    return;
  }
  for( int l=0; l < 4; l++ ) {
    const char* line3=lines[l+4];
    const unsigned int l3=strlen(line3);
    char* line1=lines[l];
    char* line2=lines[l+8];
    int l1=strlen(line1);
    int l2=strlen(line2);
    if( l1==0 ) {
      line1=line2;
      l1=l2;
      line2="";
      l2=0;
    }
    unsigned int line_lengths=l1+3*l3;
    if( l2>0 ) {
      InkPageSprite.drawString(0,24+48*l,line2,&AsciiFont8x16); // 24 = 48/2
      InkPageSprite.drawString(0, 8+48*l,line1,&AsciiFont8x16); //  8 = 48/2 -16
      int d=l2-l1;
      if( d>0 ) line_lengths+=d;
    } else if( l1>0 ) {
      if( l1+l3 > 8 ) InkPageSprite.drawString(0,16+48*l,line1,&AsciiFont8x16); // 16 = 48/2 - 16/2
      else            InkPageSprite.drawString(0,   48*l,line1,&AsciiFont24x48);
    }
    if( l3==0 ) continue;
    if( line_lengths > 25  ) InkPageSprite.drawString(200- 8*l3,16+48*l,line3,&AsciiFont8x16);
    else                     InkPageSprite.drawString(200-24*l3,   48*l,line3,&AsciiFont24x48);
  }

  InkPageSprite.drawString(0,184,timestr, &AsciiFont8x16);
  const int x1 = 200-strlen(sleepstr)*8;
  if( x1>0 ) {
    InkPageSprite.drawString(x1,184,sleepstr,&AsciiFont8x16);
    const int x2 = x1+(strlen(timestr)-strlen(batstr))*8;
    if( x2>0 ) {
      InkPageSprite.drawString(x2/2,184,batstr,&AsciiFont8x16);
    }
  }
}

void display_error_and_shutdown(char* errormsg, unsigned int seconds) {
  InkPageSprite.creatSprite(0,0,200,200,true);
  size_t len=strlen(errormsg);
  size_t line_len;
  unsigned int line_high;
  unsigned int max_lines;
  Ink_eSPI_font_t* font;
  if( len <= 8*4 ) {
    max_lines=4;
    line_len=8;
    font=&AsciiFont24x48;
    line_high=48;
  } else {
    max_lines=8;
    line_len=25;
    font=&AsciiFont8x16;
    line_high=16;
  }
#ifdef VERBOSE
    Serial.println("display_error_and_shutdown called.");
#endif
#if VERBOSE > 1
    Serial.print("message : ");       Serial.println(errormsg);
    Serial.print("shutdown time : "); Serial.println(seconds);
    Serial.print("msg  len  : ");     Serial.println(len);
    Serial.print("line len  : ");     Serial.println(line_len);
    Serial.print("line high : ");     Serial.println(line_high);
    Serial.print("lines max : ");     Serial.println(max_lines);
#endif
  char line[line_len+1]; line[line_len]=0;
  for( unsigned int line_step=0; line_step<max_lines; line_step++ ) {
    unsigned int pos=line_step*line_len;
    int max_chars=len-pos;
#if VERBOSE > 1
    Serial.print("pos       : "); Serial.println(pos);
    Serial.print("max_chars : "); Serial.println(max_chars);
#endif
    if( max_chars > 0 ) {
      strncpy(line,&errormsg[pos],line_len);
#ifdef VERBOSE
    Serial.print("display_error_and_shutdown: ");
    Serial.println(line);
#endif
      InkPageSprite.drawString(0,line_high*line_step,line,font);
    }
  }
  M5.M5Ink.clear();
  InkPageSprite.pushSprite();
  delay(1000);  // Give M5 time to draw sprite
#ifdef VERBOSE
    Serial.println("display_error_and_shutdown: done");
#endif
  if( seconds<1 ) M5.shutdown();
  else            M5.shutdown(seconds);
  delay(2000);  // Just in case shutdown take a while
}

void getNtpTime() {
  RTC_TimeTypeDef time;
  M5.rtc.GetTime(&time);
  const unsigned int seconds_since_daystart=((time.Hours*60)+time.Minutes)*60+time.Seconds;
  // Die Zeit vom NTP nur bis maximal eine Stunde nach Wachzeit-Start holen. Danach bis zum nächsten Tag der RTC vertrauen.
  if( seconds_since_daystart > wakeStart+3600 ) {
#ifdef VERBOSE
    Serial.println("It is more then one hour after wakeStart. Do not update RTC by NTP.");
#endif
    return;
  }
  RTC_DateTypeDef date;
  M5.rtc.GetDate(&date);
  configTime(3600, 3600, "de.pool.ntp.org");
  struct tm currenttime;
  if( getLocalTime(&currenttime,15000) ) {
    time.Seconds = currenttime.tm_sec;
    time.Minutes = currenttime.tm_min;
    time.Hours   = currenttime.tm_hour;
    M5.rtc.SetTime(&time);
    date.Date    = currenttime.tm_mday;
    date.Month   = currenttime.tm_mon + 1;
    date.Year    = currenttime.tm_year + 1900;
    M5.rtc.SetDate(&date);
  } else {
#ifdef VERBOSE
    Serial.println("Failed to obtain time");
#endif
  }
}

int calcShutdownSeconds() {
  RTC_TimeTypeDef time;
  RTC_DateTypeDef date;
  M5.rtc.GetTime(&time);
  M5.rtc.GetDate(&date);
  // Erstelle Zeitanzeige für aktuelle RTC-Zeit.
  sprintf(timestr,"%02d.%02d. %02d:%02d",date.Date,date.Month,time.Hours,time.Minutes);
  // Berechne die Anzahl der Sekunde seit Tagesbeginn
  const unsigned int seconds=((time.Hours*60)+time.Minutes)*60+time.Seconds;
  // Berechne die Anzal Sekunden bis das Gerät vom Timer neu gestartet werden soll.
  unsigned int r;
  if( seconds < wakeStart ) r=wakeStart-seconds;
  else if ( seconds > wakeEnd ) r=24*60*60+wakeStart-seconds;
  else r=(seconds/600+1)*600-seconds+50; // Immer auf die nächsten vollen 10 Minuten springen
  sprintf(sleepstr,"%dm",r/60);
  return r;
}

// Convert the input number into BCD number format: per 4 Bit one decimal
uint8_t byte2Bcd(uint8_t b) {
  const uint8_t h = b/10;
  return (h<<4)|(b-10*h);
}

void shutdown(unsigned int seconds) {
  if( seconds < 120 ) {
        M5.shutdown(seconds);
  } else {
     M5.M5Ink.deepSleep();
     M5.rtc.clearIRQ();
#ifdef VERBOSE
     Serial.printf("Shutdown Sekunden: %ds\n",seconds);
#endif
     RTC_TimeTypeDef time;
     M5.rtc.GetTime(&time);
#if VERBOSE > 1
     Serial.printf("Aktuelle Uhrzeit: %02d:%02d:%02d\n",time.Hours,time.Minutes,time.Seconds);
#endif
     // Berechne die Anzahl Sekunden in Minuten um.
     // Runde dabei auf ganze Minuten ab.
     unsigned int minutes=seconds/60;
     // Addiere die aktuelle Uhrzeit dazu.
     minutes+=time.Minutes+60*time.Hours;
     // Bestimme die resultierende Uhrzeit
     unsigned int hours=minutes/60;
     minutes-=hours*60;
     while( hours > 23 ) hours-=24;
#ifdef VERBOSE
     Serial.printf("Timer: %02d:%02d\n",hours,minutes);
#endif
     // wirte the calculated wake up time into the timer-register
     uint8_t out_buf[4] = {0x80,0x80,0x80,0x80};
     out_buf[0] = byte2Bcd(minutes);
     out_buf[1] = byte2Bcd(hours);
     for (int i=0; i<4; i++) M5.rtc.WriteReg(9+i,out_buf[i]);
     const uint8_t reg = M5.rtc.ReadReg(1);
     M5.rtc.WriteReg(1,reg|2);
     delay(20);
#ifdef VERBOSE
     Serial.println("AUSSCHALTEN.");
     delay(2000); // just that the Serial can send data before device powers off.
#endif
     // Schalte Gerät aus
     pinMode(1, OUTPUT);
     digitalWrite(1, LOW);
     digitalWrite(POWER_HOLD_PIN, LOW);
  }
  delay(2000);  // Just in case shutdown take a while
}

void setup() {
  M5.begin();
  Serial.begin(19200);
  connect2Wifi();
  digitalWrite(LED_EXT_PIN,LOW);
  wait4Wifi();
  connect2MqttBroker();
  getNtpTime();
  int n=0;
  while( !M5.M5Ink.isInit() ) {
    n++;
    if(n>20) {
#ifdef VERBOSE
      Serial.println("Ink Init faild");
#endif
      M5.shutdown(300);
      delay(2000); // Just in case shutdown take a while
    }
    delay(100);
  }
}

void loop() {
  int seconds=calcShutdownSeconds();
  getMqttMessage();
  getBatVoltage();
  lines2Sprite();
  M5.M5Ink.clear();
  InkPageSprite.pushSprite();
#if VERBOSE > 1
  Serial.print("Go to sleep for ");
  Serial.print(seconds);
  Serial.println(" seconds.");
#endif
  if(bat_low) {
    display_error_and_shutdown("Battery empty",0);
  } else if( seconds < 10 ) {
    delay(seconds*1000);
  } else {
    delay(2000);
    shutdown(seconds);
  }
}
