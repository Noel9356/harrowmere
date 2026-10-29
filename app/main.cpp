// The process entry point. Deliberately thin: the composition root - the
// command line, the configuration, the frame loop, the capability
// acquisition, the shutdown sequence - lives in engine::App (app.cpp), so
// this file stays the only one that knows about the process boundary and the
// application stays a named type another assembly could construct and drive
// itself.
#include "app.hpp"

int main(
    int argc, char *argv[]
)
{
    engine::App app;
    return app.run(argc, argv);
}
