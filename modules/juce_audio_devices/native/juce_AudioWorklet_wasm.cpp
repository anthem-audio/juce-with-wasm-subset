/*
  ==============================================================================

   This file is part of the JUCE framework.
   Copyright (c) Raw Material Software Limited

   JUCE is an open source framework subject to commercial or open source
   licensing.

   By downloading, installing, or using the JUCE framework, or combining the
   JUCE framework with any other source code, object code, content or any other
   copyrightable work, you agree to the terms of the JUCE End User Licence
   Agreement, and all incorporated terms including the JUCE Privacy Policy and
   the JUCE Website Terms of Service, as applicable, which will bind you. If you
   do not agree to the terms of these agreements, we will not license the JUCE
   framework to you, and you must discontinue the installation or download
   process and cease use of the JUCE framework.

   JUCE End User Licence Agreement: https://juce.com/legal/juce-8-licence/
   JUCE Privacy Policy: https://juce.com/juce-privacy-policy
   JUCE Website Terms of Service: https://juce.com/juce-website-terms-of-service/

   Or:

   You may also use this code under the terms of the AGPLv3:
   https://www.gnu.org/licenses/agpl-3.0.en.html

   THE JUCE FRAMEWORK IS PROVIDED "AS IS" WITHOUT ANY WARRANTY, AND ALL
   WARRANTIES, WHETHER EXPRESSED OR IMPLIED, INCLUDING WARRANTY OF
   MERCHANTABILITY OR FITNESS FOR A PARTICULAR PURPOSE, ARE DISCLAIMED.

  ==============================================================================
*/

#include <iostream>
#include <thread>

#include <emscripten.h>
#include <emscripten/atomic.h>

namespace juce
{

class AudioRingBufferWasm {
public:
  uint32_t head;
  uint32_t tail;
  uint32_t capacity;
  uint32_t mask;

  juce::MemoryBlock buffer;

  AudioRingBufferWasm(uint32_t capacityPow2) :
    head(0),
    tail(0),
    capacity(capacityPow2),
    mask(capacityPow2 - 1)
  {
    buffer.setSize(capacity * sizeof(float), false);
    buffer.fillWith(0);
  }

  uint32_t size() const {
    uint32_t currentHead = emscripten_atomic_load_u32((uint32_t*)&head);
    uint32_t currentTail = emscripten_atomic_load_u32((uint32_t*)&tail);
    return (currentHead - currentTail) & mask;
  }

  bool tryEnqueue(float value) {
    uint32_t currentHead = emscripten_atomic_load_u32((uint32_t*)&head);
    uint32_t currentTail = emscripten_atomic_load_u32((uint32_t*)&tail);
    if (((currentHead + 1) & mask) == (currentTail & mask)) {
      // Full
      return false;
    }

    uint32_t nextHead = (currentHead + 1) & mask;
    float* bufferPtr = static_cast<float*>(buffer.getData());
    emscripten_atomic_store_f32((float*)&bufferPtr[currentHead], value);
    emscripten_atomic_store_u32((uint32_t*)&head, nextHead);

    return true;
  }

  bool tryDequeue(float& value) {
    uint32_t currentHead = emscripten_atomic_load_u32((uint32_t*)&head);
    uint32_t currentTail = emscripten_atomic_load_u32((uint32_t*)&tail);
    if ((currentHead & mask) == (currentTail & mask)) {
      // Empty
      return false;
    }

    uint32_t nextTail = (currentTail + 1) & mask;
    float* bufferPtr = static_cast<float*>(buffer.getData());
    value = emscripten_atomic_load_f32((float*)&bufferPtr[currentTail]);
    emscripten_atomic_store_u32((uint32_t*)&tail, nextTail);
    return true;
  }
};

class WebAudioThread : public Thread {
public:
    AudioBuffer<float> outputBuffer;
    AudioBuffer<float> inputBuffer; // Will be zeroed out. Not used yet.

    AudioRingBufferWasm outputRingBuffer;

    MemoryBlock signals;

    AudioIODeviceCallback* callback = nullptr;

    WebAudioThread() : Thread("WebAudioThread"),
            outputBuffer(2, 512),
            outputRingBuffer(4096 * 2), // Interleaved two-channel buffer
            inputBuffer(2, 512),
            signals(1 * sizeof(int32_t), true) {
        inputBuffer.clear();
    }

    ~WebAudioThread() override {}

