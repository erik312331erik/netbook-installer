/*
 * Copyright (c) 1998-2000 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 *
 * The contents of this file constitute Original Code as defined in and
 * are subject to the Apple Public Source License Version 1.1 (the
 * "License").  You may not use this file except in compliance with the
 * License.  Please obtain a copy of the License at
 * http://www.apple.com/publicsource and read it before using this file.
 *
 * This Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON-INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 *
 * @APPLE_LICENSE_HEADER_END@
 */

#include <IOKit/audio/IOAudioDebug.h>
#include <IOKit/audio/IOAudioEngineUserClient.h>
#include <IOKit/audio/IOAudioEngine.h>
#include <IOKit/audio/IOAudioTypes.h>
#include <IOKit/audio/IOAudioStream.h>
#include <IOKit/audio/IOAudioDebug.h>
#include <IOKit/audio/IOAudioDefines.h>

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/IOKitKeys.h>

#define super OSObject

class IOAudioClientBufferSet : public OSObject
{
    OSDeclareDefaultStructors(IOAudioClientBufferSet);

public:
    UInt32							bufferSetID;
    IOAudioEngineUserClient	*		userClient;
    IOAudioClientBuffer64	*		outputBufferList;
    IOAudioClientBuffer64	*		inputBufferList;
    IOAudioEnginePosition			nextOutputPosition;
    AbsoluteTime					outputTimeout;
    AbsoluteTime					sampleInterval;
    IOAudioClientBufferSet *		mNextBufferSet;
    thread_call_t					watchdogThreadCall;
    UInt32							generationCount;
    bool							timerPending;
    
    bool init(UInt32 setID, IOAudioEngineUserClient *client);
    void free();
    
#ifdef DEBUG
    void retain() const;
    void release() const;
#endif

    void resetNextOutputPosition();

    void allocateWatchdogTimer();
    void freeWatchdogTimer();
    
    void setWatchdogTimeout(AbsoluteTime *timeout);
    void cancelWatchdogTimer();

    static void watchdogTimerFired(IOAudioClientBufferSet *clientBufferSet, UInt32 generationCount);
};

OSDefineMetaClassAndStructors(IOAudioClientBufferSet, OSObject)

bool IOAudioClientBufferSet::init(UInt32 setID, IOAudioEngineUserClient *client)
{
    audioDebugIOLog(3, "IOAudioClientBufferSet[%p]::init(%lx, %p)", this, setID, client);

    if (!super::init()) {
        return false;
    }
    
    if (client == NULL) {
        return false;
    }
    
    bufferSetID = setID;
    client->retain();
    userClient = client;
    
    outputBufferList = NULL;
    inputBufferList = NULL;
    mNextBufferSet = NULL;
    watchdogThreadCall = NULL;
    generationCount = 0;
    timerPending = false;
    
    resetNextOutputPosition();
    
    return true;
}

void IOAudioClientBufferSet::free()
{
    audioDebugIOLog(3, "IOAudioClientBufferSet[%p]::free()", this);

    if (watchdogThreadCall) {
        freeWatchdogTimer();
    }
    
    if (userClient != NULL) {
        userClient->release();
        userClient = NULL;
    }
    
    super::free();
}

#ifdef DEBUG
void IOAudioClientBufferSet::retain() const
{
   super::retain();
}

void IOAudioClientBufferSet::release() const
{
    super::release();
}
#endif

void IOAudioClientBufferSet::resetNextOutputPosition()
{
    nextOutputPosition.fLoopCount = 0;
    nextOutputPosition.fSampleFrame = 0;
}

void IOAudioClientBufferSet::allocateWatchdogTimer()
{
    audioDebugIOLog(3, "IOAudioClientBufferSet[%p]::allocateWatchdogTimer()", this);

    if (watchdogThreadCall == NULL) {
        watchdogThreadCall = thread_call_allocate((thread_call_func_t)IOAudioClientBufferSet::watchdogTimerFired, (thread_call_param_t)this);
    }
}

void IOAudioClientBufferSet::freeWatchdogTimer()
{
    audioDebugIOLog(3, "IOAudioClientBufferSet[%p]::freeWatchdogTimer()", this);

    if (watchdogThreadCall != NULL) {
        cancelWatchdogTimer();
        thread_call_free(watchdogThreadCall);
        watchdogThreadCall = NULL;
    }
}

void IOAudioClientBufferSet::setWatchdogTimeout(AbsoluteTime *timeout)
{
	bool				result;

    if (watchdogThreadCall == NULL) {
        // allocate it here
        IOLog("IOAudioClientBufferSet[%p]::setWatchdogTimeout() - no thread call.\n", this);
    }
    
    assert(watchdogThreadCall);
    
    outputTimeout = *timeout;
    
    generationCount++;
    
	userClient->lockBuffers();

		retain();
    
    timerPending = true;

    result = thread_call_enter1_delayed(watchdogThreadCall, (thread_call_param_t)generationCount, (*(uint64_t *)&outputTimeout));
	if (result) {
		release();		// canceled the previous call
	}

	userClient->unlockBuffers();
}

void IOAudioClientBufferSet::cancelWatchdogTimer()
{
    audioDebugIOLog(3, "IOAudioClientBufferSet[%p]::cancelWatchdogTimer()", this);

	if (NULL != userClient) {
		userClient->retain();
		userClient->lockBuffers();
		if (timerPending) {
			timerPending = false;
			if (thread_call_cancel(watchdogThreadCall))
				release();
		}
		userClient->unlockBuffers();
		userClient->release();
	}
}

void IOAudioClientBufferSet::watchdogTimerFired(IOAudioClientBufferSet *clientBufferSet, UInt32 generationCount)
{
    IOAudioEngineUserClient *userClient;

	assert(clientBufferSet);
    assert(clientBufferSet->userClient);

	if (clientBufferSet) {
#ifdef DEBUG
		AbsoluteTime now;
		clock_get_uptime((uint64_t *)&now);
		audioDebugIOLog(3, "IOAudioClientBufferSet[%p]::watchdogTimerFired(%ld):(%lx,%lx)(%lx,%lx)(%lx,%lx)", clientBufferSet, generationCount, now.hi, now.lo, clientBufferSet->outputTimeout.hi, clientBufferSet->outputTimeout.lo, clientBufferSet->nextOutputPosition.fLoopCount, clientBufferSet->nextOutputPosition.fSampleFrame);
#endif

		userClient = clientBufferSet->userClient;
		if (userClient) {
			userClient->retain();
			userClient->lockBuffers();
	
			if(clientBufferSet->timerPending != false) {
				userClient->performWatchdogOutput(clientBufferSet, generationCount);
			}
	
			clientBufferSet->release();

			userClient->unlockBuffers();
			userClient->release();
		}

		// clientBufferSet->release code was down here...
	} else {
		IOLog ("IOAudioClientBufferSet::watchdogTimerFired assert (clientBufferSet == NULL) failed\n");
	}
}

#undef super
#define super IOUserClient

OSDefineMetaClassAndStructors(IOAudioEngineUserClient, IOUserClient)

OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 0);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 1);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 2);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 3);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 4);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 5);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 6);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 7);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 8);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 9);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 10);
OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 11);


OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 12);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 13);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 14);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 15);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 16);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 17);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 18);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 19);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 20);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 21);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 22);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 23);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 24);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 25);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 26);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 27);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 28);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 29);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 30);
OSMetaClassDefineReservedUnused(IOAudioEngineUserClient, 31);

// New code added here

// OSMetaClassDefineReservedUsed(IOAudioEngineUserClient, 5);
bool IOAudioEngineUserClient::initWithAudioEngine(IOAudioEngine *engine, task_t task, void *securityToken, UInt32 type, OSDictionary* properties)
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::initWithAudioEngine(%p, 0x%lx, %p, 0x%lx, %p)", this, engine, (UInt32)task, securityToken, type, properties);
    
	// Declare Rosetta compatibility
	if (properties) {
		properties->setObject(kIOUserClientCrossEndianCompatibleKey, kOSBooleanTrue);
	}
	
    if (!initWithTask(task, securityToken, type, properties)) {
        return false;
    }

    if (!engine || !task) {
        return false;
    }

    clientTask = task;
    audioEngine = engine;
    
    setOnline(false);

    clientBufferSetList = NULL;
    
    clientBufferLock = IORecursiveLockAlloc();
    if (!clientBufferLock) {
        return false;
    }
    
    workLoop = audioEngine->getWorkLoop();
    if (!workLoop) {
        return false;
    }
    
    workLoop->retain();
    
    commandGate = IOCommandGate::commandGate(this);
    if (!commandGate) {
        return false;
    }
    
 	reserved = (ExpansionData *)IOMalloc (sizeof(struct ExpansionData));
	if (!reserved) {
		return false;
	}

	reserved->extendedInfo = NULL;
 	reserved->classicMode = 0;

	workLoop->addEventSource(commandGate);
    
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].object = this;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].func = (IOMethod) &IOAudioEngineUserClient::registerBuffer64;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].count0 = 4;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].count1 = 0;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].object = this;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].func = (IOMethod) &IOAudioEngineUserClient::unregisterBuffer64;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].count0 = 2;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].count1 = 0;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallGetConnectionID].object = this;
    reserved->methods[kIOAudioEngineCallGetConnectionID].func = (IOMethod) &IOAudioEngineUserClient::getConnectionID;
    reserved->methods[kIOAudioEngineCallGetConnectionID].count0 = 0;
    reserved->methods[kIOAudioEngineCallGetConnectionID].count1 = 1;
    reserved->methods[kIOAudioEngineCallGetConnectionID].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallStart].object = this;
    reserved->methods[kIOAudioEngineCallStart].func = (IOMethod) &IOAudioEngineUserClient::clientStart;
    reserved->methods[kIOAudioEngineCallStart].count0 = 0;
    reserved->methods[kIOAudioEngineCallStart].count1 = 0;
    reserved->methods[kIOAudioEngineCallStart].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallStop].object = this;
    reserved->methods[kIOAudioEngineCallStop].func = (IOMethod) &IOAudioEngineUserClient::clientStop;
    reserved->methods[kIOAudioEngineCallStop].count0 = 0;
    reserved->methods[kIOAudioEngineCallStop].count1 = 0;
    reserved->methods[kIOAudioEngineCallStop].flags = kIOUCScalarIScalarO;

    reserved->methods[kIOAudioEngineCallGetNearestStartTime].object = this;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].func = (IOMethod) &IOAudioEngineUserClient::getNearestStartTime;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].count0 = 3;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].count1 = 0;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].flags = kIOUCScalarIScalarO;

    trap.object = this;
    trap.func = (IOTrap) &IOAudioEngineUserClient::performClientIO;

    return true;
}

