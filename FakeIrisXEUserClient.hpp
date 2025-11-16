#ifndef _FAKEIRISXEUSERCLIENT_H
#define _FAKEIRISXEUSERCLIENT_H

#include <IOKit/IOUserClient.h>
#include "FakeIrisXEAccelerator.hpp"
#include <IOKit/IOUserClient.h>
#include <libkern/c++/OSObject.h>
#include "FakeIrisXEFramebuffer.hpp"



class FakeIrisXEUserClient : public IOUserClient
{
    OSDeclareDefaultStructors(FakeIrisXEUserClient);
   #define super IOUserClient

    
    private:
        FakeIrisXEFramebuffer* fOwner { nullptr };
        task_t fTask;                                 // 👈 the client task
   
    

    public:
        bool start(IOService* provider) override;
        void stop(IOService* provider) override;
        IOReturn clientClose() override;
    
    
    enum MemoryType : UInt32 {
           kMemoryTypeFramebuffer = 0
       };
    

        IOReturn externalMethod(uint32_t selector,
                                IOExternalMethodArguments* args,
                                IOExternalMethodDispatch* dispatch,
                                OSObject* target,
                                void* ref) override;
    
    bool initWithTask(task_t owningTask, void* securityID, UInt32 type, OSDictionary* properties)override;
    
 
       // NEW: expose memory to IOConnectMapMemory64
       virtual IOReturn clientMemoryForType(UInt32 type,
                                            IOOptionBits *options,
                                            IOMemoryDescriptor **memory) override;



        static const IOExternalMethodDispatch sMethods[];
    };


#endif /* _FAKEIRISXEUSERCLIENT_H */
