// pad - implementation
#include "pad.h"
#include "dbg.h"
#include <cell/pad.h>
#include <io/pad.h>
#include <string.h>

#define PAD_BUTTON_COUNT 16

// Which digital word (1 or 2) each button lives in, and its control bit - indexed by PadButton.
static const struct { int word; uint16_t bit; } padControlBits[PAD_BUTTON_COUNT] = {
   { 1, CELL_PAD_CTRL_UP },       { 1, CELL_PAD_CTRL_DOWN },   { 1, CELL_PAD_CTRL_LEFT },  { 1, CELL_PAD_CTRL_RIGHT },
   { 2, CELL_PAD_CTRL_CROSS },    { 2, CELL_PAD_CTRL_CIRCLE }, { 2, CELL_PAD_CTRL_SQUARE }, { 2, CELL_PAD_CTRL_TRIANGLE },
   { 2, CELL_PAD_CTRL_L1 },       { 2, CELL_PAD_CTRL_R1 },     { 2, CELL_PAD_CTRL_L2 },    { 2, CELL_PAD_CTRL_R2 },
   { 1, CELL_PAD_CTRL_START },    { 1, CELL_PAD_CTRL_SELECT }, { 1, CELL_PAD_CTRL_L3 },    { 1, CELL_PAD_CTRL_R3 },
};

static CellPadData portData[MAX_PORT_NUM];
static unsigned remoteButtons[MAX_PORT_NUM];
static uint16_t lastRemoteCode[MAX_PORT_NUM];
static unsigned remoteSpecialButtons[MAX_PORT_NUM];
static PadButtonState remoteSpecialStates[PAD_REMOTE_BUTTON_COUNT];
static volatile unsigned rawRemoteSpecial;
static unsigned prevRemoteSpecial;
static PadButtonState buttonStates[PAD_BUTTON_COUNT];
static Stick leftStick;
static Stick rightStick;
static volatile uint16_t rawDigital1;
static volatile uint16_t rawDigital2;
static uint16_t prev1 = 0;
static uint16_t prev2 = 0;

static PadButtonState getState(int held, int wasHeld)
{
   if (held && !wasHeld) return PAD_BUTTON_STATE_PRESSED;
   if (held && wasHeld) return PAD_BUTTON_STATE_HELD;
   if (!held && wasHeld) return PAD_BUTTON_STATE_RELEASED;
   return PAD_BUTTON_STATE_UP;
}

static int buttonHeld(int button, uint16_t digital1, uint16_t digital2)
{
   uint16_t word = padControlBits[button].word == 1 ? digital1 : digital2;
   return (word & padControlBits[button].bit) != 0;
}

// Translate BD Remote key codes into the same logical buttons used by the rest of the app.
// The aliases preserve the mappings used by the upstream Yo! Player implementation.
static unsigned remoteCodeToButton(uint16_t code)
{
   switch (code) {
      case BTN_BD_UP:       return 1u << PAD_BTN_UP;
      case BTN_BD_DOWN:     return 1u << PAD_BTN_DOWN;
      case BTN_BD_LEFT:     return 1u << PAD_BTN_LEFT;
      case BTN_BD_RIGHT:    return 1u << PAD_BTN_RIGHT;
      case BTN_BD_CROSS:    return 1u << PAD_BTN_CROSS;
      case BTN_BD_CIRCLE:   return 1u << PAD_BTN_CIRCLE;
      case BTN_BD_SQUARE:   return 1u << PAD_BTN_SQUARE;
      case BTN_BD_TRIANGLE: return 1u << PAD_BTN_TRIANGLE;
      case BTN_BD_L1:       return 1u << PAD_BTN_L1;
      case BTN_BD_R1:       return 1u << PAD_BTN_R1;
      case BTN_BD_L2:       return 1u << PAD_BTN_L2;
      case BTN_BD_R2:       return 1u << PAD_BTN_R2;
      case BTN_BD_START:    return 1u << PAD_BTN_START;
      case BTN_BD_SELECT:   return 1u << PAD_BTN_SELECT;
      case BTN_BD_L3:       return 1u << PAD_BTN_L3;
      case BTN_BD_R3:       return 1u << PAD_BTN_R3;

      // BD Remote transport/navigation keys that have no direct DualShock key.
      case BTN_BD_ENTER:
      case BTN_BD_PLAY:
      case BTN_BD_PAUSE:
         return 1u << PAD_BTN_CROSS;
      case BTN_BD_RETURN:
         return 1u << PAD_BTN_CIRCLE;
      case BTN_BD_SCAN_FWD:
         return 1u << PAD_BTN_RIGHT;
      case BTN_BD_SCAN_REV:
         return 1u << PAD_BTN_LEFT;
      case BTN_BD_DISPLAY:
         return 1u << PAD_BTN_L2;
      default:
         return 0;
   }
}

