/*
 * Cyclone V & Arria 10 HPS Interrupt Controller
 * ---------------------------------------------
 *
 * Driver for enabling and using the General Interrupt
 * Controller (GIC). The driver includes code to create
 * a vector table, and register interrupts.
 *
 * The code makes use of function pointers to register
 * interrupt handlers for specific interrupt IDs.
 *
 * ISR Handlers
 * ------------
 *
 * This driver takes care of handling the IRQ interrupt
 * generated by peripherals through the GIC. It provides
 * a handler which checks which interrupt source triggered
 * the IRQ, and then calls the handler which has been
 * assigned for that interrupt ID.
 *
 * For the other interrupts, FIQ, Data Abort, Prefetch Abort
 * and Undefined Instruction Interrupts, there is a default
 * handler which simply enters a while(1) loop to hang the
 * processor. Alternatively it can be configured to restart
 * the program by defining the following macro globally:
 *
 *     -D DEFAULT_ISR_JUMP_TO_ENTRY
 *
 * It is also possible to provide your own handlers for these
 * sources. These handlers are defined as weak aliases of the
 * default ISR handler which can be overridden by simply adding
 * your own implementation of one of the following five functions:
 *
 *    // Undefined Instruction
 *    __undef void __undef_isr (void){   }
 *    // Pre-fetch Abort
 *    __abort void __pftcAb_isr(void){   }
 *    // Data Abort
 *    __abort void __dataAb_isr(void){   }
 *    // Fast IRQ
 *    __fiq void __fiq_isr   (void){   }
 *
 * For software IRQs (SVC/SWI), the standard handler is always
 * used as it provides additional decoding and context handling.
 * To add your own handling, provide the following function
 * implementation.
 *
 *    // Software IRQ
 *    __swi void __svc_handler(unsigned int id, unsigned int param[4]){   }
 *
 * The driver supports both Cyclone V devices (default) or
 * Arria 10 devices (-D __ARRIA_10__).
 *
 * Company: University of Leeds
 * Author: T Carpenter
 *
 * Change Log:
 *
 * Date       | Changes
 * -----------+----------------------------------
 * 31/01/2024 | Correct ISR attributes
 * 22/01/2024 | Split Vector table to Util/startup_arm.c
 * 14/01/2024 | Make use of Util/lowlevel.h
 *            | Ensure data alignment checks start disabled in case
 *            | preloader enables them. If this flag is set, then
 *            | data abort would occur if -mno-unaligned-access not used.
 * 27/12/2023 | Convert to support ARM Compiler 6 (armclang).
 *            | Add support for Arria 10 devices.
 *            | Properly configure all interrupt stacks, which can
 *            | be positioned by setting HPS_IRQ_STACK_LIMIT or HPS_IRQ_STACK_SCATTER
 * 10/09/2018 | Embed creation of Vector Table and setup of VBAR
 *            | into driver to avoid extra assembly files.
 *            | Add ability to use static variables in ISR handlers
 * 12/03/2018 | Creation of driver
 *
 */

#include "HPS_IRQ.h"

#include "Util/lowlevel.h"

#include <stdio.h>

//Default handler for unhandled interrupt callback.
__irq void HPS_IRQ_unhandledIRQ(HPSIRQSource interruptID, void* param, bool* handled) {
    while(1); //Crash - use the watchdog timer to reset us.
}


/*
 * Arm GIC Register Map
 *
 * This is a partial map of registers we require for handling IRQs
 */

#ifdef __ARRIA10__
// For Arria 10 HPS

#define MPCORE_GIC_CPUIF     0xFFFFC100
#define MPCORE_GIC_DIST      0xFFFFD000

#else
// For Cyclone V HPS

#define MPCORE_GIC_CPUIF     0xFFFEC100
#define MPCORE_GIC_DIST      0xFFFED000

#endif

// Interrupt controller (GIC) CPU interface(s)
#define ICCICR               (0x00/sizeof(unsigned int))  // + to CPU interface control
#define ICCPMR               (0x04/sizeof(unsigned int))  // + to interrupt priority mask
#define ICCIAR               (0x0C/sizeof(unsigned int))  // + to interrupt acknowledge
#define ICCEOIR              (0x10/sizeof(unsigned int))  // + to end of interrupt reg

// Interrupt (INT) controller (GIC) distributor interface(s)
#define ICDDCR               (0x000/sizeof(unsigned int)) // + to distributor control reg
#define ICDISER              (0x100/sizeof(unsigned int)) // + to INT set-enable regs
#define ICDICER              (0x180/sizeof(unsigned int)) // + to INT clear-enable regs
#define ICDIPTR              (0x800/sizeof(unsigned int)) // + to INT processor targets regs
#define ICDICFR              (0xC00/sizeof(unsigned int)) // + to INT configuration regs


