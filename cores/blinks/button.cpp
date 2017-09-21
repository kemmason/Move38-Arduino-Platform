/*

    Control the 6 RGB LEDs visible on the face of the tile


    THEORY OF OPERATION
    ===================
    
	Button is simple. Push it, and it calls an ISR - even when in deep sleep. 

*/


#include "hardware.h"

#include <avr/interrupt.h>
#include <util/delay.h>					// _delay_ms() for debounce

#include "button.h"
#include "utils.h"

// TODO: Do proper debounce when we have a timer available.



// Callback that is called when the button state changes.
// Note that you could get multiple consecutive calls with the
// Same state if the button quickly toggles back and forth quickly enough that
// we miss one phase. This is particularly true if there is a keybounce exactly when
// and ISR is running.

// Use BUTTON_DOWN() to check buttonstate when called. 

// Confirmed that all the pre/postamble pushes and pops compile away if this is left blank

// Weak reference so it (almost) compiles away if not used. 
// (looks like GCC is not yet smart enough to see an empty C++ virtual invoke. Maybe some day!)

void __attribute__((weak)) button_onChange(void) {
}


struct ISR_CALLBACK_BUTTON : ISR_CALLBACK_BASE<ISR_CALLBACK_BUTTON> {
    
    static const uint8_t running_bit = ISR_GATE_BUTTON_RUNNING_BIT;
    static const uint8_t pending_bit = ISR_GATE_BUTTON_PENDING_BIT;
    
    static inline void callback(void) {
        
        button_onChange();
        
    }
    
};


static volatile uint8_t button_flag;

ISR(BUTTON_ISR)
{ 
    uint8_t s = BUTTON_DOWN();
        
	if (s) {
		button_flag = 1;
	}
    
    ISR_CALLBACK_BUTTON::invokeCallback();
                   
}


void button_init(void) {
		
	// Pin change interrupt setup
	SBI( PCICR , BUTTON_PCI );          // Enable the pin group
	
}

// Enable pullup and interrupts on button

void button_enable(void) {

	// GPIO setup
	SBI( BUTTON_PORT , BUTTON_BIT);     // Leave in input mode, enable pull-up

	// Pin change interrupt setup
	SBI( BUTTON_MASK , BUTTON_PCINT);   // Enable pin in Pin Change Mask Register
    
}    


// Disable pull-up and interrupts
// You'd want to do this to save power in the case where the
// button is stuck down and therefore shorting out the pull-up

void button_disable(void) {

    // disable pin change interrupt 
    CBI( BUTTON_MASK , BUTTON_PCINT);   // Enable pin in Pin Change Mask Register

    CBI( BUTTON_PORT , BUTTON_BIT);     // Leave in input mode, disable pull-up
        
}


// Returns 1 if button pressed since the last time this was called

uint8_t button_pressed(void) {
	
	// TODO: Use a proper timer to debounce here? Does it really matter for this?

	_delay_ms( BUTTON_DEBOUNCE_MS );
	
	if (button_flag) {
		
		button_flag=0;
		return 1;
	} 
	
	return 0;
	
}

// Returns 1 if button is currently down

uint8_t button_down(void) {
	
	return BUTTON_DOWN();
	
}
