#pragma once
#include <IOKit/IOService.h>
#include <FakeIrisXEFramebuffer.hpp>

class FakeIrisXEAccelDevice final : public IOService {
    OSDeclareDefaultStructors(FakeIrisXEAccelDevice)

public:
    bool init(OSDictionary* dict = nullptr) override;
       void free(void) override;
       IOService* probe(IOService* provider, SInt32* score) override;
       bool start(IOService* provider) override;
       void stop(IOService* provider) override;

    
    // Allow the SharedUserClient to find the FB (optional but handy)
    
    FakeIrisXEFramebuffer*  fFB{nullptr};
      FakeIrisXEFramebuffer* getFramebuffer() const { return fFB; }

      // newUserClient → create our shared client
      IOReturn newUserClient(task_t owningTask,
                             void* securityID,
                             UInt32 type,
                             OSDictionary* properties,
                             IOUserClient** handler) override;
    
    
    

private:
    void publishAccelProperties();
    
    
    
    
};
