/*
 * CanInterface.cpp
 *
 *  Created on: 19 Sep 2018
 *      Author: David
 */

#include "CanInterface.h"

#if SUPPORT_CAN_EXPANSION

#include "CanMotion.h"
#include "CommandProcessor.h"
#include <CanMessageBuffer.h>
#include "Movement/DDA.h"
#include "Movement/DriveMovement.h"
#include "Movement/StepTimer.h"
#include <RTOSIface/RTOSIface.h>

extern "C"
{
	#include "mcan/mcan.h"
	#include "pmc/pmc.h"
}

const unsigned int NumCanBuffers = 40;

#define USE_BIT_RATE_SWITCH		0

//#define CAN_DEBUG

Mcan* const MCAN_MODULE = MCAN1;
constexpr IRQn MCanIRQn = MCAN1_INT0_IRQn;

static mcan_module mcan_instance;

static volatile uint32_t canStatus = 0;

enum class CanStatusBits : uint32_t
{
	receivedStandardFDMessage = 1,
	receivedExtendedFDMessage = 2,
	receivedStandardFDMessageInFIFO = 4,
	receivedStandardNormalMessageInFIFO0 = 8,
	receivedStandardFDMessageInFIFO0 = 16,
	receivedExtendedFDMessageInFIFO1 = 32,
	acknowledgeError = 0x10000,
	formatError = 0x20000,
	busOff = 0x40000
};

#ifdef CAN_DEBUG
static uint32_t GetAndClearStatusBits()
{
	const uint32_t st = canStatus;
	canStatus = 0;
	return st;
}
#endif

/* mcan_transfer_message_setting */
constexpr uint32_t TxBufferIndexEmergency = 0;
constexpr uint32_t TxBufferIndexTimeSync = 1;
constexpr uint32_t TxBufferIndexMotion = 2;
constexpr uint32_t TxBufferIndexRequest = 3;

/* mcan_receive_message_setting */
constexpr uint32_t RxFifoIndexBroadcast = 0;
constexpr uint32_t RxFifoIndexRequest = 0;
constexpr uint32_t RxFifoIndexResponse = 1;

constexpr uint32_t CanClockIntervalMillis = 10000; //20;

// CanSender management task
constexpr size_t CanSenderTaskStackWords = 400;
static Task<CanSenderTaskStackWords> canSenderTask;

constexpr size_t CanReceiverTaskStackWords = 1000;
static Task<CanReceiverTaskStackWords> canReceiverTask;

constexpr size_t CanClockTaskStackWords = 300;
static Task<CanSenderTaskStackWords> canClockTask;

static CanMessageBuffer * volatile pendingBuffers;
static CanMessageBuffer * volatile lastBuffer;			// only valid when pendingBuffers != nullptr

static TaskHandle taskWaitingOnFifo0 = nullptr;
static TaskHandle taskWaitingOnFifo1 = nullptr;

