/////////////////////// visiGenieSerial 06/10/2015 ///////////////////////
//
//      Library to utilise the 4D Systems Genie interface to displays
//      that have been created using the Visi-Genie creator platform.
//      This is intended to be used with the Arduino platform.
//
//      Improvements/Updates by
//        4D Systems Engineering, October 2015, www.4dsystems.com.au
//        4D Systems Engineering, September 2015, www.4dsystems.com.au
//        4D Systems Engineering, August 2015, www.4dsystems.com.au
//        4D Systems Engineering, May 2015, www.4dsystems.com.au
//        Matt Jenkins, March 2015, www.majenko.com
//        Clinton Keith, January 2015, www.clintonkeith.com
//        4D Systems Engineering, July 2014, www.4dsystems.com.au
//        Clinton Keith, March 2014, www.clintonkeith.com
//        Clinton Keith, January 2014, www.clintonkeith.com
//        4D Systems Engineering, January 2014, www.4dsystems.com.au
//        4D Systems Engineering, September 2013, www.4dsystems.com.au
//      Written by
//        Rob Gray (GRAYnomad), June 2013, www.robgray.com
//      Based on code by
//        Gordon Henderson, February 2013, <projects@drogon.net>
//
//      Copyright (c) 2012-2014 4D Systems Pty Ltd, Sydney, Australia
/*********************************************************************
 * This file is part of visiGenieSerial:
 *    visiGenieSerial is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU Lesser General Public License as
 *    published by the Free Software Foundation, either version 3 of the
 *    License, or (at your option) any later version.
 *
 *    visiGenieSerial is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Lesser General Public License for more details.
 *
 *    You should have received a copy of the GNU Lesser General Public
 *    License along with visiGenieSerial.
 *    If not, see <http://www.gnu.org/licenses/>.
 *********************************************************************/

#include "visiGenieSerial.h"
#include <math.h>
#include <string.h>

#define DEC 10
#define HEX 16
#define OCT 8
#define BIN 2

#if (ARDUINO_BASED == 1)
  #if (ARDUINO >= 100)
      # include "Arduino.h" // for Arduino 1.0
  #else
      # include "WProgram.h" // for Arduino 23
  #endif
#endif

static void        flushEventQueue     (void);
static void        handleError         (void);
static void        setLinkState        (uint16_t newstate);
static uint16_t    getLinkState        (void);
static bool        enqueueEvent        (uint8_t * data);
static uint8_t     getchar             (void);
static uint16_t    getCharSerial       (void);
static void        waitForIdle         (void);
static void        pushLinkState       (uint8_t newstate);
static void        popLinkState        (void);
static void        fatalError          (void);
static void        flushSerialInput    (void);
static void        resync              (void);

static EventQueueStruct EventQueue;
static uint8_t LinkStates[MAX_LINK_STATES];
static uint8_t *LinkState;
static int Timeout;
static int Error;
static uint8_t rxframe_count;
static int FatalErrors;
static UserApiConfig  *deviceSerial;
static UserApiConfig  *debugSerial;
static UserEventHandlerPtr UserHandler;
static UserBytePtr UserByteReader;
static UserDoubleBytePtr UserDoubleByteReader;

void genieInitWithConfig(UserApiConfig *config) {

    UserHandler = NULL;
    UserByteReader = NULL;
    UserDoubleByteReader = NULL;
    debugSerial = NULL;
    LinkStates[0] = GENIE_LINK_IDLE;
    LinkState = &LinkStates[0];
    Timeout = TIMEOUT_PERIOD;
    Error = ERROR_NONE;
    rxframe_count = 0;
    FatalErrors = 0;
    deviceSerial = config;
    pushLinkState(GENIE_LINK_IDLE);
    flushEventQueue();
}

void genieAssignDebugPort(UserApiConfig *config) {
    debugSerial = config;
}

////////////////////// GetEventData ////////////////////////
//
// Returns the LSB and MSB of the event's data combined into
// a single uint16
//
// The data is transmitted from the display in big-endian format
// and stored the same so the user can't just access it as an int
// directly from the structure.
//
uint16_t genieGetEventData (GenieFrame * e) {
    return  (e->reportObject.data_msb << 8) + e->reportObject.data_lsb;
}

