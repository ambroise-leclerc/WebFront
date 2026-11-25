// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2026-11-25
//
// WebFront — tests/unit
// Role: Application spaces under a path prefix: isolation of functions, pages and broadcasts, resolver, last page, release.
// Thread: bridge-io | Alloc: unrestricted
// LLM: pins the contracts of Space.hpp and BasicWF::spaces with pages connected over loopback

// Application spaces under a path prefix (#671).

#include <webfront/networking/TCPNetworkingStdnet.hpp>
#include <webfront/Space.hpp>
#include <webfront/system/DefaultFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/WebFront.hpp>

#ifdef _WIN32
    #include <webfront/system/WindowsCompat.hpp>
#else
    #include <poll.h>
#endif

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

using namespace webfront::bdd;
using namespace webfront;
using namespace std::chrono_literals;

namespace {
using Stdnet = networking::TCPStdnet;
using App    = BasicWF<Stdnet, fs::DefaultFS>;

bool spaceWaitUntil(auto condition, std::chrono::milliseconds timeout = 5s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline)
            return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

/// A loopback connection that records what the server sends.
class Connection {
public:
    explicit Connection(std::string_view port) : socket(context) {
        socket.connect(Stdnet::Resolve("127.0.0.1", port));
    }

    void send(std::span<const std::byte> data) {
        socket.send(Stdnet::ConstBuffer(data.data(), data.size()));
    }
    void send(std::string_view text) {
        send(std::as_bytes(std::span(text)));
    }

    /// True once the server sent these bytes, false after the deadline or when it closed the connection first.
    bool receiveUntil(std::string_view text, std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (bytes.find(text) == std::string::npos)
            if (!receiveSome(deadline))
                return false;
        return true;
    }

    /// True once the server closed the connection, false after the deadline.
    bool receiveUntilClosed(std::chrono::milliseconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!closed)
            if (!receiveSome(deadline))
                return closed;
        return true;
    }

    [[nodiscard]] const std::string& received() const {
        return bytes;
    }

private:
    bool receiveSome(std::chrono::steady_clock::time_point deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0 || closed)
            return false;
        ::pollfd descriptor{};
        descriptor.fd     = socket.native_handle();
        descriptor.events = POLLIN;
#ifdef _WIN32
        const int ready = ::WSAPoll(&descriptor, 1, static_cast<int>(remaining));
#else
        const int ready = ::poll(&descriptor, 1, static_cast<int>(remaining));
#endif
        if (ready <= 0)
            return false;
        std::array<char, 4096> buffer{};
        std::size_t            size = 0;
        try {
            size = socket.receive(Stdnet::Buffer(buffer));
        } catch (const std::system_error&) {
            size = 0;  // reset by the server
        }
        if (size == 0) {
            closed = true;
            return false;
        }
        bytes.append(buffer.data(), size);
        return true;
    }

    Stdnet::IoContext context;
    Stdnet::Socket    socket;
    std::string       bytes;
    bool              closed = false;
};

/// The whole answer of the server to one HTTP request.
std::string httpGet(std::string_view port, std::string_view target) {
    Connection connection{port};
    connection.send("GET " + std::string(target) + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    connection.receiveUntilClosed();
    return connection.received();
}

/// The payload of a browser's call to a C++ function; a call identifier other than 0 asks for an answer.
std::vector<std::byte> callPayload(const std::string& name, msg::CallId callId) {
    msg::FunctionCall<> message;
    message.setCallId(callId);
    websocket::Frame<Stdnet> frame{std::span(reinterpret_cast<const std::byte*>(message.header().data()), message.header().size())};
    message.encodeParameter(name, frame);
    frame.freeze();
    std::vector<std::byte> payload;
    const auto             buffers = frame.toBuffers();
    for (auto buffer = std::next(buffers.begin()); buffer != buffers.end(); ++buffer) {
        const auto* first = static_cast<const std::byte*>(buffer->data());
        payload.insert(payload.end(), first, first + buffer->size());
    }
    return payload;
}

/// A masked binary frame from the browser.
std::vector<std::byte> clientFrame(std::span<const std::byte> payload) {
    require(payload.size() < 126);
    constexpr std::array<std::byte, 4> mask{std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}};
    std::vector<std::byte>             frame{std::byte{0x82}, std::byte{static_cast<std::uint8_t>(0x80u | payload.size())}};
    frame.insert(frame.end(), mask.begin(), mask.end());
    for (std::size_t index = 0; index < payload.size(); ++index)
        frame.push_back(payload[index] ^ mask[index % mask.size()]);
    return frame;
}

