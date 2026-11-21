#pragma once
#include "../WebFront.hpp"
#include "Server.hpp"

namespace webfront::tls {
template <frontend::FrontendType Frontend = frontend::DefaultFrontend, http::BuffersPolicyType Policy = http::DefaultBuffersPolicy>
using WebFront = BasicWF<Server, fs::DefaultFS, Policy, Frontend>;
}  // namespace webfront::tls
