// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2022-02-04
//
// WebFront — http
// Role: MimeType, constructible from a file name's extension and convertible to string.
// Thread: worker-safe | Alloc: heap-init
// LLM: small lookup table; extend it together with the tests

#pragma once
#include <array>
#include <string_view>

namespace webfront::http {

struct MimeType {
    enum Type { plain, html, css, js, jpg, png, gif, json, pdf, ttf, ico, svg, webp, csv, wasm };
    Type type;

    constexpr MimeType(Type mimeType) : type(mimeType) {}
    constexpr MimeType(std::string_view extension) : type(fromExtension(extension).type) {}

    [[nodiscard]] static constexpr MimeType fromExtension(std::string_view e) {
        struct Mapping {
            std::string_view extension;
            Type             type;
        };
        constexpr auto mappings = std::array<Mapping, 17>{
            {
             {"js", js},
             {"mjs", js},
             {"htm", html},
             {"html", html},
             {"jpg", jpg},
             {"jpeg", jpg},
             {"css", css},
             {"png", png},
             {"gif", gif},
             {"json", json},
             {"pdf", pdf},
             {"ttf", ttf},
             {"ico", ico},
             {"svg", svg},
             {"webp", webp},
             {"csv", csv},
             {"wasm", wasm},
             }
        };
        if (e.starts_with('.'))
            e.remove_prefix(1);
        for (const auto& mapping : mappings)
            if (e == mapping.extension)
                return mapping.type;
        return plain;
    }

    [[nodiscard]] constexpr std::string_view toString() const {
        // One name per Type, in declaration order.
        constexpr auto names = std::array{"text/plain",
                                          "text/html",
                                          "text/css",
                                          "application/javascript",
                                          "image/jpeg",
                                          "image/png",
                                          "image/gif",
                                          "application/json",
                                          "application/pdf",
                                          "font/ttf",
                                          "image/x-icon",
                                          "image/svg+xml",
                                          "image/webp",
                                          "text/csv",
                                          "application/wasm"};
        static_assert(names.size() == wasm + 1, "MimeType names must match the Type enumerators");
        return names[type];
    }
};

}  // namespace webfront::http