// Used so that a pointer to a kernel IOAudioStream isn't passed out of the kernel ( 32 bit version )
IOReturn IOAudioEngineUserClient::safeRegisterClientBuffer(UInt32 audioStreamIndex, void * sourceBuffer, UInt32 bufSizeInBytes, UInt32 bufferSetID) {

	audioDebugIOLog(3, "IOAudioEngineUserClient::safeRegisterClientBuffer deprecated for 32 bit %p ", sourceBuffer); 
	IOAudioStream *					audioStream;
	audioDebugIOLog(3, "IOAudioEngineUserClient::safeRegisterClientBuffer32 %p ", sourceBuffer); 
	
	audioStream = audioEngine->getStreamForID(audioStreamIndex);
	if (!audioStream) {
		audioDebugIOLog(3, "no stream associated with audioStreamIndex 0x%lx ", audioStreamIndex); 
		return kIOReturnBadArgument;
	}
	
	return registerClientBuffer(audioStream, sourceBuffer, bufSizeInBytes, bufferSetID);
	
}

// Used so that a pointer to a kernel IOAudioStream isn't passed out of the kernel ( 64 bit version ) <rdar://problems/5321701>
// New method added for 64 bit support <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::safeRegisterClientBuffer64(UInt32 audioStreamIndex, mach_vm_address_t * sourceBuffer, UInt32 bufSizeInBytes, UInt32 bufferSetID) 
{
	IOReturn retVal = kIOReturnBadArgument; 
	IOAudioStream *					audioStream;
	audioDebugIOLog(3, "+ IOAudioEngineUserClient::safeRegisterClientBuffer64 %p ", sourceBuffer); 
	
	audioStream = audioEngine->getStreamForID(audioStreamIndex);
	if (!audioStream) {
		audioDebugIOLog(3, "  no stream associated with audioStreamIndex 0x%lx ", audioStreamIndex); 
	}
	else
	{
		retVal = registerClientBuffer64(audioStream, * sourceBuffer, bufSizeInBytes, bufferSetID);
	}
	audioDebugIOLog(3, "- IOAudioEngineUserClient::safeRegisterClientBuffer64 " ); 
	return retVal;
}
// Used to pass extra information about how many samples are actually in a buffer and other things related to interesting non-mixable audio formats.
IOReturn IOAudioEngineUserClient::registerClientParameterBuffer (void  * paramBuffer, UInt32 bufferSetID)
{
	IOReturn						result = kIOReturnSuccess;
	IOAudioClientBufferSet			*bufferSet = NULL;
	IOAudioClientBufferExtendedInfo64 *extendedInfo;

    if (!isInactive()) {
        if (!paramBuffer || ((IOAudioStreamDataDescriptor *)paramBuffer)->fVersion > kStreamDataDescriptorCurrentVersion) {
            return kIOReturnBadArgument;
        }
        
        lockBuffers();		// added here because it was turned off in findBufferSet // MPC

		// this buffer set can't have already been registered with extended info
        extendedInfo = findExtendedInfo64 (bufferSetID);
		if (extendedInfo) 
		{
			unlockBuffers();
            return kIOReturnBadArgument;
		}

		// make sure that this buffer set has already been registered for output
        bufferSet = findBufferSet(bufferSetID);

		unlockBuffers();
		
        if (bufferSet) {
			IOAudioClientBufferExtendedInfo64 *info;
			
			extendedInfo = (IOAudioClientBufferExtendedInfo64*)IOMalloc (sizeof (IOAudioClientBufferExtendedInfo64));
			if (!extendedInfo) {
				return kIOReturnError;
			}

			// Can only be for output buffers, so always kIODirectionIn
			extendedInfo->mAudioClientBufferExtended32.paramBufferDescriptor = IOMemoryDescriptor::withAddressRange(* (mach_vm_address_t*)paramBuffer, (((IOAudioStreamDataDescriptor *)paramBuffer)->fNumberOfStreams * 4) + 8, kIODirectionIn, clientTask);
			if (!extendedInfo->mAudioClientBufferExtended32.paramBufferDescriptor) 
			{
				result = kIOReturnInternalError;
				goto Exit;
			}
			
			if ((result = extendedInfo->mAudioClientBufferExtended32.paramBufferDescriptor->prepare()) != kIOReturnSuccess) 
			{
				goto Exit;
			}
			
			extendedInfo->mAudioClientBufferExtended32.paramBufferMap = extendedInfo->mAudioClientBufferExtended32.paramBufferDescriptor->map();
			
			if (extendedInfo->mAudioClientBufferExtended32.paramBufferMap == NULL) 
			{
				IOLog("IOAudioEngineUserClient<0x%x>::registerClientParameterBuffer() - error mapping memory.\n", (unsigned int)this);
				result = kIOReturnVMError;
				goto Exit;
			}
			
			extendedInfo->mAudioClientBufferExtended32.paramBuffer = (void *)extendedInfo->mAudioClientBufferExtended32.paramBufferMap->getVirtualAddress();
			if (extendedInfo->mAudioClientBufferExtended32.paramBuffer == NULL)
			{
				result = kIOReturnVMError;
				goto Exit;
			}
	
			extendedInfo->mUnmappedParamBuffer64 = * (mach_vm_address_t*)paramBuffer;
			extendedInfo->mNextExtended64 = NULL;
			
			if (reserved->extendedInfo) 
			{
				// Get to the end of the linked list of extended info and add this new entry there
				info = reserved->extendedInfo;
				if (info)
				{
					while (info->mNextExtended64) 
					{
						info = info->mNextExtended64;
					}
	
					info->mNextExtended64 = extendedInfo;
				}
			} 
			else 
			{
				// The list is empty, so this the start of the list
				reserved->extendedInfo = extendedInfo;
			}
		}
     } 
	 else 
	 {
        result = kIOReturnNoDevice;
    }

Exit:
				 audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientParameterBuffer() - result = 0x%x\n", result);
	return result;
}

IOAudioClientBufferExtendedInfo *IOAudioEngineUserClient::findExtendedInfo(UInt32 bufferSetID)
{
	IOAudioClientBufferExtendedInfo64 *extendedInfo; // <rdar://problems/5321701>
    
IOAudioClientBufferExtendedInfo * retVal = NULL;
    extendedInfo = reserved->extendedInfo;
    while (extendedInfo && (extendedInfo->mAudioClientBufferExtended32.bufferSetID != bufferSetID)) 
	{
        extendedInfo = extendedInfo->mNextExtended64;
    }
    if ( extendedInfo)
	{
    	retVal = &(extendedInfo->mAudioClientBufferExtended32);
	}
	return retVal;
}

// New method added for 64 bit support <rdar://problems/5321701>
IOAudioClientBufferExtendedInfo64 *IOAudioEngineUserClient::findExtendedInfo64(UInt32 bufferSetID)
{
    IOAudioClientBufferExtendedInfo64 *extendedInfo; // <rdar://problems/5321701>
    
    extendedInfo = reserved->extendedInfo;
    while (extendedInfo && (extendedInfo->mAudioClientBufferExtended32.bufferSetID != bufferSetID)) {
        extendedInfo = extendedInfo->mNextExtended64;
    }
    
    return extendedInfo;
}
IOReturn IOAudioEngineUserClient::getNearestStartTime(IOAudioStream *audioStream, IOAudioTimeStamp *ioTimeStamp, UInt32 isInput)
{
    assert(commandGate);
    
    return commandGate->runAction(getNearestStartTimeAction, (void *)audioStream, (void *)ioTimeStamp, (void *)isInput);
}

IOReturn IOAudioEngineUserClient::getNearestStartTimeAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        
        if (userClient) {
            result = userClient->getClientNearestStartTime((IOAudioStream *)arg1, (IOAudioTimeStamp *)arg2, (UInt32)arg3);
        }
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::getClientNearestStartTime(IOAudioStream *audioStream, IOAudioTimeStamp *ioTimeStamp, UInt32 isInput)
{
    IOReturn result = kIOReturnSuccess;

    if (audioEngine && !isInactive()) {
		result = audioEngine->getNearestStartTime(audioStream, ioTimeStamp, isInput);
	}

	return result;
}

IOAudioEngineUserClient *IOAudioEngineUserClient::withAudioEngine(IOAudioEngine *engine, task_t clientTask, void *securityToken, UInt32 type, OSDictionary *properties)
{
    IOAudioEngineUserClient *client;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient::withAudioEngine(%p, 0x%lx, %p, 0x%lx, %p)", engine, (UInt32)clientTask, securityToken, type, properties);

    client = new IOAudioEngineUserClient;

    if (client) {
        if (!client->initWithAudioEngine(engine, clientTask, securityToken, type, properties)) {
            client->release();
            client = NULL;
        }
    }

    return client;
}

// Original code
IOAudioEngineUserClient *IOAudioEngineUserClient::withAudioEngine(IOAudioEngine *engine, task_t clientTask, void *securityToken, UInt32 type)
{
    IOAudioEngineUserClient *client;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient::withAudioEngine(%p, 0x%lx, %p, 0x%lx)", engine, (UInt32)clientTask, securityToken, type);

    client = new IOAudioEngineUserClient;

    if (client) {
        if (!client->initWithAudioEngine(engine, clientTask, securityToken, type)) {
            client->release();
            client = NULL;
        }
    }

    return client;
}

