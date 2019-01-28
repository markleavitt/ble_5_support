/*
 * Copyright (c) 2018 Particle Industries, Inc.  All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation, either
 * version 3 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "core_hal.h"
#include "service_debug.h"
#include <nrf52840.h>
#include "hal_event.h"
#include <FreeRTOS.h>
#include <task.h>
#include <semphr.h>
#include "hw_config.h"
#include "syshealth_hal.h"
#include <nrfx_types.h>
#include <nrf_mbr.h>
#include <nrf_sdm.h>
#include <nrf_sdh.h>
#include <nrf_rtc.h>
#include "button_hal.h"
#include "hal_platform.h"
#include "user.h"
#include "dct.h"
#include "rng_hal.h"
#include "interrupts_hal.h"
#include <nrf_power.h>
#include "ota_module.h"
#include "bootloader.h"
#include <stdlib.h>
#include <malloc.h>
#include "rtc_hal.h"
#include "timer_hal.h"
#include "pinmap_impl.h"
#include <nrf_pwm.h>
#include "system_error.h"
#include <nrf_lpcomp.h>
#include <nrfx_gpiote.h>
#include <nrf_drv_clock.h>
#include "usb_hal.h"
#include "usart_hal.h"
#include "system_error.h"
#include <nrfx_rtc.h>
#include "gpio_hal.h"
#include "exflash_hal.h"

#define BACKUP_REGISTER_NUM        10
static int32_t backup_register[BACKUP_REGISTER_NUM] __attribute__((section(".backup_registers")));
static volatile uint8_t rtos_started = 0;

#define RTC_ID   1
static const app_irq_priority_t RTC_IRQ_Priority = APP_IRQ_PRIORITY_LOWEST;
static const nrfx_rtc_t m_rtc = NRFX_RTC_INSTANCE(RTC_ID);

static struct Last_Reset_Info {
    int reason;
    uint32_t data;
} last_reset_info = { RESET_REASON_NONE, 0 };

typedef enum Feature_Flag {
    FEATURE_FLAG_RESET_INFO = 0x01,
    FEATURE_FLAG_ETHERNET_DETECTION = 0x02
} Feature_Flag;

#define STOP_MODE_EXIT_CONDITION_NONE   0x00
#define STOP_MODE_EXIT_CONDITION_PIN    0x01
#define STOP_MODE_EXIT_CONDITION_RTC    0x02
static volatile struct {
    uint32_t    source;
    uint32_t    pin_index;
} wakeup_info;

void HardFault_Handler( void ) __attribute__( ( naked ) );

void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);

void SysTick_Handler(void);
void SysTickOverride(void);

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info);
void app_error_handler_bare(uint32_t err);

extern char link_heap_location, link_heap_location_end;
extern char link_interrupt_vectors_location;
extern char link_ram_interrupt_vectors_location;
extern char link_ram_interrupt_vectors_location_end;
extern char _Stack_Init;

static void* new_heap_end = &link_heap_location_end;

extern void malloc_enable(uint8_t);
extern void malloc_set_heap_end(void*);
extern void* malloc_heap_end();
#if defined(MODULAR_FIRMWARE)
void* module_user_pre_init();
#endif

extern void nrf5AlarmInit(void);
extern void nrf5AlarmDeinit(void);

__attribute__((externally_visible)) void prvGetRegistersFromStack( uint32_t *pulFaultStackAddress ) {
    /* These are volatile to try and prevent the compiler/linker optimising them
    away as the variables never actually get used.  If the debugger won't show the
    values of the variables, make them global my moving their declaration outside
    of this function. */
    volatile uint32_t r0;
    volatile uint32_t r1;
    volatile uint32_t r2;
    volatile uint32_t r3;
    volatile uint32_t r12;
    volatile uint32_t lr; /* Link register. */
    volatile uint32_t pc; /* Program counter. */
    volatile uint32_t psr;/* Program status register. */

    r0 = pulFaultStackAddress[ 0 ];
    r1 = pulFaultStackAddress[ 1 ];
    r2 = pulFaultStackAddress[ 2 ];
    r3 = pulFaultStackAddress[ 3 ];

    r12 = pulFaultStackAddress[ 4 ];
    lr = pulFaultStackAddress[ 5 ];
    pc = pulFaultStackAddress[ 6 ];
    psr = pulFaultStackAddress[ 7 ];

    /* Silence "variable set but not used" error */
    if (false) {
        (void)r0; (void)r1; (void)r2; (void)r3; (void)r12; (void)lr; (void)pc; (void)psr;
    }

    if (SCB->CFSR & (1<<25) /* DIVBYZERO */) {
        // stay consistent with the core and cause 5 flashes
        UsageFault_Handler();
    } else {
        PANIC(HardFault,"HardFault");

        /* Go to infinite loop when Hard Fault exception occurs */
        while (1) {
            ;
        }
    }
}


