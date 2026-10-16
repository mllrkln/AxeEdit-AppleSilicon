#import <Foundation/Foundation.h>
#include "KeepAwake.h"

KeepAwake::KeepAwake (const char* reason)
{
    id token = [[NSProcessInfo processInfo] beginActivityWithOptions: NSActivityUserInitiatedAllowingIdleSystemSleep
                                                              reason: [NSString stringWithUTF8String: reason]];
    activity = (void*) [token retain];
}

KeepAwake::~KeepAwake()
{
    if (id token = (id) activity)
    {
        [[NSProcessInfo processInfo] endActivity: token];
        [token release];
    }
}