bool IOAudioEngineUserClient::initWithAudioEngine(IOAudioEngine *engine, task_t task, void *securityToken, UInt32 type)
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::initWithAudioEngine(%p, 0x%lx, %p, 0x%lx)", this, engine, (UInt32)task, securityToken, type);
    
    if (!initWithTask(task, securityToken, type)) {
        return false;
    }

    if (!engine || !task) {
        return false;
    }

    clientTask = task;
    audioEngine = engine;
    
    setOnline(false);

    clientBufferSetList = NULL;
    
    clientBufferLock = IORecursiveLockAlloc();
    if (!clientBufferLock) {
        return false;
    }
    
    workLoop = audioEngine->getWorkLoop();
    if (!workLoop) {
        return false;
    }
    
    workLoop->retain();
    
    commandGate = IOCommandGate::commandGate(this);
    if (!commandGate) {
        return false;
    }
    
 	reserved = (ExpansionData *)IOMalloc (sizeof(struct ExpansionData));
	if (!reserved) {
		return false;
	}

	reserved->extendedInfo = NULL;
 	reserved->classicMode = 0;

	workLoop->addEventSource(commandGate);
    
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].object = this;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].func = (IOMethod) &IOAudioEngineUserClient::registerBuffer64;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].count0 = 4;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].count1 = 0;
    reserved->methods[kIOAudioEngineCallRegisterClientBuffer].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].object = this;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].func = (IOMethod) &IOAudioEngineUserClient::unregisterBuffer64;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].count0 = 2;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].count1 = 0;
    reserved->methods[kIOAudioEngineCallUnregisterClientBuffer].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallGetConnectionID].object = this;
    reserved->methods[kIOAudioEngineCallGetConnectionID].func = (IOMethod) &IOAudioEngineUserClient::getConnectionID;
    reserved->methods[kIOAudioEngineCallGetConnectionID].count0 = 0;
    reserved->methods[kIOAudioEngineCallGetConnectionID].count1 = 1;
    reserved->methods[kIOAudioEngineCallGetConnectionID].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallStart].object = this;
    reserved->methods[kIOAudioEngineCallStart].func = (IOMethod) &IOAudioEngineUserClient::clientStart;
    reserved->methods[kIOAudioEngineCallStart].count0 = 0;
    reserved->methods[kIOAudioEngineCallStart].count1 = 0;
    reserved->methods[kIOAudioEngineCallStart].flags = kIOUCScalarIScalarO;
    
    reserved->methods[kIOAudioEngineCallStop].object = this;
    reserved->methods[kIOAudioEngineCallStop].func = (IOMethod) &IOAudioEngineUserClient::clientStop;
    reserved->methods[kIOAudioEngineCallStop].count0 = 0;
    reserved->methods[kIOAudioEngineCallStop].count1 = 0;
    reserved->methods[kIOAudioEngineCallStop].flags = kIOUCScalarIScalarO;

    reserved->methods[kIOAudioEngineCallGetNearestStartTime].object = this;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].func = (IOMethod) &IOAudioEngineUserClient::getNearestStartTime;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].count0 = 3;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].count1 = 0;
    reserved->methods[kIOAudioEngineCallGetNearestStartTime].flags = kIOUCScalarIScalarO;

    trap.object = this;
    trap.func = (IOTrap) &IOAudioEngineUserClient::performClientIO;

    return true;
}

void IOAudioEngineUserClient::free()
{
	IOAudioClientBufferExtendedInfo64 *			cur;
	IOAudioClientBufferExtendedInfo64 *			prev;

    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::free()", this);

    freeClientBufferSetList();			// Moved above clientBufferLock code below
    
    if (clientBufferLock) {
        IORecursiveLockFree(clientBufferLock);
        clientBufferLock = NULL;
    }
    
    if (notificationMessage) {
        IOFreeAligned(notificationMessage, sizeof(IOAudioNotificationMessage));
        notificationMessage = NULL;
    }
    
    if (commandGate) {
        if (workLoop) {
            workLoop->removeEventSource(commandGate);
        }
        
        commandGate->release();
        commandGate = NULL;
    }
    
    if (workLoop) {
        workLoop->release();
        workLoop = NULL;
    }
    
	if (reserved) {
		if (NULL != reserved->extendedInfo) {
			cur = reserved->extendedInfo;
			while (cur) {
				prev = cur;
				if (NULL != prev) {
					IOFree (prev, sizeof (IOAudioClientBufferExtendedInfo64));
				}
				cur = cur->mNextExtended64;
			}
		}
		IOFree (reserved, sizeof(struct ExpansionData));
	}

    super::free();
}

void IOAudioEngineUserClient::freeClientBufferSetList()
{
    while (clientBufferSetList) {
        IOAudioClientBufferSet *nextSet;
        
		// Move call up here to fix 3472373
        clientBufferSetList->cancelWatchdogTimer();

        while (clientBufferSetList->outputBufferList) {
            IOAudioClientBuffer64 *nextBuffer = clientBufferSetList->outputBufferList->mNextBuffer64;
            
            freeClientBuffer(clientBufferSetList->outputBufferList);
            
            clientBufferSetList->outputBufferList = nextBuffer;
        }

        while (clientBufferSetList->inputBufferList) {
            IOAudioClientBuffer64 *next = clientBufferSetList->inputBufferList->mNextBuffer64;
            
            freeClientBuffer(clientBufferSetList->inputBufferList);
            
            clientBufferSetList->inputBufferList = next;
        }
        
        nextSet = clientBufferSetList->mNextBufferSet;
        
        clientBufferSetList->release();
        
        clientBufferSetList = nextSet;
    }
    
}

void IOAudioEngineUserClient::freeClientBuffer(IOAudioClientBuffer64 *clientBuffer) 
{
    if (clientBuffer) {
        if (clientBuffer->mAudioClientBuffer32.audioStream) {
            clientBuffer->mAudioClientBuffer32.audioStream->removeClient(&(clientBuffer->mAudioClientBuffer32) ); 
            clientBuffer->mAudioClientBuffer32.audioStream->release();
			clientBuffer->mAudioClientBuffer32.audioStream = NULL;
        }
        
        if (clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor != NULL) {
            clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor->complete();
            clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor->release();
			clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor = NULL;
        }
        
        if (clientBuffer->mAudioClientBuffer32.sourceBufferMap != NULL) {
            clientBuffer->mAudioClientBuffer32.sourceBufferMap->release();
			clientBuffer->mAudioClientBuffer32.sourceBufferMap = NULL;
        }

        IOFreeAligned(clientBuffer, sizeof(IOAudioClientBuffer64));
		clientBuffer = NULL;
    }
}

void IOAudioEngineUserClient::stop(IOService *provider)
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::stop(%p)", this, provider);

    assert(commandGate);
    
    commandGate->runAction(stopClientAction);
    
    // We should be both inactive and offline at this point, 
    // so it is safe to free the client buffer set list without holding the lock
    
    freeClientBufferSetList();

    super::stop(provider);
}

IOReturn IOAudioEngineUserClient::clientClose()
{
    IOReturn result = kIOReturnSuccess;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::clientClose()", this);

    if (audioEngine && !isInactive()) {
        assert(commandGate);
            
        result = commandGate->runAction(closeClientAction);
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::clientDied()
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::clientDied()", this);

    return clientClose();
}

IOReturn IOAudioEngineUserClient::closeClientAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        if (userClient) {
            result = userClient->closeClient();
        }
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::closeClient()
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::closeClient()", this);

    if (audioEngine && !isInactive()) {
        if (isOnline()) {
            stopClient();
        }
        audioEngine->clientClosed(this);
        audioEngine = NULL;
    }
    
    return kIOReturnSuccess;
}

void IOAudioEngineUserClient::setOnline(bool newOnline)
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::setOnline(%d)", this, newOnline);

    if (online != newOnline) {
        online = newOnline;
        setProperty(kIOAudioEngineUserClientActiveKey, (unsigned long long)(online ? 1 : 0), sizeof(unsigned long long)*8);
    }
}

bool IOAudioEngineUserClient::isOnline()
{
    return online;
}

void IOAudioEngineUserClient::lockBuffers()
{
    assert(clientBufferLock);
    
    IORecursiveLockLock(clientBufferLock);
}

void IOAudioEngineUserClient::unlockBuffers()
{
    assert(clientBufferLock);
    
    IORecursiveLockUnlock(clientBufferLock);
}

IOReturn IOAudioEngineUserClient::clientMemoryForType(UInt32 type, UInt32 *flags, IOMemoryDescriptor **memory)
{
    IOReturn						result = kIOReturnSuccess;
	IOBufferMemoryDescriptor		*theMemoryDescriptor = NULL;

    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::clientMemoryForType(0x%lx, 0x%lx, %p)", this, type, *flags, memory);

	assert(audioEngine);

    switch(type) {
        case kIOAudioStatusBuffer:
			theMemoryDescriptor = audioEngine->getStatusDescriptor();
            break;
		case kIOAudioBytesInInputBuffer:
			theMemoryDescriptor = audioEngine->getBytesInInputBufferArrayDescriptor();
			break;
		case kIOAudioBytesInOutputBuffer:
			theMemoryDescriptor = audioEngine->getBytesInOutputBufferArrayDescriptor();
			break;
        default:
            result = kIOReturnUnsupported;
            break;
    }

	if (!result && theMemoryDescriptor) {
		theMemoryDescriptor->retain();		// Don't release it, it will be released by mach-port automatically
		*memory = theMemoryDescriptor;
		*flags = kIOMapReadOnly;
	} else {
		result = kIOReturnError;
	}

    return result;
}

IOExternalMethod *IOAudioEngineUserClient::getExternalMethodForIndex(UInt32 index)
{
    IOExternalMethod *method = 0;

    if (index < kIOAudioEngineNumCalls) {
        method = &reserved->methods[index];
    }

    return method;
}

IOExternalTrap *IOAudioEngineUserClient::getExternalTrapForIndex( UInt32 index )
{
	IOExternalTrap *result = NULL;
	
    if (index == kIOAudioEngineTrapPerformClientIO) {
		result = &trap;
	} else if (index == (0x1000 | kIOAudioEngineTrapPerformClientIO)) {
		reserved->classicMode = 1;
		result = &trap;
    }

    return result;
}

IOReturn IOAudioEngineUserClient::registerNotificationPort(mach_port_t port, UInt32 type, UInt32 refCon)
{
    IOReturn result = kIOReturnSuccess;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::registerNotificationPort(0x%lx, 0x%lx, 0x%lx)", this, (UInt32)port, type, refCon);

    switch (type) {
        case kIOAudioEngineAllNotifications:
            assert(commandGate);
            
            result = commandGate->runAction(registerNotificationAction, (void *)port, (void *)refCon);
            
            break;
        default:
            IOLog("IOAudioEngineUserClient[%p]::registerNotificationPort() - ERROR: invalid notification type specified - no notifications will be sent.\n", this);
            result = kIOReturnBadArgument;
            break;
    }
    // Create a single message, but keep a dict or something of all of the IOAudioStreams registered for
    // refCon is IOAudioStream *
    
    return result;
}

