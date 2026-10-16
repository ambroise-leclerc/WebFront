/// @brief Every stdnet and stdexec header WebFront uses.
///
/// CEF defines macros (NO_UNIQUE_ADDRESS, ALWAYS_INLINE, NOINLINE) that are also the labels stdexec pastes
/// onto STDEXEC_ATTRIBUTE_CASE_: a stdexec header included after CEF's headers does not compile.
/// frontend/CEF.hpp includes this header before CEF's, so a stdnet or stdexec header used anywhere in
/// WebFront must be added here.
#pragma once

#include <exec/start_detached.hpp>

#include <network>