/// A page whose bridge connected from `path`, as WebFront.js does from the page's directory.
class Page : public Connection {
public:
    Page(std::string_view port, std::string_view path) : Connection(port) {
        send("GET " + std::string(path)
             + " HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        upgraded = receiveUntil("\r\n\r\n") && received().starts_with("HTTP/1.1 101");
        if (upgraded) {
            // A masked binary frame of two bytes, with a zero mask: the handshake and the browser's byte order.
            const std::array<unsigned char, 8>
                handshake{0x82, 0x82, 0, 0, 0, 0, static_cast<unsigned char>(msg::Command::handshake), static_cast<unsigned char>(msg::JSEndian::little)};
            send(std::as_bytes(std::span(handshake)));
        }
    }

    void call(const std::string& name, msg::CallId callId = 0) {
        send(clientFrame(callPayload(name, callId)));
    }

    bool upgraded = false;
};

/// Runs the application's server on its own thread until destroyed.
struct Running {
    explicit Running(App& application)
        : app(application), thread([&application] {
              application.run();
          }) { }
    ~Running() {
        app.stop();
        thread.join();
        app.shutdown();
    }
    App&        app;
    std::thread thread;
};

/// Code templated on the application type, attached here to a space.
template <typename Application>
struct Bridge {
    explicit Bridge(Application& application) {
        application.cppFunction("bridged", [](typename Application::UI ui) {
            ui.jsFunction("bridgedBack")(std::string{"bridged-back"});
        });
    }
};

Task destroyLater(std::unique_ptr<App::Space>& space, std::atomic<bool>& destroyed) {
    space.reset();
    destroyed = true;
    co_return;
}
}  // namespace

const Scenario twoSpaces{"Two spaces with a function of the same name answer independently", {"spaces"}, [] {
                             App                                                app{WebFrontConfig{.port = "0"}};
                             std::map<std::string, std::unique_ptr<App::Space>> spaces;
                             std::optional<Bridge<App::Space>>                  bridge;
                             app.cppFunction("shared", [] {
                                 return std::string{"from-application"};
                             });
                             app.spaces("/s/", [&](std::string_view segment) -> std::optional<http::RouteReply> {
                                 auto space = std::make_unique<App::Space>(app, "/s/", segment);
                                 space->cppFunction("whoAmI", [name = std::string(segment)] {
                                     return "space-" + name;
                                 });
                                 if (segment == "alpha") {
                                     space->cppFunction("shared", [] {
                                         return std::string{"alpha-own"};
                                     });
                                     bridge.emplace(*space);
                                 }
                                 spaces.emplace(segment, std::move(space));
                                 return std::nullopt;
                             });
                             Running running{app};
                             Page    alpha{app.port(), "/s/alpha/"};
                             Page    beta{app.port(), "/s/beta/"};
                             Page    root{app.port(), "/"};
                             require(alpha.upgraded && beta.upgraded && root.upgraded);

                             when("each page calls the function of the same name", [&] {
                                 alpha.call("whoAmI", 1);
                                 beta.call("whoAmI", 1);
                                 root.call("whoAmI", 1);
                                 then("each answers from its own space, and a page outside the spaces finds none", [&] {
                                     require(alpha.receiveUntil("space-alpha"));
                                     require(beta.receiveUntil("space-beta"));
                                     require(root.receiveUntil("was not found"));
                                     require(alpha.received().find("space-beta") == std::string::npos);
                                     require(beta.received().find("space-alpha") == std::string::npos);
                                 });
                             });
                             when("each page calls a function of the application that one space also registered", [&] {
                                 alpha.call("shared", 2);
                                 beta.call("shared", 2);
                                 then("the space's own takes precedence, and the application's is visible to the others", [&] {
                                     require(alpha.receiveUntil("alpha-own"));
                                     require(beta.receiveUntil("from-application"));
                                     require(alpha.received().find("from-application") == std::string::npos);
                                 });
                             });
                             when("a page calls the function of code templated on the application type, attached to its space", [&] {
                                 alpha.call("bridged");
                                 beta.call("bridged", 3);
                                 then("the space's page reaches it, and another space's does not", [&] {
                                     require(alpha.receiveUntil("bridged-back"));
                                     require(beta.receiveUntil("was not found"));
                                 });
                             });
                         }};

