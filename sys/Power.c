#include "Driver.h"
#include "Power.tmh"



//
// Start Bluetooth communication here
// 
_Use_decl_annotations_
NTSTATUS
DsHidMini_EvtWdfDeviceSelfManagedIoInit(
	WDFDEVICE  Device
)
{
	NTSTATUS status = STATUS_SUCCESS;
	PDEVICE_CONTEXT pDevCtx;
	WDF_DEVICE_PROPERTY_DATA propertyData;
	WCHAR deviceAddress[13];
	UINT64 hostAddress = 0;
	size_t friendlyNameSize = 0;
	PWSTR friendlyName;
	WCHAR eventName[49];

	PAGED_CODE();

	FuncEntry(TRACE_POWER);


	pDevCtx = DeviceGetContext(Device);

	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeUsb)
	{
		//
		// Set friendly name
		// 

		friendlyName = WdfMemoryGetBuffer(
			pDevCtx->Connection.Usb.ProductString,
			&friendlyNameSize
		);
		
		WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Device_FriendlyName);
		propertyData.Flags |= PLUGPLAY_PROPERTY_PERSISTENT;
		propertyData.Lcid = LOCALE_NEUTRAL;

		status = WdfDeviceAssignProperty(
			Device,
			&propertyData,
			DEVPROP_TYPE_STRING,
			(ULONG)friendlyNameSize + sizeof(L'\0'),
			friendlyName
		);

		if (!NT_SUCCESS(status))
		{
			TraceError(
				TRACE_POWER,
				"Setting DEVPKEY_Device_FriendlyName failed with status %!STATUS!",
				status
			);
		}
		
		//
		// Set device address property
		// 
		
		if(swprintf_s(
			deviceAddress, 
			ARRAYSIZE(deviceAddress), 
			L"%02X%02X%02X%02X%02X%02X",
			pDevCtx->DeviceAddress.Address[0],
			pDevCtx->DeviceAddress.Address[1],
			pDevCtx->DeviceAddress.Address[2],
			pDevCtx->DeviceAddress.Address[3],
			pDevCtx->DeviceAddress.Address[4],
			pDevCtx->DeviceAddress.Address[5]
		) != -1)
		{
			WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Bluetooth_DeviceAddress);
			propertyData.Flags |= PLUGPLAY_PROPERTY_PERSISTENT;
			propertyData.Lcid = LOCALE_NEUTRAL;

			status = WdfDeviceAssignProperty(
				Device,
				&propertyData,
				DEVPROP_TYPE_STRING,
				ARRAYSIZE(deviceAddress) * sizeof(WCHAR),
				deviceAddress
			);

			if (!NT_SUCCESS(status))
			{
				TraceError(
					TRACE_POWER,
					"Setting DEVPKEY_Bluetooth_DeviceAddress failed with status %!STATUS!",
					status
				);
			}
		}

		//
		// Attempt automatic pairing
		// 
		if (!pDevCtx->Configuration.DisableAutoPairing)
		{
			//
			// Auto-pair to first found radio
			// 
			status = DsUsb_Ds3PairToFirstRadio(pDevCtx);

			if (!NT_SUCCESS(status))
			{
				TraceError(TRACE_POWER,
					"DsUsb_Ds3PairToFirstRadio failed with status %!STATUS!",
					status);
			}
		}
		else
		{
			TraceInformation(
				TRACE_POWER,
				"Auto-pairing disabled in device configuration");
		}

		//
		// Set host radio address property
		// 

		WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_BluetoothRadio_Address);
		propertyData.Flags |= PLUGPLAY_PROPERTY_PERSISTENT;
		propertyData.Lcid = LOCALE_NEUTRAL;

		hostAddress = (UINT64)(pDevCtx->HostAddress.Address[5]) |
			(UINT64)(pDevCtx->HostAddress.Address[4]) << 8 |
			(UINT64)(pDevCtx->HostAddress.Address[3]) << 16 |
			(UINT64)(pDevCtx->HostAddress.Address[2]) << 24 |
			(UINT64)(pDevCtx->HostAddress.Address[1]) << 32 |
			(UINT64)(pDevCtx->HostAddress.Address[0]) << 40;
		
		status = WdfDeviceAssignProperty(
			Device,
			&propertyData,
			DEVPROP_TYPE_UINT64,
			sizeof(UINT64),
			&hostAddress
		);

		if (!NT_SUCCESS(status))
		{
			TraceError(
				TRACE_POWER,
				"Setting DEVPKEY_BluetoothRadio_Address failed with status %!STATUS!",
				status
			);
		}

		status = STATUS_SUCCESS;
	}
	
	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeBth)
	{
		//
		// Send magic packet, starts input report sending
		// 
		DsBth_Ds3Init(pDevCtx);

#pragma region HID Interrupt Read

		status = WdfIoTargetFormatRequestForIoctl(
			pDevCtx->Connection.Bth.BthIoTarget,
			pDevCtx->Connection.Bth.HidInterrupt.ReadRequest,
			IOCTL_BTHPS3_HID_INTERRUPT_READ,
			NULL,
			NULL,
			pDevCtx->Connection.Bth.HidInterrupt.ReadMemory,
			NULL
		);
		if (!NT_SUCCESS(status))
		{
			TraceError(
				TRACE_POWER,
				"WdfIoTargetFormatRequestForIoctl failed with status %!STATUS!",
				status
			);
		}

		WdfRequestSetCompletionRoutine(
			pDevCtx->Connection.Bth.HidInterrupt.ReadRequest,
			DsBth_HidInterruptReadRequestCompletionRoutine,
			pDevCtx
		);

		if (WdfRequestSend(
			pDevCtx->Connection.Bth.HidInterrupt.ReadRequest,
			pDevCtx->Connection.Bth.BthIoTarget,
			WDF_NO_SEND_OPTIONS
		) == FALSE) {
			status = WdfRequestGetStatus(pDevCtx->Connection.Bth.HidInterrupt.ReadRequest);
		}
		if (!NT_SUCCESS(status))
		{
			TraceError(
				TRACE_POWER,
				"WdfRequestSend failed with status %!STATUS!",
				status
			);
		}

#pragma endregion

#pragma region HID Control Write

		//
		// Send initial output report
		// 
		status = DsBth_SendHidControlWriteRequestAsync(pDevCtx);

		//
		// Send preset output report (delayed)
		// 
		WdfTimerStart(
			pDevCtx->Connection.Bth.Timers.HidOutputReport,
			WDF_REL_TIMEOUT_IN_SEC(1)
		);

#pragma endregion
	}

	swprintf_s(
		eventName,
		ARRAYSIZE(eventName),
		L"Global\\DsHidMiniConfigHotReloadEvent%012llX",
		*(PULONGLONG)&pDevCtx->DeviceAddress
	);

	TraceVerbose(
		TRACE_POWER,
		"Configuration reload event name: %ls",
		eventName
	);

	TCHAR* szSD = TEXT("D:")        // Discretionary ACL.
		TEXT("(A;OICI;GA;;;BA)");   // Allow full control to administrators.
	TEXT("(A;OICI;GA;;;SY)");   // Allow full control to the local system.
	SECURITY_ATTRIBUTES sa;
	sa.nLength = sizeof(SECURITY_ATTRIBUTES);
	sa.bInheritHandle = FALSE;
	ConvertStringSecurityDescriptorToSecurityDescriptor(
		szSD, 
		SDDL_REVISION_1, 
		&((&sa)->lpSecurityDescriptor), 
		NULL
	);

	pDevCtx->ConfigurationReloadEvent = CreateEvent(
		&sa,
		FALSE,
		FALSE,
		eventName
	);

	if (pDevCtx->ConfigurationReloadEvent == NULL)
	{
		TraceError(
			TRACE_POWER,
			"Failed to create reload event"
		);
	}

	FuncExit(TRACE_POWER, "status=%!STATUS!", status);

	return status;
}