//////////////////////// genieGetNextByte ///////////////////////////
//
// Read one byte from the serial device.  Blocking.
//
uint8_t genieGetNextByte() {
    while (deviceSerial->available() < 1) {
        continue;
    }
    return deviceSerial->read();
}

//////////////////////// genieGetNextDoubleByte ///////////////////////////
//
// Reads two bytes from the serial device and joins them into one
// double byte.  Blocking.
//
uint16_t genieGetNextDoubleByte(void) {
    uint16_t out;
    while (deviceSerial->available() < 1) {
        continue;
    }
    out = (deviceSerial->read()) << 8;
    out |= deviceSerial->read();
    return out;
}

//////////////////////// Genie::EventIs ///////////////////////////
//
// Compares the cmd, object and index fields of the event's
// structure.
//
// Returns:     TRUE if all the fields match the caller's parms
//              FALSE if any of them don't
//
static bool eventIs(GenieFrame * e, uint8_t cmd, uint8_t object, uint8_t index) {
    return (e->reportObject.cmd == cmd &&
            e->reportObject.object == object &&
            e->reportObject.index == index);
}

////////////////////// Genie::WaitForIdle ////////////////////////
//
// Wait for the link to become idle or for the timeout period,
// whichever comes first.
//
static void waitForIdle (void) {
    uint16_t do_event_result;
    long timeout = deviceSerial->millis() + Timeout;

    for ( ; deviceSerial->millis() < timeout;) {
        do_event_result = genieDoEvents(false);

        // if there was a character received from the
        // display restart the timeout because doEvents
        // is in the process of receiving something
        if (do_event_result == GENIE_EVENT_RXCHAR) {
            timeout = deviceSerial->millis() + Timeout;
        }

        if (getLinkState() == GENIE_LINK_IDLE) {
            return;
        }
    }

    Error = ERROR_TIMEOUT;
    handleError();
    return;
}

////////////////////// Genie::pushLinkState //////////////////////
//
// Push a link state onto a FILO stack
//
int linkCount = 0;
static void pushLinkState (uint8_t newstate) {
    if (linkCount >= MAX_LINK_STATES) {
        resync();
    }

    linkCount++;
    LinkState++;
    //if (debugSerial) { *debugSerial << " newstate = " << newstate << " LinkState count = " << linkCount << ", Freemem = " << freeRam() << ", " << (unsigned long)&LinkState[0] << ", rxframe_count = " << rxframe_count << endl; } ;
    setLinkState(newstate);
}

////////////////////// Genie::popLinkState //////////////////////
//
// Pop a link state from a FILO stack
//
static void popLinkState (void) {
    //if (debugSerial) { *debugSerial << "popLinkState\n"; }
    if (LinkState > &LinkStates[0]) {
        *LinkState = 0xFF;
        LinkState--;
        linkCount--;
    }
}

