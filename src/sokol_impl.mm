// -----------------------------------------------------------------------------
// sokol_impl.mm — single translation unit that compiles all sokol libraries.
//
// Why .mm and not .cpp? Because on macOS, sokol_gfx + sokol_app pull in
// <Metal/Metal.h> and <Foundation/Foundation.h>, which contain Objective-C
// syntax (@class, @protocol). Those only parse under Objective-C or
// Objective-C++. Everywhere else in the project we stay in pure C++.
//
// DRY/SRP: this file owns the SOKOL_IMPL expansion so no other TU duplicates
// it. Everyone else just #includes the sokol headers as declarations.
// -----------------------------------------------------------------------------

#define SOKOL_IMPL

// Backend is selected by CMake via a -DSOKOL_METAL / -DSOKOL_GLCORE / etc.

#include "sokol_log.h"
#include "sokol_gfx.h"
#include "sokol_app.h"
#include "sokol_glue.h"
#include "sokol_time.h"
#include "sokol_debugtext.h"
