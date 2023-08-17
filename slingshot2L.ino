#include <Wire.h> 
#include <LiquidCrystal_I2C.h>
#include <Bounce2.h>
#include <avr/wdt.h>

// ***************************************
//
// Slingshot 2L - Standalone lap timer for 1 or 2 lane use.
//
// Parts tested with
//  1 - Arduino Uno
//  2 - I2C 16x2 LCD display
//  3 - KITR0611S slotted opto sensor
//
// This work is licensed under a Creative Commons Attribution-NonCommercial 4.0 International License.
// You are free to use and modify the code for any non-commercial use.
//
// If you find this sketch and instructions useful,
// please consider making a charitable donation.
//
// contact slngshotx@gmail.com for info.
//
// ***************************************

// ***************************************
// This first section is stuff that can be easily changed
// ***************************************
// Defines the max lanes we are supporting 
#define NUM_LANES      2

// Uses 2 buttons 1 for advance state and 1 for select of options
#define BTN_ADVANCE    8
#define BTN_SELECT     9

// Minimum lap time in ms
#define MIN_LAPTIME    1000

// Input pin debounce in ms
#define BTN_DEBOUNCE   5

// Input Pins & intr numbers for lane sensors
#define LANE1_PIN      2
#define LANE2_PIN      3
#define LANE1_INTR     0
#define LANE2_INTR     1


// Unique adress of the LCD
LiquidCrystal_I2C lcd(0x27,16,2);

// Number of laps to race
int iNumLaps = 1;

// Define the output pins to be used for start lights
const int iLightPin [] =
{
  A0,A1,A2
};

// ***************************************
// End of editable stuff
// ***************************************

// Name & Version
char *sVersion = " Slingshot2L v3b";

// Store all records to be displayed later
struct
{
  unsigned long tReactionTime [NUM_LANES];
  unsigned long tTotalRaceTime [NUM_LANES];
  unsigned long tBestLapTime [NUM_LANES];
  char sLanePos [NUM_LANES][9];
} 
tRecs;

// Store all race state info
struct 
{
  int iLastLaneSignal [NUM_LANES];
  unsigned long tLastLaneSignalTime [NUM_LANES];
  unsigned long lastLapTime[NUM_LANES];
  // Decides what state the system is in
  // 0 - Initialiased
  // 1 - Setup
  // 2 - Starting
  // 3 - Started
  // 4 - Finished
  int iRaceState;
  int iLastRecordDisplayed;
  int iRecordToDisplay;
  // Number of laps run
  int iLapCount [NUM_LANES];
  // Keep track of position and finish time
  int iPosition;
  unsigned long tFinishTime;
  int needToDisplay;
} 
tState;

int iPinValue = LOW;
// Number of stats screens
int iNumStatsToDisplay = 6;

// Lines to display on LCD
char line1Display [NUM_LANES][16];
char line2Display [NUM_LANES][16];

// Instantiate a Bounce object 1 for each input
Bounce iBtnAdvanceDB = Bounce(); 
Bounce iBtnSelectDB = Bounce(); 

/*
 * Setup everything we need
 */
void setup()
{
  //Initialise the LCD
  lcd.init();
  lcd.backlight();

  // Set the interrupts for lane sensors
  pinMode(LANE1_PIN, INPUT_PULLUP);
  pinMode(LANE2_PIN, INPUT_PULLUP);

  // Setup the outputs for start lights
  for (int i=0; i<sizeof(iLightPin); i++)
  {
    pinMode(iLightPin[i], OUTPUT);
  }

  // Set the button pins for input
  pinMode(BTN_ADVANCE, INPUT_PULLUP);
  iBtnAdvanceDB.attach(BTN_ADVANCE);
  iBtnAdvanceDB.interval(BTN_DEBOUNCE);
  pinMode(BTN_SELECT, INPUT_PULLUP);
  iBtnSelectDB.attach(BTN_SELECT);
  iBtnSelectDB.interval(BTN_DEBOUNCE);

  randomSeed(analogRead(0));
}

/*
 * Main loop will loop around reading any input pins
 */
void loop()
{
  iBtnSelectDB.update();
  iBtnAdvanceDB.update();

  // Always read the 2 buttons
  if (iBtnAdvanceDB.fell())
  {
    processAdvance();
  }

  if (iBtnSelectDB.fell())
  {
    processSelect();
  }

  // Initialise everything at the start
  if (tState.iRaceState == 0)
  {
    doInit();
  }

  // Display start lights if we are starting
  if (tState.iRaceState == 2)
  {
    doRaceStart();
  }

  // Read each of the lanes in turn only if we have started
  if (tState.iRaceState == 3)
  {
    // Check for anything to display
    if (tState.needToDisplay > 0)
    {
      displayRaceInfo();
      tState.needToDisplay--;

      // Check for race over
      isRaceOver();
    }
  }

  // Race finished display stats
  if (tState.iRaceState == 4)
  {
    if (tState.iRecordToDisplay != tState.iLastRecordDisplayed)
    {
      tState.iLastRecordDisplayed = displayRaceRecords(tState.iRecordToDisplay);
    }
  }
}