///////////////////////// Genie::DoEvents /////////////////////////
//
// This is the heart of the Genie comms state machine.
//
uint16_t genieDoEvents (bool DoHandler) {
    uint8_t c;
    static uint8_t  rx_data[6];
    static uint8_t  checksum = 0;
    c = getchar();
    static struct MagicReportHeader magicHeader;
    static uint8_t magicByte = 0;

    //if (debugSerial && c != 0xFD) *debugSerial << _HEX(c)<<", "<<"["<<getLinkState()<<"], ";
    ////////////////////////////////////////////
    //
    // If there are no characters to process and we have
    // queued events call the user's handler function.
    //
    if (Error == ERROR_NOCHAR) {
        if ((EventQueue.n_events > 0) && (UserHandler != NULL) && DoHandler) {
            (UserHandler)();
        }

        return GENIE_EVENT_NONE;
    }

    //if (debugSerial) { *debugSerial << "Freemem = " << freeRam()<< endl; } ;
    //return GENIE_EVENT_RXCHAR; // debug

    ///////////////////////////////////////////
    //
    // Main state machine
    //

    switch (getLinkState()) {
        case GENIE_LINK_IDLE:
            switch (c) {
                case GENIE_REPORT_EVENT:
                    // event frame out of the blue, set the link state
                    // and fall through to the frame-accumulate code
                    // at the end of this function
                    pushLinkState(GENIE_LINK_RXEVENT);
                    break;

                case GENIEM_REPORT_BYTES:
                    magicByte = 0;
                    pushLinkState(GENIE_LINK_RXMBYTES);
                    break;
   
                case GENIEM_REPORT_DBYTES:
                    magicByte = 0;
                    pushLinkState(GENIE_LINK_RXMDBYTES);
                    break;

                default:
                    // error, bad character, no other character
                    // is acceptable in this state
                    return GENIE_EVENT_RXCHAR;
            }

            break;

        case GENIE_LINK_WFAN:
            switch (c) {
                case GENIE_ACK:
                    popLinkState();
                    return GENIE_EVENT_RXCHAR;

                case GENIE_NAK:
                    popLinkState();
                    Error = ERROR_NAK;
                    handleError();
                    return GENIE_EVENT_RXCHAR;

                case GENIE_REPORT_EVENT:
                    // event frame out of the blue while waiting for an ACK
                    // save/set the link state and fall through to the
                    // frame-accumulate code at the end of this function
                    pushLinkState(GENIE_LINK_RXEVENT);
                    break;

                case GENIEM_REPORT_BYTES:
                    magicByte = 0;
                    pushLinkState(GENIE_LINK_RXMBYTES);
                    break;
   
                case GENIEM_REPORT_DBYTES:
                    magicByte = 0;
                    pushLinkState(GENIE_LINK_RXMDBYTES);
                    break;

                case GENIE_REPORT_OBJ:
                default:
                    // error, bad character
                    return GENIE_EVENT_RXCHAR;
            }

            break;

        case GENIE_LINK_WF_RXREPORT: // waiting for the first byte of a report
            switch (c) {
                case GENIE_REPORT_EVENT:
                    // event frame out of the blue while waiting for the first
                    // byte of a report frame
                    // save/set the link state and fall through to the
                    // frame-accumulate code at the end of this function
                    pushLinkState(GENIE_LINK_RXEVENT);
                    break;

                case GENIEM_REPORT_BYTES:
                    magicByte = 0;
                    pushLinkState(GENIE_LINK_RXMBYTES);
                    break;
   
                case GENIEM_REPORT_DBYTES:
                    magicByte = 0;
                    pushLinkState(GENIE_LINK_RXMDBYTES);
                    break;

                case GENIE_REPORT_OBJ:
                    // first byte of a report frame
                    // replace the GENIE_LINK_WF_RXREPORT link state
                    // with GENIE_LINK_RXREPORT to indicate that we
                    // are now receiving a report frame
                    popLinkState();
                    pushLinkState(GENIE_LINK_RXREPORT);
                    break;

                case GENIE_ACK:
                case GENIE_NAK:
                default:
                    // error, bad character
                    return GENIE_EVENT_RXCHAR;
                    //              break;
            }

        case GENIE_LINK_RXREPORT:       // already receiving report
        case GENIE_LINK_RXEVENT:        // already receiving event
        case GENIE_LINK_RXMBYTES:       // already receiving magic bytes
        case GENIE_LINK_RXMDBYTES:      // already receiving mahic doublebytes
        default:
            break;
    }

    ///////////////////////////////////////////////////////
    // We get here if we are in the process of receiving
    // a report or event frame. Accumulate GENIE_FRAME_SIZE
    // bytes into a local buffer then queue them as a frame
    // into the event queue
    //
    if (getLinkState() == GENIE_LINK_RXREPORT ||
            getLinkState() == GENIE_LINK_RXEVENT) {
        checksum = (rxframe_count == 0) ? c : checksum ^ c;
        rx_data[rxframe_count] = c;

        if (rxframe_count == GENIE_FRAME_SIZE - 1) {
            // all bytes received, if the CS is good
            // queue the frame and restore the link state
            if (checksum == 0) {
                enqueueEvent(rx_data);
                rxframe_count = 0;
                // revert the link state to whatever it was before
                // we started accumulating this frame
                popLinkState();
                return GENIE_EVENT_RXCHAR;
            } else {
                Error = ERROR_BAD_CS;
                handleError();
            }
        }

        rxframe_count++;
        return GENIE_EVENT_RXCHAR;
    }

    ///////////////////////////////////////////////////////
    // We get here if we are in the process of receiving
    // a magic report.  When the header has been received
    // trigger the byte or double-byte handler to receive
    // the rest of the data.
    //
    if (getLinkState() == GENIE_LINK_RXMBYTES || 
        getLinkState() == GENIE_LINK_RXMDBYTES) {

        switch(magicByte) {
            case 0:
                magicHeader.cmd = c;
                magicByte++;
                break;
            case 1:
                magicHeader.index = c;
                magicByte++;
                break;
            case 2:
                magicHeader.length = c;
                magicByte++;
                if (magicHeader.cmd == GENIEM_REPORT_BYTES) {
                    if (UserByteReader != NULL) {
                        UserByteReader(magicHeader.index, magicHeader.length);
                    } else {
                        // No handler defined - we need to sink the bytes.
                        while (--magicHeader.length > 0) {
                            (void)genieGetNextByte();
                        }
                    }
                } else if (magicHeader.cmd == GENIEM_REPORT_DBYTES) {
                    if (UserDoubleByteReader != NULL) {
                        UserDoubleByteReader(magicHeader.index, magicHeader.length);
                    } else {
                        // No handler defined - we need to sink the bytes.
                        while (--magicHeader.length > 0) {
                            (void)genieGetNextDoubleByte();
                        }
                    }
                }
                // Now we want to discard the checksum. We don't yet
                // know what has been going on with the data, so we
                // can't calculate the checksum.
                (void)genieGetNextByte();
                popLinkState();
                break;
        }
        return GENIE_EVENT_RXCHAR;
    }
    return GENIE_EVENT_RXCHAR; // What should we really return here?!
}

