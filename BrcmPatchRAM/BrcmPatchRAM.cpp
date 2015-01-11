/*
 *  Released under "The GNU General Public License (GPL-2.0)"
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or (at your
 *  option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 *  or FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMessage.h>

#include <IOKit/usb/IOUSBInterface.h>

#include <libkern/zlib.h>

#include "Common.h"
#include "hci.h"
#include "BrcmPatchRAM.h"

OSDefineMetaClassAndStructors(BrcmPatchRAM, IOService)

/******************************************************************************
 * BrcmPatchRAM::probe - parse kernel extension Info.plist
 ******************************************************************************/
IOService* BrcmPatchRAM::probe(IOService *provider, SInt32 *probeScore)
{
    DEBUG_LOG("%s::probe\n", getName());
    
    IOLog("%s [%04x:%04x]: Version 0.8 starting.\n", getName(), mVendorId, mProductId);
    
    mDevice = OSDynamicCast(IOUSBDevice, provider);
    
    if (mDevice != NULL)
    {
        BrcmFirmwareStore* firmwareStore;
        
        mDevice->retain();
        
        if (mDevice->open(this))
        {
            mVendorId = mDevice->GetVendorID();
            mProductId = mDevice->GetProductID();
            
            // Print out additional device information
            printDeviceInfo();
            
            // Set device configuration to composite configuration index 0
            if (!setConfiguration(0))
                return false;
            
            // Obtain first interface
            mInterface = findInterface();
            
            if (mInterface != NULL)
            {
                mInterface->retain();
                mInterface->open(this);
                
                mInterruptPipe = findPipe(kUSBInterrupt, kUSBIn);
                mBulkPipe = findPipe(kUSBBulk, kUSBOut);
                
                if (mInterruptPipe != NULL && mBulkPipe != NULL)
                {
                    // getFirmwareVersion additionally re-synchronizes outstanding responses
                    UInt16 firmwareVersion = getFirmwareVersion();
                    
                    if (firmwareVersion > 0)
                        IOLog("BrcmPatchRAM: Current firmware version v%d, no upgrade required.\n", firmwareVersion);
                    
                    if (firmwareVersion == 0 && (firmwareStore = getFirmwareStore()) != NULL)
                    {
                        OSArray* instructions = firmwareStore->getFirmware(OSDynamicCast(OSString, getProperty("FirmwareKey")));
                        
                        if (instructions != NULL)
                        {
                            // Initiate firmware upgrade
                            hciCommand(&HCI_VSC_DOWNLOAD_MINIDRIVER, sizeof(HCI_VSC_DOWNLOAD_MINIDRIVER));
                            queueRead();
                            
                            // Wait for mini driver download
                            IOSleep(5);
                            
                            // Write firmware data to bulk pipe
                            OSCollectionIterator* iterator = OSCollectionIterator::withCollection(instructions);
                            
                            OSData* data;
                            while ((data = OSDynamicCast(OSData, iterator->getNextObject())))
                            {
                                bulkWrite((void *)data->getBytesNoCopy(), data->getLength());
                                queueRead();
                            }
                            
                            OSSafeRelease(iterator);
                            
                            hciCommand(&HCI_VSC_END_OF_RECORD, sizeof(HCI_VSC_END_OF_RECORD));
                            queueRead();
                            queueRead();
                            
                            IOSleep(100);
                            
                            //hciCommand(&HCI_VSC_WAKEUP, sizeof(HCI_VSC_WAKEUP));
                            //queueRead();
                            
                            hciCommandSync(&HCI_RESET, sizeof(HCI_RESET));
                            //queueRead();
                            
                            IOSleep(50);
                            
                            resetDevice();
                            
                            getDeviceStatus();
                            
                            IOLog("%s [%04x:%04x]: Firmware upgrade completed successfully.\n", getName(), mVendorId, mProductId);
                        }
                    }
                }
            }
            
            if (mInterruptPipe != NULL)
            {
                mInterruptPipe->Abort();
                mInterruptPipe->release();
            }
            
            if (mBulkPipe != NULL)
            {
                mBulkPipe->Abort();
                mBulkPipe->release();
            }
            
            if (mInterface != NULL)
            {
                mInterface->close(this);
                mInterface->release();
            }
       
            mDevice->close(this);
        }
        
        mDevice->release();
    }
    else
        IOLog("%s: Provider is not a USB device.\n", getName());
    
    return NULL;
}

