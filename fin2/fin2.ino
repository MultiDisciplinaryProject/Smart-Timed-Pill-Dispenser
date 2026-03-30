#include <WiFi.h>
#include <WebServer.h>
#include <ESP32Servo.h>
#include <EEPROM.h>
#include <time.h>

const char* ssid = "vivo Y28 5G";
const char* password = "Casablanca@07";
const char* patientPassword = "1111";
const char* doctorPassword  = "1234";

int userRole = 0; // 0=none, 1=patient, 2=doctor

WebServer server(80);
Servo myServo;

int servoPin = 13;
bool alarmActive = false;
unsigned long alarmStart = 0;

String scheduleNames[4] = {"Morning","Lunch","Evening","Night"};
int scheduleAngles[4] = {45,90,135,180};
String scheduleTimes[4];

bool takenToday[4] = {false,false,false,false};

#define EEPROM_SIZE 512
#define MAX_HISTORY 7

String historyDates[MAX_HISTORY];
bool historyTaken[MAX_HISTORY][4];
int historyIndex = 0;

/* ================= SETUP ================= */

void setup(){
  
  Serial.begin(115200);
  EEPROM.begin(EEPROM_SIZE);
  myServo.attach(servoPin);
  myServo.write(0);

  loadTimes();
  loadHistory();

  WiFi.begin(ssid,password);
  while(WiFi.status()!=WL_CONNECTED){
    delay(500);
  }

  configTime(19800,0,"pool.ntp.org");

  server.on("/", handleHome);
  server.on("/history", handleHistory);
  server.on("/settings", handleSettings);

  server.on("/login", HTTP_POST, handleLogin);
  server.on("/logout", handleLogout);

  server.on("/manual", HTTP_POST, handleManual);
  server.on("/saveTimes", HTTP_POST, handleSaveTimes);
  server.on("/resetToday", handleResetToday);
  server.on("/saveHistoryNow", handleSaveHistoryNow);
  server.on("/clearHistory", handleClearHistory);
  server.on("/stop", handleStop);

  server.begin();
}

/* ================= LOOP ================= */

void loop(){
  server.handleClient();
  checkSchedule();
  checkAlarmTimeout();
  dailyReset();
}

/* ================= COMMON UI ================= */

String getNavbar(){
  String nav = "<div style='background:#020617;padding:15px;'>";
  nav += "<a href='/'>Home</a> | ";
  nav += "<a href='/history'>History</a> | ";

  if(userRole==2)
    nav += "<a href='/settings'>Settings</a> | ";

  nav += "<a href='/logout' style='color:red;'>Logout</a>";
  nav += "</div>";
  return nav;
}

String getHTMLStart(){
  return "<html><head><meta charset='UTF-8'>"
  "<style>"
  "body{background:#0f172a;color:white;font-family:Arial;text-align:center;}"
  ".card{background:#1e293b;margin:20px;padding:20px;border-radius:15px;}"
  "button{padding:10px 20px;border:none;border-radius:10px;background:#22c55e;color:white;}"
  "input,select{padding:8px;border-radius:8px;}"
  "</style></head><body>";
}

/* ================= LOGIN ================= */

void handleLogin(){
  String role = server.arg("role");
  String pass = server.arg("pass");

  if(role=="patient" && pass==patientPassword) userRole=1;
  else if(role=="doctor" && pass==doctorPassword) userRole=2;
  else userRole=0;

  server.sendHeader("Location","/");
  server.send(303);
}

void handleLogout(){
  userRole=0;
  server.sendHeader("Location","/");
  server.send(303);
}

/* ================= HOME ================= */