//////////////////////// Genie::getchar //////////////////////////
//
// Get a character from the selected Genie serial port
//
// Returns: ERROR_NOHANDLER if an Rx handler has not
//              been defined
//          ERROR_NOCHAR if no bytes have beeb received
//          The char if there was one to get
// Sets:    Error with any errors encountered
//
static uint8_t getchar() {
    uint16_t result;
    Error = ERROR_NONE;
    return getCharSerial();
}

///////////////////////////////////////////////////////////////////
// Serial port 0 (Serial) Rx  handler
// Return ERROR_NOCHAR if no character or the char in the lower
// byte if there is.
//
static uint16_t getCharSerial (void) {
#ifdef SERIAL

    if (deviceSerial->available() == 0) {
        Error = ERROR_NOCHAR;
        return ERROR_NOCHAR;
    }

    return (uint16_t) deviceSerial->read() & 0xFF;
#endif
  return 0;
}


/////////////////// Genie::fatalError ///////////////////////
//
static void fatalError(void) {
    if (FatalErrors++ > MAX_GENIE_FATALS) {
        //      *LinkState = GENIE_LINK_SHDN;
        //      Error = ERROR_NODISPLAY;
    }
}

///////////////// Genie::flushSerialInput ///////////////////
//
// Removes and discards all characters from the currently
// used serial port's Rx buffer.
//
static void flushSerialInput(void) {
    while (deviceSerial->read() >= 0);
}

/////////////////////// resync //////////////////////////
//
// This function does nothing for RESYNC_PERIOD to allow the display
// time to stop talking, then it flushes everything so the link
// can start again.
//
// Untested, will need work I'm sure.
//
static void resync (void) {
    //for (long timeout = userConfig->millis() + RESYNC_PERIOD ; userConfig->millis() < timeout;) {};
    flushSerialInput();
    flushEventQueue();
    linkCount = 0;
    LinkState = &LinkStates[0];
    *LinkState = GENIE_LINK_IDLE;
}

///////////////////////// handleError /////////////////////////
//
// So far really just a debugging aid, but can be enhanced to
// help recover from errors.
//
static void handleError (void) {
    //if (debugSerial) { *debugSerial << "Handle Error Called!\n"; }
}