/*
 * Global variables for the IRQ driver
 *
 * This does not use the driver_ctx scheme as there can
 * only be one interrupt handler instance.
 */

static bool __isInitialised = false;

typedef struct {
    HPSIRQSource interruptID; //The ID of the interrupt source this handler is for
    IsrHandlerFunc_t handler; //Function pointer to be called to handle this ID
    void* param;              //Parameters to pass to interrupt handler
    bool enabled;
} IsrHandler_t;

static unsigned int __isr_handler_count;
static IsrHandler_t* __isr_handlers;
static IsrHandlerFunc_t __isr_unhandledIRQCallback;

static volatile unsigned int* __gic_cpuif_ptr = (unsigned int *)MPCORE_GIC_CPUIF;
static volatile unsigned int* __gic_dist_ptr  = (unsigned int *)MPCORE_GIC_DIST;

// User software interrupt handler. Can be overridden
__swi void __svc_handler (unsigned int id, unsigned int* val) __attribute__ ((weak));

/*
 * Next we need our interrupt service routine for IRQs
 *
 * This will check the interrupt id against all registered handlers
 * and cause an unhandledIRQCallback call if it is an unhandled interrupt
 */

__irq void __irq_isr (void) {
    // If not initialised, jump to default ISR handler.
    if (!__isInitialised) {
        __asm__("B __default_isr");
    }
    // Otherwise initialised, handle IRQs
    bool isr_handled = false;
    // Read the ICCIAR value to get interrupt ID
    HPSIRQSource int_ID = (HPSIRQSource)__gic_cpuif_ptr[ICCIAR];
    // Check to see if we have a registered handler
    unsigned int handler;
    for (handler = 0; handler < __isr_handler_count; handler++) {
        if (int_ID == __isr_handlers[handler].interruptID) {
            //If we have found a handler for this ID
            __isr_handlers[handler].handler(int_ID, __isr_handlers[handler].param, &isr_handled); //Call it and check status
            break;
        }
    }
    //Check if we have an unhandled interrupt
    if (!isr_handled) {
        __isr_unhandledIRQCallback(int_ID, NULL, NULL); //Call the unhandled IRQ callback.
    }

    //Otherwise write to the End of Interrupt Register (ICCEOIR) to mark as handled
    __gic_cpuif_ptr[ICCEOIR] = (unsigned int)int_ID;
    //And done.
    return;
}


/*
 * Software Interrupt Handler
 *
 * The SVC vector is used by the debugger to run semi-hosting
 * commands which allow IO commands to send data to the debugger
 * e.g. using printf.
 *
 * When the debugger is not connected, we still need to handle
 * this SVC call otherwise the processor will hang. As we have no
 * other SVC calls by default, we can handle by simply returning.
 */

__irq void __svc_isr (void) {
    // Store the four registers r0-r3 to the stack. These contain any parameters
    // to be passed to the SVC handler. Also store r12 as we need something we
    // can clobber, as well as the link register which will be popped back to
    // the page counter on return.
    __asm volatile (
        "STMFD   sp!, {r0-r3, r12, lr}  ;"
        // Grab SPSR. We will restore this at the end, but also need it to see if
        // the caller was in thumb mode.
        "MRS     r12, spsr              ;"
        // Extract the SVC ID. This is embedded in the SVC instruction itself which
        // is located one instruction before the value of the current banked link register.
        "TST     r12, %[TMask]          ;"
        // If caller was in thumb, then instructions is 2-byte, with lower byte being ID
        "LDRHNE   r0, [lr,#-2]          ;"
        "BICNE    r0, r0, #0xFF00       ;"
        // Otherwise caller was in arm mode, then instructions are 4-byte, with lower three bytes being ID.
        "LDREQ    r0, [lr,#-4]          ;"
        "BICEQ    r0, r0, #0xFF000000   ;"
        // Grab the stack pointer as this is the address in RAM where our four parameters have been saved
        "MOV      r1, sp                ;"
        // Call user handler (r0 is first parameter [ID], r1 is second parameter [SP])
        "BL       __svc_handler         ;"
        // Restore the processor state from before the SVC was triggered
        "MSR     SPSR_cxsf, r12         ;"
        // Remove pushed registers from stack, and return, restoring SPSR to CPSR
        "LDMFD   sp!, {r0-r3, r12, pc}^ ;"
        :: [TMask] "i" (1 << __PROC_CPSR_BIT_T)
    );
}

__swi void __svc_handler (unsigned int id, unsigned int* val) {

}


/*
 * Internal Helper Functions
 */

