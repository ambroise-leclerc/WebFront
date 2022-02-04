/// @date 04/02/2022 15:39:42
/// @author Ambroise Leclerc
/// @brief MimeType constructible from a filename's extension and convertible to string
#pragma once
#include <array>
#include <string_view>

namespace webfront::http {

struct MimeType {
    enum Type { plain, html, css, js, jpg, png, gif, json, pdf, ttf, ico, svg, webp, csv };
    Type type;

    constexpr MimeType(Type mimeType) : type(mimeType) {}
    constexpr MimeType(std::string_view extension) : type(fromExtension(extension).type) {}

    static constexpr MimeType fromExtension(std::string_view e) {
        if (e.starts_with('.')) e.remove_prefix(1);
        if (e == "js" || e == "mjs") return js;
        if (e == "htm" || e == "html") return html;
        if (e == "jpg" || e == "jpeg") return jpg;
        if (e == "css") return css;
        if (e == "png") return png;
        if (e == "gif") return gif;
        if (e == "json") return json;
        if (e == "pdf") return pdf;
        if (e == "ttf") return ttf;
        if (e == "ico") return ico;
        if (e == "svg") return svg;
        if (e == "webp") return webp;
        if (e == "csv") return csv;
        return plain;
    }

    [[nodiscard]] constexpr std::string_view toString() const {
        auto names =
          std::array{"text/plain",      "text/html", "text/css",     "application/javascript", "image/jpeg", "image/png", "image/gif", "application/json",
                     "application/pdf", "font/ttf",  "image/x-icon", "image/svg+xml",          "image/webp", "text/css",  "text/csv"};
        return names[type];
    }
};

} // namespace webfront::http