const Scenario pageOf{"A page of one space never receives a call or a broadcast of another", {"spaces"}, [] {
                          App                                                app{WebFrontConfig{.port = "0"}};
                          std::map<std::string, std::unique_ptr<App::Space>> spaces;
                          std::atomic<int>                                   applicationStarted{0}, alphaStarted{0}, betaStarted{0};
                          std::vector<UISubscription>                        subscriptions;
                          app.onUIStarted([&](App::UI) {
                              ++applicationStarted;
                          });
                          app.spaces("/s/", [&](std::string_view segment) -> std::optional<http::RouteReply> {
                              auto  space   = std::make_unique<App::Space>(app, "/s/", segment);
                              auto& counter = segment == "alpha" ? alphaStarted : betaStarted;
                              space->onUIStarted([&counter](App::UI ui) {
                                  ++counter;
                                  ui.jsFunction("welcome")(std::string{"welcome-to-own-space"});
                              });
                              spaces.emplace(segment, std::move(space));
                              return std::nullopt;
                          });
                          Running running{app};
                          Page    alpha{app.port(), "/s/alpha/"};
                          Page    beta{app.port(), "/s/beta/"};
                          require(spaceWaitUntil([&] {
                              return alphaStarted == 1 && betaStarted == 1 && applicationStarted == 2;
                          }));
                          App::Space& alphaSpace = *spaces.at("alpha");
                          App::Space& betaSpace  = *spaces.at("beta");

                          then("each space's handler saw its own page only, and the application's saw both", [&] {
                              require(alphaStarted == 1);
                              require(betaStarted == 1);
                              require(applicationStarted == 2);
                              require(alphaSpace.connectedBrowsers() == 1);
                              require(app.connectedBrowsers() == 2);
                          });
                          when("each space broadcasts and calls its pages, then the application broadcasts to every page", [&] {
                              betaSpace.broadcast("onState")(std::string{"beta-state"});
                              alphaSpace.broadcast("onState", {.maxPendingWrites = 1})(std::string{"alpha-state"});
                              std::size_t visited = 0;
                              alphaSpace.forEachUI([&](App::UI ui) {
                                  ++visited;
                                  ui.jsFunction("each")(std::string{"alpha-each"});
                              });
                              app.broadcast("onAll")(std::string{"application-done"});
                              then("each page received its space's calls before the application's, and nothing of the other space", [&] {
                                  require(visited == 1);
                                  require(alpha.receiveUntil("application-done"));
                                  require(beta.receiveUntil("application-done"));
                                  require(alpha.received().find("alpha-state") != std::string::npos);
                                  require(alpha.received().find("alpha-each") != std::string::npos);
                                  require(alpha.received().find("beta-state") == std::string::npos);
                                  require(beta.received().find("beta-state") != std::string::npos);
                                  require(beta.received().find("alpha-state") == std::string::npos);
                                  require(beta.received().find("alpha-each") == std::string::npos);
                              });
                          });
                      }};

