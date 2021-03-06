/*

    Control the 6 RGB LEDs visible on the face of the tile


    THEORY OF OPERATION
    ===================
    
	The pixels are multiplexed so that only one is lit at any given moment. 
	The lit pixel is selected by driving its anode high and then driving the common
	cathodes of the red, green, and blue LEDs inside each pixel with a PWM signal to control the brightness.
	
	The PWM signals are generated by hardware timers, so these cathodes can only be connected to
	pins that have timer functions on them.	
	
	An ISR driven by a timer interrupt steps though the sequence of anodes. 
	This is driven by the same timer that generates the PWM signals, and we pick our polarities sothat
	the LEDs light up at the end of each PWM cycle so that the ISR has time to step to the next LED
	before it actually lights up. 
	
	The PWM timing is slightly complicated by the fact that the compare values that generate the PWM signals are 
	loaded from a hardware buffer at the end of each PWM cycle, so we need to load the values of the NEXT
	pixel while the current pixel is still being driven. 
	
	The blue cathode is slightly different. It has a charge pump to drive the cathode voltage lower than 0V
	so it will still work even when the battery is lower than the blue forward voltage (~2.5V).
	A second timer drives the charge pump high to charge it up, then low to generate the negative cathode voltage.
	The means that the blue diode is out of phase with red and green ones. The blue hardware timer is
	lockstep with the one that generates the red and green PWM signals and the ISR interrupt. 

*/


// TODO: Really nail down the gamma mapping and maybe switch everything to 5 bit per channel
// TODO: Really nail down the blue booster 

#include "hardware.h"
#include "bitfun.h"

#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <string.h>             // memcpy()

#include "pixel.h"
#include "utils.h"

#include "callbacks.h"

#include "timer.h"      // We piggyback actual timer callback in pixel since we are using that clock for PWM

// Here are the raw compare register values for each pixel
// These are precomputed from brightness values because we read them often from inside an ISR
// Note that for red & green, 255 corresponds to OFF and 250 is about maximum prudent brightness
// since we are direct driving them. No danger here since the pins are limited to 20mA, but they do get so
// bright that is gives me a headache. 

typedef struct  {
    uint8_t rawValueR;
    uint8_t rawValueG;
    uint8_t rawValueB;
} rawpixel_t;

// We need these struct gymnastics because C fixed array typedefs do not work 
// as you (I?) think they would...
// https://stackoverflow.com/questions/4523497/typedef-fixed-length-array

typedef struct {
    rawpixel_t rawpixels[PIXEL_COUNT];
} rawpixelset_t;    

// Double buffer the raw pixels so we can switch quickly and atomically

#define RAW_PIXEL_SET_BUFFER_COUNT 2

static rawpixelset_t rawpixelsetbuffer[RAW_PIXEL_SET_BUFFER_COUNT];

static rawpixelset_t *displayedRawPixelSet=&rawpixelsetbuffer[0];        // Currently being displayed
static rawpixelset_t *bufferedRawPixelSet =&rawpixelsetbuffer[1];        // Benignly Updateable 

