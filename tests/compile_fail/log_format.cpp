// Compile-fail cases for the log call sites: a format string is checked at compile time.
// DVV_COMPILE_FAIL_CASE selects one:
//   0  control: matching format and arguments compile
//   1  too few arguments for the format
//   2  an argument of the wrong type for the format
#include "dvv/base/log.h"

int main() {
#if DVV_COMPILE_FAIL_CASE == 0
    DVV_LOG_INFO("{} {}", 1, 2);
#elif DVV_COMPILE_FAIL_CASE == 1
    DVV_LOG_INFO("{} {}", 1);
#elif DVV_COMPILE_FAIL_CASE == 2
    DVV_LOG_INFO("{:d}", "text");
#endif
    return 0;
}
