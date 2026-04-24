// -----------------------------------------------------------------------------
// dev_remote_macos.mm — macOS window screenshot helper.
//
// Shells out to /usr/sbin/screencapture with the game's own NSWindow
// number so we get a tight crop of the game contents, not the whole
// desktop. Lives in its own Objective-C++ TU so dev_remote.cpp stays
// pure C++.
//
// Why shell out instead of grabbing bytes via CGWindowListCreateImage?
//   - screencapture already handles PNG encoding, DPI, profile, etc.
//   - No need to link CoreGraphics + ImageIO + drag in a PNG writer
//   - Returns a finished .png file that Code Puppy can load directly
// -----------------------------------------------------------------------------

#import <Cocoa/Cocoa.h>
#include "sokol_app.h"

#include <cstdio>
#include <cstdlib>
#include <string>

// Forward-declared from dev_remote.cpp. Called from the main thread at
// end-of-frame when a screenshot request is pending.
extern "C" bool dev_remote_capture_window(const char* path);

bool dev_remote_capture_window(const char* path) {
    NSWindow* win = (__bridge NSWindow*)sapp_macos_get_window();
    if (!win) {
        std::fprintf(stderr, "[dev_remote] no NSWindow available\n");
        return false;
    }

    NSInteger wid = [win windowNumber];
    if (wid <= 0) {
        std::fprintf(stderr, "[dev_remote] invalid window number %ld\n", (long)wid);
        return false;
    }

    // -x   silent (no shutter sound)
    // -o   no window shadow (crops cleanly to the content rect)
    // -l<N> target window by number
    //
    // We then pipe through `sips -Z 1280` to cap the longest side at
    // 1280 px. Retina displays otherwise produce 3-5 MB PNGs, which
    // are overkill for dev preview and eat the agent's context when
    // base64'd into a vision call.
    char cmd[1024];
    std::snprintf(cmd, sizeof(cmd),
        "/usr/sbin/screencapture -x -o -l%ld '%s' && "
        "/usr/bin/sips -Z 1280 '%s' --out '%s' >/dev/null",
        (long)wid, path, path, path);
    int rc = std::system(cmd);
    if (rc != 0) {
        std::fprintf(stderr, "[dev_remote] capture pipeline exit %d\n", rc);
        return false;
    }
    return true;
}