static void setupPixelPins(void) {

	// TODO: Compare power usage for driving LOW with making input. Maybe slight savings because we don't have to drain capacitance each time? Probably not noticable...
	// TODO: This could be slightly smaller code by loading DDRD with a full byte rather than bits
	
	// Setup all the anode driver lines to output. They will be low by default on bootup
	SBI( PIXEL0_DDR , PIXEL0_BIT );
	SBI( PIXEL1_DDR , PIXEL1_BIT );
	SBI( PIXEL2_DDR , PIXEL2_BIT );
	SBI( PIXEL3_DDR , PIXEL3_BIT );
	SBI( PIXEL4_DDR , PIXEL4_BIT );
	SBI( PIXEL5_DDR , PIXEL5_BIT );
	
	// Set the R,G,B cathode sinks to HIGH so no current flows (this will turn on pull-up until next step sets direction bit)..
    
	SBI( LED_R_PORT , LED_R_BIT );       // RED
	SBI( LED_G_PORT , LED_G_BIT );       // GREEN
    
    // Note that we do not set the BLUE led pin high here. We leave it low so the pull-up will not be activated when the pin is in input mode.
    // We will drive this pin with the timer only since it is connected to the charge pump.
    // Whenb not charging or lighting, we leave it in input mode wiht no pull-up so no current can drift though it
    // and dimmly light the next LED. 
    
	//SBI( LED_B_PORT , LED_B_BIT );       // BLUE
	
	// Set the cathode sinks to output (they are HIGH from step above)
	// TODO: These will eventually be driven by timers
	SBI( LED_R_DDR , LED_R_BIT );       // RED
	SBI( LED_G_DDR , LED_G_BIT );       // GREEN
	
    // Note that we do not set the BLUE led pin to output here. We leave it floating.
    // We will drive this pin with the timer only since it is connected to the charge pump
    
    //SBI( LED_B_DDR , LED_B_BIT );       // BLUE
    
    // Leave the blue LED sink floating for now. We will enable it as needed in the pixel ISR
    // depending on weather the BLUE LED is on or not. 
    
    // We will always leave it low even when in input mode so that the PULLUP is not enabled. 
	
	//SBI( BLUE_SINK_PORT , BLUE_SINK_BIT);   // Set the sink output high so blue LED will not come on
	//SBI( BLUE_SINK_DDR  , BLUE_SINK_BIT);
	
}


// This will put all timers into sync mode, where they will stop dead
// We can then run the enable() fucntions as we please to get them all set up
// and then release them all at the same exact time
// We do this to get timer0/timer1 and timer2 to be exactly out of phase
// with each other so they can run without stepping on each other
// This assumes that one of the timers will start with its coutner 1/2 way finished
//..which timer2 does.

static void holdTimers(void) {
    SBI(GTCCR,TSM);         // Activate sync mode - both timers halted
    SBI(GTCCR,PSRASY);      // Reset prescaller for timer2
    SBI(GTCCR,PSRSYNC);     // Reset prescaller for timer0 and timer1
}


static void releaseTimers(void) {
    CBI(GTCCR,TSM);            // Release all timers at the same moment
}


// Timers are hardwired to colors. No pin portable way to do this.
// RED   = OC0A
// GREEN = OC0B
// BLUE  = OC2B     
// 
// Blue is different
// =================
// Blue is not straight PWM since it is connected to a charge pump that charges on the high and activates LED on the low



// Enable the timer that drives the pixel PWM and radial refresh 
// Broken out since we call it both from setupTimers() and enablePixels()