//Register a new handler.
// - Must have interrupts masked before calling this
static void _HPS_IRQ_doRegister(unsigned int handler, HPSIRQSource interruptID, IsrHandlerFunc_t handlerFunction, void* handlerParam) {
    volatile unsigned char* diptr;
    //Add our new handler
    __isr_handlers[handler].handler = handlerFunction;
    __isr_handlers[handler].param = handlerParam;
    __isr_handlers[handler].interruptID = interruptID;
    __isr_handlers[handler].enabled = true;
    //Then we need to enable the interrupt in the distributor
    __gic_dist_ptr[ICDISER + (interruptID / 32)] = 1 << (interruptID % 32);
    //And set the affinity to CPU0
    diptr = (unsigned char*)&(__gic_dist_ptr[ICDIPTR + interruptID / 4]);
    diptr[interruptID % 4] = 0x1;
    //Done
    return;
}

//Grow the handler table.
// - Must have interrupts masked before calling this
static HpsErr_t _HPS_IRQ_growTable(unsigned int growByN) {
    IsrHandler_t* new_handlers;
    //If length is correct, do nothing
    if (!growByN) return ERR_SUCCESS;
    //If we failed to find a match, we need to reallocated our handler array to gain more space
    new_handlers = (IsrHandler_t*)realloc(__isr_handlers, (__isr_handler_count + growByN)*sizeof(IsrHandler_t) );
    //We should be sure to check that reallocation was a success
    if (!new_handlers) {
        //If realloc returned null, then reallocation failed, cannot register new handler.
        return ERR_ALLOCFAIL;
    }
    //If we were successful in making space, update our global pointer
    __isr_handlers = new_handlers;
    //Update the count of available handlers
    __isr_handler_count = __isr_handler_count + growByN;
    return ERR_SUCCESS;
}

//Find an existing IRQ handler
static unsigned int _HPS_IRQ_findHandler(HPSIRQSource interruptID) {
    unsigned int handler;
    for (handler = 0; handler < __isr_handler_count; handler++) {
        if (__isr_handlers[handler].interruptID == interruptID) {
            //Found an existing one. We'll just overwrite it, instead of making new one.
            break;
        }
    }
    return handler;
}


//Do the unregistering of an interrupt.
// - Function will mask interrupts automatically.
static void _HPS_IRQ_doUnregister(unsigned int handler, HPSIRQSource interruptID) {
    //Before changing anything we need to mask interrupts temporarily while we change the handlers
    bool was_masked = __disable_irq();
    //Clear the handler pointer, and mark as disabled
    __isr_handlers[handler].handler = 0x0;
    __isr_handlers[handler].enabled = false;
    //Then we need to disable the interrupt in the distributor
    __gic_dist_ptr[ICDICER + (interruptID / 32)] = 1 << (interruptID & 31);
    //Finally we unmask interrupts to resume processing.
    if (!was_masked) {
        __enable_irq();
    }
}

/*
 * User Facing APIs
 */

//Initialise HPS IRQ Driver
HpsErr_t HPS_IRQ_initialise( IsrHandlerFunc_t userUnhandledIRQCallback ) {
    /* Configure Global Interrupt Controller (GIC) */
    //Disable IRQ interrupts before configuring
    __disable_irq();
    
    // Set Interrupt Priority Mask Register (ICCPMR)
    // Enable interrupts of all priorities
    __gic_cpuif_ptr[ICCPMR] = 0xFFFF;

    // Set CPU Interface Control Register (ICCICR)
    // Enable signalling of interrupts
    __gic_cpuif_ptr[ICCICR] = 0x1;

    // Configure the Distributor Control Register (ICDDCR)
    // Send pending interrupts to CPUs
    __gic_dist_ptr[ICDDCR] = 0x1;

    //Initially no handlers
    __isr_handler_count = 0;
    __isr_handlers = NULL;
    
    //Set up the unhandled IRQ callback
    if (userUnhandledIRQCallback != NULL) {
        //If the user has supplied one, use theirs
        __isr_unhandledIRQCallback = userUnhandledIRQCallback;
    } else {
        //Otherwise use default
        __isr_unhandledIRQCallback = HPS_IRQ_unhandledIRQ;
    }

    //Enable interrupts again
    __enable_irq();

    //Mark as initialised
    __isInitialised = true;
    //And done
    return ERR_SUCCESS;
}

//Check if driver initialised
// - returns true if initialised
bool HPS_IRQ_isInitialised() {
    return __isInitialised;
}