// ***************************************
// Handle the lane interupts
//
// This uses interrupts for lane signals since the 
// LCD write stuff is so slow i.e. 30ms we'd miss lane signals
//
// ***************************************
void processLaneIntr()
{
  if (tState.iRaceState != 3)
  {
    return;
  }
  
  int iPinValue = LOW;
  iPinValue = digitalRead(LANE1_PIN);
  if (iPinValue != tState.iLastLaneSignal[0])
  {
    tState.iLastLaneSignal[0] = iPinValue;
    processLane(0);
  }
  iPinValue = digitalRead(LANE2_PIN);
  if (iPinValue != tState.iLastLaneSignal[1])
  {
    tState.iLastLaneSignal[1] = iPinValue;
    processLane(1);
  }
}

// ***************************************
// Process any lane signals
// laneNo - The lane to send to RC
// ***************************************
void processLane(int laneNo)
{
  unsigned long tNow = millis();

  // Don't process it was to quick
  if ((tNow - tState.tLastLaneSignalTime[laneNo] < MIN_LAPTIME) && tState.iLapCount[laneNo] != -1)
  {
    return;
  }

  // If this lane has finished then don't log anyhing else
  if (tState.iLapCount[laneNo] == iNumLaps)
  {
    return;
  }  

  tState.lastLapTime[laneNo] = tNow - tState.tLastLaneSignalTime[laneNo];

  // If this lane has finished the race
  if (++tState.iLapCount[laneNo] == iNumLaps)
  {
    strcpy(line1Display[laneNo], "Finish  ");
    // Set the position
    sprintf(tRecs.sLanePos[laneNo], "    %d%s ", ++tState.iPosition, tState.iPosition == 1 ? "st" : "nd");
    if (tState.iPosition == 1)
    {
      tState.tFinishTime = tRecs.tTotalRaceTime[laneNo] + tState.lastLapTime[laneNo];
    }
  }
  else
  {
    sprintf(line1Display[laneNo], "Lap %3d ", tState.iLapCount[laneNo]);  
  }

  // Set the totals
  if ((tState.lastLapTime[laneNo] < tRecs.tBestLapTime[laneNo]) && (tState.iLapCount[laneNo] > 0))
  {
    tRecs.tBestLapTime[laneNo] = tState.lastLapTime[laneNo];
  }

  // Save reaction time
  if (tState.iLapCount[laneNo] == 0)
  {
    tRecs.tReactionTime[laneNo] = tState.lastLapTime[laneNo];
  }

  tRecs.tTotalRaceTime[laneNo] += tState.lastLapTime[laneNo];
  tState.tLastLaneSignalTime[laneNo] = tNow;
  tState.needToDisplay++;
}

// ***************************************
// Display the race info if needed
// ***************************************
void displayRaceInfo()
{
  // set the laptime for display on line 2
  // Clear the display and reprint all details
  for (int i=0; i<NUM_LANES; i++)
  {
    formatLapTime(i, tState.lastLapTime[i]);
  }

  lcd.clear();

  lcd.print(line1Display[0]);
  lcd.print(line1Display[1]); 

  lcd.setCursor(0, 1);
  lcd.print(line2Display[0]);
  lcd.print(line2Display[1]);
}

// ***************************************
// Format a lap time that is in millis()
// and return a formatted string.
// ***************************************
void formatLapTime(int laneNo, unsigned long lapTime)
{
  // Only display upto 999 seconds
  int lapSecs = (lapTime / 1000) > 999 ? 999 : lapTime / 1000;

  // Times of 99999999 are just init values and don't need displaying 
  if (lapTime != 99999999 && lapTime != 0)
  {
    sprintf(line2Display[laneNo], "%3d.%03d ", lapSecs, lapTime % 1000);
  }
  else
  {
    strcpy(line2Display[laneNo], "        ");
  }
}

// ***************************************
// Process select button presses, increments state
// ***************************************
void processSelect()
{
  if (++tState.iRaceState >= 5)
  {
    tState.iRaceState = 0;
  }
}

// ***************************************
// Process advance button presses, action depends on state
// ***************************************
void processAdvance()
{
  // Race stat of 0 lets us select how many laps to race
  if (tState.iRaceState == 1)
  {
    iNumLaps++;
    displayLapCount();
  }

  // Race state 4 lets us select the records to display
  if (tState.iRaceState == 4)
  {
    // What record should we display?
    if (++tState.iRecordToDisplay > iNumStatsToDisplay)
      tState.iRecordToDisplay = 1;
  }
}