    void run() override {
        while (!threadShouldExit()) {
            uint32_t* renderSignal = (uint32_t*)signals.getData();
            uint32_t expected = emscripten_atomic_load_u32(renderSignal);

            int r = emscripten_atomic_wait_u32(renderSignal, expected, 1000000);
            if (r == ATOMICS_WAIT_TIMED_OUT) {
                continue;
            }

            if (callback == nullptr) {
                // No callback, nothing to do
                continue;
            }

            // Render until the we have at least 512 * 3 samples in the ring buffer
            while (outputRingBuffer.size() < 512 * 2 * 3) {
                int numSamples = outputBuffer.getNumSamples();
                if (numSamples <= 0) {
                    jassertfalse;
                    // shut down the thread
                    return;
                }

                outputBuffer.clear();

                callback->audioDeviceIOCallbackWithContext(
                    // inputBuffer.getArrayOfReadPointers(),
                    // inputBuffer.getNumChannels(),
                    nullptr,
                    0,
                    outputBuffer.getArrayOfWritePointers(),
                    outputBuffer.getNumChannels(),
                    outputBuffer.getNumSamples(),
                    AudioIODeviceCallbackContext()
                );

                // Copy output buffer to shared ring buffer, interleaved
                for (int i = 0; i < outputBuffer.getNumSamples(); ++i) {
                    for (int chan = 0; chan < outputBuffer.getNumChannels(); ++chan) {
                        auto* readPtr = outputBuffer.getReadPointer(chan);
                        // We don't check for success - the ring buffer must always be big enough
                        // to hold the data we are writing to it, otherwise we have an error elsewhere.
                        outputRingBuffer.tryEnqueue(readPtr[i]);
                    }
                }
            }
        }
    }
};

class WebAudioImpl {
public:
    WebAudioImpl() : audioThread() {}
    ~WebAudioImpl() {}