IOReturn IOAudioEngineUserClient::registerNotificationAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient::registerNotificationAction(%p, %p)", owner, arg1);

    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        
        if (userClient) {
            result = userClient->registerNotification((mach_port_t)arg1, (UInt32)arg2);
        }
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::registerNotification(mach_port_t port, UInt32 refCon)
{
    IOReturn result = kIOReturnSuccess;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::registerFormatNotification(0x%lx, 0x%lx)", this, (UInt32)port, refCon);

    if (!isInactive()) {
        if (port == MACH_PORT_NULL) {	// We need to remove this notification
            if (notificationMessage != NULL) {
                notificationMessage->messageHeader.msgh_remote_port = MACH_PORT_NULL;
            }
        } else {
            if (notificationMessage == NULL) {
                notificationMessage = (IOAudioNotificationMessage *)IOMallocAligned(sizeof(IOAudioNotificationMessage), sizeof (IOAudioNotificationMessage *));
                
                if (notificationMessage) {
                    notificationMessage->messageHeader.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
                    notificationMessage->messageHeader.msgh_size = sizeof(IOAudioNotificationMessage);
                    notificationMessage->messageHeader.msgh_local_port = MACH_PORT_NULL;
                    notificationMessage->messageHeader.msgh_reserved = 0;
                    notificationMessage->messageHeader.msgh_id = 0;
                    notificationMessage->messageHeader.msgh_remote_port = port;
                    notificationMessage->ref = refCon;              
                } else {
                    result = kIOReturnNoMemory;
                }
            } else {
            	notificationMessage->messageHeader.msgh_remote_port = port;
                notificationMessage->ref = refCon;         
            }
        }
    } else {
        result = kIOReturnNoDevice;
    }
    
    return result;
}


IOReturn IOAudioEngineUserClient::externalMethod ( uint32_t selector, IOExternalMethodArguments * arguments, 
	IOExternalMethodDispatch * dispatch, OSObject * target, void * reference)
{
	IOReturn result = kIOReturnBadArgument;
	audioDebugIOLog(3, "+ IOAudioEngineUserClient::externalMethod, selector=0x%x,   arg0 0x%llX, arg1 0x%llx, arg2 0x%llx arg3 0x%llx ", 
					selector, arguments->scalarInput[0], arguments->scalarInput[1], arguments->scalarInput[2], arguments->scalarInput[3]);
    audioDebugIOLog(3, " scalarInputCount=0x%x  structureInputSize 0x%x, scalarOutputCount 0x%x, structureOutputSize 0x%x ", 
					arguments->scalarInputCount, arguments->structureInputSize, arguments->scalarOutputCount, arguments->structureOutputSize );
	
	// Dispatch the method call
	switch (selector)
	{
	case kIOAudioEngineCallRegisterClientBuffer:
		if (arguments != 0)		
		{
			result = registerBuffer64((IOAudioStream *)arguments->scalarInput[0], (mach_vm_address_t)arguments->scalarInput[1], (UInt32)arguments->scalarInput[2], (UInt32)arguments->scalarInput[3] );
		}
		break;
	case kIOAudioEngineCallUnregisterClientBuffer:
		if (arguments != 0)		
		{
			result = unregisterBuffer64((mach_vm_address_t)arguments->scalarInput[0], (UInt32)arguments->scalarInput[1] );
		}
		break;	default:
		result = super::externalMethod(selector, arguments, dispatch, target, reference );
		break;
	}
	audioDebugIOLog(3, "- IOAudioEngineUserClient::externalMethod " );
	return result;
}

// 32 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::registerBuffer(IOAudioStream *audioStream, void * sourceBuffer, UInt32 bufSizeInBytes, UInt32 bufferSetID)
{
    assert(commandGate);

    audioDebugIOLog(3, "IOAudioEngineUserClient::registerBuffer Deprecated 0x%llx %p 0x%lx 0x%lx", (unsigned long long ) audioStream, sourceBuffer, bufSizeInBytes, bufferSetID); 

    return kIOReturnUnsupported;
}

// 64 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::registerBuffer64(IOAudioStream *audioStream, mach_vm_address_t sourceBuffer, UInt32 bufSizeInBytes, UInt32 bufferSetID)
{
    assert(commandGate);
	audioDebugIOLog(3, "IOAudioEngineUserClient::registerBuffer64 0x%llx 0x%llx 0x%lx 0x%lx", (unsigned long long ) audioStream, sourceBuffer, bufSizeInBytes, bufferSetID); 
	
    return commandGate->runAction(registerBufferAction, audioStream, &sourceBuffer, (void *)bufSizeInBytes, (void *)bufferSetID);
}

// 32 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::unregisterBuffer( void * sourceBuffer, UInt32 bufferSetID)
{
 	audioDebugIOLog(3, "IOAudioEngineUserClient::unregisterBuffer 32 bit version NOT SUPPORTED " ); 
    return kIOReturnUnsupported;
}

// 64 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::unregisterBuffer64( mach_vm_address_t  sourceBuffer, UInt32 bufferSetID)
{
    assert(commandGate);
    
    return commandGate->runAction(unregisterBufferAction, ( void * ) & sourceBuffer, (void *)bufferSetID);
}

