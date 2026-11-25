# Application spaces: several users in one server

A `WebFront` holds one set of C++ functions, page handlers and routes. An application that serves several independent users
from one process (one emulation per session URL `/s/<id>/`, for instance) gives each of them a **space**: its own
functions, handlers, broadcasts and routes, under a path prefix.

```cpp
#include <webfront/WebFront.hpp>

webfront::WebFront app{webfront::WebFrontConfig{.address = "0.0.0.0", .port = "8080"}};
std::map<std::string, std::unique_ptr<webfront::WebFront::Space>, std::less<>> sessions;  // used on the server thread

app.spaces("/s/", [&](std::string_view id) -> std::optional<webfront::http::RouteReply> {
    if (!knownSession(id))
        return webfront::http::textReply(webfront::http::Response::notFound, "Unknown session\n");
    if (sessions.size() >= maxSessions)
        return webfront::http::textReply(webfront::http::Response::serviceUnavailable, "The server is full\n");
    auto space = std::make_unique<webfront::WebFront::Space>(app, "/s/", id);
    space->cppFunction("step", [&emulator = emulatorOf(id)] { return emulator.step(); });
    space->onUIStarted([](webfront::UI ui) { ui.jsFunction("hello")(std::string{"welcome"}); });
    sessions.emplace(id, std::move(space));
    return std::nullopt;  // created
});
```

## Where a page belongs

`app.spaces(prefix, resolver)` declares a family of spaces. An HTTP request or a WebSocket whose path starts with
`prefix + segment + "/"` belongs to the space `segment`; anything else belongs to the application as before.

- **Files.** The path without `prefix + segment` is served as it would be at the root: `/s/abc/index.html` is
  `index.html` of the document root (or the embedded files), `/s/abc/WebFront.js` the bridge. A page with relative
  URLs works at `/s/abc/` as at `/`. `/s/abc`, without the slash that relative URLs need, is redirected to `/s/abc/`.
- **The bridge.** `WebFront.js` opens its WebSocket on the directory of the page (`ws://host/s/abc/` for a page of
  `/s/abc/` or `/s/abc/sub/`): that path tells the server the page's space. Outside the spaces the path is ignored.
- **Routes.** `space.addRoute("/data/", handler)` answers `/s/abc/data/...`; the handler receives the path within the
  space (`RouteRequest::path` starts with `/data/`). Under a space, its routes come first, then the application's, then
  the files.

## The resolver

The resolver is called on the server context for a segment that has no space yet, for a page as for its WebSocket. It
either creates the space (`Space{app, prefix, segment}`, which the application owns) and returns nothing, or returns the
reply that refuses it, which reaches the browser as it is. Returning nothing without creating the space answers 404. A
space may also be created beforehand, outside the resolver, when the application knows its sessions in advance. A segment
that cannot name a space (`.`, or a decoded `/`, `\` or control character) is answered 404 without calling the resolver.

## What a space offers

A space has the members of the application that concern its pages, with the same signatures, and its `UI` type is the
application's: code templated on the application type (`template <typename App> class Bridge`) attaches to a space
unchanged.

| Member | Scope |
|---|---|
| `cppFunction`, `hasCppFunction` | The space's pages. A function of the application stays visible to every space; the space's own of the same name takes precedence. |
| `onUIStarted`, `subscribeUIStarted`, `subscribeUIClosed` | The space's pages, after (started) or before (closed) the application's handlers, which see every page. |
| `broadcast`, `forEachUI`, `connectedBrowsers` | The space's pages only. The application's reach every page, those of the spaces included. |
| `addRoute` | Paths under the space, relative to it. |
| `onLastUIClosed` | Called once each time the last connected page of the space closes. |
| `spawn`, `resumeAfter` | The application's: tasks outside the space's lifetime. |

`UI::jsFunction`, `UI::document()` and the other members of a UI work as for any page.

## Lifetime

The application decides when a space ends. `onLastUIClosed` tells it that the last page closed (a reload closes the page
and opens it again a moment later), so the usual pattern keeps the space through a grace delay:

```cpp
space->onLastUIClosed([&app, &sessions, id = std::string(id)] {
    static_cast<void>(app.spawn([](auto& app, auto& sessions, std::string id) -> webfront::Task {
        co_await app.resumeAfter(std::chrono::seconds(30));
        if (auto found = sessions.find(id); found != sessions.end() && found->second->connectedBrowsers() == 0)
            sessions.erase(found);
    }(app, sessions, id)));
});
```

Destroying a space closes its pages (the application's `subscribeUIClosed` handlers are told; the space's own handlers
are released first and are not), unregisters its functions and routes, and lets the resolver decide again for its segment.
Destroy a space on the server thread, where its callbacks run (in a task of `app.spawn`, in `onLastUIClosed`, in a
callback), or at a time when none of its callbacks can run; destroy the spaces before their application.

## Limits

- The read grants of `UI::grantRead` and the PTY pages belong to the application: a space has no `addReadRoute`.
- A space isolates what WebFront dispatches. It is not a security boundary between users of one process: the session
  identifier in the URL is the only thing that selects a space, so an application that must keep sessions private
  chooses unguessable identifiers or checks the user in its resolver and its callbacks.
- `onLastUIClosed` counts connected pages: a space that no page ever reached is not reported, and the application ends
  it with its own timeout.
