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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <emscripten.h>
#include <emscripten/webaudio.h>

namespace juce
{


// One AudioContext/worklet is reused by the module's virtual audio device.
// Its stack has module lifetime: Emscripten's hybrid worklet runtime registers
// pthread metadata in it, and does not provide a matching thread teardown API.
class WebAudioImpl
{
    static constexpr int numOutputChannels = 2;
    static constexpr const char* processorName = "juce-wasm-processor";

    struct State
    {
        alignas (16) std::array<uint8_t, 1024 * 1024> workletStack {};
        std::atomic<AudioIODeviceCallback*> callback { nullptr };
        std::atomic<bool> rt_inCallback { false };
        // 0 = initializing, 1 = ready, -1 = failed.
        std::atomic<int> initialization { 0 };
        std::atomic<bool> initializationStarted { false };
        std::atomic<bool> closing { false };
        EMSCRIPTEN_WEBAUDIO_T context = 0;
        EMSCRIPTEN_WEBAUDIO_T node = 0;
        int sampleRate = 0;
        int blockSize = 128;
        int outputLatency = 0;
        bool closeStarted = false; // Browser main thread only.
    };

    static State& getSharedState()
    {
        static State sharedState;
        return sharedState;
    }

    State& state = getSharedState();
    bool initializationTimedOut = false;

    static void onMainThread (void (*function) (void*), void* argument)
    {
        MAIN_THREAD_EM_ASM ({ getWasmTableEntry ($0) ($1); }, function, argument);
    }

    static void releaseContextHandle (void* userData)
    {
        auto& s = *static_cast<State*> (userData);
        // The context is already suspended, so this API's suspend() resolves
        // immediately. The JS closure below retains it until close() finishes.
        emscripten_destroy_audio_context (s.context);
    }

    static void closeOnMainThread (void* userData)
    {
        auto& s = *static_cast<State*> (userData);

        if (s.initialization.load() == 0 || s.closeStarted)
            return;

        s.closeStarted = true;

        if (s.node != 0)
            emscripten_destroy_web_audio_node (s.node);

        if (s.context == 0)
            return;

        EM_ASM ({
            const context = emscriptenGetAudioObject ($0);
            context.suspend().then (() => {
                getWasmTableEntry ($1) ($2);
                return context.close();
            }).catch (error => {
                console.error ('Failed to close JUCE AudioWorklet', error);
            });
        }, s.context, &releaseContextHandle, &s);
    }

    static void completeInitialization (State* state, bool success)
    {
        auto& s = *state;
        s.initialization.store (success ? 1 : -1, std::memory_order_release);

        if (s.closing.load())
            closeOnMainThread (state);
    }

    static bool rt_process (int, const AudioSampleFrame*, int numOutputs,
                            AudioSampleFrame* outputs, int, const AudioParamFrame*, void* userData)
    {
        auto& s = *static_cast<State*> (userData);
        // Sequential consistency across these two atomics makes stop() a fence
        // for callback lifetime, including a callback starting concurrently.
        s.rt_inCallback.store (true);
        auto* callback = s.callback.load();

        if (numOutputs == 1)
        {
            auto& output = outputs[0];
            if (callback != nullptr
                && output.numberOfChannels == numOutputChannels
                && output.samplesPerChannel == s.blockSize)
            {
                float* channels[numOutputChannels] {
                    output.data, output.data + output.samplesPerChannel
                };
                callback->audioDeviceIOCallbackWithContext (
                    nullptr, 0, channels, numOutputChannels,
                    output.samplesPerChannel, AudioIODeviceCallbackContext {});
            }
            else
            {
                std::fill_n (output.data,
                             output.numberOfChannels * output.samplesPerChannel, 0.0f);
            }
        }

        s.rt_inCallback.store (false);
        return true;
    }

    static void processorCreated (EMSCRIPTEN_WEBAUDIO_T context, bool success, void* userData)
    {
        auto* state = static_cast<State*> (userData);
        auto& s = *state;
        if (! success)
        {
            completeInitialization (state, false);
            return;
        }

        int outputChannels[] { numOutputChannels };
        EmscriptenAudioWorkletNodeCreateOptions options {};
        options.numberOfInputs = 0;
        options.numberOfOutputs = 1;
        options.outputChannelCounts = outputChannels;
        options.channelCount = numOutputChannels;
        options.channelCountMode = WEBAUDIO_CHANNEL_COUNT_MODE_EXPLICIT;
        s.node = emscripten_create_wasm_audio_worklet_node (
            context, processorName, &options, &rt_process, &s);

        if (s.node != 0)
            emscripten_audio_node_connect (s.node, context, 0, 0);

        completeInitialization (state, s.node != 0);
    }

    static void threadInitialized (EMSCRIPTEN_WEBAUDIO_T context, bool success, void* userData)
    {
        auto* state = static_cast<State*> (userData);
        if (! success)
        {
            completeInitialization (state, false);
            return;
        }

        WebAudioWorkletProcessorCreateOptions options {};
        options.name = processorName;
        emscripten_create_wasm_audio_worklet_processor_async (
            context, &options, &processorCreated, state);
    }