static void pixelTimersOn(void) {
    
    // Timer0 to drive R & G PWM. We also use the overflow to jump to the next multiplexed pixel.
    
    // We are running in FAST PWM mode where we continuously count up to TOP and then overflow.
	// Since we are using both outputs, I think we are stuck with Mode 3 = Fast PWM that does not let use use a different TOP
	// Mode 3 - Fast PWM TOP=0xFF, Update OCRX at BOTTOM, TOV set at MAX
	
	// Looking at the diagram in the datasheet, it appears that the OCRs are set at the same time as the TOV INT (at MAX)
	
	// The outputs are HIGH at the beginning and LOW at the end. HIGH turns OFF the LED and LEDs should be low duty cycle,
    // so this gives us time to advance to the next pixel while LED is off to avoid visual glitching. 
    	    
    // First turn everything off so no glitch while we get ready
    
    // Writing OCR0A=MAX will result in a constantly high or low output (depending on the
    // polarity of the output set by the COM0A[1:0] bits.)
    // So setting OCR to MAX will turn off the LED because the output pin will be constantly HIGH
    
    // Timer0 (R,G)        
    OCR0A = 255;                            // Initial value for RED (off)
    OCR0B = 255;                            // Initial value for GREEN (off)
    TCNT0 =   0;                            // This will match BOTTOM so SET the output pins (set is LED off)
    
    SBI( TCCR0B , FOC0A );                  // Force output compare 0A - should set the output
    SBI( TCCR0B , FOC0B );                  // Force output compare 0B - should set the output
    

	// When we get here, timer 0 is not running, timer pins are driving red and green LEDs and they are off.  

    // We are using Timer0 mode 3 here for FastPWM which defines the TOP (the value when the overflow interrupt happens) as 255    
    
    TCCR0A =
        _BV( WGM00 ) | _BV( WGM01 ) |       // Set mode=3 (0b11)
        _BV( COM0A1) |                      // Clear OC0A on Compare Match, set OC0A at BOTTOM, (non-inverting mode) (clearing turns LED on)
        _BV( COM0B1)                        // Clear OC0B on Compare Match, set OC0B at BOTTOM, (non-inverting mode)
    ;

    #if TIMER_TOP != 256
        #Timer TOP is hardcoded as 256 in this mode, must match value uesed in calculations
    #endif
    
	// IMPORTANT:If you change the mode, you must update PIXEL_STEPS_PER_OVR above!!!!
    
    // Next setup Timer2 for blue PWM. 
    // When the output pin goes low, it pulls down on the charge pump cap
    // which pulls down the blue RGB cathode to negative voltage, lighting the blue led
    
    TCCR2A =
    _BV( COM2B1) |                        // 1 0 = Clear OC0B on Compare Match (blue on), set OC0B at BOTTOM (blue off), (non-inverting mode)
    _BV( WGM01) | _BV( WGM00)             // Mode 3 - Fast PWM TOP=0xFF
    ;

    #if TIMER_TOP != 256 
        #Timer TOP is hardcoded as 256 in this mode, must match value used in calculations
    #endif

    
    // Timer2 (B)                           // Charge pump is attached to OC2B
    
    OCR2A = 128;                            // Fire a match interrupt half way though the cycle. 
                                            // This lets us sample the IR LEDs at double the overflow rate.
                                            // It is also nice because in the match ISR we *only* do IR stuff.
    
    OCR2B = 255;                            // Initial value for BLUE (off)
    TCNT2=    0;                            // This is BOTTOM, so when we force a compare the output should be SET (set is LED off, charge pump charging) 
    
    SBI( TCCR2B , FOC2B );                  // This should force compare between OCR2B and TCNT2, which should SET the output in our mode (LED off)
        
    // Ok, everything is ready so turn on the timers!
    
        
    holdTimers();           // Hold the timers so when we start them they will be exactly synced  

    // If you change this prescaller, you must update the the TIMER_PRESCALLER
    
    TCCR0B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV( CS01 );                        // clkIO/8 (From prescaler)- ~ This line also turns on the Timer0

    #if TIMER_PRESCALER != 8
        # Actual hardware prescaller for Timer1 must match value used in calculations 
    #endif

    // IMPORTANT!
    // If you change this prescaler, you must update the the TIMER_PRESCALLER

    // The two timers might be slightly unsynchronized by a cycle, but that should not matter since all the action happens at the end of the cycle anyway.
    
    TCCR2B =                                // Turn on clk as soon as possible after setting COM bits to get the outputs into the right state
        _BV( CS21 );                        // clkI/O/8 (From prescaler)- This line also turns on the Timer0
                    

    #if TIMER_PRESCALER != 8
        # Actual hardware prescaller for Timer2 must match value used in calculations
    #endif

                                            // NOTE: There is a datasheet error that calls this bit CA21 - it is actually defined as CS21
    releaseTimers();                        // Timer0 and timer1 now in lockstep
        
}


static void setupTimers(void) {
    
	TIMSK0 = _BV( TOIE0 ) ;                  // The corresponding interrupt is executed if an overflow in Timer/Counter0 occurs
    
    TIMSK2 = _BV( OCIE2A );                  // Generate an interrupt when OCR2A matches, which happens excatly out of phase with the overflow. 

}

void pixel_init(void) {
    
    // First initialize the buffers
    for( uint8_t i = 0 ; i < RAW_PIXEL_SET_BUFFER_COUNT ; i++ ) {
        rawpixelset_t *rawpixelset = &rawpixelsetbuffer[ i ];
        for( uint8_t j =0; j < PIXEL_COUNT ; j++ ) {
            rawpixelset->rawpixels[j].rawValueR = 255;
            rawpixelset->rawpixels[j].rawValueG = 255;
            rawpixelset->rawpixels[j].rawValueB = 255;
        }                
    }    
            
	setupPixelPins();
	setupTimers();
}

// Note that LINE is 0-5 whereas the pixels are labeled p1-p6 on the board. 

