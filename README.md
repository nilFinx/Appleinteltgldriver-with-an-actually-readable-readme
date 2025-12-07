# AppleintelTGLDriver.kext

***__Common sense warning!__***

I am NOT trying to contribute to the project. I have never made a Kext for macOS in my life. I am no longer interested in helping any of your OCS + OCAT/OCC EFI. I am not going to give you my EFI for "references". I am not releasing the proppy.lua file.

I don't know what went on my mind to even make this repo, but this repo makes the README slightly more readable.

I don't support the project. To anybody genuinely waiting for this project, it won't happen, at least not until any huge contributor comes, which would be a miracle. I would rather work on virtio-gpu kext instead of fixing ONE iGPU.

A Kext that "improves" support of Iris Xe graphics on macOS. Contributions are very welcome.

This project is not w*rked on regularly, as the owner only knows how to vibe code.

# Status

* Get the Kext to load - DONE

(insert garbage info here)

* Map MMIO + wake - DONE

BAR0 mapped

FORCEWAKE works

FORCEWAKE_ACK confirmed

GT power wells enabled

GT clock domains awaken

"GPU is alive."

* Build a minimal framebuffer - DONE

IOFramebuffer subclass loads

Framebuffer allocated

WindowServer sees our display device

macOS boots to GUI using our framebuffer

have real working display powered entirely by our custom kext.

* Enable the entire Tiger Lake display pipeline - DONE

Pipe A

Transcoder A

Plane 1A

ARGB8888

1920×1080

60 Hz

Stride 7680

eDP panel lit by our code

Internal display runs using our driver.
Screen corruption / 2-split issue fixed.

* Accelerator framework...?? - DONE

FakeIrisXEAccelerator published

IOAccelerator properties exposed

Metal shows “Supported”

AcceleratorUserClient attaches

Shared ring buffer implemented

User-space tools can ping the accelerator

FB mapping → user space works

Kernel sees CLEAR commands

✔ Accelerator stack is working end-to-end.



The remaining steps are:

1. GEM buffer objects
2. GGTT binder
3. Command streamer ring
4. Execlists context
5. GuC firmware
6. BLT engine
7. 3D pipeline
8. Metal integration
These are huge, but not unknown.
We follow Linux i915 (which is open-source).
We follow Intel PRM Vol15–17