    static void initializeOnMainThread (void* userData)
    {
        auto& s = *static_cast<State*> (userData);
        EmscriptenWebAudioCreateAttributes options {};
        options.latencyHint = "interactive";
        s.context = emscripten_create_audio_context (&options);
        s.sampleRate = emscripten_audio_context_sample_rate (s.context);
        s.blockSize = emscripten_audio_context_quantum_size (s.context);
        s.outputLatency = EM_ASM_INT ({
            const context = emscriptenGetAudioObject ($0);
            return Math.ceil (((context.baseLatency || 0) + (context.outputLatency || 0))
                              * context.sampleRate);
        }, s.context);

        // User activation must be handled in the browser event itself, rather
        // than asynchronously proxying it through the engine's pthread.
        EM_ASM ({
            const context = emscriptenGetAudioObject ($0);
            const resume = () => {
                if (context.state === 'suspended')
                    context.resume().catch (() => {});
            };
            document.addEventListener ('pointerdown', resume, true);
            document.addEventListener ('keydown', resume, true);
            context.addEventListener ('statechange', () => {
                if (context.state === 'closed') {
                    document.removeEventListener ('pointerdown', resume, true);
                    document.removeEventListener ('keydown', resume, true);
                }
            });
        }, s.context);

        emscripten_start_wasm_audio_worklet_thread_async (
            s.context, s.workletStack.data(), s.workletStack.size(),
            &threadInitialized, &s);
    }

public:
    WebAudioImpl()
    {
        jassert (! emscripten_is_main_browser_thread());
        if (! state.initializationStarted.exchange (true))
        {
            std::atexit ([] {
                auto& s = getSharedState();
                s.callback.store (nullptr);
                s.closing.store (true);
                onMainThread (&closeOnMainThread, &s);
            });
            onMainThread (&initializeOnMainThread, &state);
        }
        for (int i = 0; state.initialization.load (std::memory_order_acquire) == 0; ++i)
        {
            if (i >= 1000)
            {
                initializationTimedOut = true;
                break;
            }
            Thread::sleep (10);
        }
    }

    ~WebAudioImpl()
    {
        stop();
    }

    bool isReady() const
    {
        return ! initializationTimedOut
               && state.initialization.load (std::memory_order_acquire) == 1;
    }

    double getSampleRate() const { return isReady() ? state.sampleRate : 0.0; }
    int getBlockSize() const { return isReady() ? state.blockSize : 128; }
    int getOutputLatency() const { return isReady() ? state.outputLatency : 0; }

    void start (AudioIODeviceCallback* callback)
    {
        state.callback.store (callback);
        onMainThread ([] (void* data) {
            auto& s = *static_cast<State*> (data);
            EM_ASM ({
                emscriptenGetAudioObject ($0).resume().catch (() => {});
            }, s.context);
        }, &state);
    }

    void stop()
    {
        auto* previous = state.callback.exchange (nullptr);
        if (previous == nullptr)
            return;

        while (state.rt_inCallback.load())
            Thread::sleep (1);

        previous->audioDeviceStopped();
        onMainThread ([] (void* data) {
            auto& s = *static_cast<State*> (data);
            EM_ASM ({
                emscriptenGetAudioObject ($0).suspend().catch (() => {});
            }, s.context);
        }, &state);
    }

    bool isPlaying() const { return state.callback.load() != nullptr; }
};

class WasmAudioIODevice : public AudioIODevice
{
    WebAudioImpl webAudioImpl;
    bool isOpenFlag = false;

public:
    WasmAudioIODevice() : AudioIODevice ("web-audio-output", "Web Audio Device") {}
    ~WasmAudioIODevice() override { close(); }

    StringArray getOutputChannelNames() override { return { "Channel 1", "Channel 2" }; }
    StringArray getInputChannelNames() override { return {}; }
    Array<double> getAvailableSampleRates() override { return { webAudioImpl.getSampleRate() }; }
    Array<int> getAvailableBufferSizes() override { return { webAudioImpl.getBlockSize() }; }
    int getDefaultBufferSize() override { return webAudioImpl.getBlockSize(); }

    String open (const BigInteger& inputChannels, const BigInteger& outputChannels,
                 double sampleRate, int bufferSizeSamples) override
    {
        close();
        if (! webAudioImpl.isReady())
            return "Failed to initialize the WebAssembly AudioWorklet. See the browser console.";
        if (! inputChannels.isZero() || outputChannels.getHighestBit() > 1)
            return "Web Audio supports stereo output and no input.";
        if (sampleRate != webAudioImpl.getSampleRate()
            || bufferSizeSamples != webAudioImpl.getBlockSize())
            return "Web Audio requires the AudioContext sample rate and render quantum.";
        isOpenFlag = true;
        return {};
    }

    void close() override { stop(); isOpenFlag = false; }
    bool isOpen() override { return isOpenFlag; }

    void start (AudioIODeviceCallback* callback) override
    {
        stop();
        if (isOpenFlag && callback != nullptr)
        {
            callback->audioDeviceAboutToStart (this);
            webAudioImpl.start (callback);
        }
    }

    void stop() override { webAudioImpl.stop(); }
    bool isPlaying() override { return webAudioImpl.isPlaying(); }
    String getLastError() override { return webAudioImpl.isReady() ? String{} : "AudioWorklet initialization failed"; }
    int getCurrentBufferSizeSamples() override { return webAudioImpl.getBlockSize(); }
    double getCurrentSampleRate() override { return webAudioImpl.getSampleRate(); }
    int getCurrentBitDepth() override { return 32; }
    BigInteger getActiveOutputChannels() const override
    {
        return isOpenFlag ? BigInteger().setRange (0, 2, true) : BigInteger();
    }
    BigInteger getActiveInputChannels() const override { return {}; }
    int getOutputLatencyInSamples() override { return webAudioImpl.getOutputLatency(); }
    int getInputLatencyInSamples() override { return 0; }
};

//==============================================================================
class WasmAudioIODeviceType final : public AudioIODeviceType
{
public:
    WasmAudioIODeviceType() : AudioIODeviceType ("Web Audio Device") {}
    ~WasmAudioIODeviceType() override = default;

    void scanForDevices() override {}

    StringArray getDeviceNames (bool wantInputNames = false) const override {
        if (wantInputNames) {
            return {};
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