IOReturn IOAudioEngineUserClient::registerBufferAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
        audioDebugIOLog(3, "IOAudioEngineUserClient::registerBufferAction %p ", arg1 ); 
   
    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        
        if (userClient) {
			result = userClient->safeRegisterClientBuffer64( (UInt32)arg1, ( mach_vm_address_t * ) arg2, (UInt32)arg3, (UInt32)arg4);
        }
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::unregisterBufferAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        
        if (userClient) {
            result = userClient->unregisterClientBuffer64( ( mach_vm_address_t * )arg1, (UInt32)arg2);
        }
    }
    
    return result;
}
// 32 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::registerClientBuffer(IOAudioStream *audioStream, void * sourceBuffer, UInt32 bufSizeInBytes, UInt32 bufferSetID)
{
	audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::registerClientBuffer  32 bit version Deprecated (%p[%ld], %p, 0x%lx, 0x%lx)", this, audioStream, audioStream->getStartingChannelID(), sourceBuffer, bufSizeInBytes, bufferSetID);
	return kIOReturnUnsupported;
}
// 64 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::registerClientBuffer64(IOAudioStream *audioStream, mach_vm_address_t  sourceBuffer, UInt32 bufSizeInBytes, UInt32 bufferSetID)
{
    IOReturn result = kIOReturnSuccess;
    IOAudioClientBuffer64 *clientBuffer;
    IODirection bufferDirection;
    const IOAudioStreamFormat *streamFormat;
   
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::registerClientBuffer64  (%p[%ld], 0x%llx, 0x%lx, 0x%lx)", this, audioStream, audioStream->getStartingChannelID(), sourceBuffer, bufSizeInBytes, bufferSetID);
    if (!isInactive()) 
	{
        IOAudioClientBufferSet *clientBufferSet;
        IOAudioClientBuffer64 **clientBufferList;
        
        if (!sourceBuffer || !audioStream || (bufSizeInBytes == 0) ) 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() bad argument");
           return kIOReturnBadArgument;
        }
        
		streamFormat = audioStream->getFormat();
        if (!streamFormat) 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() no format");
            return kIOReturnError;
        }
        
        // Return an error if this is an unmixable stream and it already has a client
        if (!streamFormat->fIsMixable && (audioStream->getNumClients() != 0)) 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() mix problem or client exists");
            return kIOReturnExclusiveAccess;
        }
        
        // allocate IOAudioClientBuffer to hold buffer descriptor, etc...
        clientBuffer = (IOAudioClientBuffer64 *)IOMallocAligned(sizeof(IOAudioClientBuffer64), sizeof (IOAudioClientBuffer64 *));
        if (!clientBuffer) 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() no clientbuffer");
	        result = kIOReturnNoMemory;
            goto Exit;
        }
		
		// make sure everthing is set to NULL [2851917]
		bzero(clientBuffer,sizeof(IOAudioClientBuffer64));
       
        clientBuffer->mAudioClientBuffer32.userClient = this;
        
        bufferDirection = audioStream->getDirection() == kIOAudioStreamDirectionOutput ? kIODirectionIn : kIODirectionOut;
        
        audioStream->retain();
        clientBuffer->mAudioClientBuffer32.audioStream = audioStream;

         clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor = IOMemoryDescriptor::withAddressRange((mach_vm_address_t)sourceBuffer, (mach_vm_size_t)bufSizeInBytes, kIODirectionNone, clientTask);
        if (!clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor) 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() no sourcebufferdescriptor");
			result = kIOReturnInternalError;
            goto Exit;
        }
        
        if ( result = clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor->prepare( kIODirectionOutIn ) != kIOReturnSuccess) 
		{
				audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() prepare error ");
				goto Exit;
	      }
        
        clientBuffer->mAudioClientBuffer32.sourceBufferMap = clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor->map();
        
		
        if (clientBuffer->mAudioClientBuffer32.sourceBufferMap == NULL) 
		{
            IOLog("IOAudioEngineUserClient<0x%x>::registerClientBuffer64() - error mapping memory.\n", (unsigned int)this);
            result = kIOReturnVMError;
            goto Exit;
        }
        
        clientBuffer->mAudioClientBuffer32.sourceBuffer = (void *)clientBuffer->mAudioClientBuffer32.sourceBufferMap->getVirtualAddress();
        if (clientBuffer->mAudioClientBuffer32.sourceBuffer == NULL) 
		{
            result = kIOReturnVMError;
            goto Exit;
        }
		// offset past per buffer info
        audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - clientBuffer->mAudioClientBuffer32.sourceBuffer before offset: %p, offset size: %ld", clientBuffer->mAudioClientBuffer32.sourceBuffer, offsetof(IOAudioBufferDataDescriptor, fData));
		clientBuffer->mAudioClientBuffer32.bufferDataDescriptor = (IOAudioBufferDataDescriptor *)(clientBuffer->mAudioClientBuffer32.sourceBuffer);
		clientBuffer->mAudioClientBuffer32.sourceBuffer = (UInt8 *)(clientBuffer->mAudioClientBuffer32.sourceBuffer) + offsetof(IOAudioBufferDataDescriptor, fData);
        audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - clientBuffer->mAudioClientBuffer32.sourceBuffer after offset: %p", clientBuffer->mAudioClientBuffer32.sourceBuffer);
	
		numSampleFrames = bufSizeInBytes;
		if (streamFormat->fIsMixable) {
			// If it's mixable the data is floats, so that's the size of each sample
			clientBuffer->mAudioClientBuffer32.numSampleFrames = bufSizeInBytes / (kIOAudioEngineDefaultMixBufferSampleSize * streamFormat->fNumChannels);
		} else {
			// If it's not mixable then the size is whatever the bitwidth is
			clientBuffer->mAudioClientBuffer32.numSampleFrames = bufSizeInBytes / ((streamFormat->fBitWidth / 8) * streamFormat->fNumChannels);
		}
        clientBuffer->mAudioClientBuffer32.numChannels = streamFormat->fNumChannels;
        clientBuffer->mUnmappedSourceBuffer64 = sourceBuffer;
		clientBuffer->mAudioClientBuffer32.unmappedSourceBuffer = (void *)sourceBuffer;
		clientBuffer->mNextBuffer64 = NULL;
        clientBuffer->mAudioClientBuffer32.mNextBuffer32 = NULL;
        clientBuffer->mAudioClientBuffer32.nextClip = NULL;
        clientBuffer->mAudioClientBuffer32.previousClip = NULL;
        clientBuffer->mAudioClientBuffer32.nextClient = NULL;
        
        lockBuffers();
        
        clientBufferSet = findBufferSet(bufferSetID);
        if (clientBufferSet == NULL) {
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - creating new IOAudioClientBufferSet " );
			clientBufferSet = new IOAudioClientBufferSet;

            if (clientBufferSet == NULL) {
                result = kIOReturnNoMemory;
                unlockBuffers();
                goto Exit;
            }
            
            if (!clientBufferSet->init(bufferSetID, this)) {
                result = kIOReturnError;
                unlockBuffers();
                goto Exit;
            }

            clientBufferSet->mNextBufferSet = clientBufferSetList;

            clientBufferSetList = clientBufferSet;
        }
        
        if (audioStream->getDirection() == kIOAudioStreamDirectionOutput) {
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - output " );
            clientBufferList = &clientBufferSet->outputBufferList;
            if (clientBufferSet->watchdogThreadCall == NULL) {
                clientBufferSet->allocateWatchdogTimer();
                if (clientBufferSet->watchdogThreadCall == NULL) {
                    result = kIOReturnNoMemory;
                    unlockBuffers();
                    goto Exit;
                }
            }
        } else {
 			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - input " );
            clientBufferList = &clientBufferSet->inputBufferList;
        }
        
        assert(clientBufferList);
        
        if (*clientBufferList == NULL) {
            *clientBufferList = clientBuffer;
        } else {
            IOAudioClientBuffer64 *clientBufPtr = *clientBufferList;
            while (clientBufPtr->mNextBuffer64 != NULL) {
                clientBufPtr = clientBufPtr->mNextBuffer64;
            }
			audioDebugIOLog(3, "  assigning  clientBufPtr->mAudioClientBuffer32.mNextBuffer32 %p ", &clientBuffer->mAudioClientBuffer32 );
            clientBufPtr->mNextBuffer64 = clientBuffer;			
			clientBufPtr->mAudioClientBuffer32.mNextBuffer32 = &clientBuffer->mAudioClientBuffer32;
        }
        
        unlockBuffers();
        
    Exit:
        
        if (result != kIOReturnSuccess) {
 			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - result (0x%x) != kIOReturnSuccess ", result );
           if (clientBuffer != NULL) {
                if (clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor != NULL) {
                    clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor->release();
					clientBuffer->mAudioClientBuffer32.sourceBufferDescriptor = NULL;
                }
                if (clientBuffer->mAudioClientBuffer32.sourceBufferMap != NULL) {
                    clientBuffer->mAudioClientBuffer32.sourceBufferMap->release();
					clientBuffer->mAudioClientBuffer32.sourceBufferMap = NULL;
                }
                if (clientBuffer->mAudioClientBuffer32.audioStream) {
                    clientBuffer->mAudioClientBuffer32.audioStream->release();
					clientBuffer->mAudioClientBuffer32.audioStream = NULL;
                }
                IOFreeAligned(clientBuffer, sizeof(IOAudioClientBuffer64));
				clientBuffer = NULL;
            }
        } else if (isOnline()) 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - isOnline adding client " );
			
            result = audioStream->addClient( &clientBuffer->mAudioClientBuffer32 ); 
        }
		else
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - !isOnline " );
		}
		
    } else {
		audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() - !isActive - no Device " );
        result = kIOReturnNoDevice;
    }
      audioDebugIOLog(3, "IOAudioEngineUserClient::registerClientBuffer64() result 0x%x", result);
   
    return result;
}
// 32 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::unregisterClientBuffer( void * sourceBuffer, UInt32 bufferSetID)
{
	IOReturn result = kIOReturnUnsupported;
	audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::unregisterClientBuffer NOT SUPPORTED for 32 bit buffer( %p, 0x%lx)", this, sourceBuffer, bufferSetID);
	return result;
}
// 64 bit version <rdar://problems/5321701>
IOReturn IOAudioEngineUserClient::unregisterClientBuffer64( mach_vm_address_t * sourceBuffer, UInt32 bufferSetID)
{
    IOReturn result = kIOReturnBadArgument;
    
    audioDebugIOLog(3, "+ IOAudioEngineUserClient[%p]::unregisterClientBuffer64(0x%p, 0x%lx)", this, sourceBuffer, bufferSetID);

    if (sourceBuffer) {
        IOAudioClientBufferSet *bufferSet;
        
        lockBuffers();
        
        bufferSet = findBufferSet(bufferSetID);
        
        if (bufferSet) {
            IOAudioClientBuffer64 *clientBuf = NULL, *previousBuf = NULL;
            IOAudioClientBuffer64 **clientBufferList = NULL;
            
            if (bufferSet->outputBufferList) 
			{
                clientBufferList = &bufferSet->outputBufferList;
				audioDebugIOLog(3, "  searching for sourceBuffer 0x%llx ", *sourceBuffer);
                clientBuf = bufferSet->outputBufferList;
                previousBuf = NULL;
				
                while (clientBuf && (clientBuf->mUnmappedSourceBuffer64 != *sourceBuffer)) 
				{
					audioDebugIOLog(3, "  checking against 0x%llx ", clientBuf->mUnmappedSourceBuffer64);
                   previousBuf = clientBuf;
                    clientBuf = clientBuf->mNextBuffer64;
                }
            }
			else
			{
				audioDebugIOLog(3, "  clientBuf for output not found ");
			}
            
            // If we didn't find the buffer in the output list, check the input list
            if (!clientBuf && bufferSet->inputBufferList) {
				audioDebugIOLog(3, "  checking input ");
				clientBufferList = &bufferSet->inputBufferList;
                clientBuf = bufferSet->inputBufferList;
                previousBuf = NULL;
                while (clientBuf && (clientBuf->mUnmappedSourceBuffer64 != *sourceBuffer)) {
                    previousBuf = clientBuf;
                    clientBuf = clientBuf->mNextBuffer64;
                }
            }

            if (clientBuf) {  
				
                assert(clientBuf->mUnmappedSourceBuffer64 == *sourceBuffer);
                
                if (previousBuf) {
                    previousBuf->mNextBuffer64 = clientBuf->mNextBuffer64;
                } else {
                    assert(clientBufferList);
                    *clientBufferList = clientBuf->mNextBuffer64;
                }
                
                if (bufferSet->outputBufferList == NULL) {
                    if (bufferSet->inputBufferList == NULL) {
                        removeBufferSet(bufferSet);
                    } else if (bufferSet->watchdogThreadCall != NULL) {
                        bufferSet->freeWatchdogTimer();
                    }
                }

                freeClientBuffer(clientBuf);		// Moved below above if statement
                
                result = kIOReturnSuccess;
            } else 
			{
				audioDebugIOLog(3, "  no clientbuffer found " );
				result = kIOReturnNotFound;
            }            
        } else 
		{
			audioDebugIOLog(3, "  no bufferSet found for id 0x%lx ", bufferSetID);
            result = kIOReturnNotFound;
        }
        
        unlockBuffers();
    }
    else
	{
	    audioDebugIOLog(3, "  no sourcebuffer " );	
	}
	audioDebugIOLog(3, "- IOAudioEngineUserClient::unregisterClientBuffer64 no sourcebuffer " );	
   return result;
}

IOAudioClientBufferSet *IOAudioEngineUserClient::findBufferSet(UInt32 bufferSetID)
{
    IOAudioClientBufferSet *bufferSet = NULL;
    
	if (0 == clientBufferSetList)
	{
 		audioDebugIOLog(3, "IOAudioEngineUserClient::findBufferSet null clientBufferSetList");
	}	
    bufferSet = clientBufferSetList;
    while (bufferSet && (bufferSet->bufferSetID != bufferSetID)) {
        bufferSet = bufferSet->mNextBufferSet;
    }
    if ( !bufferSet || ( bufferSet->bufferSetID != bufferSetID ) )
	{
		audioDebugIOLog(3, "IOAudioEngineUserClient::findBufferSet did not find clientBufferSetList for ID 0x%lx ", bufferSetID);
	}
    return bufferSet;
}

void IOAudioEngineUserClient::removeBufferSet(IOAudioClientBufferSet *bufferSet)
{
    IOAudioClientBufferSet *prevSet, *nextSet;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::removeBufferSet(%p)", this, bufferSet);
    
    lockBuffers();
    
    nextSet = clientBufferSetList;
    prevSet = NULL;
    while (nextSet && (nextSet != bufferSet)) {
        prevSet = nextSet;
        nextSet = nextSet->mNextBufferSet;
    }
    
    if (nextSet) {
        assert(nextSet == bufferSet);
        
        nextSet->cancelWatchdogTimer();
        
        if (prevSet) {
            prevSet->mNextBufferSet = nextSet->mNextBufferSet;
        } else {
            clientBufferSetList = nextSet->mNextBufferSet;
        }
        
        nextSet->release();
    }
    
    unlockBuffers();
}

