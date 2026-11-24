# A project outside WebFront that writes scenarios with webfront::bdd and has CTest discover them (#647). It finds the
# installed WebFrontBdd package, which needs neither WebFront's sources nor stdnet.
cmake_minimum_required(VERSION 3.31)
project(BddConsumer LANGUAGES CXX)

find_package(WebFrontBdd REQUIRED)
enable_testing()

add_executable(consumer_tests main.cpp)
target_link_libraries(consumer_tests PRIVATE WebFront::Bdd)
webfront_bdd_discover_tests(consumer_tests TEST_PREFIX "Consumer." PROPERTIES TIMEOUT 30 LABELS consumer)

# The same executable run as two shards.
webfront_bdd_discover_tests(consumer_tests SHARDS 2 SHARD_NAME ConsumerShards PROPERTIES TIMEOUT 30)
