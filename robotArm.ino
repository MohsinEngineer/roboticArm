#include <arduino.h>
#include "pinout.h"
#include "robotGeometry.h"
#include "interpolation.h"
#include "fanControl.h"
#include "rampsStepper.h"
#include "queue.h"
#include "command.h"

// #define DEBUG
#include "debug_utils.h"

void executeCommand(Cmd cmd);
void GoHome();
void cmdGetPosition();
void ZRot(boolean DIR, int STEP);
void GoHome();
void SetDirection();
void stepper();
void handleAsErr(Cmd(&cmd));
void cmdMove(Cmd(&cmd));
void cmdDwell(Cmd(&cmd));
void cmdGripperOn(Cmd(&cmd));
void cmdGripperOff(Cmd(&cmd));

RampsStepper stepperRotate(Z_STEP_PIN, Z_DIR_PIN, Z_ENABLE_PIN);
RampsStepper stepperLower(Y_STEP_PIN, Y_DIR_PIN, Y_ENABLE_PIN);
RampsStepper stepperHigher(X_STEP_PIN, X_DIR_PIN, X_ENABLE_PIN);
RampsStepper stepperExtruder(E_STEP_PIN, E_DIR_PIN, E_ENABLE_PIN);
FanControl fan(FAN_PIN);
RobotGeometry geometry;
Interpolation interpolator;
Queue<Cmd> queue(255); //15
Command command;
bool isRelativeCoord;

//configure gripper byj stepper
boolean Direction = true;
int Steps = 0;
int steps_to_grip = 900;
unsigned long last_time;
unsigned long currentMillis;
long time;

// for Homing
const int Z_HomeStep = 2850;        // 2850 step to Home Rotation Stepper... rotate back CW after touching Z-min switch (to set Robotic arm rotation to 0 Degree)
bool home_on_boot = true;          // set it false if you not want to Home the robotic arm automatically during Boot.
bool home_rotation_stepper = false; // enable it (set to true) if you want to home rotation stepper too... it requires end-switch mounted on rotation and attached to Z-min pin on RAMPS
const int MaxDwell = 1200;
const int OffDwell = 80; // Main stepper OFF-delay= 40minimum ; normal=80 ; base rotation needed more time
int On_Dwell = MaxDwell; // Main stepper ON-delay= 120minimum ; normal=480; Home speed=600