BrcmFirmwareStore* BrcmPatchRAM::getFirmwareStore()
{
    BrcmFirmwareStore* firmwareStore = OSDynamicCast(BrcmFirmwareStore, getResourceService()->getProperty(kBrcmFirmwareStoreService));
    
    if (firmwareStore == NULL)
        IOLog("%s [%04x:%04x]: BrcmFirmwareStore does not appear to be available.\n", getName(), mVendorId, mProductId);
    
    return firmwareStore;
}

void BrcmPatchRAM::printDeviceInfo()
{
    char product[255];
    char manufacturer[255];
    char serial[255];
    
    // Retrieve device information
    mDevice->GetStringDescriptor(mDevice->GetProductStringIndex(), product, sizeof(product));
    mDevice->GetStringDescriptor(mDevice->GetManufacturerStringIndex(), manufacturer, sizeof(manufacturer));
    mDevice->GetStringDescriptor(mDevice->GetSerialNumberStringIndex(), serial, sizeof(serial));
    
    IOLog("%s [%04x:%04x]: USB [%s v%d] \"%s\" by \"%s\"\n",
          getName(),
          mVendorId,
          mProductId,
          serial,
          mDevice->GetDeviceRelease(),
          product,
          manufacturer);
}

int BrcmPatchRAM::getDeviceStatus()
{
    IOReturn result;
    USBStatus status;
    
    if ((result = mDevice->GetDeviceStatus(&status)) != kIOReturnSuccess)
    {
        IOLog("%s [%04x:%04x]: Unable to get device status (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        return 0;
    }
    else
        DEBUG_LOG("%s [%04x:%04x]: Device status 0x%08x.\n", getName(), mVendorId, mProductId, (int)status);
    
    return (int)status;
}

bool BrcmPatchRAM::resetDevice()
{
    IOReturn result;
    
    if ((result = mDevice->ResetDevice()) != kIOReturnSuccess)
    {
        IOLog("%s [%04x:%04x]: Failed to reset the device (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        return false;
    }
    else
        DEBUG_LOG("%s [%04x:%04x]: Device reset.\n", getName(), mVendorId, mProductId);
    
    return true;
}

bool BrcmPatchRAM::setConfiguration(int configurationIndex)
{
    IOReturn result;
    const IOUSBConfigurationDescriptor* configurationDescriptor;
    UInt8 currentConfiguration = 0xFF;
    
    // Find the first config/interface
    UInt8 numconf = 0;
    
    if ((numconf = mDevice->GetNumConfigurations()) < (configurationIndex + 1))
    {
        IOLog("%s [%04x:%04x]: Composite configuration index %d is not available, %d total composite configurations.\n",
              getName(), mVendorId, mProductId, configurationIndex, numconf);
        return false;
    }
    else
        DEBUG_LOG("%s [%04x:%04x]: Available composite configurations: %d.\n", getName(), mVendorId, mProductId, numconf);
    
    configurationDescriptor = mDevice->GetFullConfigurationDescriptor(configurationIndex);
    
    // Set the configuration to the requested configuration index
    if (!configurationDescriptor)
    {
        IOLog("%s [%04x:%04x]: No configuration descriptor for configuration index: %d.\n", getName(), mVendorId, mProductId, configurationIndex);
        return false;
    }
    
    if ((result = mDevice->GetConfiguration(&currentConfiguration)) != kIOReturnSuccess)
    {
        IOLog("%s [%04x:%04x]: Unable to retrieve active configuration (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        return false;
    }
    
    // Device is already configured
    if (currentConfiguration != 0)
    {
        DEBUG_LOG("%s [%04x:%04x]: Device configuration is already set to configuration index %d.\n", getName(),
                  mVendorId, mProductId, configurationIndex);
        return true;
    }
    
    // Set the configuration to the first configuration
    if ((result = mDevice->SetConfiguration(this, configurationDescriptor->bConfigurationValue, true)) != kIOReturnSuccess)
    {
        IOLog("%s [%04x:%04x]: Unable to (re-)configure device (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        mDevice->close(this);
        return false;
    }
    
    DEBUG_LOG("%s [%04x:%04x]: Set device configuration to configuration index %d successfully.\n", getName(),
              mVendorId, mProductId, configurationIndex);
    
    return true;
}

IOUSBInterface* BrcmPatchRAM::findInterface()
{
    IOUSBFindInterfaceRequest request;
    IOUSBInterface* interface = NULL;
    
    // Find the interface for bulk endpoint transfers
    request.bAlternateSetting  = kIOUSBFindInterfaceDontCare;
    request.bInterfaceClass    = kIOUSBFindInterfaceDontCare;
    request.bInterfaceSubClass = kIOUSBFindInterfaceDontCare;
    request.bInterfaceProtocol = kIOUSBFindInterfaceDontCare;
    
    if ((interface = mDevice->FindNextInterface(NULL, &request)))
    {
        DEBUG_LOG("%s [%04x:%04x]: Interface %d (class %02x, subclass %02x, protocol %02x) located.\n",
                  getName(),
                  mVendorId,
                  mProductId,
                  interface->GetInterfaceNumber(),
                  interface->GetInterfaceClass(),
                  interface->GetInterfaceSubClass(),
                  interface->GetInterfaceProtocol());
        
        return interface;
    }
    
    IOLog("%s [%04x:%04x]: No interface could be located.\n", getName(), mVendorId, mProductId);
    
    return NULL;
}

IOUSBPipe* BrcmPatchRAM::findPipe(UInt8 type, UInt8 direction)
{
    IOUSBFindEndpointRequest findEndpointRequest;
    
    findEndpointRequest.type = type;
    findEndpointRequest.direction = direction;
    
    IOUSBPipe* pipe = mInterface->FindNextPipe(NULL, &findEndpointRequest, true);
    
    if (pipe)
    {
        DEBUG_LOG("%s [%04x:%04x]: Located pipe at 0x%02x.\n", getName(), mVendorId, mProductId, pipe->GetEndpointDescriptor()->bEndpointAddress);
        return pipe;
    }
    else
        IOLog("%s [%04x:%04x]: Unable to locate pipe.\n", getName(), mVendorId, mProductId);
    
    return NULL;
}

IOReturn BrcmPatchRAM::queueRead()
{
    IOReturn result;
    
    IOBufferMemoryDescriptor* buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, 0x200);
    
    if (buffer != NULL)
    {
        if ((result = buffer->prepare()) == kIOReturnSuccess)
        {
            IOUSBCompletion completion =
            {
                .target = this,
                .action = interruptReadEntry,
                .parameter = buffer
            };
            
            mReadQueued = true;
            
            if ((result = mInterruptPipe->Read(buffer, 0, 0, buffer->getLength(), &completion)) != kIOReturnSuccess)
            {
                mReadQueued = false;
                IOLog("%s [%04x:%04x]: Error initiating read (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
            }
            else
            {
                // Wait until read is complete
                while (mReadQueued)
                    IOSleep(1);
            }
            
            if ((result = buffer->complete()) != kIOReturnSuccess)
             IOLog("%s [%04x:%04x]: Failed to complete queued read memory buffer (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        }
        else
            IOLog("%s [%04x:%04x]: Failed to prepare queued read memory buffer (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        
        buffer->release();
    }
    else
    {
        IOLog("%s [%04x:%04x]: Unable to allocate read buffer.\n", getName(), mVendorId, mProductId);
        result = kIOReturnNoMemory;
    }

    return result;
}

void BrcmPatchRAM::interruptReadEntry(void* target, void* parameter, IOReturn status, UInt32 bufferSizeRemaining)
{
    if (target != NULL)
        ((BrcmPatchRAM*)target)->interruptReadHandler(parameter, status, bufferSizeRemaining);
}

void BrcmPatchRAM::interruptReadHandler(void* parameter, IOReturn status, UInt32 bufferSizeRemaining)
{
    IOBufferMemoryDescriptor* buffer = (IOBufferMemoryDescriptor*)parameter;
    
    if (buffer == NULL)
    {
        IOLog("%s [%04x:%04x]: Queued read, buffer is NULL.\n", getName(), mVendorId, mProductId);
        return;
    }
    
    switch (status)
    {
        case kIOReturnOverrun:
            DEBUG_LOG("%s [%04x:%04x]: read - kIOReturnOverrun\n", getName(), mVendorId, mProductId);
            mInterruptPipe->ClearStall();
        case kIOReturnSuccess:
        {
            hciParseResponse(buffer->getBytesNoCopy(), buffer->getLength() - bufferSizeRemaining, NULL, NULL);
            break;
        }
        case kIOReturnNotResponding:
            DEBUG_LOG("%s [%04x:%04x]: read - kIOReturnNotResponding\n", getName(), mVendorId, mProductId);
            break;
        default:
            DEBUG_LOG("%s [%04x:%04x]: read - Other (\"%s\" 0x%08x)\n", getName(), mVendorId, mProductId, stringFromReturn(status), status);
            break;
    }
    
    mReadQueued = false;
}

IOReturn BrcmPatchRAM::hciCommand(void * command, UInt16 length)
{
    IOReturn result;
    
    IOUSBDevRequest request =
    {
        .bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBDevice),
        .bRequest = 0,
        .wValue = 0,
        .wIndex = 0,
        .wLength = length,
        .pData = command
    };
    
    if ((result = mInterface->DeviceRequest(&request)) != kIOReturnSuccess)
        IOLog("%s [%04x:%04x]: device request failed (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
   
    return result;
}

IOReturn BrcmPatchRAM::hciCommandSync(void* command, UInt16 length)
{
    return hciCommandSync(command, length, NULL, NULL);
}

IOReturn BrcmPatchRAM::hciCommandSync(void* command, UInt16 length, void* output, UInt8* outputLength)
{
    IOReturn result;
    
    if ((result = hciCommand(command, length)) == kIOReturnSuccess)
        result = interruptRead(output, outputLength);
  
    return result;
}

IOReturn BrcmPatchRAM::hciParseResponse(void* response, UInt16 length, void* output, UInt8* outputLength)
{
    HCI_RESPONSE* header = (HCI_RESPONSE*)response;
    
    switch (header->eventCode)
    {
        case HCI_EVENT_COMMAND_COMPLETE:
        {
            HCI_COMMAND_COMPLETE* event = (HCI_COMMAND_COMPLETE*)response;
            
            switch (event->opcode)
            {
                case HCI_OPCODE_READ_VERBOSE_CONFIG:
                    DEBUG_LOG("%s [%04x:%04x]: READ VERBOSE CONFIG complete (status: 0x%02x, length: %d bytes).\n",
                              getName(), mVendorId, mProductId, event->status, header->length);
                    
                    DEBUG_LOG("%s [%04x:%04x]: Firmware version: v%d.\n",
                              getName(),
                              mVendorId, mProductId, (*(UInt16*)(((char*)response) + 10)) + 4096);
                    break;
                case HCI_OPCODE_DOWNLOAD_MINIDRIVER:
                    DEBUG_LOG("%s [%04x:%04x]: DOWNLOAD MINIDRIVER complete (status: 0x%02x, length: %d bytes).\n",
                              getName(), mVendorId, mProductId, event->status, header->length);
                    break;
                case HCI_OPCODE_LAUNCH_RAM:
                    //DEBUG_LOG("%s [%04x:%04x]: LAUNCH RAM complete (status: 0x%02x, length: %d bytes).\n",
                    //          getName(), mVendorId, mProductId, event->status, header->length);
                    break;
                case HCI_OPCODE_END_OF_RECORD:
                    DEBUG_LOG("%s [%04x:%04x]: END OF RECORD complete (status: 0x%02x, length: %d bytes).\n",
                              getName(), mVendorId, mProductId, event->status, header->length);
                    break;
                case HCI_OPCODE_RESET:
                    DEBUG_LOG("%s [%04x:%04x]: RESET complete (status: 0x%02x, length: %d bytes).\n",
                              getName(), mVendorId, mProductId, event->status, header->length);
                    break;
                default:
                    DEBUG_LOG("%s [%04x:%04x]: Event COMMAND COMPLETE (opcode 0x%04x, status: 0x%02x, length: %d bytes).\n",
                              getName(), mVendorId, mProductId, event->opcode, event->status, header->length);
                    break;                    
            }
            
            if (output != NULL && outputLength != NULL)
            {
                bzero(output, *outputLength);
                
                // Return the received data
                if (*outputLength >= length)
                {
                    DEBUG_LOG("%s [%04x:%04x]: Returning output data %d bytes.\n", getName(), mVendorId, mProductId, length);
                    
                    *outputLength = length;
                    memcpy(output, response, length);
                }
                else
                    // Not enough buffer space for data
                    return kIOReturnMessageTooLarge;
            }
            break;
        }
        case HCI_EVENT_NUM_COMPLETED_PACKETS:
            DEBUG_LOG("%s [%04x:%04x]: Number of completed packets.\n", getName(), mVendorId, mProductId);
            break;
        case HCI_EVENT_CONN_COMPLETE:
            DEBUG_LOG("%s [%04x:%04x]: Connection complete event.\n", getName(), mVendorId, mProductId);
            break;
        case HCI_EVENT_LE_META:
            DEBUG_LOG("%s [%04x:%04x]: Low-Energy meta event.\n", getName(), mVendorId, mProductId);
            break;
        default:
            DEBUG_LOG("%s [%04x:%04x]: Unknown event code (0x%02x).\n", getName(), mVendorId, mProductId, header->eventCode);
            break;
    }
    
    return kIOReturnSuccess;
}

IOReturn BrcmPatchRAM::interruptRead()
{
    return interruptRead(NULL, NULL);
}

IOReturn BrcmPatchRAM::interruptRead(void* output, UInt8* length)
{
    IOReturn result;
    IOBufferMemoryDescriptor* buffer = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, 0, 0x200);
    
    IOSleep(10);
    
    if (buffer != NULL)
    {
        if ((result = buffer->prepare()) == kIOReturnSuccess)
        {
            IOByteCount reqCount = buffer->getLength();
            IOByteCount readCount;
            
            if ((result = mInterruptPipe->Read(buffer, 0, 0, reqCount, (IOUSBCompletion*)NULL, &readCount)) == kIOReturnSuccess)
                result = hciParseResponse(buffer->getBytesNoCopy(), readCount, output, length);
            else
                IOLog("%s [%04x:%04x]: Failed to read from interrupt pipe sychronously (\"%s\" 0x%08x).\n", getName(),
                      mVendorId, mProductId, stringFromReturn(result), result);
        }
        else
            IOLog("%s [%04x:%04x]: Failed to prepare interrupt read memory buffer (\"%s\" 0x%08x).\n", getName(),
                  mVendorId, mProductId, stringFromReturn(result), result);
        
        if ((result = buffer->complete()) != kIOReturnSuccess)
            IOLog("%s [%04x:%04x]: Failed to complete interrupt read memory buffer (\"%s\" 0x%08x).\n", getName(),
                  mVendorId, mProductId, stringFromReturn(result), result);
        
        buffer->release();
    }
    else
    {
        IOLog("%s [%04x:%04x]: Unable to allocate interrupt read buffer.\n", getName(), mVendorId, mProductId);
        result = kIOReturnNoMemory;
    }
  
    return result;
}

IOReturn BrcmPatchRAM::bulkWrite(void* data, UInt16 length)
{
    IOReturn result;
    IOMemoryDescriptor* buffer = IOMemoryDescriptor::withAddress(data, length, kIODirectionIn);
    
    if (buffer != NULL)
    {
        if ((result = buffer->prepare()) == kIOReturnSuccess)
        {
            if ((result = mBulkPipe->Write(buffer, 0, 0, buffer->getLength(), (IOUSBCompletion*)NULL)) == kIOReturnSuccess)
            {
                //DEBUG_LOG("%s: Wrote %d bytes to bulk pipe.\n", getName(), length);
            }
            else
                IOLog("%s [%04x:%04x]: Failed to write to bulk pipe (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        }
        else
           IOLog("%s [%04x:%04x]: Failed to prepare bulk write memory buffer (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        
        if ((result = buffer->complete()) != kIOReturnSuccess)
            IOLog("%s [%04x:%04x]: Failed to complete bulk write memory buffer (\"%s\" 0x%08x).\n", getName(), mVendorId, mProductId, stringFromReturn(result), result);
        
        buffer->release();
    }
    else
    {
        IOLog("%s [%04x:%04x]: Unable to allocate bulk write buffer.\n", getName(), mVendorId, mProductId);
        result = kIOReturnNoMemory;
    }
    
    return result;
}

UInt16 BrcmPatchRAM::getFirmwareVersion()
{
    char response[0x20];
    UInt8 length = sizeof(response);
    
    if (hciCommand(&HCI_VSC_READ_VERBOSE_CONFIG, sizeof(HCI_VSC_READ_VERBOSE_CONFIG)) == kIOReturnSuccess)
    {
        // There might be other outstanding events pending,
        // keep reading data until we find our matching response
        for (int i = 0; i < 100; i++)
        {
            if (interruptRead(response, &length) == kIOReturnSuccess)
            {
                HCI_RESPONSE* header = (HCI_RESPONSE*)response;
                
                if (header->eventCode == HCI_EVENT_COMMAND_COMPLETE)
                {
                    HCI_COMMAND_COMPLETE* event = (HCI_COMMAND_COMPLETE*)response;
                    
                    if (event->opcode == HCI_OPCODE_READ_VERBOSE_CONFIG)
                        return *(UInt16*)(((char*)response) + 10);
                }
            }
        }
    }

    return 0xFFFF;
}

const char* BrcmPatchRAM::stringFromReturn(IOReturn rtn)
{
    static const IONamedValue IOReturn_values[] = {
        {kIOReturnIsoTooOld,          "Isochronous I/O request for distant past"     },
        {kIOReturnIsoTooNew,          "Isochronous I/O request for distant future"   },
        {kIOReturnNotFound,           "Data was not found"                           },
        {kIOUSBUnknownPipeErr,        "Pipe ref not recognized"                      },
        {kIOUSBTooManyPipesErr,       "Too many pipes"                               },
        {kIOUSBNoAsyncPortErr,        "No async port"                                },
        {kIOUSBNotEnoughPowerErr,     "Not enough power for selected configuration"  },
        {kIOUSBEndpointNotFound,      "Endpoint not found"                           },
        {kIOUSBConfigNotFound,        "Configuration not found"                      },
        {kIOUSBTransactionTimeout,    "Transaction timed out"                        },
        {kIOUSBTransactionReturned,   "Transaction has been returned to the caller"  },
        {kIOUSBPipeStalled,           "Pipe has stalled, error needs to be cleared"  },
        {kIOUSBInterfaceNotFound,     "Interface reference not recognized"           },
        {kIOUSBLowLatencyBufferNotPreviouslyAllocated,
            "Attempted to user land low latency isoc calls w/out calling PrepareBuffer" },
        {kIOUSBLowLatencyFrameListNotPreviouslyAllocated,
            "Attempted to user land low latency isoc calls w/out calling PrepareBuffer" },
        {kIOUSBHighSpeedSplitError,   "Error on hi-speed bus doing split transaction"},
        {kIOUSBSyncRequestOnWLThread, "Synchronous USB request on workloop thread."  },
        {kIOUSBDeviceNotHighSpeed,    "The device is not a high speed device."       },
        {kIOUSBClearPipeStallNotRecursive,
            "IOUSBPipe::ClearPipeStall should not be called rescursively"               },
        {kIOUSBLinkErr,               "USB link error"                               },
        {kIOUSBNotSent2Err,           "Transaction not sent"                         },
        {kIOUSBNotSent1Err,           "Transaction not sent"                         },
        {kIOUSBNotEnoughPipesErr,     "Not enough pipes in interface"                },
        {kIOUSBBufferUnderrunErr,     "Buffer Underrun (Host hardware failure)"      },
        {kIOUSBBufferOverrunErr,      "Buffer Overrun (Host hardware failure"        },
        {kIOUSBReserved2Err,          "Reserved"                                     },
        {kIOUSBReserved1Err,          "Reserved"                                     },
        {kIOUSBWrongPIDErr,           "Pipe stall, Bad or wrong PID"                 },
        {kIOUSBPIDCheckErr,           "Pipe stall, PID CRC error"                    },
        {kIOUSBDataToggleErr,         "Pipe stall, Bad data toggle"                  },
        {kIOUSBBitstufErr,            "Pipe stall, bitstuffing"                      },
        {kIOUSBCRCErr,                "Pipe stall, bad CRC"                          },
        {0,                           NULL                                           }
    };
    
    const char* result = IOFindNameForValue(rtn, IOReturn_values);
    
    if (result)
        return result;
    
    return super::stringFromReturn(rtn);
}