void HardFault_Handler(void) {
    __asm volatile
    (
        " tst lr, #4                                                \n"
        " ite eq                                                    \n"
        " mrseq r0, msp                                             \n"
        " mrsne r0, psp                                             \n"
        " ldr r1, [r0, #24]                                         \n"
        " ldr r2, handler2_address_const                            \n"
        " bx r2                                                     \n"
        " handler2_address_const: .word prvGetRegistersFromStack    \n"
    );
}

void app_error_fault_handler(uint32_t _id, uint32_t _pc, uint32_t _info) {
    volatile uint32_t id = _id;
    volatile uint32_t pc = _pc;
    volatile uint32_t info = _info;
    (void)id; (void)pc; (void)info;
    PANIC(HardFault,"HardFault");
    while(1) {
        ;
    }
}

void app_error_handler_bare(uint32_t error_code) {
    PANIC(HardFault,"HardFault");
    while(1) {
        ;
    }
}

void app_error_handler(uint32_t error_code, uint32_t line_num, const uint8_t * p_file_name)
{
    PANIC(HardFault,"HardFault");
    while(1) {
    }
}

void MemManage_Handler(void) {
    /* Go to infinite loop when Memory Manage exception occurs */
    PANIC(MemManage,"MemManage");
    while (1) {
        ;
    }
}

void BusFault_Handler(void) {
    /* Go to infinite loop when Bus Fault exception occurs */
    PANIC(BusFault,"BusFault");
    while (1) {
        ;
    }
}

void UsageFault_Handler(void) {
    /* Go to infinite loop when Usage Fault exception occurs */
    PANIC(UsageFault,"UsageFault");
    while (1) {
        ;
    }
}

void SysTickOverride(void) {
    HAL_SysTick_Handler();
}

void SysTickChain() {
    SysTick_Handler();
    SysTickOverride();
}

/**
 * Called by HAL_Core_Init() to pre-initialize any low level hardware before
 * the main loop runs.
 */
void HAL_Core_Init_finalize(void) {
    uint32_t* isrs = (uint32_t*)&link_ram_interrupt_vectors_location;
    isrs[IRQN_TO_IDX(SysTick_IRQn)] = (uint32_t)SysTickChain;
}

void HAL_Core_Init(void) {
    HAL_Core_Init_finalize();
}

void HAL_Core_Config_systick_configuration(void) {
    // SysTick_Configuration();
    // sd_nvic_EnableIRQ(SysTick_IRQn);
    // dcd_migrate_data();
}

/**
 * Called by HAL_Core_Config() to allow the HAL implementation to override
 * the interrupt table if required.
 */
void HAL_Core_Setup_override_interrupts(void) {
    uint32_t* isrs = (uint32_t*)&link_ram_interrupt_vectors_location;
    /* Set MBR to forward interrupts to application */
    *((volatile uint32_t*)0x20000000) = (uint32_t)isrs;
    /* Reset SoftDevice vector address */
    *((volatile uint32_t*)0x20000004) = 0xFFFFFFFF;

    SCB->VTOR = 0x0;

    /* Init softdevice */
    sd_mbr_command_t com = {SD_MBR_COMMAND_INIT_SD, };
    uint32_t ret = sd_mbr_command(&com);
    SPARK_ASSERT(ret == NRF_SUCCESS);
    /* Forward unhandled interrupts to the application */
    sd_softdevice_vector_table_base_set((uint32_t)isrs);
    /* Enable softdevice */
    nrf_sdh_enable_request();
    /* Wait until softdevice enabled*/
    while (!nrf_sdh_is_enabled()) {
        ;
    }
}

void HAL_Core_Restore_Interrupt(IRQn_Type irqn) {
    uint32_t handler = ((const uint32_t*)&link_interrupt_vectors_location)[IRQN_TO_IDX(irqn)];

    // Special chain handler
    if (irqn == SysTick_IRQn) {
        handler = (uint32_t)SysTickChain;
    }

    volatile uint32_t* isrs = (volatile uint32_t*)&link_ram_interrupt_vectors_location;
    isrs[IRQN_TO_IDX(irqn)] = handler;
}

/*******************************************************************************
 * Function Name  : HAL_Core_Config.
 * Description    : Called in startup routine, before calling C++ constructors.
 * Input          : None.
 * Output         : None.
 * Return         : None.
 *******************************************************************************/