void setup()
{
  Serial.begin(115200);
  isRelativeCoord = false;  // Default to absolute coordonates

  //various pins..
  pinMode(HEATER_0_PIN, OUTPUT);
  pinMode(HEATER_1_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  //unused Stepper..
  pinMode(E_STEP_PIN, OUTPUT);
  pinMode(E_DIR_PIN, OUTPUT);
  pinMode(E_ENABLE_PIN, OUTPUT);

  //unused Stepper..
  pinMode(Q_STEP_PIN, OUTPUT);
  pinMode(Q_DIR_PIN, OUTPUT);
  pinMode(Q_ENABLE_PIN, OUTPUT);

  //GripperPins
  pinMode(STEPPER_GRIPPER_PIN_0, OUTPUT);
  pinMode(STEPPER_GRIPPER_PIN_1, OUTPUT);
  pinMode(STEPPER_GRIPPER_PIN_2, OUTPUT);
  pinMode(STEPPER_GRIPPER_PIN_3, OUTPUT);

  //reduction of steppers..
  stepperHigher.setReductionRatio(32.0 / 9.0, 200 * 16); //big gear: 32, small gear: 9, steps per rev: 200, microsteps: 16
  stepperLower.setReductionRatio(32.0 / 9.0, 200 * 16);  //(32.0 / 9.0, 200 * 16)
  stepperRotate.setReductionRatio(32.0 / 9.0, 200 * 16);
  stepperExtruder.setReductionRatio(32.0 / 9.0, 200 * 16);

  //start positions..
  stepperHigher.setPositionRad(PI / 2.0); //90°
  stepperLower.setPositionRad(0);         // 0°
  stepperRotate.setPositionRad(0);        // 0°
  stepperExtruder.setPositionRad(0);

  //enable and init..
  if (home_on_boot)
  {
    Serial.println("Homing start...");
    GoHome(); // Go to home position on boot
    Serial.println("Home Done.");
  }
  else
  {
    setStepperEnable(false);
  }
  Serial.println("start");
  interpolator.setInterpolation(0, 16, 126, 0, 0, 16, 126, 0); // initial position is actual home position

  pinMode(Y_MIN_PIN, INPUT_PULLUP);
  pinMode(X_MIN_PIN, INPUT_PULLUP);
  pinMode(Z_MIN_PIN, INPUT_PULLUP);

  // //testcode for EndStops
  DEBUG_PRINT("// Y limit switch status: " + String(digitalRead(Y_MIN_PIN)));
  DEBUG_PRINT("// X limit switch status: " + String(digitalRead(X_MIN_PIN)));
  DEBUG_PRINT("// Z limit switch status: " + String(digitalRead(Z_MIN_PIN)));  // My Zlimit switch is a NC as opposed to the X&Y
}

void setStepperEnable(bool enable)
{
  stepperRotate.enable(enable);
  stepperLower.enable(enable);
  stepperHigher.enable(enable);
  stepperExtruder.enable(enable);
  fan.enable(enable);
}

void loop()
{

  //update and Calculate all Positions, Geometry and Drive all Motors...
  interpolator.updateActualPosition();
  geometry.set(interpolator.getXPosmm(), interpolator.getYPosmm(), interpolator.getZPosmm());
  stepperRotate.stepToPositionRad(geometry.getRotRad());
  stepperLower.stepToPositionRad(geometry.getLowRad());
  stepperHigher.stepToPositionRad(geometry.getHighRad());
  stepperExtruder.stepToPositionRad(interpolator.getEPosmm());
  stepperRotate.update();
  stepperLower.update();
  stepperHigher.update();
  fan.update();
  if (!queue.isFull())
  {
    if (command.handleGcode())
    {
      queue.push(command.getCmd());
      printOk();
    }
  }
  if ((!queue.isEmpty()) && interpolator.isFinished())
  {
    executeCommand(queue.pop());
  }

  if (millis() % 500 < 250)
  {
    digitalWrite(LED_PIN, HIGH);
  }
  else
  {
    digitalWrite(LED_PIN, LOW);
  }
}

void cmdToRelative()
{
  isRelativeCoord = true;
  printComment("Robot is now in Relative coordonates...");
}

void cmdToAbsolute()
{
  isRelativeCoord = false;
  printComment("Robot is now in Absolute coordonates...");
}

void cmdMove(Cmd(&cmd))
{

  if (isRelativeCoord == true)
  {
    Point pos = interpolator.getPosmm();

    // If some axis values were not passed initialyze them to 0
    if (isnan(cmd.valueX)) {
      cmd.valueX = 0;
    }
    if (isnan(cmd.valueY)) {
      cmd.valueY = 0;
    }
    if (isnan(cmd.valueZ)) {
      cmd.valueZ = 0;
    }

    // In Relative Coordinate mode, increment the current positions with the passed values
    cmd.valueX += pos.xmm;
    cmd.valueY += pos.ymm;
    cmd.valueZ += pos.zmm;
  }

  DEBUG_PRINT("// cmd.value = " + String(cmd.valueX) + ", " + String(cmd.valueY) + ", " + String(cmd.valueZ));
  
  interpolator.setInterpolation(cmd.valueX, cmd.valueY, cmd.valueZ, cmd.valueE, cmd.valueF);
}
void cmdDwell(Cmd(&cmd))
{
  delay(int(cmd.valueT * 1000));
}
void cmdGripperOn(Cmd(&cmd))
{
  digitalWrite(HEATER_1_PIN, HIGH);
  Direction = true;
  for (int i = 1; i <= steps_to_grip; i++)
  {
    stepper();
    delay(1);
  }
  delay(50);
  digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
  digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
  digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
  digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);

  digitalWrite(HEATER_1_PIN, LOW);
}

void cmdGripperOff(Cmd(&cmd))
{
  digitalWrite(HEATER_1_PIN, HIGH);
  Direction = false;
  for (int i = 1; i <= steps_to_grip; i++)
  {
    stepper();
    delay(1);
  }
  delay(50);
  digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
  digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
  digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
  digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
  digitalWrite(HEATER_1_PIN, LOW);
}
void cmdStepperOn()
{
  setStepperEnable(true);
}
void cmdStepperOff()
{
  setStepperEnable(false);
}
void cmdFanOn()
{
  fan.enable(true);
}
void cmdFanOff()
{
  fan.enable(false);
}