//
// Initialize USB communication here
// 
_Use_decl_annotations_
NTSTATUS
DsHidMini_EvtDevicePrepareHardware(
	WDFDEVICE  Device,
	WDFCMRESLIST  ResourcesRaw,
	WDFCMRESLIST  ResourcesTranslated
)
{
	NTSTATUS                                status = STATUS_SUCCESS;
	PDEVICE_CONTEXT                         pDevCtx;
	WDF_USB_DEVICE_SELECT_CONFIG_PARAMS     configParams;
	UCHAR                                   index;
	WDFUSBPIPE                              pipe;
	WDF_USB_PIPE_INFORMATION                pipeInfo;
	UCHAR									controlTransferBuffer[CONTROL_TRANSFER_BUFFER_LENGTH];
	WDF_DEVICE_PROPERTY_DATA				propertyData;
	ULONG									requiredSize = 0;
	DEVPROPTYPE								propertyType;

	UNREFERENCED_PARAMETER(ResourcesRaw);
	UNREFERENCED_PARAMETER(ResourcesTranslated);

	FuncEntry(TRACE_POWER);

	pDevCtx = DeviceGetContext(Device);

	//
	// Common properties
	// 

	WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_DsHidMini_HidDeviceMode);
	pDevCtx->Configuration.HidDeviceMode = DsHidMiniDeviceModeSixaxisCompatible;

	(void)WdfDeviceQueryPropertyEx(
		Device,
		&propertyData,
		sizeof(UCHAR),
		&pDevCtx->Configuration.HidDeviceMode,
		&requiredSize,
		&propertyType
	);

	TraceVerbose(TRACE_POWER, "[COM] HidDeviceMode: 0x%02X",
		pDevCtx->Configuration.HidDeviceMode);

	WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_DsHidMini_OutputReportTimerPeriodMs);
	pDevCtx->Configuration.OutputReportTimerPeriodMs = 250;

	(void)WdfDeviceQueryPropertyEx(
		Device,
		&propertyData,
		sizeof(ULONG),
		&pDevCtx->Configuration.OutputReportTimerPeriodMs,
		&requiredSize,
		&propertyType
	);

	TraceVerbose(TRACE_POWER, "[COM] OutputReportTimerPeriodMs: %d",
		pDevCtx->Configuration.OutputReportTimerPeriodMs);
	
	//
	// Initialize USB framework object
	// 
	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeUsb
		&& pDevCtx->Connection.Usb.UsbDevice == NULL)
	{
		status = WdfUsbTargetDeviceCreate(Device,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDevCtx->Connection.Usb.UsbDevice
		);

		if (!NT_SUCCESS(status)) {
			TraceError( 
				TRACE_POWER,
				"WdfUsbTargetDeviceCreate failed with status %!STATUS!", 
				status
			);
			return status;
		}
	}

	//
	// Grab pipes and meta information
	// 
	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeUsb)
	{
		WdfUsbTargetDeviceGetDeviceDescriptor(
			pDevCtx->Connection.Usb.UsbDevice,
			&pDevCtx->Connection.Usb.UsbDeviceDescriptor
		);

		pDevCtx->VendorId = pDevCtx->Connection.Usb.UsbDeviceDescriptor.idVendor;
		TraceVerbose(TRACE_POWER, "[USB] VID: 0x%04X", pDevCtx->VendorId);
		pDevCtx->ProductId = pDevCtx->Connection.Usb.UsbDeviceDescriptor.idProduct;
		TraceVerbose(TRACE_POWER, "[USB] PID: 0x%04X", pDevCtx->ProductId);
		
#pragma region USB Interface & Pipe settings

		WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&configParams);

		status = WdfUsbTargetDeviceSelectConfig(pDevCtx->Connection.Usb.UsbDevice,
			WDF_NO_OBJECT_ATTRIBUTES,
			&configParams
		);

		if (!NT_SUCCESS(status)) {
			TraceError( TRACE_POWER,
				"WdfUsbTargetDeviceSelectConfig failed %!STATUS!", status);
			return status;
		}

		pDevCtx->Connection.Usb.UsbInterface = configParams.Types.SingleInterface.ConfiguredUsbInterface;

		//
		// Grab product name to use as FriendlyName
		// 
		status = WdfUsbTargetDeviceAllocAndQueryString(
			pDevCtx->Connection.Usb.UsbDevice,
			WDF_NO_OBJECT_ATTRIBUTES,
			&pDevCtx->Connection.Usb.ProductString,
			NULL,
			pDevCtx->Connection.Usb.UsbDeviceDescriptor.iProduct,
			0x0409
		);
		if (!NT_SUCCESS(status))
		{
			TraceEvents(
				TRACE_LEVEL_WARNING,
				TRACE_DEVICE,
				"Requesting iProduct failed (status: 0x%x), device might not support this string",
				status
			);
		}

		//
		// Get pipe handles
		//
		for (index = 0; index < WdfUsbInterfaceGetNumConfiguredPipes(pDevCtx->Connection.Usb.UsbInterface); index++) {

			WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);

			pipe = WdfUsbInterfaceGetConfiguredPipe(
				pDevCtx->Connection.Usb.UsbInterface,
				index, //PipeIndex,
				&pipeInfo
			);
			//
			// Tell the framework that it's okay to read less than
			// MaximumPacketSize
			//
			WdfUsbTargetPipeSetNoMaximumPacketSizeCheck(pipe);

			if (WdfUsbPipeTypeInterrupt == pipeInfo.PipeType &&
				WdfUsbTargetPipeIsInEndpoint(pipe)) {
				TraceInformation(TRACE_POWER,
					"InterruptReadPipe is 0x%p\n", pipe);
				pDevCtx->Connection.Usb.InterruptInPipe = pipe;
			}
		}

		if (!pDevCtx->Connection.Usb.InterruptInPipe)
		{
			status = STATUS_INVALID_DEVICE_STATE;
			TraceError( TRACE_POWER,
				"Device is not configured properly %!STATUS!\n",
				status);

			return status;
		}

