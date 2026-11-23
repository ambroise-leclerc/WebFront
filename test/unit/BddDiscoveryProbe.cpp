// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
/// @brief Scenarios whose titles hold the characters that split or join CMake lists, for the BddDiscovery case.
#include <webfront/testing/Bdd.hpp>

using namespace webfront::bdd;

const Scenario openBracket{"A [b title", [] {}};
const Scenario semicolon{"Second; one", [] {}};
const Scenario closeBracket{"Third ]x", [] {}};
const Scenario pairedBrackets{"Fourth [tag] title", [] {}};
const Scenario literalMarkers{"Literal <bdd-open> %5B %25 <bdd-semicolon>", [] {}};
const Scenario endsLikeDelimiter{"Ends like a delimiter ]==", [] {}};
const Scenario trailingBackslash{"Ends with a backslash \\", [] {}};
