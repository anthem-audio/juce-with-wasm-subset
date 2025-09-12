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

namespace juce
{

class MidiInput::Pimpl {};
class MidiOutput::Pimpl {};

//==============================================================================
// A no-op device type that reports no inputs/outputs and can't create devices.
class WasmAudioIODeviceType final : public AudioIODeviceType
{
public:
    WasmAudioIODeviceType() : AudioIODeviceType ("WASM (stub)") {}
    ~WasmAudioIODeviceType() override = default;

    void scanForDevices() override {}
    StringArray getDeviceNames (bool /*wantInputNames*/ = false) const override { return {}; }
    int getDefaultDeviceIndex (bool /*forInput*/ = false) const override        { return -1; }
    int getIndexOfDevice (AudioIODevice* /*device*/, bool /*asInput*/) const override { return -1; }
    bool hasSeparateInputsAndOutputs() const override                           { return true; }

    AudioIODevice* createDevice (const String& /*outputDeviceName*/,
                                 const String& /*inputDeviceName*/) override
    {
        // No devices can be created in the stub.
        return nullptr;
    }
};

} // namespace juce