#pragma endregion

		status = DsUsbConfigContReaderForInterruptEndPoint(Device);

		if (NT_SUCCESS(status))
		{
			//
			// Request device MAC address
			// 
			status = USB_SendControlRequest(
				pDevCtx,
				BmRequestDeviceToHost,
				BmRequestClass,
				GetReport,
				Ds3FeatureDeviceAddress,
				0,
				controlTransferBuffer,
				CONTROL_TRANSFER_BUFFER_LENGTH);

			if (!NT_SUCCESS(status))
			{
				TraceError( TRACE_POWER,
					"Requesting device address failed with %!STATUS!", status);
				return status;
			}

			RtlCopyMemory(
				&pDevCtx->DeviceAddress,
				&controlTransferBuffer[4],
				sizeof(BD_ADDR));

			TraceEvents(TRACE_LEVEL_INFORMATION,
			            TRACE_POWER,
			            "Device address: %02X:%02X:%02X:%02X:%02X:%02X",
			            pDevCtx->DeviceAddress.Address[0],
			            pDevCtx->DeviceAddress.Address[1],
			            pDevCtx->DeviceAddress.Address[2],
			            pDevCtx->DeviceAddress.Address[3],
			            pDevCtx->DeviceAddress.Address[4],
			            pDevCtx->DeviceAddress.Address[5]
			);
			
			//
			// Request host BTH address
			// 
			status = USB_SendControlRequest(
				pDevCtx,
				BmRequestDeviceToHost,
				BmRequestClass,
				GetReport,
				Ds3FeatureHostAddress,
				0,
				controlTransferBuffer,
				CONTROL_TRANSFER_BUFFER_LENGTH);

			if (!NT_SUCCESS(status))
			{
				TraceError( TRACE_POWER,
					"Requesting host address failed with %!STATUS!", status);
				return status;
			}

			RtlCopyMemory(
				&pDevCtx->HostAddress,
				&controlTransferBuffer[2],
				sizeof(BD_ADDR));

			//
			// Send initial output report
			// 
			status = USB_SendControlRequest(
				pDevCtx,
				BmRequestHostToDevice,
				BmRequestClass,
				SetReport,
				USB_SETUP_VALUE(HidReportRequestTypeOutput, HidReportRequestIdOne),
				0,
				(PVOID)G_Ds3UsbHidOutputReport,
				DS3_USB_HID_OUTPUT_REPORT_SIZE);

			//
			// Copy output report for later modification and reuse
			// 
			if (!pDevCtx->Connection.Usb.OutputReport)
			{
				pDevCtx->Connection.Usb.OutputReport = malloc(DS3_USB_HID_OUTPUT_REPORT_SIZE);

				RtlCopyMemory(
					pDevCtx->Connection.Usb.OutputReport,
					G_Ds3UsbHidOutputReport,
					DS3_USB_HID_OUTPUT_REPORT_SIZE
				);
			}
		}
	}

	//
	// Grab device properties exposed via BthPS3
	// 
	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeBth)
	{
		WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Bluetooth_DeviceVID);

		status = WdfDeviceQueryPropertyEx(
			Device,
			&propertyData,
			sizeof(USHORT),
			&pDevCtx->VendorId,
			&requiredSize,
			&propertyType
		);
		if (!NT_SUCCESS(status))
		{
			TraceError(
				TRACE_POWER,
				"Requesting DEVPKEY_Bluetooth_DeviceVID failed with %!STATUS!", 
				status
			);
			return status;
		}

		TraceVerbose(TRACE_POWER, "[BTH] VID: 0x%04X", pDevCtx->VendorId);

		WDF_DEVICE_PROPERTY_DATA_INIT(&propertyData, &DEVPKEY_Bluetooth_DevicePID);

		status = WdfDeviceQueryPropertyEx(
			Device,
			&propertyData,
			sizeof(USHORT),
			&pDevCtx->ProductId,
			&requiredSize,
			&propertyType
		);
		if (!NT_SUCCESS(status))
		{
			TraceError(
				TRACE_POWER,
				"Requesting DEVPKEY_Bluetooth_DevicePID failed with %!STATUS!",
				status
			);
			return status;
		}

		TraceVerbose(TRACE_POWER, "[BTH] PID: 0x%04X", pDevCtx->ProductId);
	}

	FuncExit(TRACE_POWER, "status=%!STATUS!", status);

	return status;
}