//Globally enable or disable interrupts
// - If trying to enable:
//    - Requires that driver has been initialised
//    - Returns ERR_SUCCESS if interrupts have been enabled
// - If trying to disable
//    - Returns ERR_SUCCESS if interrupts have been disabled
//    - Returns ERR_SKIPPED if interrupts were already disabled
HpsErr_t HPS_IRQ_globalEnable(bool enable) {
    // Configure global IRQ flag
    if (enable) {
        if (!HPS_IRQ_isInitialised()) return ERR_NOINIT;
        __enable_irq();
        return ERR_SUCCESS;
    } else {
        bool wasMasked = __disable_irq();
        return wasMasked ? ERR_SUCCESS : ERR_SKIPPED;
    }
}

//Register an IRQ handler
HpsErr_t HPS_IRQ_registerHandler(HPSIRQSource interruptID, IsrHandlerFunc_t handlerFunction, void* handlerParam) {
    unsigned int handler;
    bool was_masked;
    if (!HPS_IRQ_isInitialised()) return ERR_NOINIT;

    //First check if a handler already exists (we can overwrite it if it does)
    handler = _HPS_IRQ_findHandler(interruptID);

    //Before changing anything we need to mask interrupts temporarily while we change the handlers
    was_masked = __disable_irq();

    //Grow the handler table if ID not found
    if (handler == __isr_handler_count) {
        //Grow the table by one
        HpsErr_t status = _HPS_IRQ_growTable(1);
        if (IS_ERROR(status)) return status;
    }

    //Add our new handler
    _HPS_IRQ_doRegister(handler, interruptID, handlerFunction, handlerParam);

    //Finally we unmask interrupts to resume processing.
    if (!was_masked) {
        __enable_irq();
    }
    //And done.
    return ERR_SUCCESS;
}

//Register multiple IRQ handlers
HpsErr_t HPS_IRQ_registerHandlers(HPSIRQSource* interruptIDs, IsrHandlerFunc_t* handlerFunctions, void** handlerParams, unsigned int count) {
    unsigned int handlers [count];
    bool was_masked;
    //Validate inputs
    if (!HPS_IRQ_isInitialised()) return ERR_NOINIT;
    if (!interruptIDs || !handlerFunctions) return ERR_NULLPTR;
    //First check if any handlers already exist (we can overwrite any that do)
    unsigned int growBy = 0;
    for (unsigned int idx = 0; idx < count; idx++) {
        unsigned int handler = _HPS_IRQ_findHandler(interruptIDs[idx]);
        if (handler == __isr_handler_count) {
            //Handler not found.
            //Add on to handler amount we previously needed to grow by so that each subsequent
            //not found goes after the next.
            handler += growBy;
            //And increment the number we need to grow the table by
            growBy++;
        }
        handlers[idx] = handler;
    }

    //Before changing anything we need to mask interrupts temporarily while we change the handlers
    was_masked = __disable_irq();

    //Ensure the handler table is big enough
    HpsErr_t status = _HPS_IRQ_growTable(growBy);
    if (IS_ERROR(status)) return status;

    //Add our new handlers
    for (unsigned int idx = 0; idx < count; idx++) {
        void* param;
        if (!handlerParams) {
            param = NULL;
        } else {
            param = handlerParams[idx];
        }
        _HPS_IRQ_doRegister(handlers[idx], interruptIDs[idx], handlerFunctions[idx], param);
    }

    //Finally we unmask interrupts to resume processing.
    if (!was_masked) {
        __enable_irq();
    }
    //And done.
    return ERR_SUCCESS;

}


HpsErr_t HPS_IRQ_unregisterHandler(HPSIRQSource interruptID) {
    unsigned int handler;
    if (!HPS_IRQ_isInitialised()) return ERR_NOINIT;
    //See if we can find the requested handler
    handler = _HPS_IRQ_findHandler(interruptID);
    //If so, unregister it
    if (handler != __isr_handler_count) {
        //Found it, so unregister
        _HPS_IRQ_doUnregister(handler, interruptID);
        return ERR_SUCCESS;
    }
    //Whoops, handler doesn't exist.
    return ERR_NOTFOUND;
}

HpsErr_t HPS_IRQ_unregisterHandlers(HPSIRQSource* interruptIDs, unsigned int count) {
    unsigned int handler;
    HPSIRQSource interruptID;
    HpsErr_t status = ERR_SUCCESS;
    //Validate inputs
    if (!HPS_IRQ_isInitialised()) return ERR_NOINIT;
    if (!interruptIDs) return ERR_NULLPTR;
    //Loop through all interrupt IDs
    for (unsigned int idx = 0; idx < count; idx++) {
        interruptID = interruptIDs[idx];
        handler = _HPS_IRQ_findHandler(interruptID);
        if (handler != __isr_handler_count) {
            //Found it, so unregister
            _HPS_IRQ_doUnregister(handler, interruptID);
        } else {
            //Otherwise at least one was not found.
            status = ERR_NOTFOUND;
        }
    }
    //Done. Return whether any were not found.
    return status;
}

