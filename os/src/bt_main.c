/**
 *	This is the operating system main entry point, from where it will boot and load
 *	kernel modules.
 *
 *	Once all kernel modules are loaded and initialised, we shall look user application
 *	processes to be loaded.
 *
 **/

#include <bitthunder.h>
#include <mm/bt_heap.h>
#include <string.h>
#include <bt_kernel.h>
#include <lib/putc.h>

extern int main(int argc, char **argv);

struct _BT_OPAQUE_HANDLE {
	BT_HANDLE_HEADER h;
};

static void idle_task(void *pParam) {

	//BT_BOOL bState = BT_TRUE;
	BT_GpioSetDirection(7, BT_GPIO_DIR_OUTPUT);

	BT_TICK ticks = BT_kTickCount();
	BT_TICK ticks_a = ticks;

	while(1) {
		ticks_a = BT_kTickCount();
		BT_GpioSet(7, BT_TRUE);
		BT_kTaskDelayUntil(&ticks_a, 10);
		BT_GpioSet(7, BT_FALSE);
		BT_kTaskDelayUntil(&ticks_a, 50);
		BT_GpioSet(7, BT_TRUE);
		BT_kTaskDelayUntil(&ticks_a, 10);
		BT_GpioSet(7, BT_FALSE);

		BT_kTaskDelayUntil(&ticks, 1000);
	}

	BT_kTaskDelete(NULL);
}

/**
 *	@ Note: argc and argv may be used for kernel booting parameters at a later date.
 *
 *
 **/
int bt_main(int argc, char **argv) {

	BT_ERROR Error;

	// Get Machine Description.
	BT_MACHINE_DESCRIPTION *pMachine = BT_GetMachineDescription(&Error);
	if(pMachine->szpName) {
		Error = 0;
	}

	if (pMachine->pfnMachineInit) pMachine->pfnMachineInit(pMachine);


	const BT_INTEGRATED_DRIVER *pDriver = BT_GetIntegratedDriverByName(pMachine->pInterruptController->name);
	if(pDriver) {
		pDriver->pfnProbe(pMachine->pInterruptController, &Error);
	}

	BT_HANDLE hUart = NULL;

	if (pMachine->pBootLogger)
	{
		pDriver = BT_GetIntegratedDriverByName(pMachine->pBootLogger->name);
		if(pDriver) {
			hUart = pDriver->pfnProbe(pMachine->pBootLogger, &Error);
		}
	}

	BT_UART_CONFIG oConfig;
	BT_SetPowerState(hUart, BT_POWER_STATE_AWAKE);

	oConfig.eMode			= BT_UART_MODE_BUFFERED;
	oConfig.ucDataBits		= BT_UART_8_DATABITS;
	oConfig.ucStopBits		= BT_UART_ONE_STOP_BIT;
	oConfig.ucParity		= BT_UART_PARITY_NONE;
	oConfig.ulBaudrate		= 115200;
	oConfig.ulRxBufferSize	= 128;
	oConfig.ulTxBufferSize	= 128;

	BT_UartSetConfiguration(hUart, &oConfig);

	BT_UartEnable(hUart);

	BT_SetStandardHandle(hUart);

	BT_kPrint("%s (%s)", BT_VERSION_STRING, BT_VERSION_NAME);

	BT_kPrint("Start Loading kernel modules...");
	//BT_CharDeviceWrite(hUart, 0, strlen(string), (BT_u8 *)string);

	BT_kPrint("Enumerate integrated devices");


	// Go through the module initialisation routines!
	Error = BT_InitialiseKernelModules(hUart);


	Error = BT_ProbeIntegratedDevices(hUart);

	BT_kPrint("Enter user-mode, and start user-space application...");

	BT_kPrint("Relinquish control of the boot UART device...(Goodbye)");

	BT_SetStandardHandle(NULL);

	if (hUart)
	{
		BT_CloseHandle(hUart);
	}

	//int retval = main(argc, argv);

	BT_THREAD_CONFIG oThreadConfig = {
		.ulStackDepth 	= 128,
		.ulPriority		= 0,
	};

	BT_kTaskCreate(idle_task, "BT_IDLE", &oThreadConfig, &Error);

	oThreadConfig.ulStackDepth = 512;
	BT_kTaskCreate((BT_FN_TASK_ENTRY) main, "MAIN", &oThreadConfig, &Error);

	BT_kStartScheduler();
	//vTaskStartScheduler();

	// Write a debug message to the debugger port,
	//	The main application has quit on us!

	return BT_ERR_NONE;
}