static void activateAnode( uint8_t line ) {         
    
    // TODO: These could probably be compressed with some bit hacking
    
    switch (line) {
        
        case 0:
            SBI( PIXEL0_PORT , PIXEL0_BIT );
            break;
        
        case 1:
            SBI( PIXEL1_PORT , PIXEL1_BIT );
            break;
        
        case 2:
            SBI( PIXEL2_PORT , PIXEL2_BIT );
            break;
            
        case 3:
            SBI( PIXEL3_PORT , PIXEL3_BIT );
            break;
        
        case 4:
            SBI( PIXEL4_PORT , PIXEL4_BIT );
            break;           

        case 5:
            SBI( PIXEL5_PORT , PIXEL5_BIT );
            break;
        
    }
    
}

// Deactivate all anodes. Faster to blindly do all of them than to figure out which is
// is currently on and just do that one. 

static void deactivateAnodes(void) {           
    	
    // Each of these compiles to a single instruction        
    CBI( PIXEL0_PORT , PIXEL0_BIT );
    CBI( PIXEL1_PORT , PIXEL1_BIT );
    CBI( PIXEL2_PORT , PIXEL2_BIT );
    CBI( PIXEL3_PORT , PIXEL3_BIT );
    CBI( PIXEL4_PORT , PIXEL4_BIT );
    CBI( PIXEL5_PORT , PIXEL5_BIT );
            
}

/*

volatile uint8_t vccAboveBlueFlag=0;        // Is the battery voltage higher than the blue LED forward voltage?
                                            // If so, then we need a different strategy to dim since the LED
											// will always be on Even when the pump is not pushing. 
											// Instead we will do straight PWM on the SINK. 
											// For now, there are only two modes to keep it simple.
											// TODO: Take into account the brightness level and the Vcc and pick which is the most efficient dimming
											// strategy cycle-by-cycle. 

#define BLUE_LED_THRESHOLD_V 2.6

void updateVccFlag(void) {                  // Set the flag based on ADC check of the battery voltage. Don't have to do more than once per minute.
	vccAboveBlueFlag = (adc_lastVccX10() > BLUE_LED_THRESHOLD_V);	
	vccAboveBlueFlag = 1;	
}

*/

                                     
static uint8_t currentPixelIndex;      // Which pixel are we on now?

// Each pixel has 5 phases -
// 0=Charging blue pump. All anodes are low. 
// 1=Resting after pump charge. Get ready to show blue.
// 2=Displaying blue
// 3=Displaying green
// 4=Displaying red


// We need a rest because the pump sink is not connected to an OCR pin
// so we need a 3 phase commit to turn off led, turn on pump, turn off pump, turn on led

// TODO: Use 2 transistors to tie the pump sink and source to the same OCR pin. 

static uint8_t phase=0;

// To swap the display buffer, you set this and then wait until it is unset by the 
// background display ISR
// This makes display updates atomic, and swaps always happen between frames to avoid tearing and aliasing

static volatile uint8_t pendingRawPixelBufferSwap =0;

// Need to compute timekeeping based off the pixel interrupt

// This is hard coded into the Timer setup code in pixels.cpp

// Number of timer cycles per overflow interrupt
// Hard coded into the timer setup code


// Some interesting time calculations:
// Clock 4mhz
// Prescaller is 8 
// ... so Timer clock is 4Mhz/8 = 500KHz
// ... so one timer step is 2us
// 256 steps per phase
// ... so a phase is 2us * 256 = 512us
// 4 phase per pixel
// ... so one pixel takes 512us * 5 = ~2.5ms
// 6 pixels per frame
// ... so one frame takes 6 * 2.5ms = ~15ms
// ... so refresh rate is 1/15ms = ~66Hz

// Called every time pixel timer0 overflows
// Since OCR PWM values only get loaded from buffers at overflow by the AVR, 
// this gives us plenty of time to get the new values into the buffers for next
// pass, so none of this is timing critical as long as we finish in time for next
// pass 
               