// MCAN module initialization.
static void configure_mcan()
{
	// Initialise the CAN hardware
	mcan_config config_mcan;
	mcan_get_config_defaults(&config_mcan);
	mcan_init(&mcan_instance, MCAN_MODULE, &config_mcan);
	mcan_enable_fd_mode(&mcan_instance);

	mcan_extended_message_filter_element et_filter;

	// Set up a filter to receive all request messages addressed to us in FIFO 0
	mcan_get_extended_message_filter_element_default(&et_filter);
	et_filter.F0.bit.EFID1 = (CanId::MasterAddress << CanId::DstAddressShift);
	et_filter.F0.bit.EFEC = MCAN_EXTENDED_MESSAGE_FILTER_ELEMENT_F0_EFEC_STF0M_Val;		// RxFifoIndexRequest
	et_filter.F1.bit.EFID2 = (CanId::BoardAddressMask << CanId::DstAddressShift) | CanId::ResponseBit;
	et_filter.F1.bit.EFT = 2;
	mcan_set_rx_extended_filter(&mcan_instance, &et_filter, 0);

	// Set up a filter to receive all broadcast messages also in FIFO 0 (does this include broadcasts that we send?)
	mcan_get_extended_message_filter_element_default(&et_filter);
	et_filter.F0.bit.EFID1 = CanId::BroadcastAddress << CanId::DstAddressShift;
	et_filter.F0.bit.EFEC = MCAN_EXTENDED_MESSAGE_FILTER_ELEMENT_F0_EFEC_STF0M_Val;		// RxFifoIndexBroadcast
	et_filter.F1.bit.EFID2 = (CanId::BoardAddressMask << CanId::DstAddressShift);
	et_filter.F1.bit.EFT = 2;
	mcan_set_rx_extended_filter(&mcan_instance, &et_filter, 1);

	// Set up a filter to receive response messages in FIFO 1
	mcan_get_extended_message_filter_element_default(&et_filter);
	et_filter.F0.bit.EFID1 = (CanId::MasterAddress << CanId::DstAddressShift) | CanId::ResponseBit;
	et_filter.F0.bit.EFEC = MCAN_EXTENDED_MESSAGE_FILTER_ELEMENT_F0_EFEC_STF1M_Val;		// RxFifoIndexResponse
	et_filter.F1.bit.EFID2 = (CanId::BoardAddressMask << CanId::DstAddressShift) | CanId::ResponseBit;
	et_filter.F1.bit.EFT = 2;
	mcan_set_rx_extended_filter(&mcan_instance, &et_filter, 2);

	mcan_enable_interrupt(&mcan_instance, (mcan_interrupt_source)(MCAN_FORMAT_ERROR | MCAN_ACKNOWLEDGE_ERROR | MCAN_BUS_OFF | MCAN_RX_FIFO_0_NEW_MESSAGE | MCAN_RX_FIFO_1_NEW_MESSAGE));
	NVIC_ClearPendingIRQ(MCanIRQn);
	NVIC_SetPriority(MCanIRQn, NvicPriorityMCan);
	NVIC_EnableIRQ(MCanIRQn);

	mcan_start(&mcan_instance);
}

extern "C" void CanSenderLoop(void *);
extern "C" void CanClockLoop(void *);
extern "C" void CanReceiverLoop(void *);

void CanInterface::Init()
{
	CanMessageBuffer::Init(NumCanBuffers);
	pendingBuffers = nullptr;

	ConfigurePin(APIN_CAN1_TX);
	ConfigurePin(APIN_CAN1_RX);
	pmc_enable_upll_clock();			// configure_mcan sets up PCLK5 to be the UPLL divided by something, so make sure the UPLL is running
	configure_mcan();

	CanMotion::Init();

	// Create the task that sends CAN messages
	canClockTask.Create(CanClockLoop, "CanClock", nullptr, TaskPriority::CanClockPriority);
	canSenderTask.Create(CanSenderLoop, "CanSender", nullptr, TaskPriority::CanSenderPriority);
	canReceiverTask.Create(CanReceiverLoop, "CanReceiver", nullptr, TaskPriority::CanReceiverPriority);
}

CanAddress CanInterface::GetCanAddress()
{
	return CanId::MasterAddress;
}

// Send extended CAN message in fd mode,
static status_code mcan_fd_send_ext_message(uint32_t id_value, const uint8_t *data, size_t dataLength, uint32_t whichTxBuffer)
{
	const uint32_t trigMask = (uint32_t)1 << whichTxBuffer;
	if ((mcan_instance.hw->MCAN_TXBRP & trigMask) != 0)
	{
		mcan_instance.hw->MCAN_TXBCR = trigMask;
		while ((mcan_instance.hw->MCAN_TXBRP & trigMask) != 0) { }
	}

	const uint32_t dlc = (dataLength <= 8) ? dataLength
							: (dataLength <= 24) ? ((dataLength + 3) >> 2) + 6
								: ((dataLength + 15) >> 4) + 11;
	mcan_tx_element tx_element;
	tx_element.T0.reg = MCAN_TX_ELEMENT_T0_EXTENDED_ID(id_value) | MCAN_TX_ELEMENT_T0_XTD;
	tx_element.T1.reg = MCAN_TX_ELEMENT_T1_DLC(dlc)
						| MCAN_TX_ELEMENT_T1_EFC
#if USE_BIT_RATE_SWITCH
						| MCAN_TX_ELEMENT_T1_BRS
#endif
						| MCAN_TX_ELEMENT_T1_FDF;

	memcpy(tx_element.data, data, dataLength);

	status_code rc = mcan_set_tx_buffer_element(&mcan_instance, &tx_element, whichTxBuffer);
	if (rc != STATUS_OK)
	{
		DEBUG_HERE;
	}
	else
	{
		rc = mcan_tx_transfer_request(&mcan_instance, trigMask);
		if (rc != STATUS_OK)
		{
			DEBUG_HERE;
		}
	}
	return rc;
}

