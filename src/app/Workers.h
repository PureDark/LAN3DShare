#pragma once

namespace lan3d {
// One worker per child process. argv[0] is ignored, as with a normal executable.
// The desktop client dispatches these from its private worker command line;
// the small CLI entry points remain available for automated verification.
int RunSender(int argc, wchar_t** argv);
int RunReceiver(int argc, wchar_t** argv);
}
