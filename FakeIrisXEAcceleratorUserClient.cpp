#include "FakeIrisXEAcceleratorUserClient.hpp"
#include "FakeIrisXEAccelerator.hpp"
#include "FakeIrisXEAccelShared.h"

#include <IOKit/IOLib.h>

OSDefineMetaClassAndStructors(FakeIrisXEAcceleratorUserClient, IOUserClient);

// initWithTask - kernel correct signature
bool FakeIrisXEAcceleratorUserClient::initWithTask(task_t owningTask, void* securityID, UInt32 type)
{
    if (!IOUserClient::initWithTask(owningTask, securityID, type))
        return false;
    fTask = owningTask;
    return true;
}



bool FakeIrisXEAcceleratorUserClient::start(IOService* provider)
{
    if (!IOUserClient::start(provider))
        return false;

    fOwner = OSDynamicCast(FakeIrisXEAccelerator, provider);
    if (!fOwner) return false;

    IOLog("(FakeIrisXEFramebuffer) [AccelUC] started\n");

    // Only allocate page, do NOT attach yet
    fSharedMem = IOBufferMemoryDescriptor::inTaskWithOptions(
        kernel_task,
        kIODirectionInOut | kIOMemoryKernelUserShared,
        XE_PAGE);

    if (!fSharedMem) return false;

    bzero(fSharedMem->getBytesNoCopy(), XE_PAGE);
    fSharedHdr = (volatile XEHdr*)fSharedMem->getBytesNoCopy();

    fSharedHdr->magic    = XE_MAGIC;
    fSharedHdr->version  = XE_VERSION;
    fSharedHdr->capacity = XE_PAGE - sizeof(XEHdr);
    fSharedHdr->head     = 0;
    fSharedHdr->tail     = 0;

    fRingBase = ((uint8_t*)fSharedHdr) + sizeof(XEHdr);

    return true;
}







void FakeIrisXEAcceleratorUserClient::stop(IOService* provider)
{
    IOLog("(FakeIrisXEFramebuffer) [AccelUC] stop\n");

    // DO NOT TOUCH fOwner->fTimer EVER
    // Accelerator owns its timer; UC must never manage it.

    if (fSharedMem) {
        fSharedMem->release();
        fSharedMem = nullptr;
        fSharedHdr = nullptr;
        fRingBase = nullptr;
    }

    fOwner = nullptr;

    IOUserClient::stop(provider);
}







IOReturn FakeIrisXEAcceleratorUserClient::clientClose()
{
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEAcceleratorUserClient::externalMethod(uint32_t selector,
                                                         IOExternalMethodArguments* args,
                                                         IOExternalMethodDispatch*,
                                                         OSObject*,
                                                         void*)
{
    if (!fOwner) return kIOReturnNotReady;

    switch (selector) {
        case kAccelSel_Ping:
            return kIOReturnSuccess;
        case kAccelSel_InjectTest:
            return doInjectTest();
        case kAccelSel_GetCaps:
            if (!args || !args->structureOutput) return kIOReturnBadArgument;
            {
                XEAccelCaps caps{};
                fOwner->getCaps(caps); // accelerator must implement getCaps(XEAccelCaps&)
                bcopy(&caps, args->structureOutput, sizeof(caps));
                args->structureOutputSize = sizeof(caps);
                return kIOReturnSuccess;
            }
        case kAccelSel_CreateContext:
            if (!args || !args->structureInput) return kIOReturnBadArgument;
            {
                const XECreateCtxIn* in = reinterpret_cast<const XECreateCtxIn*>(args->structureInput);
                XECreateCtxOut out{};
                out.ctxId = fOwner->createContext(in->sharedGPUPtr, in->flags);
                if (!args->structureOutput || args->structureOutputSize < sizeof(out)) return kIOReturnMessageTooLarge;
                bcopy(&out, args->structureOutput, sizeof(out));
                args->structureOutputSize = sizeof(out);
                return kIOReturnSuccess;
            }
        case kAccelSel_Submit:
            // For your current test harness we can treat submit as a stub that returns success.
            return kIOReturnSuccess;
        case kAccelSel_Flush:
            // optional: call accelerator flush( ctx )
            if (args && args->scalarInput && args->scalarInputCount >= 1) {
                uint32_t ctxId = static_cast<uint32_t>(args->scalarInput[0]);
                return fOwner->flush(ctxId);
            }
            return fOwner->flush(0);
        case kAccelSel_DestroyContext:
            if (!args || !args->scalarInput || args->scalarInputCount < 1) return kIOReturnBadArgument;
            return fOwner->destroyContext(static_cast<uint32_t>(args->scalarInput[0]));
     
            
        default:
            return kIOReturnUnsupported;
    }
}

// Provide the shared memory descriptor to userspace when they request type==1
// Provide the shared memory descriptor to userspace when they request type==1
IOReturn FakeIrisXEAcceleratorUserClient::clientMemoryForType(
        UInt32 type,
        IOOptionBits *options,
        IOMemoryDescriptor **memory )
{
    if (type == 1) {
        *options = kIOMapDefaultCache;

        if (!fSharedMem) return kIOReturnNotFound;

        *memory = fSharedMem;
        (*memory)->retain();

        // *** IMPORTANT ***
        fOwner->attachShared( fSharedMem );
        fOwner->startWorkerLoop();        // <-- FIX

        return kIOReturnSuccess;
    }
    return IOUserClient::clientMemoryForType(type, options, memory);
}



IOReturn FakeIrisXEAcceleratorUserClient::doInjectTest()
{
    if (!fOwner || !fOwner->fHdr || !fOwner->fRingBase)
        return kIOReturnNotReady;

    XECmd cmd{};
    cmd.opcode = XE_CMD_CLEAR;
    cmd.bytes  = 4;      // <<<< CLEAR has a 4-byte payload
    cmd.ctxId  = 0;

    uint32_t color = 0xFFFF0000;

    uint32_t head = fOwner->fHdr->head;
    uint32_t cap  = fOwner->fHdr->capacity;

    uint32_t total = xe_align(sizeof(XECmd) + cmd.bytes);

    // write header
    if (head + sizeof(XECmd) <= cap)
        memcpy(fOwner->fRingBase + head, &cmd, sizeof(cmd));
    else {
        uint32_t first = cap - head;
        memcpy(fOwner->fRingBase + head, &cmd, first);
        memcpy(fOwner->fRingBase, ((uint8_t*)&cmd)+first, sizeof(cmd)-first);
    }

    // write payload
    uint32_t poff = (head + sizeof(XECmd)) % cap;
    if (poff + 4 <= cap)
        memcpy(fOwner->fRingBase + poff, &color, 4);
    else {
        uint32_t first = cap - poff;
        memcpy(fOwner->fRingBase + poff, &color, first);
        memcpy(fOwner->fRingBase, ((uint8_t*)&color)+first, 4-first);
    }

    fOwner->fHdr->head = (head + total) % cap;
    OSSynchronizeIO();

    IOLog("[UC] InjectTest wrote CLEAR (head=%u)\n", fOwner->fHdr->head);
    return kIOReturnSuccess;
}
