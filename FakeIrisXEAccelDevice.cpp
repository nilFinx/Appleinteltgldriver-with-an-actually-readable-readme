// FakeIrisXEAccelDevice.cpp
#include "FakeIrisXEAccelDevice.hpp"
#include "FakeIrisXEFramebuffer.hpp"
#include "FakeIrisXESharedUserClient.hpp"

#define super IOService

OSDefineMetaClassAndStructors(FakeIrisXEAccelDevice, IOService)

bool FakeIrisXEAccelDevice::init(OSDictionary* dict)
{
    if (!super::init(dict)) return false;
    return true;
}

void FakeIrisXEAccelDevice::free(void)
{
    fFB = nullptr;
    super::free();
}

IOService* FakeIrisXEAccelDevice::probe(IOService* provider, SInt32* score)
{
    auto ret = super::probe(provider, score);
    // be slightly greedy so we beat generic matches
    if (score) *score += 10;
    return ret;
}

bool FakeIrisXEAccelDevice::start(IOService* provider)
{
    if (!super::start(provider)) return false;

    // We expect the provider to be our framebuffer
    fFB = OSDynamicCast(FakeIrisXEFramebuffer, provider);
    if (!fFB) {
        IOLog("(FakeIrisXEAccelDevice) provider is not FakeIrisXEFramebuffer\n");
        return false;
    }

    // Publish a few useful properties for debugging / consumers
    setProperty("IOClass", "FakeIrisXEAccelDevice");
    setProperty("MetalPluginClassName", "FakeIrisXEAccelerator"); // you already expose this elsewhere
    setProperty("IOGVACapabilities", "basic"); // placeholder hint

    registerService(); // make it discoverable to user space
    return true;
}

void FakeIrisXEAccelDevice::stop(IOService* provider)
{
    super::stop(provider);
}

IOReturn FakeIrisXEAccelDevice::newUserClient(task_t owningTask,
                                              void*,
                                              UInt32 type,
                                              OSDictionary*,
                                              IOUserClient** handler)
{
    // Single shared client type for now (type is ignored)
    FakeIrisXESharedUserClient* client = OSTypeAlloc(FakeIrisXESharedUserClient);
    if (!client) return kIOReturnNoMemory;
    if (!client->initWithTask(owningTask, nullptr, 0)) {
        client->release();
        return kIOReturnNoMemory;
    }
    if (!client->attach(this)) {
        client->release();
        return kIOReturnUnsupported;
    }
    if (!client->start(this)) {
        client->detach(this);
        client->release();
        return kIOReturnUnsupported;
    }
    *handler = client;
    return kIOReturnSuccess;
}