// Interrupt handler for MCAN, including RX,TX,ERROR and so on processes
void MCAN1_INT0_Handler(void)
{
	const uint32_t status = mcan_read_interrupt_status(&mcan_instance);

	if (status & MCAN_RX_BUFFER_NEW_MESSAGE)
	{
#if 1
		// We don't enable this interrupt, so it should never happen
		mcan_clear_interrupt_status(&mcan_instance, MCAN_RX_BUFFER_NEW_MESSAGE);
#else
		mcan_clear_interrupt_status(&mcan_instance, MCAN_RX_BUFFER_NEW_MESSAGE);
		for (unsigned int i = 0; i < CONF_MCAN1_RX_BUFFER_NUM; i++)
		{
			if (mcan_rx_get_buffer_status(&mcan_instance, i))
			{
				const uint32_t rx_buffer_index = i;
				mcan_rx_clear_buffer_status(&mcan_instance, i);
				mcan_get_rx_buffer_element(&mcan_instance, &rx_element_buffer, rx_buffer_index);
				if (rx_element_buffer.R0.bit.XTD)
				{
					canStatus |= (uint32_t)CanStatusBits::receivedExtendedFDMessage;
				}
				else
				{
					canStatus |= (uint32_t)CanStatusBits::receivedStandardFDMessage;
				}
			}
		}
#endif
	}

	if (status & MCAN_RX_FIFO_0_NEW_MESSAGE)
	{
		mcan_clear_interrupt_status(&mcan_instance, MCAN_RX_FIFO_0_NEW_MESSAGE);
		if (taskWaitingOnFifo0 != nullptr)
		{
			TaskBase::GiveFromISR(taskWaitingOnFifo0);
		}
	}

	if (status & MCAN_RX_FIFO_1_NEW_MESSAGE)
	{
		mcan_clear_interrupt_status(&mcan_instance, MCAN_RX_FIFO_1_NEW_MESSAGE);
		if (taskWaitingOnFifo1 != nullptr)
		{
			TaskBase::GiveFromISR(taskWaitingOnFifo1);
		}
		canStatus |= (uint32_t)CanStatusBits::receivedExtendedFDMessageInFIFO1;
	}

	if ((status & MCAN_ACKNOWLEDGE_ERROR))
	{
		mcan_clear_interrupt_status(&mcan_instance, (mcan_interrupt_source)(MCAN_ACKNOWLEDGE_ERROR));
		canStatus |= (uint32_t)CanStatusBits::acknowledgeError;
	}

	if ((status & MCAN_FORMAT_ERROR))
	{
		mcan_clear_interrupt_status(&mcan_instance, (mcan_interrupt_source)(MCAN_FORMAT_ERROR));
		canStatus |= (uint32_t)CanStatusBits::formatError;
	}

	if (status & MCAN_BUS_OFF)
	{
		mcan_clear_interrupt_status(&mcan_instance, MCAN_BUS_OFF);
		mcan_stop(&mcan_instance);
		canStatus |= (uint32_t)CanStatusBits::busOff;
		configure_mcan();
	}
}

// -------------------- End of code adapted from Atmel quick start example ----------------------------------

static_assert(CONF_MCAN_ELEMENT_DATA_SIZE == sizeof(CanMessage), "Mismatched message sizes");

