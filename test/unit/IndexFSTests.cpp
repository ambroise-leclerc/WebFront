// SPDX-License-Identifier: Apache-2.0 OR CECILL-B
// Author: Ambroise Leclerc
// Created: 2023-03-16
//
// WebFront — tests/unit
// Role: The IndexFS embedded provider: WebFront.js, favicon and the minimal index page.
// Thread: bridge-io | Alloc: heap-init
// LLM: pins the closed asset list

#include <webfront/system/IndexFS.hpp>
#include <webfront/testing/Bdd.hpp>
#include <webfront/tooling/HexDump.hpp>

#include <algorithm>
#include <iostream>
#include <string_view>

using namespace webfront::bdd;

using namespace std;
using namespace webfront;

const Scenario indexFileSystemProvides{"IndexFileSystem provides basic files for browser support", [] {
                                           given("An IndexFS", [&] {
                                               using FS = fs::IndexFS;
                                               when("Requesting index.html", [&] {
                                                   auto indexFile = FS::open("index.html");
                                                   then("A correct HTML5 file loading WebFront.js and the fallback module is returned", [&] {
                                                       require(indexFile.has_value());

                                                       // The readable source (js/core/fallback/index.html) is embedded raw, like WebFront.js,
                                                       // so it is never Brotli-encoded; read it in full rather than assuming it fits a
                                                       // single fixed-size buffer.
                                                       require(!indexFile->isEncoded());
                                                       string           html;
                                                       array<char, 512> buffer;
                                                       while (auto readSize = indexFile->read(buffer))
                                                           html.append(buffer.data(), readSize);

                                                       require(html.starts_with("<!DOCTYPE html>"));
                                                       // A script element that names the source: no std::regex, whose GCC 13 header reports a false
                                                       // -Wmaybe-uninitialized when it is instantiated (#628).
                                                       const auto hasScript = [&](string_view source) {
                                                           for (auto tag = html.find("<script"); tag != string::npos; tag = html.find("<script", tag + 1)) {
                                                               const auto end = html.find('>', tag);
                                                               if (end != string::npos
                                                                   && string_view(html).substr(tag, end - tag).find(source) != string_view::npos)
                                                                   return true;
                                                           }
                                                           return false;
                                                       };
                                                       require(hasScript("src=\"WebFront.js\""));
                                                       require(hasScript("src=\"webfront-fallback.mjs\""));
                                                   });
                                               });

                                               when("Requesting webfront-fallback.mjs", [&] {
                                                   auto moduleFile = FS::open("webfront-fallback.mjs");
                                                   then("The fallback module awaiting webFront.ready is returned", [&] {
                                                       require(moduleFile.has_value());
                                                       require(!moduleFile->isEncoded());
                                                       require(moduleFile->getEncoding().empty());

                                                       string           source;
                                                       array<char, 512> buffer;
                                                       while (auto readSize = moduleFile->read(buffer))
                                                           source.append(buffer.data(), readSize);
                                                       require(source.find("webFront.ready") != string::npos);
                                                   });
                                               });

                                               when("Requesting favicon.ico", [&] {
                                                   auto faviconFile = FS::open("favicon.ico");
                                                   then("correct data is returned", [&] {
                                                       require(faviconFile.has_value());
                                                       array<uint8_t, 1024> buffer;
                                                       auto readSize = faviconFile->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                                                       array<uint8_t, 16> head, tail;
                                                       require(!faviconFile->isEncoded());
                                                       require(faviconFile->getEncoding().empty());
                                                       require(readSize == 766);
                                                       head = {0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x20, 0x20, 0x10, 0x00, 0x01, 0x00, 0x04, 0x00, 0xE8, 0x02};
                                                       tail = {0X00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x01, 0xE0, 0x00, 0x00, 0x07};
                                                       require(equal(head.begin(), head.end(), buffer.begin()));
                                                       require(equal(tail.begin(), tail.end(), buffer.begin() + readSize - tail.size()));
                                                       readSize = faviconFile->read(span(reinterpret_cast<char*>(buffer.data()), buffer.size()));
                                                       require(readSize == 0);
                                                   });
                                               });
                                               when("Requesting WebFront.js", [&] {
                                                   auto webfrontJSFile = FS::open("WebFront.js");
                                                   then("Wbefront.js V0.0.1 content should be returned", [&] {
                                                       require(webfrontJSFile.has_value());

                                                       // WebFront.js is embedded verbatim rather than pre-compressed: deflate streams are not
                                                       // reproducible across platforms, so the raw bytes are what the build can verify. Nothing
                                                       // downstream advertises a Content-Encoding for it (see HTTPServer::handleRequest).
                                                       require(!webfrontJSFile->isEncoded());
                                                       require(webfrontJSFile->getEncoding().empty());

                                                       array<char, 3> sourceHeader{};
                                                       webfrontJSFile->read(sourceHeader);
                                                       require(sourceHeader == array<char, 3>{'/', '/', '/'});
                                                   });
                                               });
                                           });
                                       }};