const Scenario resolverRefusal{"The resolver's refusal reaches the browser", {"spaces"}, [] {
                                   App              app{WebFrontConfig{.port = "0"}};
                                   std::atomic<int> resolved{0};
                                   app.spaces("/s/", [&](std::string_view segment) -> std::optional<http::RouteReply> {
                                       ++resolved;
                                       if (segment == "full")
                                           return http::textReply(http::Response::serviceUnavailable, "The server is full");
                                       return std::nullopt;  // neither created nor refused: unknown
                                   });
                                   Running running{app};

                                   then("a page of a refused space receives the resolver's reply", [&] {
                                       const auto answer = httpGet(app.port(), "/s/full/index.html");
                                       require(answer.starts_with("HTTP/1.1 503"));
                                       require(answer.find("The server is full") != std::string::npos);
                                   });
                                   then("a segment the resolver neither creates nor refuses is not found", [&] {
                                       require(httpGet(app.port(), "/s/ghost/").starts_with("HTTP/1.1 404"));
                                   });
                                   then("a WebSocket of a refused space is not upgraded and connects no browser", [&] {
                                       Page page{app.port(), "/s/full/"};
                                       require(!page.upgraded);
                                       require(page.received().starts_with("HTTP/1.1 503"));
                                       require(app.connectedBrowsers() == 0);
                                   });
                                   then("paths outside the prefix never reach the resolver", [&] {
                                       require(httpGet(app.port(), "/WebFront.js").starts_with("HTTP/1.1 200"));
                                       require(httpGet(app.port(), "/s/").starts_with("HTTP/1.1 404"));
                                       require(resolved == 0);
                                   });
                               }};

const Scenario lastPage{"The last-page notice fires once each time a space empties", {"spaces"}, [] {
                            App                           app{WebFrontConfig{.port = "0"}};
                            std::unique_ptr<App::Space>   space;
                            std::atomic<int>              notices{0}, spaceClosed{0}, applicationClosed{0}, started{0};
                            std::optional<UISubscription> spaceSubscription;
                            app.spaces("/s/", [](std::string_view) -> std::optional<http::RouteReply> {
                                return http::textReply(http::Response::notFound, "Unknown session");
                            });
                            space = std::make_unique<App::Space>(app, "/s/", "alpha");  // created beforehand, outside the resolver
                            space->onUIStarted([&](App::UI) {
                                ++started;
                            });
                            space->onLastUIClosed([&] {
                                ++notices;
                            });
                            spaceSubscription.emplace(space->subscribeUIClosed([&](App::UI) {
                                ++spaceClosed;
                            }));
                            auto    applicationSubscription = app.subscribeUIClosed([&](App::UI) {
                                ++applicationClosed;
                            });
                            Running running{app};
                            auto    first  = std::make_unique<Page>(app.port(), "/s/alpha/");
                            auto    second = std::make_unique<Page>(app.port(), "/s/alpha/sub/");  // a page of a subdirectory
                            require(spaceWaitUntil([&] {
                                return started == 2;
                            }));

                            when("one of the two pages closes", [&] {
                                first.reset();
                                require(spaceWaitUntil([&] {
                                    return spaceClosed == 1;
                                }));
                                then("the space is not empty: no notice", [&] {
                                    require(applicationClosed == 1);
                                    require(notices == 0);
                                });
                                andWhen("the other closes too", [&] {
                                    second.reset();
                                    require(spaceWaitUntil([&] {
                                        return notices >= 1 && applicationClosed == 2;
                                    }));
                                    std::this_thread::sleep_for(50ms);
                                    then("the notice fired once, after the closed handlers", [&] {
                                        require(notices == 1);
                                        require(spaceClosed == 2);
                                    });
                                    andWhen("a page joins the emptied space and closes", [&] {
                                        auto again = std::make_unique<Page>(app.port(), "/s/alpha/");
                                        require(spaceWaitUntil([&] {
                                            return started == 3;
                                        }));
                                        again.reset();
                                        then("the space is reported empty again", [&] {
                                            require(spaceWaitUntil([&] {
                                                return notices == 2;
                                            }));
                                        });
                                    });
                                });
                            });
                        }};