////////////////////// Genie::flushEventQueue ////////////////////
//
// Reset all the event queue variables and start from scratch.
//
static void flushEventQueue(void) {
    EventQueue.rd_index = 0;
    EventQueue.wr_index = 0;
    EventQueue.n_events = 0;
}

////////////////////// DequeueEvent ///////////////////
//
// Copy the bytes from a queued input event to a buffer supplied
// by the caller.
//
// Parms:   GenieFrame * buff, a pointer to the user's buffer
//
// Returns: TRUE if there was an event to copy
//          FALSE if not
//
bool genieDequeueEvent(GenieFrame * buff) {
    if (EventQueue.n_events > 0) {
        memcpy (buff, &EventQueue.frames[EventQueue.rd_index],
                GENIE_FRAME_SIZE);
        EventQueue.rd_index++;
        EventQueue.rd_index &= MAX_GENIE_EVENTS - 1;
        EventQueue.n_events--;
        return TRUE;
    }

    return FALSE;
}

////////////////////// Genie::EnqueueEvent ///////////////////
//
// Copy the bytes from a buffer supplied by the caller
// to the input queue
//
// Parms:   uint8_t * data, a pointer to the user's data
//
// Returns: TRUE if there was an empty location in the queue
//              to copy the data into
//          FALSE if not
// Sets:    ERROR_REPLY_OVR if there was no room in the queue
//
static bool enqueueEvent (uint8_t * data) {
    if (EventQueue.n_events < MAX_GENIE_EVENTS - 2) {
        int i, j ;
        bool fnd=false ;
        j = EventQueue.wr_index ;
        for (i = EventQueue.n_events; i > 0; i--) 
        {
            j-- ;
            if (j < 0)
                j = MAX_GENIE_EVENTS - 1;
            if (   (EventQueue.frames[j].reportObject.cmd == data[0])
                && (EventQueue.frames[j].reportObject.object == data[1])
                && (EventQueue.frames[j].reportObject.index == data[2])  )
            {
                EventQueue.frames[j].reportObject.data_msb = data[3] ;
                EventQueue.frames[j].reportObject.data_lsb = data[4] ;
                fnd = true ;
                break ;
            }
        }
        if (!fnd)
        {
            memcpy (&EventQueue.frames[EventQueue.wr_index], data,
                    GENIE_FRAME_SIZE);
            EventQueue.wr_index++;
            EventQueue.wr_index &= MAX_GENIE_EVENTS - 1;
            EventQueue.n_events++;
            //if (debugSerial) { *debugSerial << "Enque Event " << _HEX(*data) << ", count = " << EventQueue.n_events << endl; }
            return TRUE;
        }
    } else {
        Error = ERROR_REPLY_OVR;
        handleError();
        return FALSE;
    }
}

//////////////////////// Genie::ReadObject ///////////////////////
//
// Send a read object command to the Genie display. Note that this
// function does not wait for the reply, that will be read in due
// course by DoEvents() and subsequently by the user's event
// handler.
//
bool genieReadObject (uint16_t object, uint16_t index) {
    uint8_t checksum;
    // Discard any pending reply frames
    //flushEventQueue();    // Removed due to preventing more than 2 readObjects being queued
    waitForIdle();
    Error = ERROR_NONE;
    deviceSerial->write((uint8_t)GENIE_READ_OBJ);
    checksum   = GENIE_READ_OBJ ;
    deviceSerial->write(object);
    checksum  ^= object ;
    deviceSerial->write(index);
    checksum  ^= index ;
    deviceSerial->write(checksum);
    pushLinkState(GENIE_LINK_WF_RXREPORT);
    return TRUE;
}

///////////////////// Genie::SetLinkState ////////////////////////
//
// Set the logical state of the link to the display.
//
// Parms:   uint16_t newstate, a value to be written to the
//              link's Genie::LinkState variable. Valid values are
//      GENIE_LINK_IDLE         0
//      GENIE_LINK_WFAN         1 // waiting for Ack or Nak
//      GENIE_LINK_WF_RXREPORT  2 // waiting for a report frame
//      GENIE_LINK_RXREPORT     3 // receiving a report frame
//      GENIE_LINK_RXEVENT      4 // receiving an event frame
//      GENIE_LINK_SHDN         5
//
static void setLinkState (uint16_t newstate) {
    *LinkState = newstate;

    if (newstate == GENIE_LINK_RXREPORT || \
            newstate == GENIE_LINK_RXEVENT) {
        rxframe_count = 0;
    }
}

