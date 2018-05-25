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
#include <nrf_mbr.h>
#include <nrf_sdm.h>
#include <nrf_sdh.h>

volatile uint8_t rtos_started = 0;

void HardFault_Handler( void ) __attribute__( ( naked ) );

void HardFault_Handler(void);
void MemManage_Handler(void);
void BusFault_Handler(void);
void UsageFault_Handler(void);

void SysTick_Handler(void);
void SysTickOverride(void);

void app_error_fault_handler(uint32_t id, uint32_t pc, uint32_t info);
void app_error_handler_bare(uint32_t err);

extern char link_interrupt_vectors_location;
extern char link_ram_interrupt_vectors_location;
extern char link_ram_interrupt_vectors_location_end;

__attribute__((externally_visible)) void prvGetRegistersFromStack( uint32_t *pulFaultStackAddress )
{
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
}
    else {
        PANIC(HardFault,"HardFault");

        /* Go to infinite loop when Hard Fault exception occurs */
        while (1)
        {
        }
    }
}


void HardFault_Handler(void)
{
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

void app_error_fault_handler(uint32_t _id, uint32_t _pc, uint32_t _info)
{
    volatile uint32_t id = _id;
    volatile uint32_t pc = _pc;
    volatile uint32_t info = _info;
    (void)id; (void)pc; (void)info;
    PANIC(HardFault,"HardFault");
    while(1) {
    }
}

void app_error_handler_bare(uint32_t error_code)
{
    PANIC(HardFault,"HardFault");
    while(1) {
    }
}

void MemManage_Handler(void)
{
    /* Go to infinite loop when Memory Manage exception occurs */
    PANIC(MemManage,"MemManage");
    while (1)
    {
    }
}

void BusFault_Handler(void)
{
    /* Go to infinite loop when Bus Fault exception occurs */
    PANIC(BusFault,"BusFault");
    while (1)
    {
    }
}

void UsageFault_Handler(void)
{
    /* Go to infinite loop when Usage Fault exception occurs */
    PANIC(UsageFault,"UsageFault");
    while (1)
    {
    }
}

void SysTickOverride(void)
{
    System1MsTick();

    /* Handle short and generic tasks for the device HAL on 1ms ticks */
    // HAL_1Ms_Tick();

    HAL_SysTick_Handler();

    // HAL_System_Interrupt_Trigger(SysInterrupt_SysTick, NULL);
}

void SysTickChain()
{
    SysTick_Handler();
    SysTickOverride();
}

/**
 * Called by HAL_Core_Init() to pre-initialize any low level hardware before
 * the main loop runs.
 */
void HAL_Core_Init_finalize(void)
{
    uint32_t* isrs = (uint32_t*)&link_ram_interrupt_vectors_location;
    isrs[IRQN_TO_IDX(SysTick_IRQn)] = (uint32_t)SysTickChain;
}

void HAL_Core_Init(void)
{
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
void HAL_Core_Setup_override_interrupts(void)
{
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
}


/*******************************************************************************
 * Function Name  : HAL_Core_Config.
 * Description    : Called in startup routine, before calling C++ constructors.
 * Input          : None.
 * Output         : None.
 * Return         : None.
 *******************************************************************************/
void HAL_Core_Config(void)
{
    DECLARE_SYS_HEALTH(ENTERED_SparkCoreConfig);

#ifdef DFU_BUILD_ENABLE
    USE_SYSTEM_FLAGS = 1;
#endif

    /* Forward interrupts */
    memcpy(&link_ram_interrupt_vectors_location, &link_interrupt_vectors_location, &link_ram_interrupt_vectors_location_end-&link_ram_interrupt_vectors_location);
    uint32_t* isrs = (uint32_t*)&link_ram_interrupt_vectors_location;
    SCB->VTOR = (uint32_t)isrs;

    Set_System();

#ifdef DFU_BUILD_ENABLE
    Load_SystemFlags();
#endif

    // TODO: Use current LED theme
    LED_SetRGBColor(RGB_COLOR_WHITE);
    LED_On(LED_RGB);
}

void HAL_Core_Setup(void)
{
    HAL_Core_Setup_override_interrupts();
    /* DOES NOT DO ANYTHING
     * SysTick is enabled within FreeRTOS
     */
    HAL_Core_Config_systick_configuration();
}

bool HAL_Core_Mode_Button_Pressed(uint16_t pressedMillisDuration)
{
    return false;
}

void HAL_Core_Mode_Button_Reset(uint16_t button)
{
}

void HAL_Core_System_Reset(void)
{
}

void HAL_Core_System_Reset_Ex(int reason, uint32_t data, void *reserved)
{
}

void HAL_Core_Factory_Reset(void)
{
}

void HAL_Core_Enter_Safe_Mode(void* reserved)
{
}

void HAL_Core_Enter_Bootloader(bool persist)
{
}

void HAL_Core_Enter_Stop_Mode(uint16_t wakeUpPin, uint16_t edgeTriggerMode, long seconds)
{
}

int32_t HAL_Core_Enter_Stop_Mode_Ext(const uint16_t* pins, size_t pins_count, const InterruptMode* mode, size_t mode_count, long seconds, void* reserved)
{
    return -1;
}

void HAL_Core_Execute_Stop_Mode(void)
{
}

void HAL_Core_Enter_Standby_Mode(uint32_t seconds, uint32_t flags)
{
}

void HAL_Core_Execute_Standby_Mode(void)
{
}

int HAL_Core_Get_Last_Reset_Info(int *reason, uint32_t *data, void *reserved)
{
    return -1;
}

/**
 * @brief  Computes the 32-bit CRC of a given buffer of byte data.
 * @param  pBuffer: pointer to the buffer containing the data to be computed
 * @param  BufferSize: Size of the buffer to be computed
 * @retval 32-bit CRC
 */
uint32_t HAL_Core_Compute_CRC32(const uint8_t *pBuffer, uint32_t bufferSize)
{
    return 0;
}

// todo find a technique that allows accessor functions to be inlined while still keeping
// hardware independence.
bool HAL_watchdog_reset_flagged()
{
    return false;
}

void HAL_Notify_WDT()
{
}

uint16_t HAL_Core_Mode_Button_Pressed_Time()
{
    return 0;
}

void HAL_Bootloader_Lock(bool lock)
{
}


unsigned HAL_Core_System_Clock(HAL_SystemClock clock, void* reserved)
{
    return 1;
}


static TaskHandle_t  app_thread_handle;
#define APPLICATION_STACK_SIZE 6144

/**
 * The mutex to ensure only one thread manipulates the heap at a given time.
 */
xSemaphoreHandle malloc_mutex = 0;

static void init_malloc_mutex(void)
{
    malloc_mutex = xSemaphoreCreateRecursiveMutex();
}

void __malloc_lock(void* ptr)
{
    if (malloc_mutex)
        while (!xSemaphoreTakeRecursive(malloc_mutex, 0xFFFFFFFF)) {}
}

void __malloc_unlock(void* ptr)
{
    if (malloc_mutex)
        xSemaphoreGiveRecursive(malloc_mutex);
}

/**
 * The entrypoint to our application.
 * This should be called from the RTOS main thread once initialization has been
 * completed, constructors invoked and and HAL_Core_Config() has been called.
 */
void application_start()
{
    rtos_started = 1;

    // one the key is sent to the cloud, this can be removed, since the key is fetched in
    // Spark_Protocol_init(). This is just a temporary measure while the key still needs
    // to be fetched via DFU.

    HAL_Core_Setup();

    /*
    generate_key();

    if (HAL_Feature_Get(FEATURE_RESET_INFO))
    {
        // Load last reset info from RCC / backup registers
        Init_Last_Reset_Info();
    }
    */

    app_setup_and_loop();
}

void application_task_start(void* arg)
{
    application_start();
}

/**
 * Called from startup_stm32f2xx.s at boot, main entry point.
 */
int main(void)
{
    init_malloc_mutex();
    xTaskCreate( application_task_start, "app_thread", APPLICATION_STACK_SIZE/sizeof( portSTACK_TYPE ), NULL, 2, &app_thread_handle);

    vTaskStartScheduler();

    /* we should never get here */
    while (1);

    return 0;
}



int HAL_Feature_Set(HAL_Feature feature, bool enabled)
{
    return -1;
}

bool HAL_Feature_Get(HAL_Feature feature)
{
    return false;
}

int HAL_Set_System_Config(hal_system_config_t config_item, const void* data, unsigned length)
{
    return -1;
}

int32_t HAL_Core_Backup_Register(uint32_t BKP_DR)
{
    return -1;
}

void HAL_Core_Write_Backup_Register(uint32_t BKP_DR, uint32_t Data)
{
}

uint32_t HAL_Core_Read_Backup_Register(uint32_t BKP_DR)
{
    return 0xFFFFFFFF;
}

void HAL_Core_Button_Mirror_Pin_Disable(uint8_t bootloader, uint8_t button, void* reserved)
{
}

void HAL_Core_Button_Mirror_Pin(uint16_t pin, InterruptMode mode, uint8_t bootloader, uint8_t button, void *reserved)
{
}

void HAL_Core_Led_Mirror_Pin_Disable(uint8_t led, uint8_t bootloader, void* reserved)
{
}

void HAL_Core_Led_Mirror_Pin(uint8_t led, pin_t pin, uint32_t flags, uint8_t bootloader, void* reserved)
{
}

uint32_t HAL_Core_Runtime_Info(runtime_info_t* info, void* reserved)
{
    return -1;
}

uint16_t HAL_Bootloader_Get_Flag(BootloaderFlag flag)
{
    if (flag==BOOTLOADER_FLAG_STARTUP_MODE)
        return 0xFF;
    return 0xFFFF;
}