void HAL_Core_Config(void) {
    DECLARE_SYS_HEALTH(ENTERED_SparkCoreConfig);

#ifdef DFU_BUILD_ENABLE
    USE_SYSTEM_FLAGS = 1;
#endif

    /* Forward interrupts */
    memcpy(&link_ram_interrupt_vectors_location, &link_interrupt_vectors_location, &link_ram_interrupt_vectors_location_end-&link_ram_interrupt_vectors_location);
    uint32_t* isrs = (uint32_t*)&link_ram_interrupt_vectors_location;
    SCB->VTOR = (uint32_t)isrs;

    // GPIOTE initialization
    HAL_Interrupts_Init();

    Set_System();

    hal_timer_init(NULL);

    HAL_Core_Setup_override_interrupts();

    HAL_RNG_Configuration();

    HAL_RTC_Configuration();

#if defined(MODULAR_FIRMWARE)
    if (HAL_Core_Validate_User_Module()) {
        new_heap_end = module_user_pre_init();
        if (new_heap_end > malloc_heap_end()) {
            malloc_set_heap_end(new_heap_end);
        }
    }
    else {
        // Set the heap end to the stack start to make most use of the SRAM.
        malloc_set_heap_end(&_Stack_Init);

        // Update the user module if needed
        user_update_if_needed();
    }

    // Enable malloc before littlefs initialization.
    malloc_enable(1);
#endif

#ifdef DFU_BUILD_ENABLE
    Load_SystemFlags();
#endif

    // TODO: Use current LED theme
    LED_SetRGBColor(RGB_COLOR_WHITE);
    LED_On(LED_RGB);

    FLASH_AddToFactoryResetModuleSlot(
      FLASH_INTERNAL, EXTERNAL_FLASH_FAC_XIP_ADDRESS,
      FLASH_INTERNAL, USER_FIRMWARE_IMAGE_LOCATION, FIRMWARE_IMAGE_SIZE,
      FACTORY_RESET_MODULE_FUNCTION, MODULE_VERIFY_CRC|MODULE_VERIFY_FUNCTION|MODULE_VERIFY_DESTINATION_IS_START_ADDRESS); //true to verify the CRC during copy also
}

void HAL_Core_Setup(void) {
    /* DOES NOT DO ANYTHING
     * SysTick is enabled within FreeRTOS
     */
    HAL_Core_Config_systick_configuration();

    if (bootloader_update_if_needed()) {
        HAL_Core_System_Reset();
    }
}

#if defined(MODULAR_FIRMWARE) && MODULAR_FIRMWARE
bool HAL_Core_Validate_User_Module(void)
{
    bool valid = false;

    //CRC verification Enabled by default
    if (FLASH_isUserModuleInfoValid(FLASH_INTERNAL, USER_FIRMWARE_IMAGE_LOCATION, USER_FIRMWARE_IMAGE_LOCATION))
    {
        //CRC check the user module and set to module_user_part_validated
        valid = FLASH_VerifyCRC32(FLASH_INTERNAL, USER_FIRMWARE_IMAGE_LOCATION,
                                  FLASH_ModuleLength(FLASH_INTERNAL, USER_FIRMWARE_IMAGE_LOCATION))
                && HAL_Verify_User_Dependencies();
    }
    else if(FLASH_isUserModuleInfoValid(FLASH_INTERNAL, EXTERNAL_FLASH_FAC_XIP_ADDRESS, USER_FIRMWARE_IMAGE_LOCATION))
    {
        // If user application is invalid, we should at least enable
        // the heap allocation for littlelf to set system flags.
        malloc_enable(1);

        //Reset and let bootloader perform the user module factory reset
        //Doing this instead of calling FLASH_RestoreFromFactoryResetModuleSlot()
        //saves precious system_part2 flash size i.e. fits in < 128KB
        HAL_Core_Factory_Reset();

        while(1);//Device should reset before reaching this line
    }

    return valid;
}

bool HAL_Core_Validate_Modules(uint32_t flags, void* reserved)
{
    const module_bounds_t* bounds = NULL;
    hal_module_t mod;
    bool module_fetched = false;
    bool valid = false;

    // First verify bootloader module
    bounds = find_module_bounds(MODULE_FUNCTION_BOOTLOADER, 0, HAL_PLATFORM_MCU_DEFAULT);
    module_fetched = fetch_module(&mod, bounds, false, MODULE_VALIDATION_INTEGRITY);

    valid = module_fetched && (mod.validity_checked == mod.validity_result);

    if (!valid) {
        return valid;
    }

    // Now check system-parts
    int i = 0;
    if (flags & 1) {
        // Validate only that system-part that depends on bootloader passes dependency check
        i = 1;
    }
    do {
        bounds = find_module_bounds(MODULE_FUNCTION_SYSTEM_PART, i++, HAL_PLATFORM_MCU_DEFAULT);
        if (bounds) {
            module_fetched = fetch_module(&mod, bounds, false, MODULE_VALIDATION_INTEGRITY);
            valid = module_fetched && (mod.validity_checked == mod.validity_result);
        }
        if (flags & 1) {
            bounds = NULL;
        }
    } while(bounds != NULL && valid);

    return valid;
}
#endif

bool HAL_Core_Mode_Button_Pressed(uint16_t pressedMillisDuration) {
    bool pressedState = false;

    if(BUTTON_GetDebouncedTime(BUTTON1) >= pressedMillisDuration) {
        pressedState = true;
    }
    if(BUTTON_GetDebouncedTime(BUTTON1_MIRROR) >= pressedMillisDuration) {
        pressedState = true;
    }

    return pressedState;
}

void HAL_Core_Mode_Button_Reset(uint16_t button)
{
}

void HAL_Core_System_Reset(void) {
    NVIC_SystemReset();
}

void HAL_Core_System_Reset_Ex(int reason, uint32_t data, void *reserved) {
    if (HAL_Feature_Get(FEATURE_RESET_INFO)) {
        // Save reset info to backup registers
        HAL_Core_Write_Backup_Register(BKP_DR_02, reason);
        HAL_Core_Write_Backup_Register(BKP_DR_03, data);
    }

    HAL_Core_System_Reset();
}

