#include "FakeIrisXEFramebuffer.hpp"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/acpi/IOACPIPlatformDevice.h>
#include <libkern/c++/OSSymbol.h>
#include <IOKit/IOLib.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/graphics/IOAccelerator.h>
#include <IOKit/IOKitKeys.h>           // Needed for types like OSAsyncReference
#include <IOKit/IOUserClient.h>        // Must follow after including IOKit headers
#include <IOKit/IOMessage.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <IOKit/graphics/IOGraphicsTypes.h>
#include <string.h>
#include <IOKit/graphics/IOFramebufferShared.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/pwr_mgt/IOPM.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOInterruptEventSource.h>
#include <libkern/OSAtomic.h>

using namespace libkern;

// New: Render and Media domain FORCEWAKE_ACK registers for Gen11+
#define FORCEWAKE_ACK_RENDER 0x0A188  // Read-only
#define FORCEWAKE_ACK_MEDIA  0x0A18C  // Optional, already used
//#define FORCEWAKE_ACK 0x0A188  // This was probably used as generic

constexpr const char* kIOFBDepthKey = "IOFBDepth";
constexpr const char* kIOFBCurrentPixelFormatKey = "IOFBCurrentPixelFormat";



// Connection attribute keys (from IOFramebufferShared.h, internal Apple headers)
#define kConnectionSupportsAppleSense   0x00000001
#define kConnectionSupportsLLDDCSense   0x00000002
#define kConnectionSupportsHLDDCSense   0x00000004
#define kConnectionSupportsDDCSense     0x00000008
#define kConnectionDisplayParameterCount 0x00000009
#define kConnectionFlags                0x0000000A
#define kConnectionSupportsHotPlug        0x00000001
#define kIOFBCursorSupportedKey               "IOFBCursorSupported"
#define kIOFBHardwareCursorSupportedKey       "IOFBHardwareCursorSupported"
#define kIOFBDisplayModeCountKey              "IOFBDisplayModeCount"
#define kIOFBNotifyDisplayModeChange 'dmod'
#define kIOTimingIDDefault 0

#define kIOFramebufferConsoleKey "IOFramebufferIsConsole"
#define kIO32BGRAPixelFormat 'BGRA'
#define kIOPixelFormatWideGamut 'wgam'
#define kIOCaptureAttribute 'capt'

#define kIOFBNotifyDisplayAdded  0x00000010
#define kIOFBConfigChanged       0x00000020



#ifndef kIOTimingInfoValid_AppleTimingID
#define kIOTimingInfoValid_AppleTimingID 0x00000001
#endif

#ifndef kIOFBVsyncNotification
#define kIOFBVsyncNotification iokit_common_msg(0x300)
#endif

#define MAKE_IOVRAM_RANGE_INDEX(index) ((UInt32)(index))
#define kIOFBMemoryCountKey   "IOFBMemoryCount"

#define kIOFBVRAMMemory 0
#define kIOFBCursorMemory 1

// Connection flag values
#define kIOConnectionBuiltIn            0x00000100
#define kIOConnectionDisplayPort        0x00000800

#define super IOFramebuffer
#define kIOMessageServiceIsRunning 0x00001001


#define SAFE_MMIO_WRITE(offset, value) \
    if (offset > mmioMap->getLength() - 4) { \
        IOLog("❌ MMIO offset 0x%X out of bounds\n", offset); \
        return kIOReturnError; \
    } \
    *(volatile uint32_t*)((uint8_t*)mmioBase + offset) = value;




OSDefineMetaClassAndStructors(FakeIrisXEFramebuffer, IOFramebuffer);



//probe
IOService *FakeIrisXEFramebuffer::probe(IOService *provider, SInt32 *score) {
    IOPCIDevice *pdev = OSDynamicCast(IOPCIDevice, provider);
    if (!pdev) {
        IOLog("FakeIrisXEFramebuffer::probe(): Provider is not IOPCIDevice\n");
        return nullptr;
    }

    UInt16 vendor = pdev->configRead16(kIOPCIConfigVendorID);
    UInt16 device = pdev->configRead16(kIOPCIConfigDeviceID);

    // Only proceed if it's your target device
    if (vendor == 0x8086 && device == 0x9A49) {
        IOLog("FakeIrisXEFramebuffer::probe(): Found matching GPU (8086:9A49)\n");
        if (score) {
            *score += 50000; // Force to beat IONDRVFramebuffer
        }
        // Call super::probe if you want parent class probing, but ensure it doesn't override your score
         IOService* result = super::probe(provider, score);
        if (result) return result;
        return OSDynamicCast(IOService, this); // Return this instance if it matches
    }

    return nullptr; // No match
}


bool FakeIrisXEFramebuffer::init(OSDictionary* dict) {
    if (!super::init(dict))
        return false;
   
// Initialize other members
    vramMemory = nullptr;
    mmioBase = nullptr;
    mmioMap = nullptr;
    currentMode = 0;
    currentDepth = 0;
    vramSize = 1920 * 1080 * 4;
    controllerEnabled = false;
    displayOnline = false;
    displayPublished = false;
    shuttingDown = false;
    fullyInitialized = false;  // ADD THIS
      
    
    
    
    return true;
}

IOPMPowerState FakeIrisXEFramebuffer::powerStates[kNumPowerStates] = {
    {
        1,                          // version
        0,                          // capabilityFlags
        0,                          // outputPowerCharacter
        0,                          // inputPowerRequirement
        0,                          // staticPower
        0,                          // unbudgetedPower
        0,                          // powerToAttain
        0,                          // timeToAttain
        0,                          // settleUpTime
        0,                          // timeToLower
        0,                          // settleDownTime
        0                           // powerDomainBudget
    },
    {
        1,                          // version
        IOPMPowerOn,                // capabilityFlags
        IOPMPowerOn,                // outputPowerCharacter
        IOPMPowerOn,                // inputPowerRequirement
        0, 0, 0, 0, 0, 0, 0
    }
};


    IOPCIDevice* pciDevice;
    IOMemoryMap* mmioMap;
    volatile uint8_t* mmioBase;


// --- CRITICAL MMIO HELPER FUNCTIONS ---
  // These functions ensure safe access to the memory-mapped registers.
  // They are essential for the power management block to compile and run.
inline uint32_t safeMMIORead(uint32_t offset){
      if (!mmioBase || !mmioMap || offset >= mmioMap->getLength()) {
          IOLog("❌ MMIO Read attempted with invalid offset: 0x%08X\n", offset);
          return 0;
      }
      return *(volatile uint32_t*)(mmioBase + offset);
  }

  inline void safeMMIOWrite(uint32_t offset, uint32_t value) {
      if (!mmioBase || !mmioMap || offset >= mmioMap->getLength()) {
          IOLog("❌ MMIO Write attempted with invalid offset: 0x%08X\n", offset);
          return;
      }
      *(volatile uint32_t*)(mmioBase + offset) = value;
  }