//
// Power up
// 
NTSTATUS DsHidMini_EvtDeviceD0Entry(
	_In_ WDFDEVICE              Device,
	_In_ WDF_POWER_DEVICE_STATE PreviousState
)
{
	PDEVICE_CONTEXT         pDevCtx;
	NTSTATUS                status = STATUS_SUCCESS;
	BOOLEAN                 isTargetStarted;

	pDevCtx = DeviceGetContext(Device);
	isTargetStarted = FALSE;

	FuncEntry(TRACE_POWER);

	UNREFERENCED_PARAMETER(PreviousState);

	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeUsb)
	{
		do {
			//
			// Since continuous reader is configured for this interrupt-pipe, we must explicitly start
			// the I/O target to get the framework to post read requests.
			//
			status = WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(pDevCtx->Connection.Usb.InterruptInPipe));
			if (!NT_SUCCESS(status)) {
				TraceError(TRACE_POWER, "Failed to start interrupt read pipe %!STATUS!", status);
				break;
			}

			isTargetStarted = TRUE;
		} while (FALSE);

		if (!NT_SUCCESS(status)) {
			//
			// Failure in D0Entry will lead to device being removed. So let us stop the continuous
			// reader in preparation for the ensuing remove.
			//
			if (isTargetStarted) {
				WdfIoTargetStop(
					WdfUsbTargetPipeGetIoTarget(pDevCtx->Connection.Usb.InterruptInPipe),
					WdfIoTargetCancelSentIo
				);
			}
		}

		//
		// Instruct pad to send input reports
		// 
		status = DsUsb_Ds3Init(pDevCtx);

		if (!NT_SUCCESS(status))
		{
			TraceError( TRACE_POWER,
				"DsUsb_Ds3Init failed with status %!STATUS!",
				status);
		}
	}
	
	FuncExit(TRACE_POWER, "status=%!STATUS!", status);

	return status;
}

//
// Power down
// 
NTSTATUS DsHidMini_EvtDeviceD0Exit(
	_In_ WDFDEVICE              Device,
	_In_ WDF_POWER_DEVICE_STATE TargetState
)
{
	PDEVICE_CONTEXT pDevCtx;

	UNREFERENCED_PARAMETER(TargetState);

	FuncEntry(TRACE_POWER);

	pDevCtx = DeviceGetContext(Device);

	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeUsb)
	{
		WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(
			pDevCtx->Connection.Usb.InterruptInPipe),
			WdfIoTargetCancelSentIo
		);
	}

	if (pDevCtx->ConnectionType == DsDeviceConnectionTypeBth)
	{
		WdfTimerStop(pDevCtx->Connection.Bth.Timers.HidOutputReport, FALSE);
		WdfTimerStop(pDevCtx->Connection.Bth.Timers.HidControlConsume, FALSE);

		WdfIoTargetStop(pDevCtx->Connection.Bth.BthIoTarget, WdfIoTargetCancelSentIo);
	}

	FuncExitNoReturn(TRACE_POWER);

	return STATUS_SUCCESS;
}
