#include <Keypad.h>
#include <LiquidCrystal_I2C.h>

#define I2C_ADDR    0x27
#define LCD_COLUMNS 16
#define LCD_LINES   2

LiquidCrystal_I2C lcd(I2C_ADDR, LCD_COLUMNS, LCD_LINES);

struct UserRole {
  unsigned long pinCode;  
  const char* roleName;   
};

const int TOTAL_USERS = 4;

UserRole userDatabase[TOTAL_USERS] = {
  {6969, "G-STUDENT"},
  {8834, "PARTICIPANT"},
  {6767, "JUDGES"},
  {1060, "FACULTY"}
};


typedef enum {
  STATE_INPUT,
  STATE_FEEDBACK,
  STATE_LEDOFF,
  STATE_LOCKOUT,
  STATE_EMERGENCY
} systemState;


systemState currentState = STATE_INPUT; 

unsigned long input = 0;  
uint8_t d = 0;  
unsigned long  ledmillis = 0;
unsigned long previousMillis = 0;
int count = 0;       
unsigned long lockStartTime = 0;
volatile bool emer=false;

const uint8_t ROWS = 4;
const uint8_t COLS = 4;
char keys[ROWS][COLS] = {
  { '1', '2', '3', 'A' },
  { '4', '5', '6', 'B' },
  { '7', '8', '9', 'C' },
  { '*', '0', '#', 'D' } 
};
uint8_t colPins[COLS] = { 1, 0, 3, 2 };
uint8_t rowPins[ROWS] = { 4, 5, 6, 7 };

Keypad keypad = Keypad(makeKeymap(keys), rowPins, colPins, ROWS, COLS);

void emergency(){
  emer = true;
}

void setup() {
  Wire.begin(18, 19);
  lcd.init();
  lcd.backlight();
  
  
  pinMode(8, OUTPUT);
  pinMode(10, OUTPUT);
  digitalWrite(8, LOW); 
  digitalWrite(10, LOW);

  pinMode(9, INPUT_PULLUP); 
  attachInterrupt(digitalPinToInterrupt(9), emergency, FALLING);

  lcd.print("ENTER THE CODE");
}

void readKey() {
  char key = keypad.getKey(); 
  
  if(d > 0 && (millis() - previousMillis > 7000)){
    d = 0;
    input = 0;
    lcd.clear();
    lcd.print("ENTER THE CODE");
    return;
  }
  
  if (key) {
    previousMillis = millis();
    
    if (key >= '0' && key <= '9') {
      int realNumber = key - '0'; 
      input = (input * 10) + realNumber; 
      d++;                               
      
      lcd.clear();
      lcd.print(input);
      
      if (d == 4) { 
        
        lcd.clear();
        lcd.print("Checking...");
        
        
        currentState = STATE_FEEDBACK;
        return;
      }
    }
    
    else if (key == '*' && d > 0) {
      input = input / 10; 
      d--;                
      lcd.clear();
      
      if (d == 0) {
         lcd.print("ENTER THE CODE");
      } else {
         lcd.print(input); 
      }
      return;
    }
  }
}

void check(){
  bool match = false;
  int mI = 0;

  for(int i = 0; i < TOTAL_USERS; i++){
    if(input == userDatabase[i].pinCode){
      match = true;
      mI = i;
      break;
    }
  }

  lcd.clear();

  if(match){
    lcd.print("WELCOME:");
    lcd.setCursor(0,1);
    lcd.print(userDatabase[mI].roleName);
    count = 0;
    
    
    digitalWrite(8, HIGH);  
    digitalWrite(10, HIGH); 
    currentState = STATE_LEDOFF;
    return;
  
  } else {
    count++; 
    lcd.print("ACCESS DENIED");
    lcd.setCursor(0,1);
    lcd.print("RETRY!!");
    currentState= STATE_LEDOFF;
    return;
  }


}

void ledOFF(){
  if (millis() - previousMillis >= 3000) {
    
    digitalWrite(8, LOW);  
    digitalWrite(10, LOW);

    lcd.clear();
    lcd.print("ENTER THE CODE");
    input=0;
    d=0;
    previousMillis = millis(); 
    currentState = STATE_INPUT;
    
   if (count >= 3) {
    lockStartTime = millis();       
    currentState = STATE_LOCKOUT;   
    
    lcd.clear();
    lcd.print("SYSTEM LOCKED");
    lcd.setCursor(0, 1);
    lcd.print("Wait 15 Seconds");
  } else {
    currentState = STATE_INPUT; 
    lcd.clear();
    lcd.print("ENTER THE CODE");
  }
  }  
}


void lockOut(){
  if(millis() - lockStartTime >= 15000){
    count = 0;
    currentState = STATE_INPUT;

    lcd.clear();
    lcd.print("ENTER THE CODE");
  }
}

void  ghusjaa(){
  emer = false;
  lcd.clear();
  lcd.print("EMERGENCY ACCESS");
  lcd.setCursor(0, 1);
  lcd.print("OVERRIDE ACTIVE");
  
  digitalWrite(8, HIGH); 
  digitalWrite(10, HIGH);
  currentState = STATE_LEDOFF;

}



void loop() {
  if (emer == true) {
    currentState = STATE_EMERGENCY;
  }

  switch (currentState) {
    case STATE_INPUT:
      readKey();
      break;
      
    case STATE_FEEDBACK:
      check();
      break;

    case STATE_LEDOFF:
      ledOFF();
      break;
      
    case STATE_LOCKOUT:
      lockOut();
      break;
      
    case STATE_EMERGENCY:
      ghusjaa();
      break;

  } 
}