// --- DEDICATED POWER MANAGEMENT FUNCTION ---
   // This function encapsulates the entire power-up sequence.
   // It should be called once, at the beginning of the driver's
   // life cycle, to ensure the device is in a ready state.
   void initPowerManagement() {
       IOLog("Initiating detailed power management sequence...\n");

       if (!pciDevice || !pciDevice->isOpen()) {
           IOLog("initPowerManagement(): PCI device not open -aborting");
           return;
       }
       
       
       
       
       // --- PCI Power Management ---
       uint16_t pmcsr = pciDevice->configRead16(0x84);
       IOLog("FakeIrisXEFramebuffer::start() - PCI PMCSR before = 0x%04X\n", pmcsr);
       pmcsr &= ~0x3; // Force D0
       pciDevice->configWrite16(0x84, pmcsr);
       IOSleep(10);
       pmcsr = pciDevice->configRead16(0x84);
       IOLog("FakeIrisXEFramebuffer::start() - PCI PMCSR after force = 0x%04X\n", pmcsr);

       
       
       
       // --- Power Management Sequence ---
       const uint32_t GT_PG_ENABLE = 0xA218;
       const uint32_t PUNIT_PG_CTRL = 0xA2B0;
       const uint32_t PWR_WELL_CTL = 0x45400;
       const uint32_t PWR_WELL_STATUS = 0x45408;
       const uint32_t FORCEWAKE_MT = 0xA188;
       const uint32_t FORCEWAKE_ACK = 0xA188;

       
       
       IOLog("GT_PG_ENABLE setting");
       // 1. GT Power Gating Control
       uint32_t pg_enable = safeMMIORead(GT_PG_ENABLE);
       IOLog("GT_PG_ENABLE before: 0x%08X\n", pg_enable);
       safeMMIOWrite(GT_PG_ENABLE, pg_enable & ~0x1);
       IOSleep(10);
       
       
       
       uint32_t gt_satus = safeMMIORead(0x138124);
       IOLog("GT Thread satus= 0x%08X\n", gt_satus);

       
       
       
       // 2. PUNIT Power Gating Control
       uint32_t punit_pg = safeMMIORead(PUNIT_PG_CTRL);
       IOLog("PUNIT_PG_CTRL before: 0x%08X\n", punit_pg);
       safeMMIOWrite(PUNIT_PG_CTRL, punit_pg & ~0x80000000);
       IOSleep(15);

       
       
       // 3. Power Well Control with verification
       uint32_t pw_ctl = safeMMIORead(PWR_WELL_CTL);
       IOLog("PWR_WELL_CTL before: 0x%08X\n", pw_ctl);
       safeMMIOWrite(PWR_WELL_CTL, pw_ctl | 0x2);
       IOSleep(10);
       safeMMIOWrite(PWR_WELL_CTL, safeMMIORead(PWR_WELL_CTL) | 0x4);
       IOSleep(10);

       
       
       // Verify power well status
       int tries = 0;
       while (tries++ < 20) {
           uint32_t pw_status = safeMMIORead(PWR_WELL_STATUS);
           if (pw_status & 0x80000000) break;
           IOSleep(5);
       }
       IOLog("Power Well Status after enabling: 0x%08X\n", safeMMIORead(PWR_WELL_STATUS));

       
       
       // --- FORCEWAKE Sequence with Enhanced Safety ---
       IOLog("Initiating FORCEWAKE sequence...\n");
       safeMMIOWrite(FORCEWAKE_MT, 0x000F000F);
       IOSleep(15);
       
       
       
       bool forcewake_ack = false;
       for (int i = 0; i < 100; i++) {
           uint32_t ack = safeMMIORead(FORCEWAKE_ACK);
           if (ack & 0x1) {
               forcewake_ack = true;
               break;
           }
           IOSleep(1);
       }

       if (!forcewake_ack) {
           IOLog("WARNING: Primary FORCEWAKE failed, trying fallback...\n");
           safeMMIOWrite(FORCEWAKE_MT, 0x00020002);
           IOSleep(10);
           safeMMIOWrite(0xA008, 0x00010001);
           IOSleep(10);
       }

       
       uint32_t final_ack = safeMMIORead(FORCEWAKE_ACK);
       IOLog("Final FORCEWAKE_ACK: 0x%08X\n", final_ack);

       
       
       // --- Additional Power/Clock Control ---
       safeMMIOWrite(0x09400, 0xFFFFFFFF);
       safeMMIOWrite(0x08500, 0);
       safeMMIOWrite(0xA248, safeMMIORead(0xA248) | 0x0000001F); // Bitmask: All domains

       IOLog("Power management sequence complete.\n");
   }