const Scenario destroyedSpace{"A destroyed space releases its functions and routes, and closes its pages", {"spaces"}, [] {
                                  App                            app{WebFrontConfig{.port = "0"}};
                                  std::unique_ptr<App::Space>    space;
                                  http::RouteTable::Registration route;
                                  auto                           captured = std::make_shared<int>(0);
                                  std::weak_ptr<int>             observed = captured;
                                  std::atomic<int>               started{0}, applicationClosed{0}, resolved{0};
                                  app.spaces("/s/", [&](std::string_view) -> std::optional<http::RouteReply> {
                                      ++resolved;
                                      return http::textReply(http::Response::notFound, "The session has ended");
                                  });
                                  space = std::make_unique<App::Space>(app, "/s/", "alpha");
                                  space->cppFunction("hold", [captured = std::move(captured)] {
                                      return *captured;
                                  });
                                  space->onUIStarted([&](App::UI) {
                                      ++started;
                                  });
                                  route          = space->addRoute("/data/", [](const http::RouteRequest& request) {
                                      return http::textReply(http::Response::ok, "space-data " + std::string(request.path));
                                  });
                                  auto    closed = app.subscribeUIClosed([&](App::UI) {
                                      ++applicationClosed;
                                  });
                                  Running running{app};
                                  Page    page{app.port(), "/s/alpha/"};
                                  require(spaceWaitUntil([&] {
                                      return started == 1;
                                  }));
                                  require(httpGet(app.port(), "/s/alpha/data/x").find("space-data /data/x") != std::string::npos);
                                  require(route);

                                  when("the application destroys the space on the server thread", [&] {
                                      std::atomic<bool> destroyed{false};
                                      static_cast<void>(app.spawn(destroyLater(space, destroyed)));
                                      require(spaceWaitUntil([&] {
                                          return destroyed.load();
                                      }));
                                      then("its page is closed, and the application is told", [&] {
                                          require(page.receiveUntilClosed());
                                          require(spaceWaitUntil([&] {
                                              return applicationClosed == 1 && app.connectedBrowsers() == 0;
                                          }));
                                      });
                                      then("its functions and routes are released", [&] {
                                          require(observed.expired());
                                          require(!route);
                                      });
                                      then("its paths go to the resolver again", [&] {
                                          const auto answer = httpGet(app.port(), "/s/alpha/data/x");
                                          require(answer.starts_with("HTTP/1.1 404"));
                                          require(answer.find("The session has ended") != std::string::npos);
                                          require(resolved == 1);
                                      });
                                  });
                              }};

const Scenario servedUnder{"A space serves the application's files and routes under its prefix, its own routes first", {"spaces"}, [] {
                               App                                                app{WebFrontConfig{.port = "0"}};
                               std::map<std::string, std::unique_ptr<App::Space>> spaces;
                               std::vector<http::RouteTable::Registration>        routes;
                               routes.push_back(app.addRoute("/api/", [](const http::RouteRequest&) {
                                   return http::textReply(http::Response::ok, "application-api");
                               }));
                               app.spaces("/s/", [&](std::string_view segment) -> std::optional<http::RouteReply> {
                                   auto space = std::make_unique<App::Space>(app, "/s/", segment);
                                   if (segment == "alpha")
                                       routes.push_back(space->addRoute("/api/", [](const http::RouteRequest& request) {
                                           return http::textReply(http::Response::ok, "alpha-api " + std::string(request.path));
                                       }));
                                   spaces.emplace(segment, std::move(space));
                                   return std::nullopt;
                               });
                               Running running{app};

                               then("the embedded files are served under the prefix as at the root", [&] {
                                   const auto atRoot  = httpGet(app.port(), "/WebFront.js");
                                   const auto inSpace = httpGet(app.port(), "/s/alpha/WebFront.js");
                                   require(inSpace.starts_with("HTTP/1.1 200"));
                                   requireEqual(inSpace.size(), atRoot.size());
                               });
                               then("a space's own route answers with the path within the space, before the application's", [&] {
                                   require(httpGet(app.port(), "/s/alpha/api/v1").find("alpha-api /api/v1") != std::string::npos);
                                   require(httpGet(app.port(), "/s/beta/api/v1").find("application-api") != std::string::npos);
                                   require(httpGet(app.port(), "/api/v1").find("application-api") != std::string::npos);
                               });
                               then("the space without its final slash is redirected to it, its query kept", [&] {
                                   const auto answer = httpGet(app.port(), "/s/alpha?x=1");
                                   require(answer.starts_with("HTTP/1.1 301"));
                                   require(answer.find("Location: /s/alpha/?x=1\r\n") != std::string::npos);
                               });
                           }};