static void addRemoteButtons(unsigned mask, uint16_t *digital1, uint16_t *digital2)
{
   for (int button = 0; button < PAD_BUTTON_COUNT; button++) {
      if (!(mask & (1u << button))) continue;
      if (padControlBits[button].word == 1)
         *digital1 |= padControlBits[button].bit;
      else
         *digital2 |= padControlBits[button].bit;
   }
}

void initPad(void)
{
   ioPadInit(MAX_PORT_NUM);
   memset(portData, 0, sizeof(portData));
   memset(remoteButtons, 0, sizeof(remoteButtons));
   memset(remoteSpecialButtons, 0, sizeof(remoteSpecialButtons));
   memset(remoteSpecialStates, 0, sizeof(remoteSpecialStates));
   rawRemoteSpecial = 0;
   prevRemoteSpecial = 0;
   for (int port = 0; port < MAX_PORT_NUM; port++)
      lastRemoteCode[port] = 0xffff;
   memset(buttonStates, 0, sizeof(buttonStates));
   memset(&leftStick, 0, sizeof(leftStick));
   memset(&rightStick, 0, sizeof(rightStick));
   rawDigital1 = 0;
   rawDigital2 = 0;
   prev1 = 0;
   prev2 = 0;
}

void pollPad(void)
{
   padInfo2 info;
   uint16_t digital1 = 0;
   uint16_t digital2 = 0;
   int haveInfo = ioPadGetInfo2(&info) == 0;
   int haveAnalogSource = 0;
   unsigned remoteSpecial = 0;

   if (haveInfo) {
      for (int port = 0; port < MAX_PORT_NUM; port++) {
         if (!(info.port_status[port] & 1u)) {
            memset(&portData[port], 0, sizeof(portData[port]));
            remoteButtons[port] = 0;
            remoteSpecialButtons[port] = 0;
            lastRemoteCode[port] = 0xffff;
            continue;
         }

         if (info.device_type[port] == PAD_TYPE_REMOTE) {
            padData nextData;
            uint32_t deviceType = 0;
            memset(&nextData, 0, sizeof(nextData));
            if (ioPadGetDataExtra((uint32_t)port, &deviceType, &nextData) == 0 && nextData.len > 0) {
               uint16_t code = nextData.button[25]; // BTN_BDCODE / PAD_BUTTON_OFFSET_BD_CODE
               if (code == BTN_BD_RELEASE) {
                  remoteButtons[port] = 0;
                  remoteSpecialButtons[port] = 0;
                  lastRemoteCode[port] = 0xffff;
               } else {
                  unsigned mapped = remoteCodeToButton(code);
                  remoteButtons[port] = mapped;
                  remoteSpecialButtons[port] = 0;
                  if (code == BTN_BD_AUDIO) remoteSpecialButtons[port] = 1u << PAD_REMOTE_AUDIO;
                  else if (code == BTN_BD_SUBTITLE) remoteSpecialButtons[port] = 1u << PAD_REMOTE_SUBTITLE;
                  else if (code == BTN_BD_PREV) remoteSpecialButtons[port] = 1u << PAD_REMOTE_PREV;
                  else if (code == BTN_BD_NEXT) remoteSpecialButtons[port] = 1u << PAD_REMOTE_NEXT;
                  else if (code == BTN_BD_SLOW_REV) remoteSpecialButtons[port] = 1u << PAD_REMOTE_SLOW_REV;
                  else if (code == BTN_BD_SLOW_FWD) remoteSpecialButtons[port] = 1u << PAD_REMOTE_SLOW_FWD;
                  int special = code == BTN_BD_AUDIO || code == BTN_BD_SUBTITLE ||
                                code == BTN_BD_PREV || code == BTN_BD_NEXT ||
                                code == BTN_BD_SLOW_REV || code == BTN_BD_SLOW_FWD;
                  if (!mapped && !special && lastRemoteCode[port] != code)
                     logWarn("[pad] unmapped BD Remote code=0x%04x port=%d\n", code, port);
                  lastRemoteCode[port] = code;
               }
            }
            addRemoteButtons(remoteButtons[port], &digital1, &digital2);
            remoteSpecial |= remoteSpecialButtons[port];
         } else {
            padData nextData;
            if (ioPadGetData((uint32_t)port, &nextData) == 0 && nextData.len > 0)
               portData[port] = nextData;

            digital1 |= portData[port].button[PAD_BUTTON_OFFSET_DIGITAL1];
            digital2 |= portData[port].button[PAD_BUTTON_OFFSET_DIGITAL2];

            // A BD Remote has no analogue axes. Use the first connected standard pad.
            if (!haveAnalogSource) {
               leftStick.x = (int)portData[port].button[PAD_BUTTON_OFFSET_ANALOG_LEFT_X] - 128;
               leftStick.y = (int)portData[port].button[PAD_BUTTON_OFFSET_ANALOG_LEFT_Y] - 128;
               rightStick.x = (int)portData[port].button[PAD_BUTTON_OFFSET_ANALOG_RIGHT_X] - 128;
               rightStick.y = (int)portData[port].button[PAD_BUTTON_OFFSET_ANALOG_RIGHT_Y] - 128;
               haveAnalogSource = 1;
            }
         }
      }
   } else {
      // Retain the original port-0 path if the extended info API is unavailable.
      padData nextData;
      if (ioPadGetData(0, &nextData) == 0 && nextData.len > 0)
         portData[0] = nextData;
      digital1 = portData[0].button[PAD_BUTTON_OFFSET_DIGITAL1];
      digital2 = portData[0].button[PAD_BUTTON_OFFSET_DIGITAL2];
      leftStick.x = (int)portData[0].button[PAD_BUTTON_OFFSET_ANALOG_LEFT_X] - 128;
      leftStick.y = (int)portData[0].button[PAD_BUTTON_OFFSET_ANALOG_LEFT_Y] - 128;
      rightStick.x = (int)portData[0].button[PAD_BUTTON_OFFSET_ANALOG_RIGHT_X] - 128;
      rightStick.y = (int)portData[0].button[PAD_BUTTON_OFFSET_ANALOG_RIGHT_Y] - 128;
      haveAnalogSource = 1;
   }

   if (!haveAnalogSource) {
      memset(&leftStick, 0, sizeof(leftStick));
      memset(&rightStick, 0, sizeof(rightStick));
   }

   rawDigital1 = digital1;
   rawDigital2 = digital2;
   rawRemoteSpecial = remoteSpecial;
}