//start
bool FakeIrisXEFramebuffer::start(IOService* provider) {
    IOLog("FakeIrisXEFramebuffer::start() - Entered\n");

    if (!super::start(provider)) {
        IOLog("FakeIrisXEFramebuffer::start() - super::start() failed\n");
        return false;
    }


    /*
    // Improved ACPI device discovery with proper IOACPIPlatformDevice handling
    IOACPIPlatformDevice *acpiDev = nullptr;
    const char* targetADR = "0x00020000"; // GFX0 _ADR value

    // Primary ACPI walk with enhanced type safety
    IOLog("🧭 Starting ACPI plane walk from PCI device\n");
    IORegistryEntry *acpiWalker = pciDevice;
    while ((acpiWalker = acpiWalker->getParentEntry(gIOACPIPlane)) != nullptr) {
        // Safe name and location extraction
        const char* name = acpiWalker->getName() ? acpiWalker->getName() : "unnamed";
        const char* location = acpiWalker->getLocation() ? acpiWalker->getLocation() : "no-location";
        IOLog(" → Visiting ACPI node: %s @ %s\n", name, location);

        // Check if this is actually an IOACPIPlatformDevice
        IOACPIPlatformDevice *platformDev = OSDynamicCast(IOACPIPlatformDevice, acpiWalker);
        if (!platformDev) {
            IOLog("   |_ Not an IOACPIPlatformDevice, skipping\n");
            continue;
        }

        // Safe _ADR checking
        OSData* adr = OSDynamicCast(OSData, platformDev->getProperty("_ADR"));
        if (adr && adr->getLength() == sizeof(uint32_t)) {
            uint32_t adrVal = 0;
            bcopy(adr->getBytesNoCopy(), &adrVal, sizeof(adrVal));
            IOLog("   |_ _ADR = 0x%08X\n", adrVal);
            
            if (adrVal == 0x00020000) {
                IOLog("✅ Found matching _ADR (0x00020000) - potential GFX0\n");
                acpiDev = platformDev;
                acpiDev->retain(); // Take ownership
                break;
            }
        }
    }

    
     // Fallback paths with proper type casting
    if (!acpiDev) {
        IOLog("🔍 Trying fallback paths to locate GFX0\n");
        
        // Array of possible GFX0 paths to try
        const char* gfx0Paths[] = {
            "/_SB/PC00/GFX0",
            "/_SB/PCI0/GFX0",
            "/_SB/GFX0",
            nullptr // Sentinel
        };

        for (int i = 0; gfx0Paths[i] != nullptr; i++) {
            IORegistryEntry *gfx0Entry = IORegistryEntry::fromPath(gfx0Paths[i], gIOACPIPlane);
            if (!gfx0Entry) continue;
            
            // Proper type casting check
            IOACPIPlatformDevice *platformDev = OSDynamicCast(IOACPIPlatformDevice, gfx0Entry);
            if (platformDev) {
                IOLog("✅ Found GFX0 at path: %s\n", gfx0Paths[i]);
                acpiDev = platformDev;
                acpiDev->retain(); // Take ownership
                gfx0Entry->release(); // Release the intermediate object
                break;
            }
            gfx0Entry->release(); // Clean up if cast failed
        }
    }
    
    
    
    // Final ACPI device validation and _DSM evaluation
    if (acpiDev) {
        const char* acpiName = acpiDev->getName();
        IOLog("✅ Successfully located ACPI parent: %s\n", acpiName ? acpiName : "unnamed");

        // --- _DSM Evaluation with Enhanced Safety ---
        uint8_t dsmUUID[16] = {
            0xA0, 0x12, 0x93, 0x6E, 0x50, 0x9A, 0x4C, 0x5B,
            0x8A, 0x21, 0x3A, 0x36, 0x15, 0x29, 0x2C, 0x79
        };

        OSObject *dsmParams[4] = { nullptr };
        bool dsmSuccess = false;

        do { // Error handling scope
            // Create UUID parameter
            dsmParams[0] = OSData::withBytes(dsmUUID, sizeof(dsmUUID));
            if (!dsmParams[0]) {
                IOLog("❌ Failed to create UUID parameter\n");
                break;
            }

            // Create revision parameter
            dsmParams[1] = OSNumber::withNumber(0ULL, 32);
            if (!dsmParams[1]) {
                IOLog("❌ Failed to create revision parameter\n");
                break;
            }

            // Create function index parameter
            dsmParams[2] = OSNumber::withNumber(1ULL, 32);
            if (!dsmParams[2]) {
                IOLog("❌ Failed to create function index\n");
                break;
            }

            // Create empty package parameter
            dsmParams[3] = OSArray::withCapacity(0);
            if (!dsmParams[3]) {
                IOLog("❌ Failed to create package parameter\n");
                break;
            }

     
        
            // Evaluate _DSM - now properly on IOACPIPlatformDevice
            OSObject *dsmResult = nullptr;
            IOReturn ret = acpiDev->evaluateObject("_DSM", &dsmResult, dsmParams, 4);
            
            if (ret == kIOReturnSuccess && dsmResult) {
                IOLog("✅ _DSM evaluation succeeded (%s)\n",
                      dsmResult->getMetaClass()->getClassName());
                dsmResult->release();
                dsmSuccess = true;
            } else {
                IOLog("❌ _DSM evaluation failed (0x%x)\n", ret);
            }
        } while (false);

        // Cleanup parameters
        for (int i = 0; i < 4; i++) {
            if (dsmParams[i]) dsmParams[i]->release();
        }

        if (!dsmSuccess) {
            IOLog("⚠️ _DSM method did not execute successfully\n");
        }

        acpiDev->release(); // Release our retained reference
    } else {
        IOLog("❌ Failed to locate GFX0 ACPI device\n");
        setProperty("ACPI-Status", "GFX0-not-found");
    }
    */
    
    
    pciDevice = OSDynamicCast(IOPCIDevice, provider);
    if (!pciDevice) {
        IOLog("❌ Provider is not IOPCIDevice\n");
        return false;
    }
    pciDevice->retain();

    
    
    
    
    
    // 1️⃣ Open PCI device
    IOLog("📦 Opening PCI device...\n");
    if (!pciDevice->open(this)) {
        IOLog("❌ Failed to open PCI device\n");
        OSSafeReleaseNULL(pciDevice);
        return false;
    }

    
    
    IOLog("⚠️ Skipping enablePCIPowerManagement (causes freeze on some systems)\n");
   /*
    // 2️⃣ Optional: PCI Power Management (safe here)
    IOLog("⚡️ Powering up PCI device...\n");
    if (pciDevice->hasPCIPowerManagement()) {
        IOLog("Using modern power management\n");
        pciDevice->enablePCIPowerManagement(kPCIPMCSPowerStateD0);
    }
    IOSleep(100);
*/
    
    
    
    //verify BAR0 satus
    IOLog("veryfying bar0 adddress");
    uint32_t bar0=pciDevice->configRead32(kIOPCIConfigBaseAddress0);
    IOLog("PCI BAR0 = 0x%08X\n",bar0);
    
    
    if ((bar0 & ~0xf)==0){
        
        IOLog("bar0 invalid, device not assigned memory");
        pciDevice->close(this);
        OSSafeReleaseNULL(pciDevice);
        return false;
    }
    
    
    // 3️⃣ Enable PCI Memory and IO
    IOLog("🛠 Enabling PCI memory and IO...\n");
    pciDevice->setMemoryEnable(true);
    pciDevice->setIOEnable(false);
    IOSleep(10); // Let it propagate


    // 4️⃣ Confirm enablement via config space
    uint16_t command = pciDevice->configRead16(kIOPCIConfigCommand);
    bool memEnabled = command & kIOPCICommandMemorySpace;
    bool ioEnabled  = command & kIOPCICommandIOSpace;
    if (!memEnabled) {
        IOLog("❌ Resource enable failed (PCI command: 0x%04X, mem:%d, io:%d)\n", command, memEnabled, ioEnabled);
        pciDevice->close(this);
        OSSafeReleaseNULL(pciDevice);
        return false;
    }
    IOLog("✅ PCI resource enable succeeded (command: 0x%04X)\n", command);

    
    
    
    
    IOLog("About to Map Bar0");
    // 5️⃣ MMIO BAR0 mapping
    if (pciDevice->getDeviceMemoryCount() < 1) {
        IOLog("❌ No MMIO regions available\n");
        pciDevice->close(this);
        OSSafeReleaseNULL(pciDevice);
        return false;
    }

    mmioMap = pciDevice->mapDeviceMemoryWithIndex(0);
    if (!mmioMap || mmioMap->getLength() < 0x100000) {
        IOLog("❌ BAR0 mapping failed or too small\n");
        OSSafeReleaseNULL(mmioMap);
        pciDevice->close(this);
        OSSafeReleaseNULL(pciDevice);
        return false;
    }
    mmioBase = (volatile uint8_t*)mmioMap->getVirtualAddress();
    IOLog("✅ BAR0 mapped successfully (len: 0x%llX)\n", mmioMap->getLength());

    // ❌⛔️ NEVER TOUCH MMIO YET – GPU NOT READY!

    
    
    
    
    // 6️⃣ Power management: wake up GPU
    IOLog("🔌 Calling initPowerManagement()...\n");
    initPowerManagement();
    IOLog("✅ Returned from initPowerManagement()\n");

    
    
    
    // 7️⃣ Now it's SAFE to do MMIO read/write
    uint32_t zeroReg = safeMMIORead(0x0000);
    IOLog("MMIO[0x0000] = 0x%08X\n", zeroReg);

    uint32_t ack = safeMMIORead(0xA188);
    IOLog("FORCEWAKE_ACK: 0x%08X\n", ack);

    
    
    
    // 8️⃣ Optional: MMIO register dump
    IOLog("🔍 MMIO Register Dump:\n");
    for (uint32_t offset = 0; offset < 0x40; offset += 4) {
        uint32_t val = safeMMIORead(offset);
        IOLog("[0x%04X] = 0x%08X\n", offset, val);
    }

    
    
    // 9️⃣ Mark graphics/display online
    controllerEnabled = true;
    displayOnline = true;

    
    
    
    
    /*
    //publish display
    displayInjectTimer = IOTimerEventSource::timerEventSource(this, [](OSObject* owner, IOTimerEventSource* sender) {
        auto fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
        if (!fb) return;

        IOLog("🕒 Delayed display injection\n");
        fb->publishDisplay();
    });
    if (displayInjectTimer && workLoop) {
        workLoop->addEventSource(displayInjectTimer);
        displayInjectTimer->setTimeoutMS(2000); // Delay by 2s
    }
*/
    
    
    
    
    
    
    // === GPU Acceleration Properties ===
    {
        // Required properties for Quartz Extreme / Core Animation
               OSArray* accelTypes = OSArray::withCapacity(4); // Increased capacity for more types
               if (accelTypes) {
                   accelTypes->setObject(OSSymbol::withCString("Accel"));
                   accelTypes->setObject(OSSymbol::withCString("Metal"));
                   accelTypes->setObject(OSSymbol::withCString("OpenGL"));
                   accelTypes->setObject(OSSymbol::withCString("Quartz"));
                   setProperty("IOAccelTypes", accelTypes);
                   accelTypes->release();
                   IOLog("GPU Acceleration Properties used\n"); // Added newline for cleaner log
               }

    }

    
    
    
    
    //display bounds
    OSDictionary* bounds = OSDictionary::withCapacity(2);
    bounds->setObject("Height", OSNumber::withNumber(1080, 32));
    bounds->setObject("Width", OSNumber::withNumber(1920, 32));
    setProperty("IOFramebufferBounds", bounds);
    bounds->release();


    
    
    
    
    // Constants
    const uint32_t width = 1920;
    const uint32_t height = 1080;
    const uint32_t bpp = 4; // bytes per pixel (ARGB8888)
    uint32_t rawSize = width * height * bpp;
    
     //   uint32_t alignedSize = (rawSize + 4095) & ~4095; // Round up to 4K boundary
        
    IOLog("🧠 Allocating framebuffer memory: %ux%u, %u bytes\n", width, height, rawSize);

    // Allocate framebuffer memory (shared, aligned)
    framebufferMemory = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(
        kernel_task,
        kIOMemoryPageable | kIODirectionInOut,
        rawSize,
        0xFFFFFFFFULL  // ← force under 4GB
    );

    
    // Validate
    if (!framebufferMemory) {
        IOLog("❌ Failed to allocate framebuffer memory\n");
        return false;
    }

    // Prepare for DMA access
    if (framebufferMemory->prepare() != kIOReturnSuccess) {
        IOLog("❌ Failed to prepare framebuffer memory\n");
        framebufferMemory->release();
        framebufferMemory = nullptr;
        return false;
    }

    // Zero the memory (safely)
    void* fbAddr = framebufferMemory->getBytesNoCopy();
    if (fbAddr) bzero(fbAddr, rawSize);

    IOLog("✅ Framebuffer allocated and initialized successfully\n");
    

  
    // Add to start() after VRAM allocation
    if (framebufferMemory) {
        // Create a memory descriptor for the entire range
        framebufferSurface = IOMemoryDescriptor::withAddressRange(
            framebufferMemory->getPhysicalAddress(),
            framebufferMemory->getLength(),
            kIODirectionInOut,
            kernel_task
        );
        
        if (framebufferSurface) {
            setProperty("IOFBSurface", framebufferSurface);
            IOLog("✅ Framebuffer surface registered\n");
        } else {
            IOLog("❌ Failed to create framebuffer surface\n");
        }
    }
    
    
    uint8_t* fbptr=(uint8_t*)framebufferMemory->getBytesNoCopy();
    if (fbptr) {
        IOLog("Writing solid blue to framebuffer\n");
        for (uint32_t y=0; y < 1080; y++){
            for (uint32_t x=0; x < 1920; x++) {
                uint32_t offset = (y *1920 +x)*4;
                fbptr[offset +0] = 0xFF;
                fbptr[offset +1] = 0x00;
                fbptr[offset +2] = 0x00;
                fbptr[offset +3] = 0x00;
            }
            
        }
        IOLog("Framebuffer color fill done\n");
        
    }
    
    

    // Create work loop and command gate
    workLoop = IOWorkLoop::workLoop();
    if (!workLoop) {
        IOLog("❌ Failed to create work loop\n");
        return false;
    }

       
       commandGate = IOCommandGate::commandGate(this);
       if (!commandGate || workLoop->addEventSource(commandGate) != kIOReturnSuccess) {
           IOLog("Failed to create command gate\n");
           return false;
       }
    

    
    
    
    
    
    cursorMemory = IOBufferMemoryDescriptor::withOptions(
        kIOMemoryKernelUserShared | kIODirectionInOut,
        4096,  // 4KB for cursor
        page_size
    );
    if (cursorMemory) {
        bzero(cursorMemory->getBytesNoCopy(), 4096);
        IOLog("Cursor memory allocated\n");
    } else {
        IOLog("Failed to allocate cursor memory\n");
    }
    
    

    // === Dummy EDID ===
    {
        static const uint8_t fakeEDID[128] = {
            0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,
            0x4C, 0x83, 0x40, 0x56, 0x01, 0x01, 0x01, 0x01,
            0x0D, 0x1A, 0x01, 0x03, 0x80, 0x30, 0x1B, 0x78,
            0x0A, 0xEE, 0x95, 0xA3, 0x54, 0x4C, 0x99, 0x26,
            0x0F, 0x50, 0x54, 0x00, 0x00, 0x00, 0x01, 0x01,
            0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
            0x01, 0x01, 0x02, 0x3A, 0x80, 0x18, 0x71, 0x38,
            0x2D, 0x40, 0x58, 0x2C, 0x45, 0x00, 0x13, 0x2A,
            0x21, 0x00, 0x00, 0x1E, 0x00, 0x00, 0x00, 0xFD,
            0x00, 0x38, 0x4B, 0x1E, 0x51, 0x11, 0x00, 0x0A,
            0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x00, 0x00,
            0x00, 0xFC, 0x00, 0x46, 0x61, 0x6B, 0x65, 0x20,
            0x44, 0x69, 0x73, 0x70, 0x6C, 0x61, 0x79, 0x0A,
            0x00, 0x00, 0x00, 0xFF, 0x00, 0x31, 0x32, 0x33,
            0x34, 0x35, 0x36, 0x0A, 0x20, 0x20, 0x20, 0x20,
            0x20, 0x20, 0x01, 0x55
        };
        OSData *edidData = OSData::withBytes(fakeEDID, sizeof(fakeEDID));
        if (edidData) {
            setProperty("IODisplayEDID", OSData::withBytes(fakeEDID, 128));
            edidData->release();
            IOLog("📺 Fake EDID published\n");
        }

        setProperty("IOFBHasPreferredEDID", kOSBooleanTrue);
    }

    

    // Display Timing Information

    const uint8_t timingData[] = {
        0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00,  // Header
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Serial
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Basic params
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,  // Detailed timings
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00   // Extension blocks
    };

    OSData* timingInfo = OSData::withBytes(timingData, sizeof(timingData));
    if (timingInfo) {
        setProperty("IOTimingInformation", timingInfo);
        timingInfo->release();
        IOLog("Added timing information\n");
    }
    
    
    
    timerLock = IOLockAlloc();
    if (!timerLock) {
        IOLog("❌ Failed to allocate timerLock\n");
        return false;
    }

    // Setup vsyncTimer for screen refresh (simulation only)
    if (workLoop && !isInactive()) {  // Add safety check
        vsyncTimer = IOTimerEventSource::timerEventSource(
            this,
            OSMemberFunctionCast(IOTimerEventSource::Action, this, &FakeIrisXEFramebuffer::vsyncTimerFired)
        );
        if (vsyncTimer) {
            if (workLoop->addEventSource(vsyncTimer) == kIOReturnSuccess) {
                vsyncTimer->setTimeoutMS(16);
            } else {
                vsyncTimer->release();
                vsyncTimer = nullptr;
            }
        }
    }
    
 
 
      
    OSDictionary* displayInfo = OSDictionary::withCapacity(1);
    OSDictionary* brightness = OSDictionary::withCapacity(1);
    brightness->setObject("min", OSNumber::withNumber(10, 32));
    brightness->setObject("max", OSNumber::withNumber(255, 32));
    displayInfo->setObject("brightness", brightness);
    setProperty("IODisplayParameters", displayInfo);

    setProperty("default-width", 1920, 32);
    setProperty("default-height", 1080, 32);
    
    setProperty("IOFBDepth", bpp * 8, 32);

    
    //extra property

    setProperty("IONameMatched", "GFX0");
    setProperty("IOFBHasBacklight", kOSBooleanTrue);

    
    
   
    /*
    setProperty("IOUserClientClass", "IOFramebufferUserClient");
    setProperty("IOFramebufferSharedUserClient", OSSymbol::withCString("IOAccelSharedUserClient")); // Common spoof
*/
    
    
    

    setProperty("IOFBUserClientClass", "IOFramebufferUserClient");
    setProperty("IOAccelRevision", 2, 32);
    setProperty("IOAccelVRAMSize", 128 * 1024 * 1024, 128);
    setProperty("IOFBNeedsRefresh", kOSBooleanTrue);
    setProperty("IOFBConfig", 1, 32);
    setProperty("IOFBDisplayModeID", OSNumber::withNumber((UInt64)0, 32)); // Default mode
    setProperty("IOFBStartupModeTimingID", OSNumber::withNumber((UInt64)0, 32));
    
    setProperty("IOFBWidth", width, 32);
    setProperty("IOFBHeight", height, 32);
    setProperty("IOFBBytesPerRow", width * bpp, 32);
     
     
     
    setProperty("AAPL,boot-display", kOSBooleanTrue);
    
    
    setProperty("IOGVAHEVCDecode", true);
    setProperty("IOGVAHEVCEncode", true);
    setProperty("IOGVAH264Decode", true);
    setProperty("IOGVADisplayPipeCapabilities", OSNumber::withNumber(0xFFFFFFFF, 32));

    setProperty("IOGraphicsFlags", 0x3); // or 0x51

    setProperty("IOFramebufferOpenGLIndex", OSNumber::withNumber((UInt64)0, 32));

    
    setProperty("IOFBAccelerator", this);
    setProperty("IOFBDependentID", this); // self pointer (important)
    setProperty("IOFBDependentIndex",OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOPMFeatures", OSDictionary::withCapacity(2));
    setProperty("IOFBCursorInfo", OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOFBTransform", OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOFBGammaHeaderSize", OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOFBWaitCursorFrames", OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOFBClientConnectIndex", 1, 32);
    setProperty("IOFBCursorSupported", kOSBooleanFalse);
    setProperty(kIOFBHardwareCursorSupportedKey, kOSBooleanFalse);
    if (framebufferMemory) {
        setProperty("IOFBMemorySize", framebufferMemory->getLength(), 32);
    }
    setProperty(kIOFBScalerInfoKey, 0ULL, 32);
    setProperty(kIOFBDisplayModeCountKey, static_cast<UInt64>(1), 32);
    setProperty("IOFramebufferOpenGLIndex", 0ULL, 32);
    setProperty("IOFBCurrentPixelCount", 1920ULL * 1080ULL, 128);
    setProperty("IOFBCursorScale", 0x10000, 32); // 1.0 fixed-point
    setProperty("IOFBDisplayCount", (uint32_t)1);
    setProperty("IOFBVerbose", kOSBooleanTrue);
    setProperty("IOFBTranslucencySupport", kOSBooleanTrue);
    setProperty("IOGVAVTEnable", kOSBooleanTrue);
    setProperty("IOSupportsCLUTs", kOSBooleanTrue);
    setProperty("IOAccelEnabled", kOSBooleanTrue);
    setProperty("AAPL,HasPanel", kOSBooleanTrue);
    setProperty("built-in", kOSBooleanTrue);
    setProperty("AAPL,gray-page", kOSBooleanTrue);
    setProperty("IOFBTransparency", kOSBooleanTrue);
    setProperty("IOSurfaceSupport", kOSBooleanTrue);
    setProperty("IOSurfaceAccelerator", kOSBooleanTrue);
    setProperty("IOGraphicsHasAccelerator", kOSBooleanTrue);
    setProperty("IOProviderClass", "IOPCIDevice");

    setProperty("IOAccelIndex", static_cast<unsigned int>(0), 32);
  
    
    setProperty("MetalPluginName", OSSymbol::withCString("AppleIntelICLLPGraphicsMTLDriver"));
    setProperty("MetalPluginClassName", OSSymbol::withCString("AppleIntelICLLPGraphicsMTLDriver")); // Should match MetalPluginName
    setProperty("IOVARendererID", OSNumber::withNumber(0x80860100ULL, 32)); // Use ULL for consistency
    setProperty("MetalPluginVersion", OSNumber::withNumber(120ULL, 32));
    setProperty("MetalFeatures", OSNumber::withNumber(0xFFFFFFFFULL, 32)); // Use ULL for consistency
       
    
    setProperty("VRAM,totalsize", framebufferMemory->getLength(), 32);

  
    setProperty("IOGVAVTDecodeSupport", kOSBooleanTrue);
    setProperty("IOVARendererID", OSNumber::withNumber(0x80860100, 32));
    
    setProperty("IOFBConnectFlags", kIOConnectionBuiltIn, 32);
    setProperty("IOFBCurrentConnection", OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOFBOnline", 1, 32);
    setProperty("model", OSData::withBytes("Intel Iris Xe Graphics", 22));
    setProperty("IOFramebufferDisplayIndex", (uint32_t)0);
    setProperty("IOFBStartupDisplayModeTimingID", OSNumber::withNumber((UInt64)0, 32));
    setProperty("IOFramebufferDisplay", kOSBooleanTrue);     // Very important
    setProperty("IOFBIsMainDisplay", kOSBooleanTrue);

    
    
    
    OSDictionary* fbInfo = OSDictionary::withCapacity(1);
    fbInfo->setObject("FramebufferType", OSSymbol::withCString("IntelIrisXe"));
    setProperty("IOFramebufferInformation", fbInfo);
    fbInfo->release();
    
    
    OSArray* pixelFormats = OSArray::withCapacity(2);
    pixelFormats->setObject(OSSymbol::withCString("BGRA8888"));
    pixelFormats->setObject(OSSymbol::withCString("RGBA8888"));
    setProperty("IOFBSupportedPixelFormats", pixelFormats);
    pixelFormats->release();

    

    // Cursor sizes
    OSNumber* cursorSizeNum = OSNumber::withNumber(32ULL, 32); // Renamed variable to avoid conflict
       if (cursorSizeNum) {
           OSObject* values[1] = { cursorSizeNum };
           OSArray* array = OSArray::withObjects((const OSObject**)values, 1);
           if (array) {
               setProperty("IOFBCursorSizes", array);
               array->release();
           }
           cursorSizeNum->release();
       }

    setProperty("IOFBNumberOfConnections", 1, 32);
    setProperty("IOFBConnectionFlags", OSNumber::withNumber(0x100, 32));
    setProperty("IOFramebufferIsConsole", kOSBooleanTrue);
    setProperty("IOConsoleMode", (uint64_t)0); // Use mode 0 as default

    
    IORegistryEntry* display = IORegistryEntry::fromPath("IOService:/IOResources/IOBacklightDisplay");
    if (display) {
        display->setProperty("IOBacklightDisplay", kOSBooleanFalse);
        display->release();
        IOLog("Forced display wake\n");
    }

    
    
    //real display mode dictionary
    OSDictionary* modeInfo = OSDictionary::withCapacity(4);
    modeInfo->setObject(kIOFBWidthKey, OSNumber::withNumber(1920, 32));
    modeInfo->setObject(kIOFBHeightKey, OSNumber::withNumber(1080, 32));
    modeInfo->setObject(kIOFBRefreshRateKey, OSNumber::withNumber(60 << 16, 32)); // 60Hz fixed-point
    modeInfo->setObject(kIOFBFlagsKey, OSNumber::withNumber((uint64_t)0, 32));
    OSDictionary* allModes = OSDictionary::withCapacity(1);
    allModes->setObject("0", modeInfo);
    IOLog("real display mode dictionary used");
    modeInfo->release();
    setProperty("IOFBDisplayModeInformation", allModes);
    allModes->release();
    
    
    
   
    
    // 1️⃣ Basic display setup (no MMIO yet)
    setNumberOfDisplays(1);
    IOLog("After setNumberOfDisplays\n");

    setDisplayMode(0, 0);
    setAttributeForConnection(0, kConnectionEnable, 1);
    publishDisplay();
    IOLog("✅ publishDisplay() called\n");
    
    setProperty(kIOFramebufferConsoleKey, kOSBooleanTrue);
    IOLog("✅ Marked as IOFramebufferConsole\n");

    
    
    // 2️⃣ Power management initialization
    IOLog("⚡️ starting power management...\n");
    PMinit();
    registerPowerDriver(this, powerStates, kNumPowerStates);
    getProvider()->joinPMtree(this);

    // ✅ Allocate the lock BEFORE any transition happens
    powerLock = IOLockAlloc();
    if (!powerLock) {
        IOLog("❌ Failed to allocate power lock\n");
        return false;
    }

    makeUsable();                   // ✅ Must come before power state transition
    driverActive = true;            // ✅ Also required
    changePowerStateTo(kPowerStateOn); // ✅ Now setPowerState() will use valid lock

    
    bool isMain = (getProperty("IOFBIsMainDisplay") == kOSBooleanTrue);
    bool online = (getProperty("IOFBOnline") == kOSBooleanTrue);
    IOLog("🟢 flushDisplay: main=%d, online=%d\n", isMain, online);
    flushDisplay();   // now actually commits

    
    
    // 5️⃣ Final registration so WindowServer + clients attach
    fullyInitialized = true;
    registerService();
    IOLog("✅ registerService() called at end of start()\n");
   
   

    // Clean up temporary OSObjects
    if (displayInfo) displayInfo->release();
    if (brightness)   brightness->release();

    
    
    IOLog("🏁 FakeIrisXEFramebuffer::start() - Completed\n");
    return true;

}




void FakeIrisXEFramebuffer::stop(IOService* provider) {
    IOLog("FakeIrisXEFramebuffer::stop() called\n");
    
    
    IOLockLock(timerLock);
    driverActive = false;  // Signal all timers to stop
    IOLockUnlock(timerLock);
    
    
    if (vsyncTimer) {
           vsyncTimer->cancelTimeout();
           if (workLoop) {
               workLoop->removeEventSource(vsyncTimer);
           }
           vsyncTimer->release();
           vsyncTimer = nullptr;
       }
       
       if (displayInjectTimer) {
           displayInjectTimer->cancelTimeout();
           if (workLoop) {
               workLoop->removeEventSource(displayInjectTimer);
           }
           displayInjectTimer->release();
           displayInjectTimer = nullptr;
       }

       PMstop();

    OSSafeReleaseNULL(framebufferMemory);
    OSSafeReleaseNULL(mmioMap);
    OSSafeReleaseNULL(vramMemory);
    
    if (pciDevice) {
        pciDevice->close(this);
        OSSafeReleaseNULL(pciDevice);  // ✅ Consistent with other releases
    }

    
    shuttingDown = true;
    if(vsyncTimer) vsyncTimer->cancelTimeout();
    
        super::stop(provider);
}



void FakeIrisXEFramebuffer::startIOFB() {
    IOLog("FakeIrisXEFramebuffer::startIOFB() called\n");
    deliverFramebufferNotification(0, kIOFBNotifyDisplayModeChange, nullptr); // This is enough

}

 
 

void FakeIrisXEFramebuffer::free() {
    IOLog("FakeIrisXEFramebuffer::free() called\n");
    
    if (powerLock) {
        IOLockFree(powerLock);
        powerLock = nullptr;
    }

    if (timerLock) {
        IOLockFree(timerLock);
        timerLock = nullptr;
    }

    driverActive = false;
    
    if (workLoop) {
           if (commandGate) {
               workLoop->removeEventSource(commandGate);
               commandGate->release();
               commandGate = nullptr;
           }
           if (vsyncTimer) {
               workLoop->removeEventSource(vsyncTimer);
               vsyncTimer->release();
               vsyncTimer = nullptr;
           }
           workLoop->release();
           workLoop = nullptr;
       }
       
       OSSafeReleaseNULL(framebufferSurface);
       OSSafeReleaseNULL(cursorMemory);

    super::free();
}



IOWorkLoop* FakeIrisXEFramebuffer::getWorkLoop() const {
    return workLoop;
}

bool FakeIrisXEFramebuffer::initializeHardware() {
    IOLog("initializeHardware() called\n");

    // Map MMIO space
    mmioMap = pciDevice->mapDeviceMemoryWithIndex(0);

    if (!mmioMap) {
        IOLog("Failed to map MMIO\n");
        return false;
    }
    
    mmioBase = reinterpret_cast<volatile UInt8*>(mmioMap->getVirtualAddress());

    // Initialize hardware here
    // ... GPU-specific initialization code ...
    
    return true;
}

bool FakeIrisXEFramebuffer::setupVRAM() {
    IOLog("setupVRAM() called\n");
    
    
    vramSize = 1920 * 1080 * 4;  // Ensure this is set
    vramMemory = IOBufferMemoryDescriptor::withOptions(
        kIODirectionInOut | kIOMemoryKernelUserShared,
        vramSize,
        PAGE_SIZE);
    
    if (!vramMemory) {
        IOLog("Failed to allocate VRAM\n");
        return false;
    }
    
    // Clear screen to black
       void* fbAddr = framebufferMemory->getBytesNoCopy();
       if (fbAddr) {
           memset(fbAddr, 0, framebufferMemory->getLength());
       } else {
           IOLog("❌ setupVRAM(): Failed to get framebuffer memory address.\n");
           return false;
       }
    
    
    return true;
}






IOReturn FakeIrisXEFramebuffer::enableController() {
    IOLog("🟢 enableController() called\n");

    if (!mmioBase || !framebufferMemory) {
        IOLog("❌ MMIO or framebuffer not set\n");
        return kIOReturnError;
    }

    // Treat BAR0 as an array of uint32_t
    volatile uint32_t* regs = reinterpret_cast<volatile uint32_t*>(mmioBase);

    IOPhysicalAddress phys = framebufferMemory->getPhysicalAddress();
    uint32_t low  = (uint32_t) phys;
    uint32_t high = (uint32_t)(phys >> 32);

    // Offsets in bytes → divide by 4 to get dword index
    const size_t IDX_SURF_LOW   = 0x70184 / 4;
    const size_t IDX_SURF_HIGH  = 0x70188 / 4;
    const size_t IDX_STRIDE     = 0x7018C / 4;
    const size_t IDX_CNTR       = 0x70180 / 4;
    const size_t IDX_SRC        = 0x6001C / 4;
    const size_t IDX_PIPE_CTRL  = 0x70000 / 4;
    const size_t IDX_STATUS     = 0x44000 / 4;

    // Before you touch anything else, sanity-check device is alive:
    uint32_t status = regs[IDX_STATUS];
    if (!(status & 0x80000000)) {
        IOLog("❌ Device status not ready (0x%08X)\n", status);
        return kIOReturnNotReady;
    }

    // Program scanout
    IOLog("📺 Programming DSPASURF = 0x%08X / 0x%08X\n", low, high);
    regs[IDX_SURF_LOW]  = low;
    regs[IDX_SURF_HIGH] = high;

    uint32_t stride = 1920 * 4;
    IOLog("🔨 Stride = %u bytes\n", stride);
    regs[IDX_STRIDE] = stride;

    IOLog("⚙️ DSPACNTR = enable pipe A, BGRA8888\n");
    regs[IDX_CNTR] = 0xC0100000; // BGRA8888 format

    IOLog("🔍 Setting source size %ux%u\n", 1920, 1080);
    regs[IDX_SRC] = ((1920 - 1) << 16) | (1080 - 1);

    IOLog("🚦 Enabling pipe control\n");
    regs[IDX_PIPE_CTRL] = 0x80000000;

    
    IOLog("🔍 DSPACNTR = 0x%08X\n", regs[IDX_CNTR]);
    IOLog("🔍 DSPASURF = 0x%08X\n", regs[IDX_SURF_LOW]);
    IOLog("🔍 PIPEASRC = 0x%08X\n", regs[IDX_SRC]);
    IOLog("🔍 PIPEASTAT = 0x%08X\n", regs[0x70024 / 4]);

    
    IOSleep(10);  // let the HW catch up

    
    IOLog("✅ enableController() complete\n");
    return kIOReturnSuccess;
}


void FakeIrisXEFramebuffer::disableController() {
    if(!mmioBase) return;
    
    // Disable pipe (example offsets)
    const uint32_t DSPACNTR = 0x70180;
    *(volatile uint32_t*)(mmioBase + DSPACNTR) &= ~0x80000000;
    IOLog("✅ Controller disabled\n");
}


IOReturn FakeIrisXEFramebuffer::getOnlineState(IOIndex connectIndex, bool* online)
{
    if (connectIndex != 0) return kIOReturnBadArgument;
    *online = displayOnline;
    IOLog("getOnlineState: %d\n", displayOnline);
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::setAttributeForConnection(IOIndex, IOSelect, uintptr_t) {
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::setOnlineState(IOIndex connectIndex, bool online)
{
    if (connectIndex != 0) return kIOReturnBadArgument;
       IOLog("setOnlineState: %d\n", online);
       displayOnline = online; // Update member variable
       return kIOReturnSuccess;
   }


bool FakeIrisXEFramebuffer::setupDisplayModes() {
    IOLog("setupDisplayModes() called\n");
    
       setNumberOfDisplays(1);
       return true;
}






bool FakeIrisXEFramebuffer::isConsoleDevice() const {
    return true;
}



IOReturn FakeIrisXEFramebuffer::setOnline(bool online) {

    IOLog("setOnline() called\n");
        displayOnline = online;
        // Return kIOReturnSuccess for both online/offline states if you handle them.
        return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::notifyServer(IOSelect message, void* data, size_t dataSize) {
    IOLog("notifyServer() called: message = 0x%08X\n", message);

    switch (message) {
        case kIOFBNotifyDisplayModeChange:
            // TODO: implement display mode change handling
            return kIOReturnSuccess;

        default:
            IOLog("❓ notifyServer(): unhandled message = 0x%08X\n", message);
            return kIOReturnUnsupported;
    }
}




IOReturn FakeIrisXEFramebuffer::getTimingInfoForDisplayMode(
    IODisplayModeID displayMode,
    IOTimingInformation* infoOut)
{
    bzero(infoOut, sizeof(IOTimingInformation));
    infoOut->appleTimingID = kIOTimingIDDefault;
    infoOut->flags = kIOTimingInfoValid_AppleTimingID;
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::setCLUTWithEntries(IOColorEntry* colors,
                                                   UInt32 firstIndex,
                                                   UInt32 numEntries,
                                                   IOOptionBits options) {
    IOLog("setCLUTWithEntries() called\n");

    return kIOReturnUnsupported;
}
    

IOReturn FakeIrisXEFramebuffer::setGammaTable(UInt32 channelCount,
                                              UInt32 dataCount,
                                              UInt32 dataWidth,
                                              void* data) {
    IOLog("setGammaTable() called\n");

    return kIOReturnUnsupported;
}

IOReturn FakeIrisXEFramebuffer::getGammaTable(UInt32 channelCount,
                                              UInt32* dataCount,
                                              UInt32* dataWidth,
                                              void** data) {
    IOLog("getGammaTable() called\n");
    return kIOReturnUnsupported;
}


IOReturn FakeIrisXEFramebuffer::getAttributeForConnection(IOIndex connectIndex,
                                                         IOSelect attribute,
                                                         uintptr_t* value)
{
    IOLog("getAttributeForConnection(%d, 0x%x)\n", connectIndex, attribute);
    
    if (connectIndex != 0) return kIOReturnBadArgument; // Only handle connection 0

        switch (attribute) {
            case kConnectionSupportsAppleSense:
            case kConnectionSupportsLLDDCSense:
            case kConnectionSupportsHLDDCSense:
            case kConnectionSupportsDDCSense:
            case kIOCapturedAttribute:
                *value = 0;
                return kIOReturnSuccess;

            case kIOHardwareCursorAttribute:
                *value = 1; // Report hardware cursor support (even if fake)
                return kIOReturnSuccess;

            case kConnectionFlags:
                *value = kIOConnectionBuiltIn; // Only report built-in for simplicity
                return kIOReturnSuccess;

            default:
                return kIOReturnUnsupported;
        }
    }





const char* FakeIrisXEFramebuffer::getPixelFormats(void) {
    return "BGRA8888";
}


    
    
IOReturn FakeIrisXEFramebuffer::setCursorImage(void* cursorImage) {
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible) {
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::registerForInterruptType(IOSelect interruptType, IOFBInterruptProc proc, void* ref, void** interruptRef) {
    
    return kIOReturnSuccess;
}

IOReturn FakeIrisXEFramebuffer::unregisterInterrupt(void* interruptRef) {
    return kIOReturnSuccess;
}




IOReturn FakeIrisXEFramebuffer::createAccelTask(mach_port_t* port) {

    return kIOReturnUnsupported;
}





void FakeIrisXEFramebuffer::publishDisplay() {
    
    if (!fullyInitialized) {
         IOLog("publishDisplay called before full initialization\n");
         return;
     }
    
    IOLog("FakeIrisXEFramebuffer::publishDisplay() called\n");

    if(displayPublished) return; // ✅ Prevent duplicates
        displayPublished = true;
    
    IOService* display = new IOService;
    if (display && display->init()) {
        display->setName("display0");
        display->setProperty("DisplayProductID", (uint32_t)0x717);
        display->setProperty("DisplayVendorID", (uint32_t)0x756e6b6e); // 'unkn'
        display->setProperty("IODisplayPrefsKey", "IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GFX0@2/display0");
        display->setProperty("IOFramebuffer", OSDynamicCast(OSObject, this));
        display->attach(this);
        //display->registerService();
        display->release(); // ✅ prevent leak
    }
    else {
            IOLog("❌ publishDisplay(): Failed to create or initialize childDisplay\n");
            if (display) display->release();
            return; // Exit if childDisplay failed
        }
    
    
    // CRITICAL FIX: Correctly build the IOFramebufferDisplays dictionary structure
      OSDictionary* singleDisplayInfo = OSDictionary::withCapacity(1);
      if (!singleDisplayInfo) {
          IOLog("❌ publishDisplay(): Failed to create singleDisplayInfo dictionary\n");
          return;
      }
      singleDisplayInfo->setObject("IODisplayPrefsKey", OSSymbol::withCString("IOService:/AppleACPIPlatformExpert/PCI0@0/AppleACPIPCI/GFX0@2/display0"));

      OSArray* displayList = OSArray::withCapacity(1);
      if (!displayList) {
          IOLog("❌ publishDisplay(): Failed to create displayList array\n");
          singleDisplayInfo->release();
          return;
      }
      displayList->setObject(singleDisplayInfo); // Add the display info dictionary to the array

      OSDictionary* fbDisplays = OSDictionary::withCapacity(1);
      if (!fbDisplays) {
          IOLog("❌ publishDisplay(): Failed to create fbDisplays dictionary\n");
          singleDisplayInfo->release();
          displayList->release();
          return;
      }
      fbDisplays->setObject("display0", displayList); // Add the array to the main dictionary

      setProperty("IOFramebufferDisplays", fbDisplays);

      IOLog("✅ publishDisplay(): display0 and IOFramebufferDisplays now published\n");

      // Release local references (IOKit retains them)
      singleDisplayInfo->release();
      displayList->release();
      fbDisplays->release();


      // Notify
      flushDisplay(); // Call your flushDisplay() helper
      IOWorkLoop* wl = getWorkLoop();
      if (wl) {
          
          IOTimerEventSource* notificationTimer = IOTimerEventSource::timerEventSource(
              this,
              [](OSObject* owner, IOTimerEventSource* sender) {
                  auto fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
                  if (fb && fb->driverActive) {  // ✅ Check driver state
                      fb->deliverFramebufferNotification(0, kIOFBNotifyDisplayAdded, nullptr);
                      fb->deliverFramebufferNotification(0, kIOFBConfigChanged, nullptr);
                  }
                  // ✅ Self-cleanup: remove from work loop after firing
                  if (sender && fb && fb->workLoop) {
                      fb->workLoop->removeEventSource(sender);
                  }
              }
          );

          if (notificationTimer && wl) {
              wl->addEventSource(notificationTimer);
              notificationTimer->setTimeoutMS(500);
              notificationTimer->release();  // ✅ Release local reference
          }
      }
    
}
    

void FakeIrisXEFramebuffer::vsyncTimerFired(OSObject* owner, IOTimerEventSource* sender)
{
    auto fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
    if (!fb) return;

    IOLockLock(fb->timerLock);

    if (!fb->driverActive || fb->isInactive() || !fb->fullyInitialized) {
        IOLockUnlock(fb->timerLock);
        return;
    }

    // Optional: log trace or mark vsync time
    IOLog("🔁 vsyncTimerFired\n");

    // Notify vsync interrupt source
    if (fb->vsyncSource) {
        fb->vsyncSource->interruptOccurred(nullptr, nullptr, 0);
    }

    fb->deliverFramebufferNotification(0, kIOFBVsyncNotification, nullptr);

    // Reschedule
    if (fb->vsyncTimer && fb->driverActive) {
        fb->vsyncTimer->setTimeoutMS(16); // simulate ~60Hz
    }

    IOLockUnlock(fb->timerLock);
}



bool FakeIrisXEFramebuffer::setupWorkLoop() {
    workLoop = IOWorkLoop::workLoop();
    if (!workLoop) {
        IOLog("Failed to create work loop\n");
        return false;
    }

    commandGate = IOCommandGate::commandGate(this);
    if (!commandGate) {
        IOLog("Failed to create command gate\n");
        workLoop->release();
        workLoop = nullptr;
        return false;
    }
    
    // FIX: Check if addEventSource succeeds
    if (workLoop->addEventSource(commandGate) != kIOReturnSuccess) {
        IOLog("Failed to add command gate to work loop\n");
        commandGate->release();
        commandGate = nullptr;
        workLoop->release();
        workLoop = nullptr;
        return false;
    }
    
    return true;
}



    
void FakeIrisXEFramebuffer::vsyncOccurred(OSObject* owner, IOInterruptEventSource* src, int count)
{
    auto fb = OSDynamicCast(FakeIrisXEFramebuffer, owner);
        if (!fb) return;
        // Notify WindowServer
        fb->deliverFramebufferNotification(0, kIOFBVsyncNotification, nullptr);
    }





IOReturn FakeIrisXEFramebuffer::setDisplayMode(IODisplayModeID mode, IOIndex depth) {
    if (mode != 0 || depth != 0)
        return kIOReturnUnsupportedMode;

    currentMode = mode;
    currentDepth = depth;
    
    return kIOReturnSuccess;
}



IODeviceMemory* FakeIrisXEFramebuffer::getVRAMRange()
{
    IOLog("FakeIrisXEFramebuffer::getVRAMRange()\n");

    if (!framebufferMemory)
        return nullptr;

    IOMemoryMap* map = framebufferMemory->map();
    if (!map) {
        IOLog("❌ Could not map framebuffer memory\n");
        return nullptr;
    }

    
    
    IOPhysicalAddress physAddr = framebufferMemory->getPhysicalAddress();
    IOByteCount length = framebufferMemory->getLength();
    
    IOLog("getVRAMRange(): pyhsical address = 0x%11x, length = 0x%1x\n", (uint64_t)physAddr, length);

    return IODeviceMemory::withRange(physAddr, length);
}




IOReturn FakeIrisXEFramebuffer::createSharedCursor(
    IOIndex,
    int) {
    IOLog("createSharedCursor() called\n");
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::setBounds(IOIndex index, IOGBounds *bounds) {
    IOLog("setBounds() called\n");
    if (bounds) {
        bounds->minx = 0;
        bounds->miny = 0;
        bounds->maxx = 1920;
        bounds->maxy = 1080;
    }
    return kIOReturnSuccess;
}





IOReturn FakeIrisXEFramebuffer::clientMemoryForType(UInt32 type, UInt32* flags, IOMemoryDescriptor** memory) {
    IOLog("FakeIrisXEFramebuffer::clientMemoryForType() - type: %u\n", type);

    if (type == kIOFBCursorMemory && cursorMemory) {
        cursorMemory->retain();
        *memory = cursorMemory;
        if (flags) *flags = 0;
        return kIOReturnSuccess;
    }

    if (type == kIOFBSystemAperture && framebufferMemory) {
        framebufferMemory->retain();
        *memory = framebufferMemory;
        if (flags) *flags = kIOMapReadOnly;
        return kIOReturnSuccess;
    }

    if (type == kIOFBVRAMMemory && framebufferSurface) {
        framebufferSurface->retain();
        *memory = framebufferSurface;
        if (flags) *flags = 0;
        return kIOReturnSuccess;
    }

    return kIOReturnUnsupported;
}




IOReturn FakeIrisXEFramebuffer::flushDisplay(void)
{
    // 1. Add hardware state verification
    if (!driverActive || !mmioBase) {
        IOLog("⚠️ flushDisplay: Driver not active\n");
        return kIOReturnNotReady;
    }

    // 2. Memory barrier to ensure writes complete
    OSMemoryBarrier();
    
    // 3. Optional: Trigger hardware flush if needed
    // safeMMIOWrite(0x70080, 0x1); // Example pipe flush register
    
    IOLog("🌀 flushDisplay() called - frame committed\n");
    return kIOReturnSuccess;
}





void FakeIrisXEFramebuffer::deliverFramebufferNotification(IOIndex index, UInt32 event, void* info) {
    IOLog("📩 deliverFramebufferNotification() index=%u event=0x%08X\n", index, event);
    
    
    //super::deliverFramebufferNotification(index, info);

    // The base class version only takes (index, info)
        super::deliverFramebufferNotification(index, info);
        
        // Handle specific events if needed
        switch (event) {
            case kIOFBNotifyDisplayModeChange:
                // Handle mode change
                break;
            case kIOFBVsyncNotification:
                // Handle vsync
                break;
        }
    }
    





IOReturn FakeIrisXEFramebuffer::setNumberOfDisplays(UInt32 count)
{
    
    
    IOLog("🖥️ setNumberOfDisplays(%u)\n", count);
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::flushFramebuffer() {
    IOLog("flushFramebuffer() called\n");
    
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::setPowerState(unsigned long powerStateOrdinal, IOService* whatDevice) {
    IOLog("💡 setPowerState() called: %lu\n", powerStateOrdinal);
    
    IOReturn result = IOPMAckImplied;

    if (!powerLock) {
        IOLog("❌ setPowerState() called before powerLock was allocated — skipping\n");
        return result;
    }

    IOLockLock(powerLock);

    if (powerStateOrdinal == kPowerStateOn) {
        IOLog("💡 Powering ON device\n");

        if (!framebufferMemory || !mmioBase) {
            IOLog("❌ framebufferMemory or mmioBase is null — cannot enable controller\n");
            goto exit;
        }

        if (!controllerEnabled) {
            IOLog("Calling enableController()...\n");
            if (enableController() != kIOReturnSuccess) {
                IOLog("❌ enableController() failed in setPowerState()\n");
                goto exit;
            }
            controllerEnabled = true;
        }
    } else {
        IOLog("🔌 Powering OFF device\n");
        if (controllerEnabled) {
            disableController();
            controllerEnabled = false;
        }
    }

exit:
    IOLockUnlock(powerLock);
    return result;
}







IOItemCount FakeIrisXEFramebuffer::getDisplayModeCount(void)
{
    return 1; // Must return at least 1 mode
}


IOReturn FakeIrisXEFramebuffer::getDisplayModes(IODisplayModeID *allDisplayModes) {
    if (allDisplayModes)
        allDisplayModes[0] = 0;
    
    if (!allDisplayModes) {
        IOLog("⚠️ getDisplayModes(): null pointer\n");
        return kIOReturnSuccess;
    }

    
    return kIOReturnSuccess;
}



UInt64 FakeIrisXEFramebuffer::getPixelFormatsForDisplayMode(IODisplayModeID displayMode, IOIndex depth) {
    IOLog("getPixelFormatsForDisplayMode() called\n");
    return 1ULL << 0;
}


IOReturn FakeIrisXEFramebuffer::getPixelInformation(
    IODisplayModeID displayMode,
    IOIndex depth,
    IOPixelAperture aperture,
    IOPixelInformation* info)
{
    if (!info) return kIOReturnBadArgument;
    if (displayMode != 0 || depth != 0) return kIOReturnUnsupportedMode;

    bzero(info, sizeof(IOPixelInformation));
    
    // BGRA8888 format (Intel GPU native)
      strlcpy(info->pixelFormat, "BGRA8888", sizeof(info->pixelFormat));
      info->flags = 0;
      info->pixelType = kIORGBDirectPixels;
      info->componentCount = 3;
      info->bitsPerComponent = 8;
      info->bitsPerPixel = 32;
      info->bytesPerRow = 1920 * 4;
      info->activeWidth = 1920;
      info->activeHeight = 1080;
      
      // Correct BGRA bit masks for Intel GPU
      info->componentMasks[0] = 0x00FF0000;  // Red (bits 16-23)
      info->componentMasks[1] = 0x0000FF00;  // Green (bits 8-15)
      info->componentMasks[2] = 0x000000FF;  // Blue (bits 0-7)
      info->componentMasks[3] = 0xFF000000;  // Alpha (bits 24-31)
    
    IOLog("✅ getPixelInformation(): BGRA8888, 1920x1080, 32bpp\n");
    
    if (displayMode != 0 || depth != 0) {
        IOLog("❌ getPixelInformation(): Unsupported displayMode=%d, depth=%d\n", displayMode, depth);
        return kIOReturnUnsupportedMode;
    }

    return kIOReturnSuccess;
    
}


IOIndex FakeIrisXEFramebuffer::getAperture() const {
    return kIOFBSystemAperture;
}




IODeviceMemory* FakeIrisXEFramebuffer::getApertureRange(IOPixelAperture aperture) {
    IOLog("✅ getApertureRange() called with aperture = %d\n", aperture);

    if (aperture != kIOFBSystemAperture) {
        IOLog("❌ Unsupported aperture requested\n");
        return nullptr;
    }

    return getVRAMRange(); // ✅ Clean and works
}






IOReturn FakeIrisXEFramebuffer::getFramebufferOffsetForX_Y(IOPixelAperture aperture, SInt32 x, SInt32 y, UInt32 *offset)
{
    if (!offset || aperture != kIOFBSystemAperture)
        return kIOReturnBadArgument;

    // Linear framebuffer, so no X/Y offset — return 0
    *offset = 0;
    return kIOReturnSuccess;
}


IOReturn FakeIrisXEFramebuffer::getCurrentDisplayMode(IODisplayModeID* mode, IOIndex* depth) {
    if (mode) *mode = currentMode;
    if (depth) *depth = currentDepth;
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::setMode(IODisplayModeID displayMode, IOOptionBits options, UInt32 depth) {
    IOLog("FakeIrisXEFramebuffer::setMode() CALLED! Mode ID: %u, Options: 0x%x, Depth: %u\n", displayMode, options, depth);

    // In a real scenario, you'd configure the hardware here based on displayMode.
    // For now, just track the current mode.
    currentMode = displayMode;
    currentDepth = depth;

    // Example of setting a property if you want to reflect the chosen pixel format
    if (depth == 32) { // Assuming 32-bit depth means BGRA8888
        setProperty("IOFBCurrentPixelFormat", OSString::withCString("BGRA8888"));
    } else if (depth == 16) { // Assuming 16-bit depth means RGB565
        setProperty("IOFBCurrentPixelFormat", OSString::withCString("RGB565"));
    }

    // Return success even if you don't fully configure hardware yet.
    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::getInformationForDisplayMode(
    IODisplayModeID mode,
    IODisplayModeInformation* info)
{
    IOLog("🧪 getInformationForDisplayMode() CALLED for mode = %d\n", mode);

    if (!info || mode != 0) {
        IOLog("🛑 Invalid info pointer or mode not supported\n");
        return kIOReturnUnsupportedMode;
    }

    bzero(info, sizeof(IODisplayModeInformation));
    info->maxDepthIndex = 0;
    info->nominalWidth = 1920;
    info->nominalHeight = 1080;
    info->refreshRate = (60 << 16); // 60Hz in fixed-point
   
    // Set timing information
        info->reserved[0] = kIOTimingIDDefault;
        info->reserved[1] = 0;


    IOLog("✅ Returning display mode info: 1920x1080 @ 60Hz\n");

    return kIOReturnSuccess;
}



IOReturn FakeIrisXEFramebuffer::getStartupDisplayMode(IODisplayModeID *modeID, IOIndex *depth)
{
    IOLog("getStartupDisplayMode() called\n");
    if (modeID) *modeID = 0; // Not 1!
    if (depth)  *depth  = 0;
    return kIOReturnSuccess;
}



UInt32 FakeIrisXEFramebuffer::getConnectionCount() {
    IOLog("getConnectionCount() called\n");
    return 1; // 1 display connection
}



IOReturn FakeIrisXEFramebuffer::setAttribute(IOSelect attribute, uintptr_t value) {
    IOLog("setAttribute(0x%x, 0x%lx)\n", attribute, value);
    
    if (attribute == kIOCaptureAttribute) {
            IOLog("✅ Received kIOCaptureAttribute\n");
            return kIOReturnSuccess;
        }
        return IOFramebuffer::setAttribute(attribute, value);
    }




#include <libkern/libkern.h> // For kern_return_t, kmod_info_t

// Kext entry/exit points
extern "C" kern_return_t FakeIrisXEFramebuffer_start(kmod_info_t *ki, void *data) {
    return KERN_SUCCESS;
}

extern "C" kern_return_t FakeIrisXEFramebuffer_stop(kmod_info_t *ki, void *data) {
    return KERN_SUCCESS;
}
