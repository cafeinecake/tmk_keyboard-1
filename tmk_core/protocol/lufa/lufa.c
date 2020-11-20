/*
 * Copyright 2012 Jun Wako <wakojun@gmail.com>
 * This file is based on:
 *     LUFA-120219/Demos/Device/Lowlevel/KeyboardMouse
 *     LUFA-120219/Demos/Device/Lowlevel/GenericHID
 */

/*
             LUFA Library
     Copyright (C) Dean Camera, 2012.

  dean [at] fourwalledcubicle [dot] com
           www.lufa-lib.org
*/

/*
  Copyright 2012  Dean Camera (dean [at] fourwalledcubicle [dot] com)
  Copyright 2010  Denver Gingerich (denver [at] ossguy [dot] com)

  Permission to use, copy, modify, distribute, and sell this
  software and its documentation for any purpose is hereby granted
  without fee, provided that the above copyright notice appear in
  all copies and that both that the copyright notice and this
  permission notice and warranty disclaimer appear in supporting
  documentation, and that the name of the author not be used in
  advertising or publicity pertaining to distribution of the
  software without specific, written prior permission.

  The author disclaim all warranties with regard to this
  software, including all implied warranties of merchantability
  and fitness.  In no event shall the author be liable for any
  special, indirect or consequential damages or any damages
  whatsoever resulting from loss of use, data or profits, whether
  in an action of contract, negligence or other tortious action,
  arising out of or in connection with the use or performance of
  this software.
*/

#include "report.h"
#include "host.h"
#include "host_driver.h"
#include "keyboard.h"
#include "action.h"
#include "led.h"
#include "ringbuf.h"
#include "debug.h"
#ifdef SLEEP_LED_ENABLE
#include "sleep_led.h"
#endif
#include "suspend.h"
#include "hook.h"
#include "timer.h"

#include "matrix.h"
#include "descriptor.h"
#include "console.h"
#include "lufa.h"


uint8_t keyboard_idle = 0;
/* 0: Boot Protocol, 1: Report Protocol(default) */
uint8_t keyboard_protocol = 1;
static uint8_t keyboard_led_stats = 0;

static report_keyboard_t keyboard_report_sent;


/* Host driver */
static uint8_t keyboard_leds(void);
static void send_keyboard(report_keyboard_t *report);
static void send_mouse(report_mouse_t *report);
static void send_system(uint16_t data);
static void send_consumer(uint16_t data);

host_driver_t lufa_driver = {
    keyboard_leds,
    send_keyboard,
    send_mouse,
    send_system,
    send_consumer
};


/*******************************************************************************
 * USB Events
 ******************************************************************************/
/*
 * Event Order of Plug in:
 * 0) EVENT_USB_Device_Connect
 * 1) EVENT_USB_Device_Suspend
 * 2) EVENT_USB_Device_Reset
 * 3) EVENT_USB_Device_Wake
*/
void EVENT_USB_Device_Connect(void)
{
#ifdef TMK_LUFA_DEBUG
    print("[C]");
#endif
    /* For battery powered device */
    if (!USB_IsInitialized) {
        USB_Disable();
        USB_Init();
    }
}

void EVENT_USB_Device_Disconnect(void)
{
#ifdef TMK_LUFA_DEBUG
    print("[D]");
#endif
    /* For battery powered device */
    USB_IsInitialized = false;
/* TODO: This doesn't work. After several plug in/outs can not be enumerated.
    if (USB_IsInitialized) {
        USB_Disable();  // Disable all interrupts
        USB_Controller_Enable();
        USB_INT_Enable(USB_INT_VBUSTI);
    }
*/
}

void EVENT_USB_Device_Reset(void)
{
#ifdef TMK_LUFA_DEBUG
    print("[R]");
#endif
}

void EVENT_USB_Device_Suspend()
{
#ifdef TMK_LUFA_DEBUG
    print("[S]");
#endif
    hook_usb_suspend_entry();
}

void EVENT_USB_Device_WakeUp()
{
#ifdef TMK_LUFA_DEBUG
    print("[W]");
#endif
    hook_usb_wakeup();
}

/** Event handler for the USB_ConfigurationChanged event.
 * This is fired when the host sets the current configuration of the USB device after enumeration.
 *
 * ATMega32u2 supports dual bank(ping-pong mode) only on endpoint 3 and 4,
 * it is safe to use singl bank for all endpoints.
 */
