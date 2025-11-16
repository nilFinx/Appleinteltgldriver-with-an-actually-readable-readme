#include "FakeIrisXEUserClient.hpp"
#include "FakeIrisXEFramebuffer.hpp"

#include <IOKit/IOLib.h>

#define KERNEL 1
#define KERNEL_PRIVATE 1
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>


OSDefineMetaClassAndStructors(FakeIrisXEUserClient, IOUserClient);


bool FakeIrisXEUserClient::initWithTask(task_t owningTask, void* securityID, UInt32 type, OSDictionary* properties) {
    IOLog("(FakeIrisXEUserClient) initWithTask(): client task = %p\n", owningTask);
    if (!super::initWithTask(owningTask, securityID, type, properties))
        return false;

    fTask = owningTask;
    fOwner = nullptr;
    return true;
}

bool FakeIrisXEUserClient::start(IOService* provider) {
    IOLog("(FakeIrisXEUserClient) start() called with provider=%s\n", provider ? provider->getName() : "(null)");

    if (!super::start(provider))
        return false;

    fOwner = OSDynamicCast(FakeIrisXEFramebuffer, provider);
    if (!fOwner) {
        IOLog("(FakeIrisXEUserClient) ❌ Provider is not FakeIrisXEFramebuffer\n");
        return false;
    }

    IOLog("(FakeIrisXEUserClient) ✅ Attached directly to FakeIrisXEFramebuffer\n");
    return true;
}

void FakeIrisXEUserClient::stop(IOService* provider) {
    IOLog("(FakeIrisXEUserClient) stop(): closing user client\n");
    super::stop(provider);
}

IOReturn FakeIrisXEUserClient::clientClose() {
    IOLog("(FakeIrisXEUserClient) clientClose() called\n");
    terminate();
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEUserClient::clientMemoryForType(UInt32 type,
                                                   IOOptionBits *options,
                                                   IOMemoryDescriptor **memory)
{
    if (!fOwner) return kIOReturnNotReady;
    if (!options || !memory) return kIOReturnBadArgument;

    switch (type) {
    case kMemoryTypeFramebuffer: {
        IOBufferMemoryDescriptor* fb = fOwner->getFBMemory();
        if (!fb) return kIOReturnNotReady;

        fb->retain();            // caller will release
        *memory  = fb;
        *options = 0;            // no special mapping flags required
        return kIOReturnSuccess;
    }
    default:
        return kIOReturnUnsupported;
    }
}




IOReturn FakeIrisXEUserClient::externalMethod(uint32_t selector,
                                              IOExternalMethodArguments* args,
                                              IOExternalMethodDispatch*, OSObject*, void*)
{
    switch (selector) {
    case 0: // ping
        return kIOReturnSuccess;

    case 1: { // get fb info
        if (!fOwner) return kIOReturnNotReady;
        struct FBInfo { uint32_t w,h,stride; uint64_t phys; } fb{};
        fb.w = fOwner->getWidth(); fb.h = fOwner->getHeight();
        fb.stride = fOwner->getStride(); fb.phys = fOwner->getFramebufferPhysAddr();
        if (!args || !args->structureOutput) return kIOReturnBadArgument;
        if (args->structureOutputSize < sizeof(fb)) return kIOReturnMessageTooLarge;
        bcopy(&fb, args->structureOutput, sizeof(fb));
        args->structureOutputSize = sizeof(fb);
        return kIOReturnSuccess;
    }

    default:
        return kIOReturnUnsupported;
    }
}