IOReturn IOAudioEngineUserClient::performClientIO(UInt32 firstSampleFrame, UInt32 loopCount, bool inputIO, UInt32 bufferSetID, UInt32 sampleIntervalHi, UInt32 sampleIntervalLo)
{
    IOReturn result = kIOReturnSuccess;
    
    audioDebugIOLog(7, "+ IOAudioEngineUserClient[%p]::performClientIO(0x%lx, 0x%lx, %d, 0x%lx, 0x%lx, 0x%lx )", this, firstSampleFrame, loopCount, inputIO, bufferSetID, sampleIntervalHi, sampleIntervalLo ); 	
    assert(audioEngine);
    
    if (!isInactive()) 
	{
    
        lockBuffers();
        
        if (isOnline() && (audioEngine->getState() == kIOAudioEngineRunning)) 
		{
            if (firstSampleFrame < audioEngine->numSampleFramesPerBuffer) 
			{
                IOAudioClientBufferSet *bufferSet;
                
                bufferSet = findBufferSet(bufferSetID);
                if (bufferSet) 
				{
                
                    if (inputIO) 
					{
                        result = performClientInput(firstSampleFrame, bufferSet);
                    } else 
					{
                        result = performClientOutput(firstSampleFrame, loopCount, bufferSet, sampleIntervalHi, sampleIntervalLo);
                    }
                }
				else
				{
				audioDebugIOLog(3, "  no bufferset");
 				}
            } 
			else 
			{
				audioDebugIOLog(3, " firstSampleFrame ( 0x%lx) is out of range - 0x%lx frames per buffer.",  firstSampleFrame,  audioEngine->numSampleFramesPerBuffer);
                result = kIOReturnBadArgument;
            }
        } 
		else 
		{
			audioDebugIOLog(3, "IOAudioEngineUserClient::performClientIO OFFLINE");
 	        result = kIOReturnOffline;
        }
        
        unlockBuffers();
    } else 
	{
        result = kIOReturnNoDevice;
    }
    
	audioDebugIOLog(3, "- IOAudioEngineUserClient::performClientIO result = 0x%x", result);
    return result;
}

// model a SwapFloat32 after CF
inline uint32_t CFSwapInt32(uint32_t arg) {
#if defined(__i386__) && defined(__GNUC__)
    __asm__("bswap %0" : "+r" (arg));
    return arg;
#elif defined(__ppc__) && defined(__GNUC__)
    uint32_t result;
    __asm__("lwbrx %0,0,%1" : "=r" (result) : "r" (&arg), "m" (arg));
    return result;
#else
    uint32_t result;
    result = ((arg & 0xFF) << 24) | ((arg & 0xFF00) << 8) | ((arg >> 8) & 0xFF00) | ((arg >> 24) & 0xFF);
    return result;
#endif
}


void FlipFloats(void *p, long fcnt)
{
	UInt32 *ip = (UInt32 *)p;
	
	while (fcnt--) {
		*ip = CFSwapInt32(*ip);
		ip++;
	}
}

static inline IOAudioBufferDataDescriptor * FlipBufferDataDescriptor(IOAudioBufferDataDescriptor *in, IOAudioBufferDataDescriptor *tmp, UInt32 doFlip)
{	
	if (in && doFlip) {
		tmp->fActualDataByteSize = CFSwapInt32(in->fActualDataByteSize);
		tmp->fActualNumSampleFrames = CFSwapInt32(in->fActualNumSampleFrames);
		tmp->fTotalDataByteSize = CFSwapInt32(in->fTotalDataByteSize);
		tmp->fNominalDataByteSize = CFSwapInt32(in->fNominalDataByteSize);
		return tmp;
	}
	return in;
}

IOReturn IOAudioEngineUserClient::performClientOutput(UInt32 firstSampleFrame, UInt32 loopCount, IOAudioClientBufferSet *bufferSet, UInt32 sampleIntervalHi, UInt32 sampleIntervalLo)
{
    IOReturn result = kIOReturnSuccess;
#if 0
	IOAudioClientBufferExtendedInfo		*extendedInfo;
	IOAudioStreamDataDescriptor			*dataDescriptor;
#endif

	bufferSet->sampleInterval.hi = sampleIntervalHi;
    bufferSet->sampleInterval.lo = sampleIntervalLo;
    
    if (bufferSet->outputBufferList != NULL) {
        IOAudioEnginePosition			outputEndingPosition;
		IOAudioClientBuffer64			*clientBuf;
        UInt32							numSampleFrames, numSampleFramesPerBuffer;
        UInt32							clientIndex;
		
        assert(audioEngine != NULL);

		clientIndex = 0;
		
		clientBuf = bufferSet->outputBufferList;    

		IOAudioBufferDataDescriptor localBufferDataDescriptor;
		IOAudioBufferDataDescriptor * localBufferDataDescriptorPtr = FlipBufferDataDescriptor ( clientBuf->mAudioClientBuffer32.bufferDataDescriptor, &localBufferDataDescriptor, reserved->classicMode );

		if (NULL != localBufferDataDescriptorPtr) {
			audioDebugIOLog(6, "performClientOutput -------------%ld-----------------", clientIndex);
			audioDebugIOLog ( 6, "  actual frames = %ld, actual bytes = %ld, nominal bytes = %ld, total bytes = %ld",
								localBufferDataDescriptorPtr->fActualNumSampleFrames, 
								localBufferDataDescriptorPtr->fActualDataByteSize, 
								localBufferDataDescriptorPtr->fNominalDataByteSize, 
								localBufferDataDescriptorPtr->fTotalDataByteSize );
			numSampleFrames = localBufferDataDescriptorPtr->fActualNumSampleFrames;
		} else {
			audioDebugIOLog(6, "  no buffer descriptor found, using bufferSet->outputBufferList->numSampleFrames"); 
			numSampleFrames = bufferSet->outputBufferList->mAudioClientBuffer32.numSampleFrames;
		}

		numSampleFramesPerBuffer = audioEngine->getNumSampleFramesPerBuffer();
        
        outputEndingPosition.fLoopCount = loopCount;
        outputEndingPosition.fSampleFrame = firstSampleFrame + numSampleFrames;
        
        if (outputEndingPosition.fSampleFrame >= numSampleFramesPerBuffer) {
            outputEndingPosition.fSampleFrame -= numSampleFramesPerBuffer;
            outputEndingPosition.fLoopCount++;
        }
        
        // We only want to do output if we haven't already gone past the new samples
        // If the samples are late, the watchdog will already have skipped them
        if (CMP_IOAUDIOENGINEPOSITION(&outputEndingPosition, &bufferSet->nextOutputPosition) >= 0) 
		{
            IOAudioClientBuffer64 *clientBuf;
            AbsoluteTime outputTimeout;
            
 			audioDebugIOLog(6, "  CMP_IOAUDIOENGINEPOSITION >= 0 "); 
            clientBuf = bufferSet->outputBufferList;
            
            while (clientBuf) {
                IOAudioStream *					audioStream;
				UInt32							maxNumSampleFrames;
				IOReturn						tmpResult;
				
                audioStream = clientBuf->mAudioClientBuffer32.audioStream;
        
                assert(audioStream);
                assert(audioStream->getDirection() == kIOAudioStreamDirectionOutput);
                assert(clientBuf->mAudioClientBuffer32.sourceBuffer != NULL);
                
                audioStream->lockStreamForIO();
                
				maxNumSampleFrames = clientBuf->mAudioClientBuffer32.numSampleFrames;
				// get the per buffer info
				if (NULL != localBufferDataDescriptorPtr) {
					audioDebugIOLog ( 6, "  clientBuffer = %p: actual frames = %lu, actual bytes = %lu, nominal bytes = %lu, total bytes = %lu, source buffer size = %lu", 
											clientBuf, 
											localBufferDataDescriptorPtr->fActualNumSampleFrames, 
											localBufferDataDescriptorPtr->fActualDataByteSize, 
											localBufferDataDescriptorPtr->fNominalDataByteSize, 
											localBufferDataDescriptorPtr->fTotalDataByteSize, 
											clientBuf->mAudioClientBuffer32.sourceBufferDescriptor->getLength () - offsetof ( IOAudioBufferDataDescriptor, fData ) );

					clientBuf->mAudioClientBuffer32.numSampleFrames = numSampleFrames;
					
					if ((localBufferDataDescriptorPtr->fActualDataByteSize > (clientBuf->mAudioClientBuffer32.sourceBufferDescriptor->getLength () - offsetof(IOAudioBufferDataDescriptor, fData))) ||
						(localBufferDataDescriptorPtr->fActualDataByteSize > localBufferDataDescriptorPtr->fTotalDataByteSize) ||
						(localBufferDataDescriptorPtr->fNominalDataByteSize > localBufferDataDescriptorPtr->fTotalDataByteSize)) {
						audioDebugIOLog ( 1, "  **** VBR OUTPUT ERROR! clientBuffer = %p: actual frames = %ld, actual bytes = %ld, nominal bytes = %ld, total bytes = %ld, source buffer size = %ld", 
											clientBuf, 
											localBufferDataDescriptorPtr->fActualNumSampleFrames, 
											localBufferDataDescriptorPtr->fActualDataByteSize, 
											localBufferDataDescriptorPtr->fNominalDataByteSize, 
											localBufferDataDescriptorPtr->fTotalDataByteSize, 
											clientBuf->mAudioClientBuffer32.sourceBufferDescriptor->getLength () - offsetof(IOAudioBufferDataDescriptor, fData ) );
						audioStream->unlockStreamForIO();
						result = kIOReturnBadArgument;
						goto Exit;
					}	
#ifdef DEBUG					
					if (clientBuf->mAudioClientBuffer32.numSampleFrames != localBufferDataDescriptorPtr->fActualDataByteSize / (audioStream->format.fNumChannels * sizeof(float))) {
						audioDebugIOLog ( 6, "  DEBUGGING - calculated sample frames (%ld) does not match actual sample frames (%ld)",
											localBufferDataDescriptorPtr->fActualDataByteSize / (audioStream->format.fNumChannels * sizeof(float)), 
											clientBuf->mAudioClientBuffer32.numSampleFrames);
					}
#endif
				}

#if __i386__
                if (reserved->classicMode && clientBuf->mAudioClientBuffer32.sourceBuffer != NULL) {
					const IOAudioStreamFormat *fmt = audioStream->getFormat();
					if (fmt->fIsMixable && fmt->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM)
					{
						FlipFloats(clientBuf->mAudioClientBuffer32.sourceBuffer, clientBuf->mAudioClientBuffer32.numSampleFrames * clientBuf->mAudioClientBuffer32.numChannels);
					}
				}
#endif

				tmpResult = audioStream->processOutputSamples( &( clientBuf->mAudioClientBuffer32 ), firstSampleFrame, loopCount, true);

				clientBuf->mAudioClientBuffer32.numSampleFrames = maxNumSampleFrames;
				
                audioStream->unlockStreamForIO();
                
                if (tmpResult != kIOReturnSuccess) {
					audioDebugIOLog ( 3, "  processOutputSamples failed - result 0x%x", tmpResult );
					result = tmpResult;
                }
                
                clientBuf = clientBuf->mNextBuffer64;
				
				if (clientBuf) {  // need to update localBufferDataDescriptor for the current client buffer
					localBufferDataDescriptorPtr = FlipBufferDataDescriptor ( clientBuf->mAudioClientBuffer32.bufferDataDescriptor, &localBufferDataDescriptor, reserved->classicMode );
					
					if (NULL != localBufferDataDescriptorPtr) {
						numSampleFrames = localBufferDataDescriptorPtr->fActualNumSampleFrames;
					} else {
						numSampleFrames = clientBuf->mAudioClientBuffer32.numSampleFrames;
					}
				}
				
				clientIndex++;
            }
            
            bufferSet->nextOutputPosition = outputEndingPosition;
            
            audioEngine->calculateSampleTimeout(&bufferSet->sampleInterval, numSampleFrames, &bufferSet->nextOutputPosition, &outputTimeout);
            
            // We better have a thread call if we are doing output
            assert(bufferSet->watchdogThreadCall != NULL);

            bufferSet->setWatchdogTimeout(&outputTimeout);
        } else {
			audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::performClientOutput(%lx,%lx) - missed samples (%lx,%lx)", this, loopCount, firstSampleFrame, bufferSet->nextOutputPosition.fLoopCount, bufferSet->nextOutputPosition.fSampleFrame);
            result = kIOReturnIsoTooOld;
        }
    }	

Exit:    
    return result;
}

