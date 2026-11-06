// Used by `pio test -e native` inside GaggiMate only (scripts/check_against_gaggimate.sh): that env builds just the test
// folder, so the pure sources are compiled in here. scripts/run_host_tests.sh lists the sources itself and does not
// compile this file.
#include "../../src/display/plugins/grinderlink/GLinkClient.cpp"
#include "../../src/display/plugins/grinderlink/GLinkMessages.cpp"
#include "../../src/display/plugins/grinderlink/bluetooth/glink_frame.cpp"
#include "../../src/display/plugins/grinderlink/bluetooth/glink_json.cpp"
