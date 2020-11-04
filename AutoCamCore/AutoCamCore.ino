/*
NRKbeta AutoCam.
This code is made to perform ad lib video production, and has been tested on Uno
Ethernet, Mega and Mega ADK.

At NRK we are typically using this in out radio studios. Here each participant 
has a dedicated microphone and camera, and in addition we have a camera covering
the whole scene. Based on the input levels from an auxiliary from each of the 
microphones, the code decides which camera goes live.

The logic is like a game with repeated rounds, where the result of each round
decides the outcome. In each round, we count how often each of the microphone
inputs exceeds a defined  threshold (variable: level). The outcome will be the 
camera on the participant who is talking, unless more than one is speaking at ones,
or no one is speaking. In those cases the total camera will be chosen.
     
If one of the participants talk for a long time, we do a short cut back to the
previous camera, for a listening shot.

Connect your first microphone to A0, the second to A1 and so forth.
*/

#include <avr/wdt.h>
#include <Wire.h>
#include <Adafruit_ADS1015.h>


//////////////////SETUP START//////////////////////////

int level = 200;    // This is the trigger level to exceed from the microphones when talking in them, and the most crucial value to get right.
                    // analog levels are in the range of  0-1024, normaly from 0-5v, but this can be altered by altering the analogReference
                    // in the setup section of this sketch. (https://www.arduino.cc/reference/en/language/functions/analog-io/analogreference/)

int inputs =  3;    // Number of michrophones to monitor. (Do NOT monitor inputs that ar not connected to an audio source, as they will report random values)
int camstart = 1;   // Input number on the video mixer of the camera corresponding to the first microphone to monitor (the rest must follow in order)
int total = 4;      // Input channel for total camera on video mixer

int rhythm = 1500;  // round duration i milliseconds (should be in the range of 1000-2000)
int swing = 50;     // round duration variation in percentage (should be in the range of 10-70)
int simultan = 30;  // simultaneous talk sensitivity before cut to total camera, in percent. 

#define board 53    // number of analog pin 0 on the Arduino use 13 for Uno & Ethernet and 53 for Mega & ADK
#define debug 1     // 1 to output debug info to the serial monitor, 0 to not

#define info_logging
#ifdef info_logging
#define info(__VA_ARGS__) Serial.println(__VA_ARGS__)
#endif 

#define debug_log
#ifdef debug_log
#define debugln(__VA_ARGS__) Serial.println(__VA_ARGS__);
#endif 
//////////////////SETUP END//////////////////////////

Adafruit_ADS1115 ads;

int active=total;   // Sets the active camera to the total camera at start
int last=total;     // Sets the last camera to the total camera at start

int sample;         // Current value read from the analog input
int16_t adc_sample;

int win;            // The video source of the winner of the most recent round
int count;          // The number of samples in the most recent round  
int rounds;         // The number of rounds since the most recent cut
int noone;          // The number of rounds of consecutive silence
int multi;          // The number of simultaneous active inputs in the most recent round  
int simulL;         // The number of rounds of consecutive simultaneous active inputs
String  reason;     // An explanation for the cut printed during debug

int back = active;   // variable that holds the previous video source, for cut backs
long timeout;        // variable that holds end time of the current round
boolean cutBack=0;   // switch that is set to 1 if we do a cut back

String inputString = ""; // a String to hold incoming data (config settings)
bool stringComplete = false; // whether the string is complete

void setup() {
    // OLD:
    analogReference(INTERNAL);  // try INTERNAL for Uno and INTERNAL1V1 for Mega at line audio levels

    // New ADC:
    ads.setGain(GAIN_16);
    ads.begin();

    Serial.begin(115200);       // start the serial port to print debug messages
     
    inputString.reserve(32);
    
    info("");
    info("===============================");
    info("NRKBeta AutoCam - modded by ASH");
    info("");
    echoStatus();
}
 
void loop(){
    ParseConfig();
    AutoCam();
}

void ParseConfig() {
    if (!stringComplete) {
        return;
    }
    if(inputString.startsWith("$AC,")) {
        inputString.remove(0,4);
        debugln("AutoCam Command received: "+inputString);
        // TODO: Do CRC-check if CRC is there
        while(inputString.length() > 0) {
            if(inputString.startsWith("NUM_INPUTS,")) {
                inputString.remove(0,11);
                inputs = inputString.toInt();
                echoStatus();
            }
            else if(inputString.startsWith("STATUS,")) {
                inputString.remove(0,7);
                echoStatus();
            }
            else if(inputString.startsWith("FIRST_VIDEO_INPUT,")) {
                inputString.remove(0,18);
                if(inputString.toInt() != 0) {
                    camstart = inputString.toInt();
                }
                echoStatus();
            }
            else if(inputString.startsWith("TOTAL,")) {
                inputString.remove(0,6);
                if(inputString.toInt() != 0) {
                    total = inputString.toInt();
                }
                echoStatus();
            }
            else if(inputString.startsWith("TRIGGER_LEVEL,")) {
                inputString.remove(0,14);
                if(inputString.toInt() != 0) {
                    level = inputString.toInt();
                }
                echoStatus();
            }
            else if(inputString.startsWith("GAIN,")) {
                inputString.remove(0,5);
                // Gain-value is int
                if(inputString.toInt() != 0) {
                		int gain = 2;
                    gain = inputString.toInt();
                    switch(gain) {
                    	 case 1:
                    	 		ads.setGain(GAIN_ONE);
                    	 break;
                    	 case 2:
                    	 		ads.setGain(GAIN_TWO);
                    	 break;
                    	 case 4:
                    	 		ads.setGain(GAIN_FOUR);
                    	 break;
                    	 case 8:
                    	 		ads.setGain(GAIN_EIGHT);
                    	 break;
                    	 case 16:
                    	 		ads.setGain(GAIN_SIXTEEN);
                    	 break;
                    	 case 0:
                    	 default:
                    	 		ads.setGain(GAIN_TWOTHIRDS);
                    	 break;
                    }
                    ads.begin();
                    debugln("Changed gain setting on ADC");
                    info("$ACR,GAIN,");
                    info(gain);
                }
            }
            else if(inputString.startsWith("RESETCPU")) {
                // Enable watchdog, then infinite loop to reboot
                wdt_enable(WDTO_30MS);
                while(1) {};
            }
            else {
                //debugln("String was not empty, but no command found. Looking for next command.");
                //debugln(inputString);
                int commaPos = inputString.indexOf(',');
                if(commaPos != -1) {
                    inputString.remove(0,commaPos+1);
                } else {
                    inputString = "";
                    stringComplete = false;
                }
            }
        }
    }
}
     