void handleAsErr(Cmd(&cmd))
{
  printComment("Unknown Cmd " + String(cmd.id) + String(cmd.num) + " (queued)");
  printFault();
}

void executeCommand(Cmd cmd)
{

  DEBUG_PRINT("// Passed Cmd " + String(cmd.id) + String(cmd.num));
  DEBUG_PRINT("// Passed Cmd VALUES" + String(cmd.valueX) + " " + String(cmd.valueY) + " " + String(cmd.valueZ));

  if (cmd.id == -1)
  {
    String msg = "parsing Error";
    printComment(msg);
    handleAsErr(cmd);
    return;
  }

  // In Absolute mode, fill the missing coordonate with the current position from the interpolator
  if (isnan(cmd.valueX) && !isRelativeCoord)
  {
    cmd.valueX = interpolator.getXPosmm();
  }
  if (isnan(cmd.valueY) && !isRelativeCoord)
  {
    cmd.valueY = interpolator.getYPosmm();
  }
  if (isnan(cmd.valueZ) && !isRelativeCoord)
  {
    cmd.valueZ = interpolator.getZPosmm();
  }
  if (isnan(cmd.valueE) && !isRelativeCoord)
  {
    cmd.valueE = interpolator.getEPosmm();
  }
  DEBUG_PRINT("// Passed Cmd VALUES" + String(cmd.valueX) + " " + String(cmd.valueY) + " " + String(cmd.valueZ));

  //decide what to do
  if (cmd.id == 'G')
  {
    switch (cmd.num)
    {
    case 0:
      cmdMove(cmd);
      break;
    case 1:
      cmdMove(cmd);
      break;
    case 4:
      cmdDwell(cmd);
      break;
    case 28:
      GoHome();
      break; // Added by me to Home the Robotic Arm on G28 command
    //case 21: break; //set to mm
    case 90:
      cmdToAbsolute();  // Set to ABSOLUTE Coordinate mode (default)
      break;
    case 91:
      cmdToRelative();  // Set to RELATIVE Coordinate mode
      break;
    //case 92: cmdSetPosition(cmd); break;
    default:
      handleAsErr(cmd);
    }
  }
  else if (cmd.id == 'M')
  {
    switch (cmd.num)
    {
    //case 0: cmdEmergencyStop(); break;
    case 3:
      cmdGripperOn(cmd);
      break;
    case 5:
      cmdGripperOff(cmd);
      break;
    case 17:
      cmdStepperOn();
      break;
    case 18:
      cmdStepperOff();
      break;
    case 106:
      cmdFanOn();
      break;
    case 107:
      cmdFanOff();
      break;
    case 114:
      cmdGetPosition(); // Return the current positions of all axis 
      break;
    default:
      handleAsErr(cmd);
    }
  }
  else
  {
    handleAsErr(cmd);
  }
}

// Gripper Stepper Control
void stepper()
{
  switch (Steps)
  {
  case 0:
    digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_3, HIGH);
    break;
  case 1:
    digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_2, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_3, HIGH);
    break;
  case 2:
    digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_2, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
    break;
  case 3:
    digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_1, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_2, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
    break;
  case 4:
    digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_1, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
    break;
  case 5:
    digitalWrite(STEPPER_GRIPPER_PIN_0, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_1, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
    break;
  case 6:
    digitalWrite(STEPPER_GRIPPER_PIN_0, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
    break;
  case 7:
    digitalWrite(STEPPER_GRIPPER_PIN_0, HIGH);
    digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_3, HIGH);
    break;
  default:
    digitalWrite(STEPPER_GRIPPER_PIN_0, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_1, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_2, LOW);
    digitalWrite(STEPPER_GRIPPER_PIN_3, LOW);
    break;
  }
  SetDirection();
}

void SetDirection()
{
  if (Direction == true)
  {
    Steps++;
  }
  if (Direction == false)
  {
    Steps--;
  }
  if (Steps > 7)
  {
    Steps = 0;
  }
  if (Steps < 0)
  {
    Steps = 7;
  }
}

