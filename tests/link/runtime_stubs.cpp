#include <cstdint>

// GbaIo stamps the MMIO-cap ring with the runtime clock / PC. link_tests is
// an offline unit test with no runtime stack — provide stable zeros.
extern "C" {
unsigned long long g_runtime_cycles = 0;

uint32_t runtime_current_pc(void) {
    return 0;
}
}