void HAL_Core_Factory_Reset(void) {
    system_flags.Factory_Reset_SysFlag = 0xAAAA;
    Save_SystemFlags();
    HAL_Core_System_Reset_Ex(RESET_REASON_FACTORY_RESET, 0, NULL);
}

void HAL_Core_Enter_Safe_Mode(void* reserved) {
    HAL_Core_Write_Backup_Register(BKP_DR_01, ENTER_SAFE_MODE_APP_REQUEST);
    HAL_Core_System_Reset_Ex(RESET_REASON_SAFE_MODE, 0, NULL);
}

bool HAL_Core_Enter_Safe_Mode_Requested(void)
{
    Load_SystemFlags();
    uint8_t flags = SYSTEM_FLAG(StartupMode_SysFlag);

    return (flags & 1);
}

void HAL_Core_Enter_Bootloader(bool persist) {
    if (persist) {
        HAL_Core_Write_Backup_Register(BKP_DR_10, 0xFFFF);
        system_flags.FLASH_OTA_Update_SysFlag = 0xFFFF;
        Save_SystemFlags();
    } else {
        HAL_Core_Write_Backup_Register(BKP_DR_01, ENTER_DFU_APP_REQUEST);
    }

    HAL_Core_System_Reset_Ex(RESET_REASON_DFU_MODE, 0, NULL);
}

static void fpu_sleep_prepare(void) {
    uint32_t fpscr;
    CRITICAL_REGION_ENTER();
    fpscr = __get_FPSCR();
    /*
        * Clear FPU exceptions.
        * Without this step, the FPU interrupt is marked as pending,
        * preventing system from sleeping. Exceptions cleared:
        * - IOC - Invalid Operation cumulative exception bit.
        * - DZC - Division by Zero cumulative exception bit.
        * - OFC - Overflow cumulative exception bit.
        * - UFC - Underflow cumulative exception bit.
        * - IXC - Inexact cumulative exception bit.
        * - IDC - Input Denormal cumulative exception bit.
        */
    __set_FPSCR(fpscr & ~0x9Fu);
    __DMB();
    NVIC_ClearPendingIRQ(FPU_IRQn);
    CRITICAL_REGION_EXIT();

    /*__
        * Assert no critical FPU exception is signaled:
        * - IOC - Invalid Operation cumulative exception bit.
        * - DZC - Division by Zero cumulative exception bit.
        * - OFC - Overflow cumulative exception bit.
        */
    SPARK_ASSERT((fpscr & 0x07) == 0);
}
void HAL_Core_Enter_Stop_Mode(uint16_t wakeUpPin, uint16_t edgeTriggerMode, long seconds) {
    InterruptMode m = (InterruptMode)edgeTriggerMode;
    HAL_Core_Enter_Stop_Mode_Ext(&wakeUpPin, 1, &m, 1, seconds, NULL);
}

static void wakeup_rtc_handler(nrfx_rtc_int_type_t int_type) {
    wakeup_info.source |= STOP_MODE_EXIT_CONDITION_RTC;
}

static void wakeup_gpiote_handler(void* data) {
    wakeup_info.source |= STOP_MODE_EXIT_CONDITION_PIN;
    wakeup_info.pin_index = (uint32_t)data;
}

static void wakeup_from_rtc(uint32_t seconds) {
    //Initialize RTC instance
    nrfx_rtc_config_t config = {
        .prescaler          = 0xFFF, // 125 ms counter period, 582.542 hours overflow
        .interrupt_priority = RTC_IRQ_Priority,
        .tick_latency       = 0,
        .reliable           = false
    };

    uint32_t err_code = nrfx_rtc_init(&m_rtc, &config, wakeup_rtc_handler);
    SPARK_ASSERT(err_code == NRF_SUCCESS);

    // Set compare channel to trigger interrupt after COMPARE_COUNTERTIME seconds
    nrfx_rtc_counter_clear(&m_rtc);
    err_code = nrfx_rtc_cc_set(&m_rtc, 0, seconds * 8, true);
    SPARK_ASSERT(err_code == NRF_SUCCESS);

    // Power on RTC instance
    nrfx_rtc_enable(&m_rtc);
}