/////////////////////// Genie::getLinkState //////////////////////
//
// Get the current logical state of the link to the display.
//
static uint16_t getLinkState (void) {
    return *LinkState;
}

///////////////////////// WriteObject //////////////////////
//
// Write data to an object on the display
//
uint16_t genieWriteObject (uint16_t object, uint16_t index, uint16_t data) {
    uint16_t msb, lsb ;
    uint8_t checksum ;
    waitForIdle();
    lsb = lowByte(data);
    msb = highByte(data);
    Error = ERROR_NONE;
    deviceSerial->write(GENIE_WRITE_OBJ) ;
    checksum  = GENIE_WRITE_OBJ ;
    deviceSerial->write(object) ;
    checksum ^= object ;
    deviceSerial->write(index) ;
    checksum ^= index ;
    deviceSerial->write(msb) ;
    checksum ^= msb;
    deviceSerial->write(lsb) ;
    checksum ^= lsb;
    deviceSerial->write(checksum) ;
    /*
    if (debugSerial) {
        *debugSerial << "WriteObject: " <<  ", ";
        *debugSerial << _HEX(object) << ", ";
        *debugSerial << _HEX(index) << ", ";
        *debugSerial << _HEX(msb) << ", ";
        *debugSerial << _HEX(lsb) << ", ";
        *debugSerial << _HEX(checksum) << endl;
        *debugSerial << "Freemem = " << freeRam()<< endl;
    }
    */
    pushLinkState(GENIE_LINK_WFAN);
}

/////////////////////// WriteContrast //////////////////////
//
// Alter the display contrast (backlight)
//
// Parms:   uint8_t value: The required contrast setting, only
//      values from 0 to 15 are valid. 0 or 1 for most displays
//      and 0 to 15 for the uLCD-43, uLCD-70, uLCD-35, uLCD-220RD
//
void genieWriteContrast (uint16_t value) {
    unsigned int checksum ;
    waitForIdle();
    deviceSerial->write(GENIE_WRITE_CONTRAST) ;
    checksum  = GENIE_WRITE_CONTRAST ;
    deviceSerial->write(value) ;
    checksum ^= value ;
    deviceSerial->write(checksum) ;
    pushLinkState(GENIE_LINK_WFAN);
}

/////////////////////// WriteStr ////////////////////////
//
// Write a string to the display (ASCII)
// ASCII characters are 1 byte each
//
uint16_t genieWriteStr (uint16_t index, char *string) {
    char *p;
    unsigned int checksum;
    int len = strlen (string);

    if (len > 255) {
        return -1;
    }

    waitForIdle();
    deviceSerial->write(GENIE_WRITE_STR);
    checksum  = GENIE_WRITE_STR;
    deviceSerial->write(index);
    checksum ^= index;
    deviceSerial->write((unsigned char)len);
    checksum ^= len;

    for (p = string ; *p ; ++p) {
        deviceSerial->write(*p);
        checksum ^= *p;
    }

    deviceSerial->write(checksum);
    pushLinkState(GENIE_LINK_WFAN);
    return 0;
}

#ifdef AVR
uint16_t genieWriteStr(uint16_t index, const __FlashStringHelper *ifsh){
	PGM_P p = reinterpret_cast<PGM_P>(ifsh);
	PGM_P p2 = reinterpret_cast<PGM_P>(ifsh);
	size_t n = 0;
	int len = 0;
	while (1) {
		unsigned char d = pgm_read_byte(p2++);
		len++;
		if (d == 0) break;
	}
  
 
	char arr[len];
	int x = 0;
	while (1) {
		unsigned char c = pgm_read_byte(p++);
		arr[x] = c;
		x++;
		if (c == 0) break;
	}
	WriteStr(index, arr);
	return 0;	
}
#endif