// ***************************************
// Do the start sequence
// ***************************************
void doRaceStart()
{
    // Read the pin values to start
  tState.iLastLaneSignal[0] = digitalRead(LANE1_PIN);
  tState.iLastLaneSignal[1] = digitalRead(LANE2_PIN);

  lcd.clear();
  lcd.print("   3 .");
  digitalWrite(iLightPin[0], HIGH);
  delay(1000);
  lcd.print(" 2 .");    
  digitalWrite(iLightPin[1], HIGH);
  delay(1000);
  lcd.print(" 1 .");
  digitalWrite(iLightPin[2], HIGH);
  delay(random(1000, 3000));
  lcd.clear();
  lcd.print("       GO");

  // Enable lane interrupts
  attachInterrupt(LANE1_INTR, processLaneIntr, CHANGE);
  attachInterrupt(LANE2_INTR, processLaneIntr, CHANGE);

  // On Go then put all lights out
  digitalWrite(iLightPin[0], LOW);
  digitalWrite(iLightPin[1], LOW);
  digitalWrite(iLightPin[2], LOW);

  // Once start complete set the start time 
  unsigned long tNow = millis();
  for (int i=0; i<NUM_LANES; i++)
  {
    tState.tLastLaneSignalTime[i] = tNow;
  }

  tState.needToDisplay = 0;
  tState.iRaceState++;
}

// ***************************************
// Initialise everything that needs resetting before the race
// ***************************************
void doInit()
{
  lcd.clear();
  lcd.print(sVersion);
  displayLapCount();

  for (int i=0; i<NUM_LANES; i++)
  {
    // -1 assumes that sensor is after start so won't count first crossong of sensor as a lap
    // setting to 0 will assume start is after sensor.
    tState.iLapCount[i] = -1;
    tState.lastLapTime[i] = 0;

    // Initialise the records
    tRecs.tBestLapTime[i] = 99999999;
    tRecs.tTotalRaceTime[i] = 0;
    tRecs.tReactionTime[i] = 0;
    strcpy(tRecs.sLanePos[i], "        ");

    strcpy(line1Display[i], "        ");
    strcpy(line2Display[i], "        ");
  }
  tState.iRecordToDisplay = 1;
  tState.iLastRecordDisplayed = 0;
  tState.iPosition = 0;

  tState.iRaceState++;
}

// ***************************************
// Check if the race is over
// ***************************************
void isRaceOver()
{
  // Race is over if all lanes are on iNumLaps or 1 lane 
  // has completed and all others <0 (i.e. hasnt started yet)
  int iLanesComplete = 0;
  boolean bOneLaneComplete = false;
  for (int i=0; i<NUM_LANES; i++)
  {
    if (tState.iLapCount[i] == iNumLaps)
    {
      iLanesComplete++;
      bOneLaneComplete = true;      
    }
    if (tState.iLapCount[i] < 0)
    {
      iLanesComplete++;
    }
  }

  // Advance race state if all lanes are finished
  if ((iLanesComplete == NUM_LANES) && (bOneLaneComplete))
  {
    tState.iRaceState++;
    detachInterrupt(0);
    detachInterrupt(1);
  }
}

// ***************************************
// Display the race Record
// ***************************************
int displayRaceRecords(int recordType)
{  
  lcd.clear();
  char * recordTitle = "";
  for (int i=0; i<NUM_LANES; i++)
  {
    switch (recordType)
    {
    case 1:
      recordTitle = "Position";
      strcpy(line2Display[i], tRecs.sLanePos[i]);
      break;
    case 2:
      recordTitle = "Total Race Time";
      formatLapTime(i, tRecs.tTotalRaceTime[i]);
      break;
    case 3:
      recordTitle = "Best Lap";
      formatLapTime(i, tRecs.tBestLapTime[i]);
      break;
    case 4:
      recordTitle = "Gap";
      formatLapTime(i, tRecs.tTotalRaceTime[i] > 0 ? tRecs.tTotalRaceTime[i] - tState.tFinishTime : 0);
      break;
    case 5:
      recordTitle = "Average Lap Time";
      formatLapTime(i, tRecs.tTotalRaceTime[i]/iNumLaps);
      break;
    case 6:
      recordTitle = "Reaction Time";
      formatLapTime(i, tRecs.tReactionTime[i]);
      break;
    }
  }  

  lcd.print(recordTitle); 

  lcd.setCursor(0, 1);
  lcd.print(line2Display[0]);
  lcd.print(line2Display[1]);  

  return(recordType);
}

// Simple routine to display the number of laps will be raced
void displayLapCount()
{
  char sLapMsg[16];
  sprintf(sLapMsg, "Laps %d ", iNumLaps); 
  lcd.setCursor(0, 1);
  lcd.print(sLapMsg);
}





