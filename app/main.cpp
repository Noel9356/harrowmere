// The process entry point of Harrowmere. Deliberately thin: the composition
// root - the command line, the configuration, the frame loop, the capability
// acquisition, the shutdown sequence - lives in engine::HarrowmereApp
// (harrowmere_app.cpp), so this file stays the only one that knows about the
// process boundary and the application stays a named type another assembly
// could construct and drive itself.
#include "harrowmere_app.hpp"

int main(
    int argc, char *argv[]
)
{
    engine::HarrowmereApp app;
    return app.run(argc, argv);
}