void EVENT_USB_Device_ConfigurationChanged(void)
{
#ifdef TMK_LUFA_DEBUG
    print("[c]");
#endif
    bool ConfigSuccess = true;

    /* Setup Keyboard HID Report Endpoints */
    ConfigSuccess &= ENDPOINT_CONFIG(KEYBOARD_IN_EPNUM, EP_TYPE_INTERRUPT, ENDPOINT_DIR_IN,
                                     KEYBOARD_EPSIZE, ENDPOINT_BANK_SINGLE);

#ifdef MOUSE_ENABLE
    /* Setup Mouse HID Report Endpoint */
    ConfigSuccess &= ENDPOINT_CONFIG(MOUSE_IN_EPNUM, EP_TYPE_INTERRUPT, ENDPOINT_DIR_IN,
                                     MOUSE_EPSIZE, ENDPOINT_BANK_SINGLE);
#endif

#ifdef EXTRAKEY_ENABLE
    /* Setup Extra HID Report Endpoint */
    ConfigSuccess &= ENDPOINT_CONFIG(EXTRAKEY_IN_EPNUM, EP_TYPE_INTERRUPT, ENDPOINT_DIR_IN,
                                     EXTRAKEY_EPSIZE, ENDPOINT_BANK_SINGLE);
#endif

#ifdef NKRO_ENABLE
    /* Setup NKRO HID Report Endpoints */
    ConfigSuccess &= ENDPOINT_CONFIG(NKRO_IN_EPNUM, EP_TYPE_INTERRUPT, ENDPOINT_DIR_IN,
                                     NKRO_EPSIZE, ENDPOINT_BANK_SINGLE);
#endif

#ifdef CONSOLE_ENABLE
    console_configure();
#endif
}

/*
Appendix G: HID Request Support Requirements

The following table enumerates the requests that need to be supported by various types of HID class devices.

Device type     GetReport   SetReport   GetIdle     SetIdle     GetProtocol SetProtocol
------------------------------------------------------------------------------------------
Boot Mouse      Required    Optional    Optional    Optional    Required    Required
Non-Boot Mouse  Required    Optional    Optional    Optional    Optional    Optional
Boot Keyboard   Required    Optional    Required    Required    Required    Required
Non-Boot Keybrd Required    Optional    Required    Required    Optional    Optional
Other Device    Required    Optional    Optional    Optional    Optional    Optional
*/
/** Event handler for the USB_ControlRequest event.
 *  This is fired before passing along unhandled control requests to the library for processing internally.
 */
void EVENT_USB_Device_ControlRequest(void)
{
    uint8_t* ReportData = NULL;
    uint8_t  ReportSize = 0;

    /* Handle HID Class specific requests */
    switch (USB_ControlRequest.bRequest)
    {
        case HID_REQ_GetReport:
            if (USB_ControlRequest.bmRequestType == (REQDIR_DEVICETOHOST | REQTYPE_CLASS | REQREC_INTERFACE))
            {
                Endpoint_ClearSETUP();

                // Interface
                switch (USB_ControlRequest.wIndex) {
                case KEYBOARD_INTERFACE:
                    // TODO: test/check
                    ReportData = (uint8_t*)&keyboard_report_sent;
                    ReportSize = sizeof(keyboard_report_sent);
                    break;
                }

                /* Write the report data to the control endpoint */
                Endpoint_Write_Control_Stream_LE(ReportData, ReportSize);
                Endpoint_ClearOUT();
#ifdef TMK_LUFA_DEBUG
                xprintf("[r%d]", USB_ControlRequest.wIndex);
#endif
            }

            break;
        case HID_REQ_SetReport:
            if (USB_ControlRequest.bmRequestType == (REQDIR_HOSTTODEVICE | REQTYPE_CLASS | REQREC_INTERFACE))
            {

                // Interface
                switch (USB_ControlRequest.wIndex) {
                case KEYBOARD_INTERFACE:
#ifdef NKRO_ENABLE
                case NKRO_INTERFACE:
#endif
                    Endpoint_ClearSETUP();

                    while (!(Endpoint_IsOUTReceived())) {
                        if (USB_DeviceState == DEVICE_STATE_Unattached)
                          return;
                    }
                    keyboard_led_stats = Endpoint_Read_8();

                    Endpoint_ClearOUT();
                    Endpoint_ClearStatusStage();
#ifdef TMK_LUFA_DEBUG
                    xprintf("[L%d]", USB_ControlRequest.wIndex);
#endif
                    break;
                }

            }

            break;

        case HID_REQ_GetProtocol:
            if (USB_ControlRequest.bmRequestType == (REQDIR_DEVICETOHOST | REQTYPE_CLASS | REQREC_INTERFACE))
            {
                if (USB_ControlRequest.wIndex == KEYBOARD_INTERFACE) {
                    Endpoint_ClearSETUP();
                    while (!(Endpoint_IsINReady()));
                    Endpoint_Write_8(keyboard_protocol);
                    Endpoint_ClearIN();
                    Endpoint_ClearStatusStage();
#ifdef TMK_LUFA_DEBUG
                    print("[p]");
#endif
                }
            }

            break;
        case HID_REQ_SetProtocol:
            if (USB_ControlRequest.bmRequestType == (REQDIR_HOSTTODEVICE | REQTYPE_CLASS | REQREC_INTERFACE))
            {
                if (USB_ControlRequest.wIndex == KEYBOARD_INTERFACE) {
                    Endpoint_ClearSETUP();
                    Endpoint_ClearStatusStage();

                    keyboard_protocol = (USB_ControlRequest.wValue & 0xFF);
                    clear_keyboard();
#ifdef TMK_LUFA_DEBUG
                    print("[P]");
#endif
                }
            }

            break;
        case HID_REQ_SetIdle:
            if (USB_ControlRequest.bmRequestType == (REQDIR_HOSTTODEVICE | REQTYPE_CLASS | REQREC_INTERFACE))
            {
                Endpoint_ClearSETUP();
                Endpoint_ClearStatusStage();

                keyboard_idle = ((USB_ControlRequest.wValue & 0xFF00) >> 8);
#ifdef TMK_LUFA_DEBUG
                xprintf("[I%d]%d", USB_ControlRequest.wIndex, (USB_ControlRequest.wValue & 0xFF00) >> 8);
#endif
            }

            break;
        case HID_REQ_GetIdle:
            if (USB_ControlRequest.bmRequestType == (REQDIR_DEVICETOHOST | REQTYPE_CLASS | REQREC_INTERFACE))
            {
                Endpoint_ClearSETUP();
                while (!(Endpoint_IsINReady()));
                Endpoint_Write_8(keyboard_idle);
                Endpoint_ClearIN();
                Endpoint_ClearStatusStage();
#ifdef TMK_LUFA_DEBUG
                print("[i]");
#endif
            }

            break;
    }
#ifdef CONSOLE_ENABLE
    console_control_request();
#endif
}