static void pixel_isr(void) {   
    
    // THIS IS COMPLICATED
    // Because of the buffering of the OCR registers, we are always setting values that will be loaded
    // the next time the timer overflows. 
                    
    rawpixel_t *currentPixel = &(displayedRawPixelSet->rawpixels[currentPixelIndex]);      // TODO: cache this and eliminate currentPixel since buffer only changes at end of frame
        
    switch (phase) {
        
        
        case 0:   // In this phase, we step to the next pixel and start charging the pump. All PWMs are currently off. 
        
            deactivateAnodes();  
            
            // Connect the timer to the output pin. 
            // It might have been disconnected on the the pixel if that pixel did not have any blue in it. 
            
            if ( currentPixel->rawValueB != 255 ) {          // Is blue on for this pixel?
                
                // Connect the timer to the PWM pin
                // Otherwise it floats to prevent current from leaking though the cap
                // and dimly lighting the blue LED
            
            
                TCCR2A =
                  _BV( COM2B1) |                        // 1 0 = Clear OC0B on Compare Match (blue on), set OC0B at BOTTOM (blue off), (non-inverting mode)
                  _BV( WGM01) | _BV( WGM00);             // Mode 3 - Fast PWM TOP=0xFF                  
              
                // Right now the timer is at 255, so putting out a steady high which is currently
                // just enabling the pull-up since the pin is still input until next step...
                                                      
                // It is safe to turn on the blue sink because all anodes are off (low)        

                SBI( LED_B_DDR , LED_B_BIT );                // Drive BLUE LED output pin - which is high when the LED is not being PWMed.

            
                SBI( BLUE_SINK_DDR , BLUE_SINK_BIT );        // Enable output on sink pin. Since this pin port is always 0, this will drive it low.
                                                             // Allows capacitor charge though the diode
                                    
                // Ok, now the pump capacitor is charging.  No LEDs are on.
                                    
                // TODO: Handle the case where battery is high enough to drive blue directly and skip the pump
                
            }                
            
            phase++;          
            break;     
             
        case 1:
        
            // OK, if blue is on then CAP has been charging. 
            // Nothing is on yet, no anodes are activated. 
        
            // Here we rest after charging the pump.
            // This is necessary since there is no way to ensure timing between
            // turning off the sink and turning on the PWM
            
            // If the blue led is on for this pixel, then in the previous phase we 
            // enabled the sink and connected the PWM pin. 
            
            // Now we blinkly disable the sink since we don't want it anymore no matter what
            
            CBI( BLUE_SINK_DDR , BLUE_SINK_BIT);    // Turn off blue sink (make it input) if we were charging 
                                                    // Might already be off, but faster to blindly turn off again rather than test

            // we leave the PWM pin alone - we want it connected if there is blue here
            // so it can pull the cap down.
                        
            // Now the sink is off, we are safe to activate the anode.
            // Remember that the PWM pin is still high and connected if there is blue in this pixel.
            // A little current will flow now though the capactor, but that ok. 
            // when the PWM goes low, then the boost will kick in and make the BLUE really light
                    
            activateAnode( currentPixelIndex );
        
            // Ok, now we are ready for all the PWMing to happen on this pixel 

            // Load up the blue PWM to go low and show blue (if the pump and PWM were activeated in phase #0)....        
        
            OCR2B=currentPixel->rawValueB;             // Load OCR to turn on blue at next overflow
        
            phase++;
        
            break;
                                    
            
        case 2: 
        
            // Right now, the blue led is on. Lets get ready for the red one next.             
            // TODO: Leave blue on until last phase for more brightness?     
                           
                
            OCR2B = 255;                        // Load OCR to turn off blue at next overflow
            OCR0A = currentPixel->rawValueR;    // Load OCR to turn on red at next overflow

            phase++;           
            break;
            
        case 3: // Right now, the red LED is on. Get ready for green
        
            // We are now done with BLUE, so we need to disconnect it to avoid 
            // dimly lighting the next LED. 
            
            // TODO: Pushg this forward a phase for more brightness?
        
            // Float the BLUE LED drive pin.
            // This cuts off a path for current though the pump cap
            // in cases where the blue LED is completely off. Other wise it would
            // glow dimly as it discharges though the cap.
            
            CBI( LED_B_DDR , LED_B_BIT );
            
            // Note that this actually still leaves the pull-up connected to the pin
            // since the timer is setting the output to 1, so a tiny tiny little bit does
            // leak though.
            
            // To turn off the pull-up, we must completely disconnect the timer to stop
            // it from pushing a 1.
            // Note that we always leave the bit in PORT at 0, so this will completely
            // float the pin.
            
            TCCR2A =
              _BV( WGM01) | _BV( WGM00);             // Mode 3 - Fast PWM TOP=0xFF
            
            // BLue LED is no completely disconnected form everything so should be off.
        
                                    
            OCR0A = 255;                        // Load OCR to turn off red at next overflow
            OCR0B = currentPixel->rawValueG;    // Load OCR to turn on green at next overflow
            
            phase++;
            break;
            
        case 4: // Right now the green LED is on. 
        
            #if TIMER_PHASE_COUNT!= 5
                #error If this switch does not have 5 cases, then need to update the TIMER_PHASE_COUNT in timer.h to make calculations colrrect
            #endif
                       
            OCR0B = 255;                        // Load OCR to turn off green at next overflow
            
            phase=0;                            // Step to next pixel and start over
            
            currentPixelIndex++;
            
            if (currentPixelIndex==PIXEL_COUNT) {
                currentPixelIndex=0;
                
                if (pendingRawPixelBufferSwap) {
                    
                    rawpixelset_t *temp;
                    
                    // Quickly swap the display and buffer sets                    
                    temp = displayedRawPixelSet;
                    displayedRawPixelSet = bufferedRawPixelSet;
                    bufferedRawPixelSet = temp;
                    
                    pendingRawPixelBufferSwap=0;
                    
                }                    
                                    
            }
                                            
            break;
                        
    }   
        
} 