    void initAudioContextIfNeeded() {
        if (isInitialized)
            return;

        MAIN_THREAD_EM_ASM({
            if (Module.audioContext == undefined) {
                const workletCode = `
class RingBuffer {
    constructor(headPtr, tailPtr, capacity, mask, dataPtr, heap32, heapF32) {
        this.headPtr = headPtr;
        this.tailPtr = tailPtr;
        this.capacity = capacity;
        this.mask = mask;
        this.dataPtr = dataPtr;
        this.heap32 = heap32;
        this.heapF32 = heapF32;

        this.conversionBufferF32 = new Float32Array(1);
        this.conversionBufferI32 = new Int32Array(this.conversionBufferF32.buffer);
    }

    iToF(i) {
        this.conversionBufferI32[0] = i;
        return this.conversionBufferF32[0];
    }

    fToI(f) {
        this.conversionBufferF32[0] = f;
        return this.conversionBufferI32[0];
    }

    size() {
        const head = Atomics.load(this.heap32, this.headPtr / 4);
        const tail = Atomics.load(this.heap32, this.tailPtr / 4);
        return (head - tail) & this.mask;
    }

    tryEnqueue(value) {
        const head = Atomics.load(this.heap32, this.headPtr / 4);
        const tail = Atomics.load(this.heap32, this.tailPtr / 4);
        if (((head + 1) & this.mask) === (tail & this.mask)) {
            // Full
            return false;
        }

        const nextHead = (head + 1) & this.mask;
        Atomics.store(this.heap32, (this.dataPtr / 4) + head, this.fToI(value));
        Atomics.store(this.heap32, this.headPtr / 4, nextHead);
        return true;
    }

    tryDequeue() {
        const head = Atomics.load(this.heap32, this.headPtr / 4);
        const tail = Atomics.load(this.heap32, this.tailPtr / 4);
        if ((head & this.mask) === (tail & this.mask)) {
            // Empty - since this is an audio buffer, return silence
            return 0;
        }

        const nextTail = (tail + 1) & this.mask;
        const value = this.iToF(Atomics.load(this.heap32, (this.dataPtr / 4) + tail));
        Atomics.store(this.heap32, this.tailPtr / 4, nextTail);
        return value;
    }
}

class JuceAppProcessor extends AudioWorkletProcessor {
    constructor(options) {
        super(options);

        this.SIGNAL_IDLE = 0;
        this.SIGNAL_READY_TO_RENDER = 1;
        this.SIGNAL_RENDER_DONE = 2;

        this.port.onmessage = this.handleMessage.bind(this);

        this.ringBuffer = new RingBuffer(
            options.processorOptions.buffer.headPtr,
            options.processorOptions.buffer.tailPtr,
            options.processorOptions.buffer.capacity,
            options.processorOptions.buffer.mask,
            options.processorOptions.buffer.dataPtr,
            options.processorOptions.heap32,
            options.processorOptions.heapF32
        );

        // We have to atomically load from buffersI32 and convert to Float32.
        // These are buffers that allow us to convert without additional memory
        // allocation. conversionBuffersF32 and conversionBuffersI32 both point
        // to the same memory, but with different types.
        this.conversionBufferF32 = new Float32Array(128 * 2); // Buffer size on web is 128 in testing - not sure if this ever varies
        this.conversionBufferI32 = new Int32Array(this.conversionBufferF32.buffer);

        // This is an Int32Array view into the SharedArrayBuffer. It has integer
        // values that are used for synchronizing this audio thread with the
        // main application's audio thread.
        //
        // For now there is just one signal. It is used to signal to the
        // application that a render is needed.
        this.signals = options.processorOptions.signals;
        if (this.signals.length != 1) {
            throw new Error("signals buffer must have length 1");
        }
    }

    handleMessage(event) {
        // Nothing yet
    }

    copyOutputs(outputBuffers) {
        for (let sample = 0; sample < outputBuffers[0].length; sample++) {
            for (let channel = 0; channel < outputBuffers.length; channel++) {
                const outputChannel = outputBuffers[channel];
            
                const value = this.ringBuffer.tryDequeue();
                outputChannel[sample] = value;
            }
        }
    }

    process(inputs, outputs, parameters) {
        const size = this.ringBuffer.size();

        // If we have less than 1024 samples, request more data
        if (size <= 1024 * 2) {
            // Signal to the application that we need more data.
            //
            // This signal takes longer than 128 samples to wake the app's audio
            // thread, so we maintain a buffer of 512 samples to ensure that
            // we don't underflow.
            Atomics.add(this.signals, 0, 1);
            Atomics.notify(this.signals, 0);
        }

        // If we don't have enough data yet, output silence
        if (size < 128 * 2) {
            for (let i = 0; i < outputs[0].length; ++i) {
                const channel = outputs[0][i];
                channel.fill(0);
            }
            return true;
        }

        if (outputs.length != 1) {
            throw new Error("JuceAppProcessor: Only one output is supported");
        }

        const outputBuffers = outputs[0];

        // Copy from last render request
        this.copyOutputs(outputBuffers);

        return true;
    }
}

registerProcessor('juce-app-processor', JuceAppProcessor);
                `;

                const blob = new Blob([workletCode], { type: 'application/javascript' });
                const url = URL.createObjectURL(blob);

                Module.audioContext = new AudioContext();

                Module.audioContext.audioWorklet.addModule(url).then(() => {
                    Module.audioWorkletReady = true;
                }).catch((e) => {
                    Module.audioWorkletReady = false;
                });
            }
        });

        // Wait until the worklet is ready
        while (true) {
            int ready = MAIN_THREAD_EM_ASM_INT({
                if (Module.audioWorkletReady === undefined) {
                    return 0;
                } else if (Module.audioWorkletReady) {
                    return 1;
                } else {
                    return -1;
                }
            });

            if (ready == 1) {
                break;
            } else if (ready == -1) {
                std::cerr << "Failed to load AudioWorklet" << std::endl;
                break;
            }

            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        MAIN_THREAD_EM_ASM({
            const heap32 = Module.HEAP32;
            const heapF32 = Module.HEAPF32;

            const ringBufferHeadPtr = $0;
            const ringBufferTailPtr = $1;
            const ringBufferCapacityPtr = $2;
            const ringBufferMaskPtr = $3;
            const ringBufferDataPtr = $4;
            const signalsPtr = $5;

            const signalsSize = 1;
            const bufferCapacity = Atomics.load(heap32, ringBufferCapacityPtr / 4);
            const bufferMask = Atomics.load(heap32, ringBufferMaskPtr / 4);

            Module.juceNode = new AudioWorkletNode(Module.audioContext, 'juce-app-processor', {
                numberOfOutputs: 1,
                outputChannelCount: [2],
                processorOptions: {
                    heap32: heap32,
                    heapF32: heapF32,
                    buffer: {
                        headPtr: ringBufferHeadPtr,
                        tailPtr: ringBufferTailPtr,
                        capacity: bufferCapacity,
                        mask: bufferMask,
                        dataPtr: ringBufferDataPtr,
                    },
                    signals: heap32.subarray(signalsPtr / 4, signalsPtr / 4 + signalsSize)
                }
            });

            Module.juceNode.connect(Module.audioContext.destination);
        },
            &audioThread.outputRingBuffer.head,
            &audioThread.outputRingBuffer.tail,
            &audioThread.outputRingBuffer.capacity,
            &audioThread.outputRingBuffer.mask,
            audioThread.outputRingBuffer.buffer.getData(),
            audioThread.signals.getData()
        );

        audioThread.startThread();

        // Attach a click handler to resume the audio context if needed
        MAIN_THREAD_EM_ASM({
            function resumeAudioContext() {
                if (Module.audioContext.state === 'suspended') {
                    Module.audioContext.resume();
                }
                document.removeEventListener('click', resumeAudioContext);
            }
            document.addEventListener('click', resumeAudioContext);
        });

        isInitialized = true;
    }

    double getSampleRate() {
        return MAIN_THREAD_EM_ASM_DOUBLE({
            if (Module.audioContext != undefined) {
                return Module.audioContext.sampleRate;
            } else {
                return 0;
            }
        });
    }

    void start(AudioIODeviceCallback* callback) {
        audioThread.callback = callback;
    }

    void stop() {
        // Note: After stopping, start() will not work anymore. This port
        // doesn't support restarting the audio context, as it's not needed for
        // our use case. If needed, this could be fixed.
        EM_ASM({
            Module.audioContext?.close();
        });
        audioThread.callback = nullptr;
        audioThread.stopThread(2000);
    }

    bool isPlaying() const {
        return audioThread.callback != nullptr;
    }

private:
    WebAudioThread audioThread;
    bool isInitialized = false;
};

class WasmAudioIODevice : public AudioIODevice {
private:
    bool isOpenFlag = false;

public:
    WasmAudioIODevice()
        : AudioIODevice("web-audio-device", "Web Audio Device")
    {
        webAudioImpl.initAudioContextIfNeeded();
    }