/*******************************************************************************
 * Host driver
 ******************************************************************************/
static uint8_t keyboard_leds(void)
{
    return keyboard_led_stats;
}

static void send_keyboard(report_keyboard_t *report)
{
    uint8_t timeout = 128;

    if (USB_DeviceState != DEVICE_STATE_Configured)
        return;

    /* Select the Keyboard Report Endpoint */
#ifdef NKRO_ENABLE
    if (keyboard_protocol && keyboard_nkro) {
        /* Report protocol - NKRO */
        Endpoint_SelectEndpoint(NKRO_IN_EPNUM);

        /* Check if write ready for a polling interval around 1ms */
        while (timeout-- && !Endpoint_IsReadWriteAllowed()) _delay_us(8);
        if (!Endpoint_IsReadWriteAllowed()) return;

        /* Write Keyboard Report Data */
        Endpoint_Write_Stream_LE(report, NKRO_EPSIZE, NULL);
    }
    else
#endif
    {
        /* Boot protocol */
        Endpoint_SelectEndpoint(KEYBOARD_IN_EPNUM);

        /* Check if write ready for a polling interval around 10ms */
        while (timeout-- && !Endpoint_IsReadWriteAllowed()) _delay_us(80);
        if (!Endpoint_IsReadWriteAllowed()) return;

        /* Write Keyboard Report Data */
        Endpoint_Write_Stream_LE(report, KEYBOARD_EPSIZE, NULL);
    }

    /* Finalize the stream transfer to send the last packet */
    Endpoint_ClearIN();

    keyboard_report_sent = *report;
}

static void send_mouse(report_mouse_t *report)
{
#ifdef MOUSE_ENABLE
    uint8_t timeout = 255;

    if (USB_DeviceState != DEVICE_STATE_Configured)
        return;

    /* Select the Mouse Report Endpoint */
    Endpoint_SelectEndpoint(MOUSE_IN_EPNUM);

    /* Check if write ready for a polling interval around 10ms */
    while (timeout-- && !Endpoint_IsReadWriteAllowed()) _delay_us(40);
    if (!Endpoint_IsReadWriteAllowed()) return;

    /* Write Mouse Report Data */
    Endpoint_Write_Stream_LE(report, sizeof(report_mouse_t), NULL);

    /* Finalize the stream transfer to send the last packet */
    Endpoint_ClearIN();
#endif
}