// Stop the timer that drives pixel PWM and refresh
// Used before powering down to make sure all pixels are off

static void pixelTimerOff(void) {
    
    TCCR0B = 0;                     // Timer0 stopped, so no ISR can change anything out from under us
    // Right now one LED has its anode activated so we need to turn that off
    // before driving all cathodes low
    
    
    deactivateAnodes( );
                                                                   
    TCCR2B = 0;                     // Timer/counter2 stopped.
        
    
    // PWM outputs will be stuck where ever they were, at this point.
    // Lets set them all low so no place for current to leak.
    // If diode was reverse biases, we will have a tiny leakage current.
    
    TCCR0A = 0;         // Disable both timer0 outputs
    TCCR2A = 0;         // Disable timer2 output
    
    
    // Turn off the BLUE SINK, just in case the BLUE drive pin is high 
    // this will prevent even the tiny tiny leakage though the boost cap. 
    // The ISR will turn this back on when needed.     
    
	CBI( BLUE_SINK_DDR  , BLUE_SINK_BIT);          
    
    // Now all three timer pins should be inputs
    
}
                         
            
// Called when Timer0 overflows, which happens at the end of the PWM cycle for each pixel. We advance to the next pixel.

// This fires every 500us (2Khz)
// You must finish work in this ISR in 1ms or else might miss an overflow.


ISR(TIMER0_OVF_vect)
{       
    timer_256us_callback_cli();       // Do any timing critical double-time stuff with interrupts off 
                                   // Currently used to sample & charge (but not decode) the IR LEDs
                                
    // We want to turn interrupts back on as quickly as possible here
    // to limit the amount of jitter we add to the space gaps between outgoing
    // IR pulses
                                
    sei();                      // Exhale. We want interrupt on as quickly as possible so we 
                                // don't mess up IR transmit timing. 
                                                                
    // Deal with the PWM stuff. There is a deadline here since we must get the new values
    // loaded into the double-buffered registers before the next overflow.
                                
    pixel_isr();
    
    timer_256us_callback_sei();    // Do the doubletime callback
    timer_512us_callback_sei();       // Do everything else non-timing sensitive. 
    return;	
}


ISR(TIMER2_COMPA_vect)          // Called when OCR2a matches, which we have set up to happen 
                                // exactly out of phase with the TIMER0_OVR to effectively double
                                // Double the rate we call some callbacks (currently used for IR) 
{
    timer_256us_callback_cli();    // Do any timing critical stuff with interrupts off
    
    // Currently used to sample & charge (but not decode) the IR LEDs
    
    // We want to turn interrupts back on as quickly as possible here
    // to limit the amount of jitter we add to the space gaps between outgoing
    // IR pulses
    
    sei();                      // Exhale. We want interrupt on as quickly as possible so we
    // don't mess up IR transmit timing.
    
    // Deal with the PWM stuff. There is a deadline here since we must get the new values
    // loaded into the double-buffered registers before the next overflow.
        
    timer_256us_callback_sei();       // Do everything else non-timing sensitive.
    return;
}