    StringArray getOutputChannelNames() override {
        return { "Channel 1", "Channel 2" };
    }

    StringArray getInputChannelNames() override {
        return { };
    }

    Array<double> getAvailableSampleRates() override {
        return { webAudioImpl.getSampleRate() };
    }

    Array<int> getAvailableBufferSizes() override {
        return { 512 }; // Should be verified
    }

    int getDefaultBufferSize() override {
        return 512;
    }

    String open (const BigInteger& inputChannels,
                 const BigInteger& outputChannels,
                 double sampleRate,
                 int bufferSizeSamples) override
    {
        isOpenFlag = true;
        return "";
    }

    void close() override {
        isOpenFlag = false;
    }
    
    bool isOpen() override {
        return isOpenFlag;
    }

    void start (AudioIODeviceCallback* callback) override {
        callback->audioDeviceAboutToStart(this);
        webAudioImpl.start(callback);
    }

    void stop() override {
        webAudioImpl.stop();
    }

    bool isPlaying() override {
        return webAudioImpl.isPlaying();
    }

    String getLastError() override {
        return {};
    }

    int getCurrentBufferSizeSamples() override {
        return 512;
    }

    double getCurrentSampleRate() override {
        return webAudioImpl.getSampleRate();
    }

    int getCurrentBitDepth() override {
        return 32;
    }

    BigInteger getActiveOutputChannels() const override {
        return BigInteger().setRange(0, 2, true);
    }

    BigInteger getActiveInputChannels() const override {
        return {};
    }

    int getOutputLatencyInSamples() override {
        // TODO: Web audio API provides this, but we also introduce at least
        // 1024. We should query the web audio API and add 1024 to the result.
        return 1024;
    }

    int getInputLatencyInSamples() override {
        return 0;
    }

private:
    WebAudioImpl webAudioImpl;
};

//==============================================================================
// A no-op device type that reports no inputs/outputs and can't create devices.
class WasmAudioIODeviceType final : public AudioIODeviceType
{
public:
    WasmAudioIODeviceType() : AudioIODeviceType ("Web Audio Device") {}
    ~WasmAudioIODeviceType() override = default;

    void scanForDevices() override {}

    StringArray getDeviceNames (bool wantInputNames = false) const override {
        if (wantInputNames) {
            return { "web-audio-input" };
        } else {
            return { "web-audio-output" };
        }
    }

    int getDefaultDeviceIndex (bool forInput = false) const override {
        return 0;
    }

    int getIndexOfDevice(AudioIODevice* device, bool asInput) const override {
        // We only have one device for input and one for output
        if (!asInput && device != nullptr && device->getName() == "web-audio-output") {
            return 0;
        }

        if (asInput && device != nullptr && device->getName() == "web-audio-input") {
            return 0;
        }

        return -1;
    }

    bool hasSeparateInputsAndOutputs() const override { return true; }

    AudioIODevice* createDevice (const String& /*outputDeviceName*/,
                                 const String& /*inputDeviceName*/) override
    {
        // We only have one device, so ignore the names
        return new WasmAudioIODevice();
    }
};

} // namespace juce
