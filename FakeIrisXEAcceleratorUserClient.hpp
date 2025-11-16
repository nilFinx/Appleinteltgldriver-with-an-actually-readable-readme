#ifndef FAKE_IRIS_XE_ACCELERATOR_USERCLIENT_HPP
#define FAKE_IRIS_XE_ACCELERATOR_USERCLIENT_HPP

#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include "FakeIrisXEAccelShared.h"

class FakeIrisXEAccelerator;

class FakeIrisXEAcceleratorUserClient : public IOUserClient {
    OSDeclareDefaultStructors(FakeIrisXEAcceleratorUserClient);

private:
    task_t                         fTask{nullptr};
    FakeIrisXEAccelerator*         fOwner{nullptr};

    IOBufferMemoryDescriptor*      fSharedMem{nullptr};   // kernel-owned buffer we hand to userspace (clientMemoryForType)
    volatile XEHdr*                fSharedHdr{nullptr};   // header inside fSharedMem
    uint8_t*                       fRingBase{nullptr};    // pointer to ring payload (after header)

public:
    // Kernel IOKit signature (3 args) — correct for kernel builds
    bool initWithTask(task_t owningTask, void* securityID, UInt32 type) override;

    bool start(IOService* provider) override;
    void stop(IOService* provider) override;

    IOReturn clientClose() override;
    
    IOReturn doInjectTest();   // <<< YOU MUST ADD THIS


    IOReturn externalMethod(uint32_t selector,
                            IOExternalMethodArguments* args,
                            IOExternalMethodDispatch* dispatch,
                            OSObject* target,
                            void* ref) override;

    // map shared buffer into user task memory
    IOReturn clientMemoryForType(uint32_t type, IOOptionBits* options, IOMemoryDescriptor** mem) override;
    
    
      
    
    
    
    
    
    
    
};





#endif