static void send_system(uint16_t data)
{
#ifdef EXTRAKEY_ENABLE
    uint8_t timeout = 255;

    if (USB_DeviceState != DEVICE_STATE_Configured)
        return;

    report_extra_t r = {
        .report_id = REPORT_ID_SYSTEM,
        .usage = data - SYSTEM_POWER_DOWN + 1
    };
    Endpoint_SelectEndpoint(EXTRAKEY_IN_EPNUM);

    /* Check if write ready for a polling interval around 10ms */
    while (timeout-- && !Endpoint_IsReadWriteAllowed()) _delay_us(40);
    if (!Endpoint_IsReadWriteAllowed()) return;

    Endpoint_Write_Stream_LE(&r, sizeof(report_extra_t), NULL);
    Endpoint_ClearIN();
#endif
}

static void send_consumer(uint16_t data)
{
#ifdef EXTRAKEY_ENABLE
    uint8_t timeout = 255;

    if (USB_DeviceState != DEVICE_STATE_Configured)
        return;

    report_extra_t r = {
        .report_id = REPORT_ID_CONSUMER,
        .usage = data
    };
    Endpoint_SelectEndpoint(EXTRAKEY_IN_EPNUM);

    /* Check if write ready for a polling interval around 10ms */
    while (timeout-- && !Endpoint_IsReadWriteAllowed()) _delay_us(40);
    if (!Endpoint_IsReadWriteAllowed()) return;

    Endpoint_Write_Stream_LE(&r, sizeof(report_extra_t), NULL);
    Endpoint_ClearIN();
#endif
}


/*******************************************************************************
 * main
 ******************************************************************************/
static void setup_mcu(void)
{
    /* Disable watchdog if enabled by bootloader/fuses */
    MCUSR &= ~(1 << WDRF);
    wdt_disable();

    /* Disable clock division */
    clock_prescale_set(clock_div_1);
}

static void setup_usb(void)
{
    // Leonardo needs. Without this USB device is not recognized.
    USB_Disable();

    USB_Init();
}

int main(void)  __attribute__ ((weak));
int main(void)
{
    setup_mcu();

#ifdef CONSOLE_ENABLE
    // setup xprintf and <stdio.h>: DO NOT USE print functions before this line
    console_init();
#endif

    host_set_driver(&lufa_driver);

    print("\n\nTMK:" STR(TMK_VERSION) "/LUFA\n\n");
    hook_early_init();
    keyboard_setup();
    setup_usb();
#ifdef SLEEP_LED_ENABLE
    sleep_led_init();
#endif

    sei();

    keyboard_init();

#ifndef NO_USB_STARTUP_WAIT_LOOP
    /* wait for USB startup */
    while (USB_DeviceState != DEVICE_STATE_Configured
#ifdef CONSOLE_STARTUP_WAIT
          || !console_is_ready()
#endif
    ) {
#if !defined(INTERRUPT_CONTROL_ENDPOINT)
        USB_USBTask();
#endif
        hook_usb_startup_wait_loop();
    }
    print("\nUSB configured.\n");
#endif

    hook_late_init();

    print("\nKeyboard start.\n");
    while (1) {
#ifndef NO_USB_SUSPEND_LOOP
        while (USB_DeviceState == DEVICE_STATE_Suspended) {
            hook_usb_suspend_loop();
        }
#endif

        keyboard_task();

#ifdef CONSOLE_ENABLE
        console_task();
#endif

#if !defined(INTERRUPT_CONTROL_ENDPOINT)
        USB_USBTask();
#endif
    }
}


/* hooks */
__attribute__((weak))
void hook_early_init(void) {}

__attribute__((weak))
void hook_late_init(void) {}

static uint8_t _led_stats = 0;
 __attribute__((weak))
void hook_usb_suspend_entry(void)
{
    // Turn off LED to save power and keep its status to resotre it later.
    // LED status will be updated by keyboard_task() in main loop hopefully.
    _led_stats = keyboard_led_stats;
    keyboard_led_stats = 0;

    // Calling long task here can prevent USB state transition

    matrix_clear();
    clear_keyboard();
#ifdef SLEEP_LED_ENABLE
    sleep_led_enable();
#endif
}

__attribute__((weak))
void hook_usb_suspend_loop(void)
{
#ifndef CONSOLE_UART
    // This corrupts debug print when suspend
    suspend_power_down();
#endif
    if (USB_Device_RemoteWakeupEnabled && suspend_wakeup_condition()) {
        USB_Device_SendRemoteWakeup();
    }
}

__attribute__((weak))
void hook_usb_wakeup(void)
{
    suspend_wakeup_init();
#ifdef SLEEP_LED_ENABLE
    sleep_led_disable();
#endif

    // Restore LED status and update at keyboard_task() in main loop
    keyboard_led_stats = _led_stats;

    // Calling long task here can prevent USB state transition
}

__attribute__((weak))
void hook_usb_startup_wait_loop(void) {}
