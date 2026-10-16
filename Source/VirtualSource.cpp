#include "VirtualSource.h"
#include <CoreMIDI/CoreMIDI.h>
#include <algorithm>
#include <mutex>
#include <vector>
#include <unistd.h>
#include <cstdio>

// Sends as MIDI 1.0 Universal MIDI Packets (SysEx7 start/continue/end), the way current drivers deliver
// long SysEx. It must be quick: CoreMIDI drops the rest of a virtual source's SysEx once it has been
// incomplete for a few seconds (measured: ~100 KB at 25 KB/s), so a bank goes out in well under a second.
struct VirtualSource::Impl
{
    MIDIClientRef client = 0;
    MIDIEndpointRef source = 0;
    std::mutex lock;
};

VirtualSource::VirtualSource (const std::string& name) : impl (std::make_unique<Impl>())
{
    CFStringRef cfName = CFStringCreateWithCString (nullptr, name.c_str(), kCFStringEncodingUTF8);
    if (MIDIClientCreate (cfName, nullptr, nullptr, &impl->client) == noErr)
        MIDISourceCreateWithProtocol (impl->client, cfName, kMIDIProtocol_1_0, &impl->source);
    CFRelease (cfName);
}

VirtualSource::~VirtualSource()
{
    if (impl->source != 0)  MIDIEndpointDispose (impl->source);
    if (impl->client != 0)  MIDIClientDispose (impl->client);
}

bool VirtualSource::isValid() const     { return impl->source != 0; }

int VirtualSource::uniqueId() const
{
    SInt32 uid = 0;
    MIDIObjectGetIntegerProperty (impl->source, kMIDIPropertyUniqueID, &uid);
    return (int) uid;
}

void VirtualSource::send (const uint8_t* data, size_t size)
{
    if (size < 2 || data[0] != 0xf0 || data[size - 1] != 0xf7)
        return;

    std::lock_guard<std::mutex> l (impl->lock);
    const uint8_t* payload = data + 1;
    const size_t total = size - 2;
    const size_t numPackets = std::max<size_t> (1, (total + 5) / 6);

    std::vector<uint8_t> buffer (4096);
    auto* list = reinterpret_cast<MIDIEventList*> (buffer.data());
    MIDIEventPacket* packet = MIDIEventListInit (list, kMIDIProtocol_1_0);
    int inList = 0;

    for (size_t i = 0; i < numPackets; ++i)
    {
        const size_t pos = i * 6, n = std::min<size_t> (6, total - std::min (total, pos));
        const uint32_t kind = numPackets == 1 ? 0 : i == 0 ? 1 : i == numPackets - 1 ? 3 : 2;
        uint8_t b[6] = {};
        std::copy (payload + pos, payload + pos + n, b);
        const UInt32 words[2] = { (0x3u << 28) | (kind << 20) | ((uint32_t) n << 16) | ((uint32_t) b[0] << 8) | b[1],
                                  ((uint32_t) b[2] << 24) | ((uint32_t) b[3] << 16) | ((uint32_t) b[4] << 8) | b[5] };
        packet = MIDIEventListAdd (list, buffer.size(), packet, 0, 2, words);
        ++inList;

        if (packet == nullptr || inList == 40 || i == numPackets - 1)
        {
            if (packet == nullptr)
            {
                fprintf (stderr, "VirtualSource: event list full at packet %zu of %zu\n", i, numPackets);
                return;
            }
            const OSStatus err = MIDIReceivedEventList (impl->source, list);
            if (err != noErr)
                fprintf (stderr, "VirtualSource: MIDIReceivedEventList error %d at packet %zu\n", (int) err, i);
            packet = MIDIEventListInit (list, kMIDIProtocol_1_0);
            inList = 0;
            if (i != numPackets - 1)
                usleep (500);       // CoreMIDI cuts off a virtual source's SysEx that is still incomplete after a few seconds
        }
    }
}
