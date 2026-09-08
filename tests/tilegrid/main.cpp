// Apotheosis: entry point of the TileGrid v2 host tests. The exit code is the
// number of failed checks, which is what run-tests.ps1 reports.

#include "check.h"

#include <cstdio>

int main()
{
    tilegrid::runScenarios();
    tilegrid::runFuzz();
    tilegrid::runReplay();

    std::printf("\ntilegrid: %d checks, %d failures\n", ::check::checks, ::check::failures);
    return ::check::failures;
}