void AutoCam(){
 
  //////////////////ROUND START//////////////////////
  // reset variables for each round
  int input[]={0,0,0,0,0,0};    // reset trigger count array
  count = 0;                    // reset count of samples in round 

  //determine round duration in ms
  int dur = rhythm + ((random(100)) * swing * 0.0002 * rhythm) - (swing * 0.01 * rhythm); // adding the swing to round duration   
  if (active == total){
   dur = 1.4 * dur;  // hold totals longer
   }  
  timeout = dur + millis();
  
  //read analog inputs for duration of round 
  while (timeout > millis()){             // start round
    for (int i = 1; i <=  inputs; i++) {  // loop through audio inputs
       //sample = analogRead(board+i);  
    		switch(i) {
    			case 1:
      			adc_sample = ads.readADC_Differential_0_1(); // 0-indexed channels on ADC Board
      			break;
      		case 2:
      		  adc_sample = ads.readADC_Differential_2_3();
      		  break;
      		default:
      		  adc_sample = 0;
      			break;
      	}
       	adc_sample = abs(adc_sample);
       	info(adc_sample);
       	if(adc_sample > level) {              
             input[i]=input[i]+1;         // add point if input is over the trigger level
        }    
       }
     count++;                             // count samples in round
  }

  if(debug==1){                           // log results to console
    Serial.print("Round Duration: ");
    Serial.print(dur);
    Serial.print("\t| Samples: ");
    Serial.print(count);
    for (int i = 1; i <=  inputs; i++) {  
      Serial.print("\t| input: ");
      Serial.print(i);
      Serial.print(": ");
      Serial.print(input[i]);
    }
    Serial.println();
  }
  //////////////////ROUND END////////////////////
   
  /////////ACT BASED ON RESULTS /////////////////
    active = total;      // default camera is total
    multi= 0;            // reset simultaneous counter
    win=0;               // reset winner
    reason = "Speaking"; // reset reason to cut
    
    // loop through results for round  
    // find the input with the highest score in round
    for (int i = 1; i <=  inputs; i++) {    
        if (input[i] > win){                 
            win=input[i];
            active = i + (camstart-1);
        }   
    }   
    
    // log multiple inputs that are active during round
    for (int i = 1; i <=  inputs; i++) {               
       if (input[i] > simultan*(float(win)/100)){  
          multi++;  
       }
    } 
    
    rounds ++;  // count round with equal result
    
    // if the same source has been active for 4-8 rounds, do cut back to previous source
    if(rounds > random(4,8)) {
        active = back;
        cutBack = 1;
        rounds = 0;
        reason = "Cut Back";
    }
    
    // count rounds of simultaneous active inputs, cut to total after 2
    if (multi > 1){
        simulL++;
        if (simulL > 2) {
            active = total;
            reason = "Simultaneous";
        }
    }
    
    // check for none 
    if( multi == 1 ) {
        simulL = 0;
    }
    
    if( multi == 0 ) {
        noone++;
        if ( noone == 3+(random(4)) ) {
            active = total;
            reason = "Silence";
        }
    }
     
    // if active has changed, change source on video mixer  
    if (active != last) {
        videomix(active);
        back=last;
        last = active;
        rounds=0;
    }
}

void videomix(int cam){
 // send commands to your video mixer
 Serial.print("$ACR,CUT,");
 Serial.println(cam);
 if(debug){
   Serial.print(" .   -    cut to camera ");
   Serial.print(cam);
   Serial.print(" - reason: ");
   Serial.println(reason);
 }
}

void echoStatus() {
   Serial.print("$ACR,INPUTS,");
   Serial.print(inputs);
   Serial.print(",TOTAL,");
   Serial.print(total);
   Serial.print(",TRIGGER_LEVEL,");
   Serial.print(level);
   Serial.print(",RHYTM,");
   Serial.print(rhythm);
   Serial.print(",SWING,");
   Serial.print(swing);
   Serial.print(",SIMUL,");
   Serial.print(simultan);
   Serial.print(",LEVEL,");
   Serial.print(level);
   Serial.print(",FIRST_VIDEO_INPUT,");
   Serial.print(camstart);
   Serial.println("");
}

void serialEvent() {
     while (Serial.available()) {
          char inChar = (char)Serial.read();
          inputString += inChar;
          if (inChar == '\n') {
               stringComplete = true;
          }
     }
}