//TODO can we get rid of the CanSender task if we send movement messages via the Tx FIFO?
// This task picks up motion messages and sends them
extern "C" void CanSenderLoop(void *)
{
	for (;;)
	{
		TaskBase::Take(Mutex::TimeoutUnlimited);
		while (pendingBuffers != nullptr)
		{
			CanMessageBuffer *buf;
			{
				TaskCriticalSectionLocker lock;
				buf = pendingBuffers;
				pendingBuffers = buf->next;
			}

			// Send the message
			mcan_fd_send_ext_message(buf->id.GetWholeId(), reinterpret_cast<uint8_t*>(&(buf->msg)), buf->dataLength, TxBufferIndexMotion);

#ifdef CAN_DEBUG
			// Display a debug message too
			debugPrintf("CCCR %08" PRIx32 ", PSR %08" PRIx32 ", ECR %08" PRIx32 ", TXBRP %08" PRIx32 ", TXBTO %08" PRIx32 ", st %08" PRIx32 "\n",
						MCAN1->MCAN_CCCR, MCAN1->MCAN_PSR, MCAN1->MCAN_ECR, MCAN1->MCAN_TXBRP, MCAN1->MCAN_TXBTO, GetAndClearStatusBits());
			buf->msg.DebugPrint();
			delay(50);
			debugPrintf("CCCR %08" PRIx32 ", PSR %08" PRIx32 ", ECR %08" PRIx32 ", TXBRP %08" PRIx32 ", TXBTO %08" PRIx32 ", st %08" PRIx32 "\n",
						MCAN1->MCAN_CCCR, MCAN1->MCAN_PSR, MCAN1->MCAN_ECR, MCAN1->MCAN_TXBRP, MCAN1->MCAN_TXBTO, GetAndClearStatusBits());
#else
			delay(2);		// until we have the transmit fifo working, we need to delay to allow the message to be sent
#endif
			// Free the message buffer.
			CanMessageBuffer::Free(buf);
		}
	}
}

extern "C" void CanClockLoop(void *)
{
	uint32_t lastWakeTime = xTaskGetTickCount();

	for (;;)
	{
		CanMessageBuffer * buf = CanMessageBuffer::Allocate();
		if (buf != nullptr)
		{
			CanMessageTimeSync * const msg = buf->SetupBroadcastMessage<CanMessageTimeSync>(CanId::MasterAddress);
			msg->timeSent = StepTimer::GetInterruptClocks();
			mcan_fd_send_ext_message(buf->id.GetWholeId(), reinterpret_cast<uint8_t*>(&(buf->msg)), buf->dataLength, TxBufferIndexTimeSync);
			CanMessageBuffer::Free(buf);
		}
		// Delay until it is time again
		vTaskDelayUntil(&lastWakeTime, CanClockIntervalMillis);
	}
}

// Add a buffer to the end of the send queue
void CanInterface::SendMotion(CanMessageBuffer *buf)
{
	buf->next = nullptr;
	TaskCriticalSectionLocker lock;

	if (pendingBuffers == nullptr)
	{
		pendingBuffers = buf;
	}
	else
	{
		lastBuffer->next = buf;
	}
	lastBuffer = buf;
	canSenderTask.Give();
}

// Send a request to an expansion board and get the response
void CanInterface::SendRequest(CanMessageBuffer *buf)
{
	//TODO
}

// The CanReceiver task
extern "C" void CanReceiverLoop(void *)
{
	taskWaitingOnFifo0 = TaskBase::GetCallerTaskHandle();
	for (;;)
	{
		const uint32_t rxf0s = mcan_instance.hw->MCAN_RXF0S;						// get FIFO 0 status
		if (((rxf0s & MCAN_RXF0S_F0FL_Msk) >> MCAN_RXF0S_F0FL_Pos) != 0)			// if there are any messages
		{
			CanMessageBuffer *buf = CanMessageBuffer::Allocate();
			if (buf == nullptr)
			{
				delay(2);
			}
			else
			{
				const uint32_t getIndex = (rxf0s & MCAN_RXF0S_F0GI_Msk) >> MCAN_RXF0S_F0GI_Pos;
				mcan_rx_element_fifo_0 elem;
				mcan_get_rx_fifo_0_element(&mcan_instance, &elem, getIndex);		// copy the data (TODO use our own driver, avoid double copying)
				mcan_instance.hw->MCAN_RXF0A = getIndex;							// acknowledge it, release the FIFO entry

				if (elem.R0.bit.XTD == 1 && elem.R0.bit.RTR != 1)					// if extended address and not a remote frame
				{
					// Copy the message and accompanying data to a buffer
					buf->id.SetReceivedId(elem.R0.bit.ID);
					static constexpr uint8_t dlc2len[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
					buf->dataLength = dlc2len[elem.R1.bit.DLC];
					memcpy(buf->msg.raw, elem.data, buf->dataLength);

					CommandProcessor::ProcessReceivedMessage(buf);
				}
			}
		}
		else
		{
			TaskBase::Take();
		}
	}
}

#endif

// End