int32_t HAL_Core_Enter_Stop_Mode_Ext(const uint16_t* pins, size_t pins_count, const InterruptMode* mode, size_t mode_count, long seconds, void* reserved) {
    // Initial sanity check
    if ((pins_count == 0 || mode_count == 0 || pins == NULL || mode == NULL) && seconds <= 0) {
        return SYSTEM_ERROR_NOT_ALLOWED;
    }

    // Validate pins and modes
    if ((pins_count > 0 && pins == NULL) || (pins_count > 0 && mode_count == 0) || (mode_count > 0 && mode == NULL)) {
        return SYSTEM_ERROR_NOT_ALLOWED;
    }

    for (unsigned i = 0; i < pins_count; i++) {
        if (pins[i] >= TOTAL_PINS) {
            return SYSTEM_ERROR_NOT_ALLOWED;
        }
    }

    for (int i = 0; i < mode_count; i++) {
        switch(mode[i]) {
            case RISING:
            case FALLING:
            case CHANGE:
                break;
            default:
                return SYSTEM_ERROR_NOT_ALLOWED;
        }
    }

    SysTick->CTRL &= ~SysTick_CTRL_ENABLE_Msk;

    // Detach USB
    HAL_USB_Detach();

    // Disable RTC2
    nrf5AlarmDeinit();

    // Disable external flash
    hal_exflash_uninit();

    // Flush all USARTs
    for (int usart = 0; usart < TOTAL_USARTS; usart++) {
        if (HAL_USART_Is_Enabled(usart)) {
            HAL_USART_Flush_Data(usart);
        }
    }

    // Disable all interrupt, __disable_irq()
    uint8_t dummy = 0;
    uint32_t err_code = sd_nvic_critical_region_enter(&dummy);
    SPARK_ASSERT(err_code == NRF_SUCCESS);

    wakeup_info.source = STOP_MODE_EXIT_CONDITION_NONE;

    bool hfclk_resume = false;
    if (nrf_drv_clock_hfclk_is_running()) {
        hfclk_resume = true;
        nrf_drv_clock_hfclk_release();
        while (nrf_drv_clock_hfclk_is_running()) {
            ;
        }
    }

    uint32_t exit_conditions = 0x00;

    // Suspend all GPIOTE interrupts
    HAL_Interrupts_Suspend();

    for (int i = 0; i < pins_count; i++) {
        pin_t wakeUpPin = pins[i];
        InterruptMode edgeTriggerMode = (i < mode_count) ? mode[i] : mode[mode_count - 1];

        PinMode wakeUpPinMode = INPUT;
        // Set required pinMode based on edgeTriggerMode
        switch(edgeTriggerMode) {
            case RISING: {
                wakeUpPinMode = INPUT_PULLDOWN;
                break;
            }
            case FALLING: {
                wakeUpPinMode = INPUT_PULLUP;
                break;
            }
            case CHANGE:
            default:
                wakeUpPinMode = INPUT;
                break;
        }

        HAL_Pin_Mode(wakeUpPin, wakeUpPinMode);
        HAL_InterruptExtraConfiguration irqConf = {0};
        irqConf.version = HAL_INTERRUPT_EXTRA_CONFIGURATION_VERSION_2;
        irqConf.IRQChannelPreemptionPriority = 0;
        irqConf.IRQChannelSubPriority = 0;
        irqConf.keepHandler = 1;
        irqConf.keepPriority = 1;
        HAL_Interrupts_Attach(wakeUpPin, wakeup_gpiote_handler, (void*)(uint32_t)i, edgeTriggerMode, &irqConf);

        exit_conditions |= STOP_MODE_EXIT_CONDITION_PIN;
    }

    // Configure RTC wake-up
    if (seconds > 0) {
        wakeup_from_rtc(seconds);

        exit_conditions |= STOP_MODE_EXIT_CONDITION_RTC;
    }

    // Enable all interrupt, __enable_irq()
    err_code = sd_nvic_critical_region_exit(dummy);
    APP_ERROR_CHECK(err_code);

    fpu_sleep_prepare();

    // Enter sleep mode, wait for event
    do {
        SPARK_ASSERT(sd_app_evt_wait() == NRF_SUCCESS);
    } while (wakeup_info.source == STOP_MODE_EXIT_CONDITION_NONE);

    int32_t reason = SYSTEM_ERROR_NOT_SUPPORTED;

    if (exit_conditions & STOP_MODE_EXIT_CONDITION_PIN) {
        if (wakeup_info.source & STOP_MODE_EXIT_CONDITION_PIN) {
            reason = wakeup_info.pin_index + 1;
        }
        for (int i = 0; i < pins_count; i++) {
            pin_t wakeUpPin = pins[i];
            /* Detach the Interrupt pin */
            HAL_Interrupts_Detach_Ext(wakeUpPin, 1, NULL);
        }
    }

    if (exit_conditions & STOP_MODE_EXIT_CONDITION_RTC) {
        if (wakeup_info.source & STOP_MODE_EXIT_CONDITION_RTC) {
            reason = 0;
        }
    }

    // Restore wakeup RTC
    nrfx_rtc_uninit(&m_rtc);

    // Restore GPIOTE
    HAL_Interrupts_Restore();

    // Restore hfclk
    if (hfclk_resume) {
        nrf_drv_clock_hfclk_request(NULL);
        while (!nrf_drv_clock_hfclk_is_running()) {
            ;
        }
    }

    hal_exflash_init();

    nrf5AlarmInit();

    HAL_USB_Attach();

    SysTick->CTRL |= SysTick_CTRL_ENABLE_Msk;

    return reason;
}

void HAL_Core_Execute_Stop_Mode(void) {
}