const Scenario withoutSpaces{"An application without spaces behaves as before them", {"spaces"}, [] {
                                 App app{WebFrontConfig{.port = "0"}};
                                 app.cppFunction("ping", [] {
                                     return std::string{"pong-from-application"};
                                 });

                                 then("a space cannot be created without its family, nor with a segment that cannot name one", [&] {
                                     requireThrows<std::invalid_argument>([&] {
                                         App::Space space{app, "/s/", "alpha"};
                                     });
                                     app.spaces("/s/", [](std::string_view) {
                                         return std::optional<http::RouteReply>{};
                                     });
                                     for (const auto* segment : {"a/b", "a..b", "..", ".", "", "a\\b", "a\x01b"}) {
                                         auto scope = info(segment);
                                         requireThrows<std::invalid_argument>([&] {
                                             App::Space space{app, "/s/", segment};
                                         });
                                     }
                                     requireThrows<std::invalid_argument>([&] {
                                         app.spaces("/s/sub/", [](std::string_view) {
                                             return std::optional<http::RouteReply>{};
                                         });
                                     });
                                     App::Space space{app, "/s/", "alpha"};
                                     requireThrows<std::logic_error>([&] {
                                         App::Space twin{app, "/s/", "alpha"};
                                     });
                                 });
                                 then("a page connected from any directory reaches the application's functions", [&] {
                                     Running running{app};
                                     Page    page{app.port(), "/s/alpha/"};
                                     require(page.upgraded);
                                     page.call("ping", 1);
                                     require(page.receiveUntil("pong-from-application"));
                                 });
                             }};

const Scenario encodedSeparator{"An encoded separator never selects a space other than the one its segment names", {"spaces"}, [] {
                                    App                                                app{WebFrontConfig{.port = "0"}};
                                    std::map<std::string, std::unique_ptr<App::Space>> spaces;
                                    std::vector<http::RouteTable::Registration>        routes;
                                    std::mutex                                         seenMutex;
                                    std::vector<std::string>                           seen;
                                    app.spaces("/s/", [&](std::string_view segment) -> std::optional<http::RouteReply> {
                                        {
                                            std::scoped_lock lock{seenMutex};
                                            seen.emplace_back(segment);
                                        }
                                        auto space = std::make_unique<App::Space>(app, "/s/", segment);
                                        routes.push_back(space->addRoute("/data/", [name = std::string(segment)](const http::RouteRequest& request) {
                                            return http::textReply(http::Response::ok, name + "-data " + std::string(request.path));
                                        }));
                                        spaces.emplace(segment, std::move(space));
                                        return std::nullopt;
                                    });
                                    Running running{app};
                                    require(httpGet(app.port(), "/s/alpha/data/x").find("alpha-data /data/x") != std::string::npos);

                                    then("a segment holding an encoded '/' is not found, and never reaches the resolver", [&] {
                                        const auto answer = httpGet(app.port(), "/s/alpha%2Fdata/x");
                                        require(answer.starts_with("HTTP/1.1 404"));
                                        require(answer.find("alpha-data") == std::string::npos);
                                        Page page{app.port(), "/s/alpha%2Fdata/"};
                                        require(!page.upgraded);
                                        std::scoped_lock lock{seenMutex};
                                        require(seen == std::vector<std::string>{"alpha"});
                                    });
                                    then("a WebSocket under the prefix whose target is not a safe path is refused, not given to the application", [&] {
                                        for (const auto* target : {"/s/%00/", "/s/al%00pha/", "/s/alpha/%00/", "/s/a..b/", "/s/", "/s/%2E/"}) {
                                            Page page{app.port(), target};
                                            auto scope = info(target);
                                            require(!page.upgraded);
                                            require(page.received().starts_with("HTTP/1.1 404"));
                                        }
                                        require(app.connectedBrowsers() == 0);
                                        std::scoped_lock lock{seenMutex};
                                        require(seen == std::vector<std::string>{"alpha"});
                                    });
                                    then("a space of any name its creation accepts is reachable", [&] {
                                        require(httpGet(app.port(), "/s/a%20b%25c+d.e/data/x").find("a b%c+d.e-data /data/x") != std::string::npos);
                                        Page page{app.port(), "/s/a%20b%25c+d.e/"};
                                        require(page.upgraded);
                                    });
                                    then("the encoded prefix separator does not reach a space either", [&] {
                                        require(httpGet(app.port(), "/s%2Falpha/data/x").starts_with("HTTP/1.1 404"));
                                    });
                                    then("other encoded characters of the segment and the rest are decoded as at the root", [&] {
                                        require(httpGet(app.port(), "/s/%61lpha/data/x").find("alpha-data /data/x") != std::string::npos);
                                        require(httpGet(app.port(), "/s/alpha/data/a%2Fb").find("alpha-data /data/a/b") != std::string::npos);
                                    });
                                }};

