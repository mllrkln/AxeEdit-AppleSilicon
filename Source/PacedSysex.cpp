#include "PacedSysex.h"
#include <CoreMIDI/CoreMIDI.h>
#include <atomic>
#include <mutex>
#include <unistd.h>

struct PacedSysexSender::Impl
{
    MIDIClientRef client = 0;
    MIDISysexSendRequest request {};
    std::vector<uint8_t> data;
    std::function<void (bool)> done;
    std::atomic<bool> busy { false }, cancelled { false };
    std::mutex lock;

    static void completion (MIDISysexSendRequest* r)
    {
        auto* self = static_cast<Impl*> (r->completionRefCon);
        const bool ok = ! self->cancelled && r->bytesToSend == 0;
        std::function<void (bool)> callback;
        {
            std::lock_guard<std::mutex> l (self->lock);
            callback = std::move (self->done);
            self->busy = false;
        }
        if (callback)
            callback (ok);
    }
};

PacedSysexSender::PacedSysexSender() : impl (std::make_unique<Impl>()) {}

PacedSysexSender::~PacedSysexSender()
{
    cancel();
    for (int i = 0; i < 200 && impl->busy; ++i)     // CoreMIDI still owns the request until it completes
        usleep (10000);
    if (impl->client != 0)
        MIDIClientDispose (impl->client);
}

bool PacedSysexSender::start (int destinationIndex, std::vector<uint8_t> data, std::function<void (bool)> done)
{
    if (impl->busy || data.empty() || destinationIndex < 0 || destinationIndex >= (int) MIDIGetNumberOfDestinations())
        return false;

    const MIDIEndpointRef dest = MIDIGetDestination ((ItemCount) destinationIndex);
    if (dest == 0)
        return false;

    if (impl->client == 0 && MIDIClientCreate (CFSTR ("Axe-Edit transfers"), nullptr, nullptr, &impl->client) != noErr)
        return false;

    std::lock_guard<std::mutex> l (impl->lock);
    impl->data = std::move (data);
    impl->done = std::move (done);
    impl->cancelled = false;
    impl->request = {};
    impl->request.destination = dest;
    impl->request.data = impl->data.data();
    impl->request.bytesToSend = (UInt32) impl->data.size();
    impl->request.complete = false;
    impl->request.completionProc = &Impl::completion;
    impl->request.completionRefCon = impl.get();
    impl->busy = true;

    if (MIDISendSysex (&impl->request) != noErr)
    {
        impl->busy = false;
        impl->done = nullptr;
        return false;
    }
    return true;
}

bool PacedSysexSender::busy() const     { return impl->busy; }

double PacedSysexSender::progress() const
{
    if (! impl->busy || impl->data.empty())
        return impl->busy ? 0.0 : 1.0;
    return 1.0 - (double) impl->request.bytesToSend / (double) impl->data.size();
}

void PacedSysexSender::cancel()
{
    if (impl->busy)
    {
        impl->cancelled = true;
        impl->request.complete = true;      // tells CoreMIDI to stop sending
    }
}