IOReturn IOAudioEngineUserClient::performClientInput(UInt32 firstSampleFrame, IOAudioClientBufferSet *bufferSet)
{
    IOReturn						result = kIOReturnSuccess;
    IOAudioClientBuffer64			*clientBuf;
	UInt32							numSampleFrames = 0;
    
    clientBuf = bufferSet->inputBufferList;

	IOAudioBufferDataDescriptor localBufferDataDescriptor;
	IOAudioBufferDataDescriptor * localBufferDataDescriptorPtr = 0;

	if (NULL != clientBuf) {    
		localBufferDataDescriptorPtr = FlipBufferDataDescriptor ( clientBuf->mAudioClientBuffer32.bufferDataDescriptor, &localBufferDataDescriptor, reserved->classicMode );
		if (NULL != localBufferDataDescriptorPtr) {
			audioDebugIOLog(6, "performClientInput ------------------------------");
			audioDebugIOLog ( 6, "  found buffer descriptor, using actual frames = %ld", 
								localBufferDataDescriptorPtr->fActualNumSampleFrames);
			numSampleFrames = localBufferDataDescriptorPtr->fActualNumSampleFrames;
		} else {
			audioDebugIOLog(6, "  no buffer descriptor found, using bufferSet->inputBufferList->numSampleFrames"); 
			numSampleFrames = bufferSet->inputBufferList->mAudioClientBuffer32.numSampleFrames;
		}
	}
	
    while (clientBuf) {
        IOAudioStream *					audioStream;
		UInt32							maxNumSampleFrames;
		UInt32							numSampleFramesRead;
        IOReturn						tmpResult;
        
        audioStream = clientBuf->mAudioClientBuffer32.audioStream;
        
        assert(audioStream);
        assert(audioStream->getDirection() == kIOAudioStreamDirectionInput);
        assert(clientBuf->mAudioClientBuffer32.sourceBuffer != NULL);

        audioStream->lockStreamForIO();

		maxNumSampleFrames = clientBuf->mAudioClientBuffer32.numSampleFrames;

		if (NULL != localBufferDataDescriptorPtr) {

			clientBuf->mAudioClientBuffer32.numSampleFrames = numSampleFrames;

			audioDebugIOLog ( 6, " clientBuffer = %p:  actual frames = %lu, actual bytes = %lu, nominal bytes = %lu, total bytes = %lu, source buffer size = %lu", 
									clientBuf, 
									clientBuf->mAudioClientBuffer32.numSampleFrames, 
									localBufferDataDescriptorPtr->fActualDataByteSize, 
									localBufferDataDescriptorPtr->fNominalDataByteSize, 
									localBufferDataDescriptorPtr->fTotalDataByteSize, 
									clientBuf->mAudioClientBuffer32.sourceBufferDescriptor->getLength () - offsetof ( IOAudioBufferDataDescriptor, fData ) );

	#ifdef DEBUG					
			if (clientBuf->mAudioClientBuffer32.numSampleFrames != localBufferDataDescriptorPtr->fActualDataByteSize / (audioStream->format.fNumChannels * sizeof(float))) {
				audioDebugIOLog ( 6, "  DEBUGGING - calculated sample frames (%ld) does not match actual sample frames (%ld)",
									localBufferDataDescriptorPtr->fActualDataByteSize / (audioStream->format.fNumChannels * sizeof(float)), 
									clientBuf->mAudioClientBuffer32.numSampleFrames);
			}
	#endif
			if ((localBufferDataDescriptorPtr->fActualDataByteSize > (clientBuf->mAudioClientBuffer32.sourceBufferDescriptor->getLength () - offsetof(IOAudioBufferDataDescriptor, fData))) ||
				(localBufferDataDescriptorPtr->fActualDataByteSize > localBufferDataDescriptorPtr->fTotalDataByteSize)) {
				audioDebugIOLog (1, "  *** VBR INPUT ERROR! clientBuffer = %p: actual frames = %ld, actual bytes = %ld, nominal bytes = %ld, total bytes = %ld, source buffer size = %ld", 
									clientBuf, 
									clientBuf->mAudioClientBuffer32.numSampleFrames, 
									localBufferDataDescriptorPtr->fActualDataByteSize, 
									localBufferDataDescriptorPtr->fNominalDataByteSize, 
									localBufferDataDescriptorPtr->fTotalDataByteSize, 
									clientBuf->mAudioClientBuffer32.sourceBufferDescriptor->getLength () - offsetof ( IOAudioBufferDataDescriptor, fData ) );
				audioStream->unlockStreamForIO(); 
				result = kIOReturnBadArgument;
				goto Exit;
			}	
		}

		// set the default number of frames read.  This allows drivers to override readInputSamples and still work in the VBR world
		audioStream->setDefaultNumSampleFramesRead(numSampleFrames);
        
        tmpResult = audioStream->readInputSamples( &( clientBuf->mAudioClientBuffer32 ), firstSampleFrame);
        
#if __i386__
		if (reserved->classicMode && clientBuf->mAudioClientBuffer32.sourceBuffer != NULL) {
			const IOAudioStreamFormat *fmt = audioStream->getFormat();
			if (fmt->fIsMixable && fmt->fSampleFormat == kIOAudioStreamSampleFormatLinearPCM)
			{
				FlipFloats(clientBuf->mAudioClientBuffer32.sourceBuffer, clientBuf->mAudioClientBuffer32.numSampleFrames * clientBuf->mAudioClientBuffer32.numChannels);
			}
		}
#endif        

		// get how many samples the driver actually read & update the rest of the structures
		numSampleFramesRead = audioStream->getNumSampleFramesRead();
		localBufferDataDescriptorPtr->fActualDataByteSize = numSampleFramesRead * audioStream->format.fNumChannels * sizeof(float);
		localBufferDataDescriptorPtr->fActualNumSampleFrames = numSampleFramesRead;
		FlipBufferDataDescriptor(localBufferDataDescriptorPtr, clientBuf->mAudioClientBuffer32.bufferDataDescriptor, reserved->classicMode); // save changes back to clientBuf

		audioDebugIOLog ( 5, "  numSampleFramesRead = %ld, fActualNumSampleFrames = %ld, fActualDataByteSize = %ld", 
							numSampleFramesRead, 
							localBufferDataDescriptorPtr->fActualNumSampleFrames, 
							localBufferDataDescriptorPtr->fActualDataByteSize );

		clientBuf->mAudioClientBuffer32.numSampleFrames = maxNumSampleFrames;

        audioStream->unlockStreamForIO();
        
        if (tmpResult != kIOReturnSuccess) {
			audioDebugIOLog ( 3, "  readInputSamples failed - result 0x%x", tmpResult );
            result = tmpResult;
        }
        
		audioDebugIOLog ( 3, "  next clientBuf " );
		clientBuf = clientBuf->mNextBuffer64;
		
		if (clientBuf) {  // need to update localBufferDataDescriptor for the current client buffer
			localBufferDataDescriptorPtr = FlipBufferDataDescriptor ( clientBuf->mAudioClientBuffer32.bufferDataDescriptor, &localBufferDataDescriptor, reserved->classicMode );
			
			if (NULL != localBufferDataDescriptorPtr) {
				numSampleFrames = localBufferDataDescriptorPtr->fActualNumSampleFrames;
			} else {
				numSampleFrames = clientBuf->mAudioClientBuffer32.numSampleFrames;
			}
		}
    }

Exit:    
    return result;
}