int HAL_Core_Enter_Standby_Mode(uint32_t seconds, uint32_t flags) {
    // RTC cannot be kept running in System OFF mode, so wake up by RTC
    // is not supported in deep sleep
    if (seconds > 0) {
        return SYSTEM_ERROR_NOT_SUPPORTED;
    }

    return HAL_Core_Execute_Standby_Mode_Ext(flags, NULL);
}

int HAL_Core_Execute_Standby_Mode_Ext(uint32_t flags, void* reserved) {
    // Force to use external wakeup pin on Gen 3 Device
    if (flags & HAL_STANDBY_MODE_FLAG_DISABLE_WKP_PIN) {
        return SYSTEM_ERROR_NOT_SUPPORTED;
    }

    // Uninit GPIOTE
    nrfx_gpiote_uninit();

    // Disable GPIOTE PORT interrupts
    nrf_gpiote_int_disable(GPIOTE_INTENSET_PORT_Msk);

    // Clear any GPIOTE events
    nrf_gpiote_event_clear(NRF_GPIOTE_EVENTS_PORT);

    // Disable low power comparator
    nrf_lpcomp_disable();

    // Configure wakeup pin
    NRF5x_Pin_Info* PIN_MAP = HAL_Pin_Map();
    uint32_t nrf_pin = NRF_GPIO_PIN_MAP(PIN_MAP[WKP].gpio_port, PIN_MAP[WKP].gpio_pin);
    nrf_gpio_cfg_sense_input(nrf_pin, NRF_GPIO_PIN_PULLUP, NRF_GPIO_PIN_SENSE_LOW);

    // RAM retention is configured on early boot in Set_System()

    SPARK_ASSERT(sd_power_system_off() == NRF_SUCCESS);
    while (1);
    return 0;
}

void HAL_Core_Execute_Standby_Mode(void) {
}

bool HAL_Core_System_Reset_FlagSet(RESET_TypeDef resetType) {
    uint32_t reset_reason = SYSTEM_FLAG(RCC_CSR_SysFlag);
    if (reset_reason == 0xffffffff) {
        sd_power_reset_reason_get(&reset_reason);
    }
    switch(resetType) {
        case PIN_RESET: {
            return reset_reason == NRF_POWER_RESETREAS_RESETPIN_MASK;
        }
        case SOFTWARE_RESET: {
            return reset_reason == NRF_POWER_RESETREAS_SREQ_MASK;
        }
        case WATCHDOG_RESET: {
            return reset_reason == NRF_POWER_RESETREAS_DOG_MASK;
        }
        case POWER_MANAGEMENT_RESET: {
            // SYSTEM OFF Mode
            return reset_reason == NRF_POWER_RESETREAS_OFF_MASK;
        }
        // If none of the reset sources are flagged, this indicates that
        // the chip was reset from the on-chip reset generator,
        // which will indicate a power-on-reset or a brownout reset.
        case POWER_DOWN_RESET:
        case POWER_BROWNOUT_RESET: {
            return reset_reason == 0;
        }
        default:
            return false;
    }
}

static void Init_Last_Reset_Info()
{
    if (HAL_Core_System_Reset_FlagSet(SOFTWARE_RESET))
    {
        // Load reset info from backup registers
        last_reset_info.reason = HAL_Core_Read_Backup_Register(BKP_DR_02);
        last_reset_info.data = HAL_Core_Read_Backup_Register(BKP_DR_03);
        // Clear backup registers
        HAL_Core_Write_Backup_Register(BKP_DR_02, 0);
        HAL_Core_Write_Backup_Register(BKP_DR_03, 0);
    }
    else // Hardware reset
    {
        if (HAL_Core_System_Reset_FlagSet(WATCHDOG_RESET))
        {
            last_reset_info.reason = RESET_REASON_WATCHDOG;
        }
        else if (HAL_Core_System_Reset_FlagSet(POWER_MANAGEMENT_RESET))
        {
            last_reset_info.reason = RESET_REASON_POWER_MANAGEMENT; // Reset generated when entering standby mode (nRST_STDBY: 0)
        }
        else if (HAL_Core_System_Reset_FlagSet(POWER_DOWN_RESET))
        {
            last_reset_info.reason = RESET_REASON_POWER_DOWN;
        }
        else if (HAL_Core_System_Reset_FlagSet(POWER_BROWNOUT_RESET))
        {
            last_reset_info.reason = RESET_REASON_POWER_BROWNOUT;
        }
        else if (HAL_Core_System_Reset_FlagSet(PIN_RESET)) // Pin reset flag should be checked in the last place
        {
            last_reset_info.reason = RESET_REASON_PIN_RESET;
        }
        // TODO: Reset from USB, NFC, LPCOMP...
        else
        {
            last_reset_info.reason = RESET_REASON_UNKNOWN;
        }
        last_reset_info.data = 0; // Not used
    }

    // Clear Reset info register
    sd_power_reset_reason_clr(0xFFFFFFFF);
}

int HAL_Core_Get_Last_Reset_Info(int *reason, uint32_t *data, void *reserved) {
    if (HAL_Feature_Get(FEATURE_RESET_INFO)) {
        if (reason) {
            *reason = last_reset_info.reason;
        }
        if (data) {
            *data = last_reset_info.data;
        }
        return 0;
    }
    return -1;
}

