// macOS: confirms the Mac owner (Touch ID, Apple Watch or the Mac password) before saved passwords are used.
#include "credentials.h"
#import <Foundation/Foundation.h>
#import <LocalAuthentication/LocalAuthentication.h>
#include <dispatch/dispatch.h>

bool confirmOwner(const QString &reason)
{
    LAContext *context = [[LAContext alloc] init];
    NSError *error = nil;
    // No password or biometrics set up on this Mac: nothing to confirm, the Keychain still protects the entries.
    if (![context canEvaluatePolicy:LAPolicyDeviceOwnerAuthentication error:&error])
        return true;
    __block bool ok = false;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    // The system shows the prompt in its own process: waiting here (also on the GUI thread) does not block it.
    [context evaluatePolicy:LAPolicyDeviceOwnerAuthentication
            localizedReason:reason.toNSString()
                      reply:^(BOOL success, NSError *) {
                        ok = success;
                        dispatch_semaphore_signal(done);
                      }];
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return ok;
}