void IOAudioEngineUserClient::performWatchdogOutput(IOAudioClientBufferSet *clientBufferSet, UInt32 generationCount)
{
	audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::performWatchdogOutput(%p, %ld) - (%lx,%lx)", this, clientBufferSet, generationCount, clientBufferSet->nextOutputPosition.fLoopCount, clientBufferSet->nextOutputPosition.fSampleFrame);

    lockBuffers();
    
    if (!isInactive() && isOnline()) {
        if (clientBufferSet->timerPending) {
            // If the generation count of the clientBufferSet is different than the
            // generation count passed in, then a new client IO was received just before
            // the timer fired, and we don't need to do the fake IO
            // We just leave the timerPending field set
            if (clientBufferSet->generationCount == generationCount) {
                IOAudioClientBuffer64 *clientBuffer;
                
                clientBuffer = clientBufferSet->outputBufferList;
                
                while (clientBuffer) {
                    IOAudioStream *audioStream;
                    
                    audioStream = clientBuffer->mAudioClientBuffer32.audioStream;
                    
                    assert(audioStream);
                    assert(audioStream->getDirection() == kIOAudioStreamDirectionOutput);
                    
                    audioStream->lockStreamForIO();
                    
                    audioStream->processOutputSamples( &(clientBuffer->mAudioClientBuffer32), clientBufferSet->nextOutputPosition.fSampleFrame, clientBufferSet->nextOutputPosition.fLoopCount, false);
                    
                    audioStream->unlockStreamForIO();
                    
                    clientBuffer = clientBuffer->mNextBuffer64;
                }

                if (clientBufferSet->outputBufferList != NULL) {
                    UInt32 numSampleFrames, numSampleFramesPerBuffer;
                    AbsoluteTime outputTimeout;
                    
                    numSampleFrames = clientBufferSet->outputBufferList->mAudioClientBuffer32.numSampleFrames;
                    numSampleFramesPerBuffer = audioEngine->getNumSampleFramesPerBuffer();
                    
                    clientBufferSet->nextOutputPosition.fSampleFrame += numSampleFrames;
                    
                    if (clientBufferSet->nextOutputPosition.fSampleFrame >= numSampleFramesPerBuffer) {
                        clientBufferSet->nextOutputPosition.fSampleFrame -= numSampleFramesPerBuffer;
                        clientBufferSet->nextOutputPosition.fLoopCount++;
                    }
                    
                    audioEngine->calculateSampleTimeout(&clientBufferSet->sampleInterval, numSampleFrames, &clientBufferSet->nextOutputPosition, &outputTimeout);
                    
                    clientBufferSet->setWatchdogTimeout(&outputTimeout);

                } else {
                    clientBufferSet->timerPending = false;
                }
            }
        }
    } else {
        clientBufferSet->timerPending = false;
    }
    
    unlockBuffers();
}

IOReturn IOAudioEngineUserClient::getConnectionID(UInt32 *connectionID)
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::getConnectionID(%p)", this, connectionID);

    *connectionID = (UInt32)this;
    return kIOReturnSuccess;
}

IOReturn IOAudioEngineUserClient::clientStart()
{
    assert(commandGate);

    return commandGate->runAction(startClientAction);
}

IOReturn IOAudioEngineUserClient::clientStop()
{
    assert(commandGate);
    
    return commandGate->runAction(stopClientAction);
}

IOReturn IOAudioEngineUserClient::startClientAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        if (userClient) {
            result = userClient->startClient();
        }
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::stopClientAction(OSObject *owner, void *arg1, void *arg2, void *arg3, void *arg4)
{
    IOReturn result = kIOReturnBadArgument;
    
    if (owner) {
        IOAudioEngineUserClient *userClient = OSDynamicCast(IOAudioEngineUserClient, owner);
        if (userClient) {
            result = userClient->stopClient();
        }
    }
    
    return result;
}

IOReturn IOAudioEngineUserClient::startClient()
{
    IOReturn result = kIOReturnNoDevice;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - %ld", this, audioEngine ? audioEngine->numActiveUserClients : 0);
	
	retain();

    if (audioEngine && !isInactive()) {
		audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - audioEngine && !isInactive() ", this);
       if (audioEngine->getState() != kIOAudioEnginePaused) {
		   audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - audioEngine->getState() != kIOAudioEnginePaused ", this);
           // We only need to start things up if we're not already online
            if (!isOnline()) {
                setOnline(true);
				audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - !isOnline() setting online ", this);
                result = audioEngine->startClient(this);
                
                if (result == kIOReturnSuccess) {
					audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - engine started ", this);
                   IOAudioClientBufferSet *bufferSet;
                    
                    lockBuffers();
                    
                    // add buffers to streams
                    bufferSet = clientBufferSetList;
                    while (bufferSet) {
                        IOAudioClientBuffer64 *clientBuffer;
						audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - bufferSet %p ", this, bufferSet);
						
                        clientBuffer = bufferSet->outputBufferList;
                        while (clientBuffer) {
                            if (clientBuffer->mAudioClientBuffer32.audioStream) {
								audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - output clientBuffer %p ", this, clientBuffer);
                               result = clientBuffer->mAudioClientBuffer32.audioStream->addClient( &clientBuffer->mAudioClientBuffer32 ); 
                                if (result != kIOReturnSuccess) {
                                    break;
                                }
                            }
                            clientBuffer = clientBuffer->mNextBuffer64;
                        }
            
                        clientBuffer = bufferSet->inputBufferList;
                        while (clientBuffer) {
							audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - input clientBuffer %p ", this, clientBuffer);
							if (clientBuffer->mAudioClientBuffer32.audioStream) {
                                clientBuffer->mAudioClientBuffer32.audioStream->addClient( &( clientBuffer->mAudioClientBuffer32 ) ); 
                            }
                            clientBuffer = clientBuffer->mNextBuffer64;
                        }
                        
                        bufferSet->resetNextOutputPosition();
            
                        bufferSet = bufferSet->mNextBufferSet;
                    }
                    
                    unlockBuffers();
                }
				else
				{
					audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - engine NOT started ", this);
 				}
            } 
			else {
                result = kIOReturnSuccess;
            }
        } else {
            result = kIOReturnOffline;
        }
    }

	if (kIOReturnSuccess != result) {
		audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::startClient() - error (0x%x) - setting offline ", this, result );
		setOnline(false);
	}

	release();

	return result;
}

IOReturn IOAudioEngineUserClient::stopClient()
{
    IOReturn result = kIOReturnSuccess;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::stopClient() - %ld", this, audioEngine ? audioEngine->numActiveUserClients : 0);

    if (isOnline()) {
        IOAudioClientBufferSet *bufferSet;
        
        lockBuffers();
        
        bufferSet = clientBufferSetList;
        while (bufferSet) {
            IOAudioClientBuffer64 *clientBuffer;
            
            bufferSet->cancelWatchdogTimer();
            
            clientBuffer = bufferSet->outputBufferList;
            while (clientBuffer) {
                if (clientBuffer->mAudioClientBuffer32.audioStream) {
                    clientBuffer->mAudioClientBuffer32.audioStream->removeClient( &( clientBuffer->mAudioClientBuffer32 ) ); 
                }
                clientBuffer = clientBuffer->mNextBuffer64;
            }

            clientBuffer = bufferSet->inputBufferList;
            while (clientBuffer) {
                if (clientBuffer->mAudioClientBuffer32.audioStream) {
                    clientBuffer->mAudioClientBuffer32.audioStream->removeClient( &(clientBuffer->mAudioClientBuffer32 ));  
                }
                clientBuffer = clientBuffer->mNextBuffer64;
            }
            
            bufferSet = bufferSet->mNextBufferSet;
        }
        
        unlockBuffers();

        if (audioEngine) {
            result = audioEngine->stopClient(this);
        }
    
        setOnline(false);
    }
    
    return result;
}

// Must be done on workLoop
void IOAudioEngineUserClient::sendFormatChangeNotification(IOAudioStream *audioStream)
{
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::sendFormatChangeNotification(%p)", this, audioStream);

    if (audioStream && notificationMessage && (notificationMessage->messageHeader.msgh_remote_port != MACH_PORT_NULL)) {
        io_object_t clientStreamRef;
        
        audioStream->retain();
        if (exportObjectToClient(clientTask, audioStream, &clientStreamRef) == kIOReturnSuccess) {
            kern_return_t kr;
            
            notificationMessage->type = kIOAudioEngineStreamFormatChangeNotification;
            notificationMessage->sender = clientStreamRef;
            
            kr = mach_msg_send_from_kernel(&notificationMessage->messageHeader, notificationMessage->messageHeader.msgh_size);
            if (kr != MACH_MSG_SUCCESS) {
                IOLog("IOAudioEngineUserClient::sendFormatChangeNotification() failed - msg_send returned: %d\n", kr);
                // Should also release the clientStreamRef here...
            }
        } else {
            IOLog("IOAudioEngineUserClient[%p]::sendFormatChangeNotification() - ERROR - unable to export stream object for notification - notification not sent\n", this);
        }
    } else {
		if (notificationMessage) {
			IOLog("IOAudioEngineUserClient[%p]::sendFormatChangeNotification() - ERROR - notification not sent - audioStream = %p - notificationMessage = %p - port = %ld\n", this, audioStream, notificationMessage, (UInt32)notificationMessage->messageHeader.msgh_remote_port);
		} else {
			IOLog("IOAudioEngineUserClient[%p]::sendFormatChangeNotification() - ERROR - notification not sent - audioStream = %p - notificationMessage = %p\n", this, audioStream, notificationMessage);
		}
    }
}

IOReturn IOAudioEngineUserClient::sendNotification(UInt32 notificationType)
{
    IOReturn result = kIOReturnSuccess;
    
    audioDebugIOLog(3, "IOAudioEngineUserClient[%p]::sendNotification(%ld)", this, notificationType);

    if (notificationType == kIOAudioEnginePausedNotification) {
        stopClient();
    }
        
    if (notificationMessage && (notificationMessage->messageHeader.msgh_remote_port != MACH_PORT_NULL)) {
        kern_return_t kr;
        
        notificationMessage->type = notificationType;
        notificationMessage->sender = NULL;
        
        kr = mach_msg_send_from_kernel(&notificationMessage->messageHeader, notificationMessage->messageHeader.msgh_size);
        if (kr != MACH_MSG_SUCCESS) {
            result = kIOReturnError;
        }
    }
    
    return result;
}