#if (ARDUINO_BASED == 1)
uint16_t genieWriteStr(uint16_t index, const String &s){
	//s.c_str(), s.length()
	int len = s.length();
	char arr[len + 1];
	s.toCharArray(arr,len + 1);
	WriteStr(index, arr);
	return 0;
}
#endif

/* I need to port these later on. Use existing Arduino writeStr demo for testing
uint16_t Genie::WriteStr (uint16_t index, long n) { 
	char buf[8 * sizeof(long) + 1]; // Assumes 8-bit chars plus zero byte.
	char *str = &buf[sizeof(buf) - 1];
	
	long N = n;
	n = abs(n);

	*str = '\0';

	do {
		unsigned long m = n;
		n /= 10;
		char c = m - 10 * n;
		*--str = c < 10 ? c + '0' : c + 'A' - 10;
	} while(n);
	
	if (N < 0) {
		*--str = '-';
	}
	
	WriteStr(index, str);

	

	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, long n, int base) { 
	char buf[8 * sizeof(long) + 1]; // Assumes 8-bit chars plus zero byte.
	char *str = &buf[sizeof(buf) - 1];
	
	long N;
	*str = '\0';
	if(n>=0)
	{
		// prevent crash if called with base == 1
		if (base < 2) base = 10;
		if(base == 10)
		{
			N = n;
			n = abs(n);
		}
	
		do {
			unsigned long m = n;
			n /= base;
			char c = m - base * n;
			*--str = c < 10 ? c + '0' : c + 'A' - 10;
		} while(n);
		
		if(base == 10)
		{
			if (N < 0) {
				*--str = '-';
			}
		}
			
	}
	
	else if(n<0)
	{
		unsigned long n2 = (unsigned long)n;
		uint8_t base2 = base;
		do {
		unsigned long m = n2;
		n2 /= base2;
		char c = m - base2 * n2;
		*--str = c < 10 ? c + '0' : c + 'A' - 10;
		} while(n2);
		
	}
	
	
    WriteStr(index, str);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, int n) { 
	WriteStr (index, (long) n);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, int n, int base) { 
	WriteStr (index, (long) n, base);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, unsigned long n) { 
	char buf[8 * sizeof(long) + 1]; // Assumes 8-bit chars plus zero byte.
	char *str = &buf[sizeof(buf) - 1];
	
	long N = n;
	n = abs(n);

	*str = '\0';

	do {
		unsigned long m = n;
		n /= 10;
		char c = m - 10 * n;
		*--str = c < 10 ? c + '0' : c + 'A' - 10;
	} while(n);
	
	WriteStr(index, str);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, unsigned long n, int base) { 
	char buf[8 * sizeof(long) + 1]; // Assumes 8-bit chars plus zero byte.
	char *str = &buf[sizeof(buf) - 1];
	
	*str = '\0';

	// prevent crash if called with base == 1
	if (base < 2) base = 10;
	do {
		unsigned long m = n;
		n /= base;
		char c = m - base * n;
		*--str = c < 10 ? c + '0' : c + 'A' - 10;
	} while(n);
				
    WriteStr(index, str);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, unsigned int n) { 
	WriteStr (index, (unsigned long) n);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, unsigned n, int base) { 
	WriteStr (index, (unsigned long) n, base);
	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, double number, int digits) { 
	char buf[8 * sizeof(long) + 1]; // Assumes 8-bit chars plus zero byte.
	char *str = &buf[sizeof(buf) - 1];
	*str = '\0';  

	double number2 = number;
	if (number < 0.0)
	{
	number = -number;
	}

	// Round correctly so that print(1.999, 2) prints as "2.00"
	double rounding = 0.5;
	for (int i=0; i<digits; ++i)
	rounding /= 10.0;

	number += rounding;

	unsigned long int_part = (unsigned long)number;
	double remainder = number - (double)int_part;

	// Extract digits from the remainder one at a time
	int digits2 = digits;
	str = &buf[sizeof(buf) - 1 - digits2];
	while (digits2-- > 0)
	{
	remainder *= 10.0;
	int toPrint = int(remainder);
	char c = toPrint + 48;
	*str++ = c;
	remainder -= toPrint; 
	}
	str = &buf[sizeof(buf) - 1 - digits];
	if (digits > 0) {
	*--str = '.';
	}
	// Extract the integer part of the number and print it  
	do {
	unsigned long m = int_part;
	int_part /= 10;
	char c = m - 10 * int_part;
	*--str = c < 10 ? c + '0' : c + 'A' - 10;
	} while(int_part);

	// Handle negative numbers
	if (number2 < 0.0)
	{
	 *--str = '-';
	}

	WriteStr(index, str);

	return 0;
}

uint16_t Genie::WriteStr (uint16_t index, double n){
	WriteStr(index, n, 2);

}
*/
/////////////////////// WriteStrU ////////////////////////
//
// Write a string to the display (Unicode)
// Unicode characters are 2 bytes each
//
uint16_t genieWriteStrU (uint16_t index, uint16_t *string) {
    uint16_t *p;
    unsigned int checksum;
    int len = 0;
    p = string;

    while (*p++) {
        len++;
    }

    if (len > 255) {
        return -1;
    }

    waitForIdle();
    deviceSerial->write(GENIE_WRITE_STRU);
    checksum  = GENIE_WRITE_STRU;
    deviceSerial->write(index);
    checksum ^= index;
    deviceSerial->write((unsigned char)(len));
    checksum ^= (len);
    p = string;

    while (*p) {
        deviceSerial->write (*p >> 8);
        checksum ^= *p >> 8;
        deviceSerial->write (*p);
        checksum ^= *p++ & 0xff;
    }

    deviceSerial->write(checksum);
    pushLinkState(GENIE_LINK_WFAN);
    return 0;
}