// Turn of all pixels and the timer that drives them.
// You'd want to do this before going to sleep.

void pixel_disable(void) {
    
    // First we must disable the timer or else the ISR could wake up 
    // and turn on the next pixel while we are trying to turn them off. 
    
    pixelTimerOff();
    

    
    // Ok, now all the anodes should be low so all LEDs off
    // and no timer running to turn any anodes back on
    
}

// Re-enable pixels after a call to disablePixels.
// Pixels will return to the color they had before being disabled.

void pixel_enable(void) {
            
    pixelTimersOn();
    
    // Technically the correct thing to do here would be to turn the previous pixel back on,
    // but it will get hit on the next refresh which happens muchg faster than visible.
    
    // Next time timer expires, ISR will benignly deactivate the already inactive last pixel, 
    // then turn on the next pixel and everything will pick up where it left off. 
    
}
       

// Update the pixel buffer with raw PWM register values.
// Larger pwm values map to shorter PWM cycles (255=off) so for red and green
// there is an inverse but very non linear relation between raw value and brightness.
// For blue is is more complicated because of the charge pump. The peak brightness is somewhere
// in the middle.

// Values set here are buffered into next call to pixel_displayBufferedPixels()

// This is mostly useful for utilities to find the pwm -> brightness mapping to be used
// in the gamma lookup table below.

void pixel_bufferedSetPixelRaw( uint8_t pixel, uint8_t r_pwm , uint8_t g_pwm , uint8_t b_pwm ) {

    rawpixel_t *rawpixel = &(bufferedRawPixelSet->rawpixels[pixel]);
        
    rawpixel->rawValueR= r_pwm;
    rawpixel->rawValueG= g_pwm;
    rawpixel->rawValueB= b_pwm;
    
}


// Gamma table courtesy of adafruit...
// https://learn.adafruit.com/led-tricks-gamma-correction/the-quick-fix
// Compressed down to 32 entries, normalized for our raw values that start at 255 off. 

// TODO: Possible that green and red are similar enough that we can combine them into one table and save some flash space

static const uint8_t PROGMEM gamma8R[32] = {
    255,254,253,251,250,248,245,242,238,234,230,224,218,211,204,195,186,176,165,153,140,126,111,95,78,59,40,19,13,9,3,1
};

static const uint8_t PROGMEM gamma8G[32] = {
    255,254,253,251,250,248,245,242,238,234,230,224,218,211,204,195,186,176,165,153,140,126,111,95,78,59,40,19,13,9,3,1
};

static const uint8_t PROGMEM gamma8B[32] = {
    255,254,253,251,250,248,245,242,238,234,230,224,218,211,204,195,186,176,165,153,140,126,111,95,78,59,40,19,13,9,3,1
};


// Update the pixel buffer.

void pixel_bufferedSetPixel( uint8_t pixel, pixelColor_t newColor) {

    // TODO: OMG, this could be so much more efficient by reducing the size of the gamma table 
    // to 32 entries per color and having direct mapping to raw values. 
    // We will do that when we normalize the colors. 

    rawpixel_t *rawpixel = &(bufferedRawPixelSet->rawpixels[pixel]);
    
    rawpixel->rawValueR= pgm_read_byte(&gamma8R[newColor.r]);
    rawpixel->rawValueG= pgm_read_byte(&gamma8G[newColor.g]);
    rawpixel->rawValueB= pgm_read_byte(&gamma8B[newColor.b]);
    
}    

// Display the buffered pixels by swapping the buffer. Blocks until next frame starts.

void pixel_displayBufferedPixels(void) {
    
    pendingRawPixelBufferSwap = 1;      // Signal to background that we want to swap buffers
    
    while (pendingRawPixelBufferSwap);  // wait for that to actually happen
    
    // Insure continuity by making sure that after the swap the (now) buffer starts 
    // off with the same values that the old buffer ended with 
    memcpy( bufferedRawPixelSet , displayedRawPixelSet , sizeof( rawpixelset_t  ) );  
        
}    