/**
 * @brief  Computes the 32-bit CRC of a given buffer of byte data.
 * @param  pBuffer: pointer to the buffer containing the data to be computed
 * @param  BufferSize: Size of the buffer to be computed
 * @retval 32-bit CRC
 */
uint32_t HAL_Core_Compute_CRC32(const uint8_t *pBuffer, uint32_t bufferSize) {
    // TODO: Use the peripheral lock?
    return Compute_CRC32(pBuffer, bufferSize, NULL);
}

uint16_t HAL_Core_Mode_Button_Pressed_Time() {
    return 0;
}

void HAL_Bootloader_Lock(bool lock) {

}

unsigned HAL_Core_System_Clock(HAL_SystemClock clock, void* reserved) {
    return SystemCoreClock;
}


static TaskHandle_t  app_thread_handle;
#define APPLICATION_STACK_SIZE 6144

/**
 * The mutex to ensure only one thread manipulates the heap at a given time.
 */
xSemaphoreHandle malloc_mutex = 0;

static void init_malloc_mutex(void) {
    malloc_mutex = xSemaphoreCreateRecursiveMutex();
}

void __malloc_lock(void* ptr) {
    if (malloc_mutex) {
        while (!xSemaphoreTakeRecursive(malloc_mutex, 0xFFFFFFFF)) {
            ;
        }
    }
}

void __malloc_unlock(void* ptr) {
    if (malloc_mutex) {
        xSemaphoreGiveRecursive(malloc_mutex);
    }
}

/**
 * The entrypoint to our application.
 * This should be called from the RTOS main thread once initialization has been
 * completed, constructors invoked and and HAL_Core_Config() has been called.
 */
void application_start() {
    rtos_started = 1;

    // one the key is sent to the cloud, this can be removed, since the key is fetched in
    // Spark_Protocol_init(). This is just a temporary measure while the key still needs
    // to be fetched via DFU.

    HAL_Core_Setup();

    // TODO:
    // generate_key();

    if (HAL_Feature_Get(FEATURE_RESET_INFO))
    {
        // Load last reset info from RCC / backup registers
        Init_Last_Reset_Info();
    }

    app_setup_and_loop();
}

void application_task_start(void* arg) {
    application_start();
}

/**
 * Called from startup_stm32f2xx.s at boot, main entry point.
 */
int main(void) {
    init_malloc_mutex();
    xTaskCreate( application_task_start, "app_thread", APPLICATION_STACK_SIZE/sizeof( portSTACK_TYPE ), NULL, 2, &app_thread_handle);

    vTaskStartScheduler();

    /* we should never get here */
    while (1);

    return 0;
}

static int Write_Feature_Flag(uint32_t flag, bool value, bool *prev_value) {
    if (HAL_IsISR()) {
        return -1; // DCT cannot be accessed from an ISR
    }
    uint32_t flags = 0;
    int result = dct_read_app_data_copy(DCT_FEATURE_FLAGS_OFFSET, &flags, sizeof(flags));
    if (result != 0) {
        return result;
    }
    // NOTE: inverted logic!
    const bool cur_value = !(flags & flag);
    if (prev_value) {
        *prev_value = cur_value;
    }
    if (cur_value != value) {
        if (value) {
            flags &= ~flag;
        } else {
            flags |= flag;
        }
        result = dct_write_app_data(&flags, DCT_FEATURE_FLAGS_OFFSET, 4);
        if (result != 0) {
            return result;
        }
    }
    return 0;
}

static int Read_Feature_Flag(uint32_t flag, bool* value) {
    if (HAL_IsISR()) {
        return -1; // DCT cannot be accessed from an ISR
    }
    uint32_t flags = 0;
    const int result = dct_read_app_data_copy(DCT_FEATURE_FLAGS_OFFSET, &flags, sizeof(flags));
    if (result != 0) {
        return result;
    }
    *value = !(flags & flag);
    return 0;
}

int HAL_Feature_Set(HAL_Feature feature, bool enabled) {
   switch (feature) {
        case FEATURE_RETAINED_MEMORY: {
            // TODO: Switch on backup SRAM clock
            // Switch on backup power regulator, so that it survives the deep sleep mode,
            // software and hardware reset. Power must be supplied to VIN or VBAT to retain SRAM values.
            return -1;
        }
        case FEATURE_RESET_INFO: {
            return Write_Feature_Flag(FEATURE_FLAG_RESET_INFO, enabled, NULL);
        }
#if HAL_PLATFORM_CLOUD_UDP
        case FEATURE_CLOUD_UDP: {
            const uint8_t data = (enabled ? 0xff : 0x00);
            return dct_write_app_data(&data, DCT_CLOUD_TRANSPORT_OFFSET, sizeof(data));
        }
#endif // HAL_PLATFORM_CLOUD_UDP
        case FEATURE_ETHERNET_DETECTION: {
            return Write_Feature_Flag(FEATURE_FLAG_ETHERNET_DETECTION, enabled, NULL);
        }
    }

    return -1;
}