///////////////////////////////////////////////////////////////////////
/////// Home function to home the robotic arm at limit Switches ///////
///////////////////////////////////////////////////////////////////////
// Reset arm to HOME position Z_home, Y_home then X_home... this function is called when G28 command is issued
void GoHome()
{
  printComment("Homing Lower...");
  int bState;
  stepperHigher.enable(false); // disable Higher arm Stepper so that it do not block rotation of Lower arm while lower arm home

  // Lower arm Home
  stepperLower.enable(true);
  delayMicroseconds(5); // Enables the motor-Y to move the LOWER ARM CCW until hitted the Y-min_SW
  // pinMode(Y_MIN_PIN,INPUT_PULLUP);
  digitalWrite(Y_DIR_PIN, HIGH);
  delayMicroseconds(5);            // Enables the motor-Y to move the LOWER ARM CCW until hitted the Y-min_SW
  bState = digitalRead(Y_MIN_PIN); // read the input pin:
  while (bState == HIGH)           // initally SW OPEN = High  (Loop until limit switch is pressed)
  {
    digitalWrite(Y_STEP_PIN, HIGH);
    delayMicroseconds(On_Dwell);
    digitalWrite(Y_STEP_PIN, LOW);
    delayMicroseconds(OffDwell);
    bState = digitalRead(Y_MIN_PIN);
  }

  // Upper arm Home
  printComment("Homing Upper...");
  stepperHigher.enable(true);
  // pinMode(X_MIN_PIN,INPUT_PULLUP);
  digitalWrite(X_DIR_PIN, HIGH);
  delayMicroseconds(5);            // Enables the motor-X to move the UPPER ARM CCW until hitted the X-min_SW
  bState = digitalRead(X_MIN_PIN); // read the input pin:
  while (bState == HIGH)           // assume initally SW OPEN = High  (Loop until limit switch is pressed)
  {
    digitalWrite(X_STEP_PIN, HIGH);
    delayMicroseconds(On_Dwell);
    digitalWrite(X_STEP_PIN, LOW);
    delayMicroseconds(OffDwell);
    bState = digitalRead(X_MIN_PIN);
  }

  // Rotation Home
  if (home_rotation_stepper == true) // home Rotation stepper too if home_rotation_stepper is set to true.
  {
    // Rotation Home (It will Home towards Right Side (see from Front) towards 90 degree, hit the limit switch and then again move back to 0 Degree)
    stepperRotate.enable(true);
    pinMode(Z_MIN_PIN, INPUT_PULLUP);
    digitalWrite(Z_DIR_PIN, HIGH);
    delayMicroseconds(5);            // Enables the motor-Z to rotate the ARM Anti-Clock-Wise (see from Front) until hitted the Z-min_SW
    bState = digitalRead(Z_MIN_PIN); // read the input pin:
    while (bState == LOW)           // Change to HIGH if the switch is initally SW OPEN = High  (Loop until limit switch is pressed)
    {
      digitalWrite(Z_STEP_PIN, HIGH);
      delayMicroseconds(On_Dwell);
      digitalWrite(Z_STEP_PIN, LOW);
      delayMicroseconds(OffDwell);
      bState = digitalRead(Z_MIN_PIN);
    }

    // Rotate back after touching Z-min switch (to set Robotic arm rotation to 0 Degree)
    ZRot(LOW, Z_HomeStep);
  }

  cmdStepperOn(); // M17 Gcode (Power on All Steppers)

  interpolator.setInterpolation(0, 16, 126, 0, 0, 16, 126, 0); //Set Home Position to again X0 Y16 Z126 // SR: Changed for endstop position
  interpolator.setCurrentPos(0, 16, 126, 0);
  interpolator.updateActualPosition();
  printComment("Homing Completed...");

  printComment("Current Position X, Y, Z");
  cmdGetPosition();
}

void cmdGetPosition()
{
  if(isRelativeCoord) {
    printComment("Robot is now in Relative coordinates...");
  } else {
    printComment("Robot is now in Absolute coordinates...");
  }

  //C: X:0.00 Y:0.00 Z:0.00 E:0.00
  Point pos = interpolator.getPosmm();
  Serial.print("C: X:");
  Serial.print(pos.xmm);
  Serial.print(" Y:");
  Serial.print(pos.ymm);
  Serial.print(" Z:");
  Serial.println(pos.zmm);
}

void ZRot(boolean DIR, int STEP)
{
  digitalWrite(Z_DIR_PIN, DIR);
  delayMicroseconds(5); // SET motor direction
  for (int i = 1; i <= STEP; i++)
  {
    digitalWrite(Z_STEP_PIN, HIGH);
    delayMicroseconds(On_Dwell);
    digitalWrite(Z_STEP_PIN, LOW);
    delayMicroseconds(OffDwell);
  }
}