void handleHome(){

  if(userRole==0){
    server.send(200,"text/html",
    "<html><body style='background:#111;color:white;text-align:center;'>"
    "<h2>Login</h2>"
    "<form method='POST' action='/login'>"
    "<input type='hidden' name='role' value='patient'>"
    "<input type='password' name='pass' placeholder='Patient'><br><button>Patient</button></form>"
    "<form method='POST' action='/login'>"
    "<input type='hidden' name='role' value='doctor'>"
    "<input type='password' name='pass' placeholder='Doctor'><br><button>Doctor</button></form>"
    "</body></html>");
    return;
  }

  String page = getHTMLStart();
  page += getNavbar();

  /* Schedule */
  page += "<div class='card'><h3>Today's Schedule</h3>";
  for(int i=0;i<4;i++){
    page += scheduleNames[i]+" - "+scheduleTimes[i];
    if(takenToday[i]) page+=" ✓";
    page+="<br>";
  }
  page += "</div>";

  /* Manual */
  page += "<div class='card'><h3>Manual</h3>";
  page += "<form method='POST' action='/manual'><select name='slot'>";
  for(int i=0;i<4;i++)
    page += "<option value='"+String(i)+"'>"+scheduleNames[i]+"</option>";
  page += "</select><br><br><button>Release</button></form></div>";

  /* Alarm */
  if(alarmActive){
    page += "<div class='card' style='background:red;'>ALARM<br>"
            "<button onclick=\"fetch('/stop').then(()=>location.reload())\">STOP</button></div>";
  }

  page += "</body></html>";
  server.send(200,"text/html",page);
}

/* ================= HISTORY PAGE ================= */

void handleHistory(){
  if(userRole==0) return;

  String page = getHTMLStart();
  page += getNavbar();

  page += "<div class='card'><h3>History</h3>";
  page += "<table border='1' style='margin:auto;'>";

  page += "<tr><th>Date</th>";
  for(int i=0;i<4;i++) page+="<th>"+scheduleNames[i]+"</th>";
  page+="</tr>";

  for(int i=0;i<MAX_HISTORY;i++){
    int idx=(historyIndex+i)%MAX_HISTORY;
    if(historyDates[idx]=="") continue;

    page += "<tr><td>"+historyDates[idx]+"</td>";
    for(int j=0;j<4;j++)
      page += "<td>"+String(historyTaken[idx][j]?"✓":"✗")+"</td>";
    page+="</tr>";
  }

  page+="</table></div>";

  if(userRole==2){
    page += "<div class='card'>"
            "<button onclick=\"fetch('/saveHistoryNow').then(()=>location.reload())\">Save Now</button>"
            "<br><br>"
            "<button onclick=\"fetch('/clearHistory').then(()=>location.reload())\">Clear</button>"
            "</div>";
  }

  page += "</body></html>";
  server.send(200,"text/html",page);
}

/* ================= SETTINGS ================= */

void handleSettings(){
  if(userRole!=2){
    server.send(403,"text/plain","Forbidden");
    return;
  }

  String page = getHTMLStart();
  page += getNavbar();

  page += "<div class='card'><h3>Edit Times</h3>";
  page += "<form method='POST' action='/saveTimes'>";
  for(int i=0;i<4;i++){
    page += scheduleNames[i]+": ";
    page += "<input type='time' name='t"+String(i)+"' value='"+scheduleTimes[i]+"'><br><br>";
  }
  page += "<button>Save</button></form></div>";

  page += "<div class='card'><button onclick=\"fetch('/resetToday').then(()=>location.reload())\">Reset Today</button></div>";

  page += "</body></html>";
  server.send(200,"text/html",page);
}

/* ================= CORE FUNCTIONS ================= */

void handleManual(){
  int slot=server.arg("slot").toInt();

  for(int i=0;i<=scheduleAngles[slot];i++){ myServo.write(i); delay(15);}
  delay(2000);
  for(int i=scheduleAngles[slot];i>=0;i--){ myServo.write(i); delay(15);}

  server.sendHeader("Location","/");
  server.send(303);
}

void checkSchedule(){
  struct tm t;
  if(!getLocalTime(&t)) return;

  char now[6];
  strftime(now,6,"%H:%M",&t);

  for(int i=0;i<4;i++){
    if(!takenToday[i] && scheduleTimes[i]==String(now)){
      myServo.write(scheduleAngles[i]);
      takenToday[i]=true;
      alarmActive=true;
      alarmStart=millis();
    }
  }
}