const Scenario destroyedBy{"A space destroyed by one of its handlers calls none of the handlers that follow", {"spaces"}, [] {
                               App                           app{WebFrontConfig{.port = "0"}};
                               std::unique_ptr<App::Space>   space;
                               std::atomic<int>              before{0}, after{0}, applicationClosed{0};
                               std::optional<UISubscription> first, second;
                               app.spaces("/s/", [](std::string_view) -> std::optional<http::RouteReply> {
                                   return http::textReply(http::Response::notFound, "Unknown session");
                               });
                               auto closed = app.subscribeUIClosed([&](App::UI) {
                                   ++applicationClosed;
                               });
                               space       = std::make_unique<App::Space>(app, "/s/", "alpha");

                               when("the first of two started handlers destroys the space", [&] {
                                   first.emplace(space->subscribeUIStarted([&](App::UI) {
                                       ++before;
                                       space.reset();
                                   }));
                                   second.emplace(space->subscribeUIStarted([&](App::UI) {
                                       ++after;
                                   }));
                                   Running running{app};
                                   Page    page{app.port(), "/s/alpha/"};
                                   then("the second is not called, and the page is closed", [&] {
                                       require(page.receiveUntilClosed());
                                       require(before == 1);
                                       require(after == 0);
                                   });
                               });
                               when("the first of two closed handlers destroys the space", [&] {
                                   std::atomic<int> started{0};
                                   space->onUIStarted([&](App::UI) {
                                       ++started;
                                   });
                                   first.emplace(space->subscribeUIClosed([&](App::UI) {
                                       ++before;
                                       space.reset();
                                   }));
                                   second.emplace(space->subscribeUIClosed([&](App::UI) {
                                       ++after;
                                   }));
                                   std::atomic<int> notices{0};
                                   space->onLastUIClosed([&] {
                                       ++notices;
                                   });
                                   Running running{app};
                                   auto    page = std::make_unique<Page>(app.port(), "/s/alpha/");
                                   require(spaceWaitUntil([&] {
                                       return started == 1;
                                   }));
                                   page.reset();
                                   then("neither the second nor the last-page notice is called; the application is still told", [&] {
                                       require(spaceWaitUntil([&] {
                                           return applicationClosed == 1;
                                       }));
                                       require(before == 1);
                                       require(after == 0);
                                       require(notices == 0);
                                   });
                               });
                           }};