/////////////////// AttachEventHandler //////////////////////
//
// "Attaches" a pointer to the users event handler by writing
// the pointer into the variable used by doEVents()
//
void genieAttachEventHandler (UserEventHandlerPtr handler) {
    UserHandler = handler;
}

/////////////////// AttachMagicByteReader //////////////////////
//
// "Attaches" a pointer to a user's function for receiving
// GenieMagic byte reports.
//
void genieAttachMagicByteReader(UserBytePtr handler) {
    UserByteReader = handler;
}

/////////////////// AttachMagicDoubleByteReader//////////////////////
//
// "Attaches" a pointer to a user's function for receiving
// GenieMagic doublebyte reports.
//
void genieAttachMagicDoubleByteReader(UserDoubleBytePtr handler) {
    UserDoubleByteReader = handler;
}

/////////////////////// WriteMagicBytes ////////////////////////
//
// Write an array of bytes to a Magic object
//
uint16_t genieWriteMagicBytes (uint16_t index, uint8_t *bytes, uint16_t len) {
    unsigned int checksum;

    if (len > 255) {
        return -1;
    }

    waitForIdle();
    deviceSerial->write(GENIEM_WRITE_BYTES);
    checksum  = GENIEM_WRITE_BYTES;
    deviceSerial->write(index);
    checksum ^= index;
    deviceSerial->write((unsigned char)len);
    checksum ^= len;

    for (int i = 0; i < len; i++) {
        deviceSerial->write(bytes[i]);
        checksum ^= bytes[i];
    }

    deviceSerial->write(checksum);
    pushLinkState(GENIE_LINK_WFAN);
    return 0;
}

/////////////////////// WriteMagicDBytes ////////////////////////
//
// Write an array of 16-bit short values to a Magic object
//
uint16_t genieWriteMagicDBytes (uint16_t index, uint16_t *shorts, uint16_t len) {
    unsigned int checksum;

    if (len > 255) {
        return -1;
    }

    waitForIdle();
    deviceSerial->write(GENIEM_WRITE_DBYTES);
    checksum  = GENIEM_WRITE_DBYTES;
    deviceSerial->write(index);
    checksum ^= index;
    deviceSerial->write((unsigned char)(len));
    checksum ^= (len);

    for (int i = 0; i < len; i++) {
        deviceSerial->write (shorts[i] >> 8);
        checksum ^= shorts[i] >> 8;
        deviceSerial->write (shorts[i] & 0xFF);
        checksum ^= shorts[i] & 0xff;
    }

    deviceSerial->write(checksum);
    pushLinkState(GENIE_LINK_WFAN);
    return 0;
}