void updatePadEdges(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      buttonStates[button] = getState(buttonHeld(button, digital1, digital2), buttonHeld(button, prev1, prev2));
   unsigned remoteSpecial = rawRemoteSpecial;
   for (int button = 0; button < PAD_REMOTE_BUTTON_COUNT; button++) {
      int held = (remoteSpecial & (1u << button)) != 0;
      int wasHeld = (prevRemoteSpecial & (1u << button)) != 0;
      remoteSpecialStates[button] = getState(held, wasHeld);
   }
   prevRemoteSpecial = remoteSpecial;
   prev1 = digital1;
   prev2 = digital2;
}

void updatePad(void)
{
   pollPad();
   updatePadEdges();
}

unsigned getPadDownButtons(void)
{
   uint16_t digital1 = rawDigital1, digital2 = rawDigital2;
   unsigned mask = 0;
   for (int button = 0; button < PAD_BUTTON_COUNT; button++)
      if (buttonHeld(button, digital1, digital2)) mask |= 1u << button;
   return mask;
}

PadButtonState getPadButtonState(PadButton button)
{
   return buttonStates[button];
}

int isPadButtonPressed(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_PRESSED;
}

int isPadButtonHeld(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_HELD;
}

int isPadButtonDown(PadButton button)
{
   PadButtonState state = getPadButtonState(button);
   return state == PAD_BUTTON_STATE_PRESSED || state == PAD_BUTTON_STATE_HELD;
}

int isPadButtonReleased(PadButton button)
{
   return getPadButtonState(button) == PAD_BUTTON_STATE_RELEASED;
}

PadButtonState getPadRemoteButtonState(PadRemoteButton button)
{
   if (button < 0 || button >= PAD_REMOTE_BUTTON_COUNT) return PAD_BUTTON_STATE_UP;
   return remoteSpecialStates[button];
}

int isPadRemoteButtonPressed(PadRemoteButton button)
{
   return getPadRemoteButtonState(button) == PAD_BUTTON_STATE_PRESSED;
}

Stick getPadLeftStick(void)
{
   return leftStick;
}

Stick getPadRightStick(void)
{
   return rightStick;
}