void handleStop(){
  alarmActive=false;
  server.sendHeader("Location","/");
  server.send(303);
}

void checkAlarmTimeout(){
  if(alarmActive && millis()-alarmStart>300000)
    alarmActive=false;
}

/* ================= HISTORY ================= */

void saveHistory(){
  int addr=50;
  EEPROM.write(addr++,historyIndex);

  for(int i=0;i<MAX_HISTORY;i++){
    for(int j=0;j<10;j++){
      char c = (j<historyDates[i].length())?historyDates[i][j]:' ';
      EEPROM.write(addr++,c);
    }
    for(int j=0;j<4;j++)
      EEPROM.write(addr++,historyTaken[i][j]);
  }
  EEPROM.commit();
}

void loadHistory(){
  int addr=50;
  historyIndex = EEPROM.read(addr++);

  if(historyIndex >= MAX_HISTORY) historyIndex = 0; // safety

  for(int i=0;i<MAX_HISTORY;i++){

    char date[11];

    for(int j=0;j<10;j++){
      byte val = EEPROM.read(addr++);

      // FIX: replace invalid bytes
      if(val == 255 || val < 32 || val > 126)
        date[j] = ' ';
      else
        date[j] = char(val);
    }

    date[10] = '\0';

    String d = String(date);
    d.trim();  // remove spaces

    historyDates[i] = d;

    for(int j=0;j<4;j++){
      historyTaken[i][j] = EEPROM.read(addr++);
    }
  }
}

void handleSaveHistoryNow(){
  if(userRole!=2) return;

  historyDates[historyIndex]=getDate();
  for(int i=0;i<4;i++)
    historyTaken[historyIndex][i]=takenToday[i];

  historyIndex=(historyIndex+1)%MAX_HISTORY;
  saveHistory();

  server.sendHeader("Location","/history");
  server.send(303);
}

void handleClearHistory(){
  if(userRole!=2) return;

  for(int i=0;i<MAX_HISTORY;i++){
    historyDates[i]="";
    for(int j=0;j<4;j++) historyTaken[i][j]=false;
  }
  historyIndex=0;
  saveHistory();

  server.sendHeader("Location","/history");
  server.send(303);
}

/* ================= RESET ================= */

void handleResetToday(){
  if(userRole!=2) return;

  historyDates[historyIndex]=getDate();
  for(int i=0;i<4;i++)
    historyTaken[historyIndex][i]=takenToday[i];

  historyIndex=(historyIndex+1)%MAX_HISTORY;

  for(int i=0;i<4;i++) takenToday[i]=false;

  saveHistory();

  server.sendHeader("Location","/");
  server.send(303);
}

void dailyReset(){
  struct tm t;
  if(!getLocalTime(&t)) return;

  if(t.tm_hour==0 && t.tm_min==0){
    handleResetToday();
    delay(60000);
  }
}

/* ================= TIME ================= */

String getDate(){
  struct tm t;
  if(!getLocalTime(&t)) return "0000-00-00";

  char d[11];
  strftime(d,11,"%Y-%m-%d",&t);
  return String(d);
}

void handleSaveTimes(){
  if(userRole!=2) return;

  for(int i=0;i<4;i++){
    scheduleTimes[i]=server.arg("t"+String(i));
    saveTimeToEEPROM(i,scheduleTimes[i]);
  }

  server.sendHeader("Location","/settings");
  server.send(303);
}

void saveTimeToEEPROM(int index,String val){
  int base=index*10;
  for(int i=0;i<5;i++) EEPROM.write(base+i,val[i]);
  EEPROM.commit();
}

void loadTimes(){
  for(int i=0;i<4;i++){
    char buf[6];
    int base=i*10;
    for(int j=0;j<5;j++){
      buf[j]=EEPROM.read(base+j);
      if(buf[j]==255) buf[j]='0';
    }
    buf[5]='\0';
    scheduleTimes[i]=String(buf);
  }
}