bool HAL_Feature_Get(HAL_Feature feature) {
    switch (feature) {
        case FEATURE_CLOUD_UDP: {
            return true; // Mesh platforms are UDP-only
        }
        case FEATURE_RESET_INFO: {
            return true;
        }
        case FEATURE_ETHERNET_DETECTION: {
            bool value = false;
            return (Read_Feature_Flag(FEATURE_FLAG_ETHERNET_DETECTION, &value) == 0) ? value : false;
        }
    }
    return false;
}

int32_t HAL_Core_Backup_Register(uint32_t BKP_DR) {
    if ((BKP_DR == 0) || (BKP_DR > BACKUP_REGISTER_NUM)) {
        return -1;
    }

    return BKP_DR - 1;
}

void HAL_Core_Write_Backup_Register(uint32_t BKP_DR, uint32_t Data) {
    int32_t BKP_DR_Index = HAL_Core_Backup_Register(BKP_DR);
    if (BKP_DR_Index != -1) {
        backup_register[BKP_DR_Index] = Data;
    }
}

uint32_t HAL_Core_Read_Backup_Register(uint32_t BKP_DR) {
    int32_t BKP_DR_Index = HAL_Core_Backup_Register(BKP_DR);
    if (BKP_DR_Index != -1) {
        return backup_register[BKP_DR_Index];
    }
    return 0xFFFFFFFF;
}

void HAL_Core_Button_Mirror_Pin_Disable(uint8_t bootloader, uint8_t button, void* reserved) {

}

void HAL_Core_Button_Mirror_Pin(uint16_t pin, InterruptMode mode, uint8_t bootloader, uint8_t button, void *reserved) {

}

void HAL_Core_Led_Mirror_Pin_Disable(uint8_t led, uint8_t bootloader, void* reserved) {

}

void HAL_Core_Led_Mirror_Pin(uint8_t led, pin_t pin, uint32_t flags, uint8_t bootloader, void* reserved) {

}

extern size_t pvPortLargestFreeBlock();

uint32_t HAL_Core_Runtime_Info(runtime_info_t* info, void* reserved)
{
    struct mallinfo heapinfo = mallinfo();
    // fordblks  The total number of bytes in free blocks.
    info->freeheap = heapinfo.fordblks;
    if (offsetof(runtime_info_t, total_init_heap) + sizeof(info->total_init_heap) <= info->size) {
        info->total_init_heap = (uintptr_t)new_heap_end - (uintptr_t)&link_heap_location;
    }

    if (offsetof(runtime_info_t, total_heap) + sizeof(info->total_heap) <= info->size) {
        info->total_heap = heapinfo.arena;
    }

    if (offsetof(runtime_info_t, max_used_heap) + sizeof(info->max_used_heap) <= info->size) {
        info->max_used_heap = heapinfo.usmblks;
    }

    if (offsetof(runtime_info_t, user_static_ram) + sizeof(info->user_static_ram) <= info->size) {
        info->user_static_ram = (uintptr_t)&_Stack_Init - (uintptr_t)new_heap_end;
    }

    if (offsetof(runtime_info_t, largest_free_block_heap) + sizeof(info->largest_free_block_heap) <= info->size) {
    		info->largest_free_block_heap = pvPortLargestFreeBlock();
    }

    return 0;
}

uint16_t HAL_Bootloader_Get_Flag(BootloaderFlag flag)
{
    switch (flag)
    {
        case BOOTLOADER_FLAG_VERSION:
            return SYSTEM_FLAG(Bootloader_Version_SysFlag);
        case BOOTLOADER_FLAG_STARTUP_MODE:
            return SYSTEM_FLAG(StartupMode_SysFlag);
    }
    return 0;
}

int HAL_Core_Enter_Panic_Mode(void* reserved)
{
    __disable_irq();
    return 0;
}

#if HAL_PLATFORM_CLOUD_UDP

#include "dtls_session_persist.h"
#include <string.h>

SessionPersistDataOpaque session __attribute__((section(".backup_system")));

int HAL_System_Backup_Save(size_t offset, const void* buffer, size_t length, void* reserved)
{
	if (offset==0 && length==sizeof(SessionPersistDataOpaque))
	{
		memcpy(&session, buffer, length);
		return 0;
	}
	return -1;
}

int HAL_System_Backup_Restore(size_t offset, void* buffer, size_t max_length, size_t* length, void* reserved)
{
	if (offset==0 && max_length>=sizeof(SessionPersistDataOpaque) && session.size==sizeof(SessionPersistDataOpaque))
	{
		*length = sizeof(SessionPersistDataOpaque);
		memcpy(buffer, &session, sizeof(session));
		return 0;
	}
	return -1;
}


#else

int HAL_System_Backup_Save(size_t offset, const void* buffer, size_t length, void* reserved)
{
	return -1;
}

int HAL_System_Backup_Restore(size_t offset, void* buffer, size_t max_length, size_t* length, void* reserved)
{
	return -1;
}

#endif
