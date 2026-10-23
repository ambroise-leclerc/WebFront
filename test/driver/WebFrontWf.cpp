// webfront-wf: runs one wf command of the browser developer environment without interaction and exits with its status.
//
//   webfront-wf --project <dir> [--environment <archive> --integrity <sha512-...>] [--registry-dir <dir>]
//               [--report <file>] [--timeout <seconds>] [--] <wf command> [arguments...]
//
// The environment defaults to the provisioned bootstrap archive (cmake/WebFrontBootstrap.cmake); the project is the
// directory wf reads and writes. Exit status: the command's own status; 64 usage; 70 bootstrap failure (archive missing,
// wrong integrity, incompatible protocol or API); 71 time limit of the driver; 72 the browser closed without a result.
#include "WfDriver.hpp"

#ifndef WEBFRONT_DRIVER_FRONTEND
    #error "WEBFRONT_DRIVER_FRONTEND must name the browser frontend"
#endif

int main(int argc, char** argv) {
    return webfront::testdriver::main<WEBFRONT_DRIVER_FRONTEND>(argc, argv, "webfront-wf");
}
