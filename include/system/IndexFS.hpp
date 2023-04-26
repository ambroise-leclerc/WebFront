/// @date 26/04/2022 22:39:42
/// @author Ambroise Leclerc
/// @brief Virtual file system providing access to WebFront.js, favicon.ico and a minimal index.html.
#pragma once

#include "FileSystem.hpp"
#include <tooling/StringHash.hpp>

#include <filesystem>

namespace webfront::filesystem {

class IndexFS {
public:
    IndexFS(std::filesystem::path /*docRoot*/) {}
    IndexFS() = delete;
    IndexFS(const IndexFS&) = default;
    IndexFS(IndexFS&&) = default;
    IndexFS& operator=(const IndexFS&) = default;
    IndexFS& operator=(IndexFS&&) = default;

    struct IndexHtml {
        static constexpr std::string_view encoding{"br"};
        static constexpr size_t dataSize{69};
        static constexpr std::array<uint64_t, 9> data{0xa1700400e0383dd6, 0xfcacc835c88b58d9, 0x52b82a1f239ddcf9,
                                                      0xa0d65a2581e511b4, 0x2cd0c06df85c8a2c, 0xa29e6831070def20,
                                                      0x0ff44a658bab1f4d, 0x14065fb5a7c383b8, 0x7904c81000000000};
    };

    struct WebFrontIco {
        static constexpr std::string_view encoding{"br"};
        static constexpr size_t dataSize{528};
        static constexpr std::array<uint64_t, 67> data{
          0xa1e81700f7658093, 0x2b7d8145c6c61015, 0xc4c80e31aa99183d, 0x54378e5eed6ed133, 0xfaca20ffde26c4fe,
          0x907c01bcd106aa84, 0x16c239717525dc81, 0x22b0dd830e70c053, 0x2d28ed2ecb3c4fbc, 0x0430c0d5c91daf4f,
          0x2cd10c1eb53020ac, 0xffcdde67d7cf55af, 0xdca9e2830d38c108, 0x02e090573dedc964, 0x4a677eea547dcc18,
          0x9b828b2818b06030, 0xb1d56e2bc8a235a8, 0x57f5b1c17b35af71, 0x09dcb5b387b49237, 0x2bdf037ce083b6b6,
          0x2cf8208817fca008, 0xc0188036006fe0f1, 0xe8c664f10f9ded6d, 0xf1602cf0eafe452b, 0xccc1e6541d9eed16,
          0x622383f1703e31d1, 0x6c85e32b9771da2a, 0xf1fbbb4b1b9de5ce, 0x7871d6e52607625f, 0x0f11b7ddbf8ce898,
          0x36fafcd39cf2feab, 0xdf7b812c200bd7c9, 0xa04403e83225c651, 0x5970c2e4d00fc03f, 0x8a6a815662143084,
          0x42e7539252581640, 0xd64742491a255ac8, 0x024e89050883ccf0, 0x948edad49eb3b200, 0x2932fd7a3fa2a643,
          0x0caee9e2e8369bc5, 0xf0a48e2c487a14ce, 0xbf9ea9b66b209da8, 0x688fa7b3abb9a366, 0xf9b40909d3d7fc74,
          0x3a527b181930c5eb, 0xea13b3cf8b3ba3b3, 0x6a80e6e995babfbf, 0x433e003e386581fe, 0x634428651a6664dc,
          0x8cd11a312546eb0d, 0x0b0a7259de712cf8, 0x204c13d9fc84a554, 0x5616147cfb2fc7d5, 0xd5711cb7f5dfc302,
          0xe7762297f2d9d1f1, 0x1bb50b483ff7cab2, 0xcbaddcfaecb29b0f, 0xf00f75b5bcbebdca, 0xfedcaabc563fb7eb,
          0xe7e86527d9eb7fec, 0x5f1ef92563fb2708, 0x74c08718f1cb0272, 0xcb0d2100faca96e8, 0x30c1a7e51d0d0c67,
          0x7640a3c384ef0122};
    };

    struct WebFrontJs {
        static constexpr std::string_view encoding{"gzip"};
        static constexpr size_t dataSize{3501};
        static constexpr std::array<uint64_t, 438> data{
          0x1f8b080872a34764, 0x020357656246726f, 0x6e742e6a7300d51b, 0xdb8edb36f6bd40ff, 0x81f5432dc58ac6f6,
          0x4ca6d371bc6d934e, 0x802eb24dd1c9b60f, 0x8661c8366d2b9125, 0x43a2e7b2adff7dcf, 0xe145222551d24cd1,
          0x057690c016c573e1, 0xe1b9933e3b3b23df, 0xaf0346c9f89bb3e1, 0xe86c3c1c8fc9787c, 0x7d31babe187ff9c5,
          0x19be0e8e6c97a4e4, 0x87fd324dc28c92f7, 0x7415d17425df2ed3, 0x906ec8ef74f92e4d, 0x6246565148f12349,
          0xe9975fdc0529592c, 0xe803a3f13a2353e2, 0xb05d9891afbf26f8, 0xe9e76f5cf2e79fc4, 0xd91ce3150b939838,
          0x2ef9e3cb2f08fc21, 0xbc9873cb0216ae10, 0x47316ded91653e15, 0xffca533f2c3fd115, 0xf333ca7e491396b0,
          0xc703fdb0015a0504, 0xfe397f008f079cb0, 0x585c93d99c9c4818, 0x672c885734d9901f, 0xd2347844962b74c9,
          0xdacfe180d872424e, 0x6e0579156a03a274, 0x706107208343e186, 0x3892d38362d3df05, 0xd987fb18d83ed094,
          0x3dfaab208a9ca547, 0x0eae4bd6b3c31ce9, 0xc107909c14f452ca, 0x8e696c4a41909593, 0xd46439b14192c813,
          0xf201025892afa653, 0xd253937b280b3116, 0x1fa3c83597cb7669, 0x724f627a4f3e02f4, 0x4d9a26a9d37b1b05,
          0x59469416dc05d191, 0x921e19905b9686f1, 0xd601ca037806c588, 0x134602501d907e7a, 0x5c311014fc432a3d,
          0x7762d965637d86c0, 0x170bd423a1693aca, 0x295983d80a807521, 0x75942a99ca9591ef, 0x94feac520a06827c,
          0x5e1367b130e7174f, 0x1e5f3792d5e57d72, 0x1d7c1496901d00d5, 0x5aa854d51ab4b7dc, 0x228afd6189473669,
          0xb2070508569ff39d, 0xc25dc2019c1ca4db, 0xe31e2c2ff3231a6f, 0xd98eaf63ec16ea16, 0x02c5a14722342140,
          0x25a77900388177af, 0x49041f8341450b00, 0x12b07fe584a8ad08, 0xe81a53d4b4af82d4, 0x0554809cf3af6972,
          0x16852b2a14582c01, 0x9808f51dc3bf209d, 0x8573c9197cd3deca, 0x9d3a199acb12dcd2, 0x55c0247b2d34f986,
          0x9ce42edcd3e5063d, 0x153c6a2e478d1aae, 0xe78ea619be9d821e, 0xed834f497a8dccef, 0xc318bf8d7033d86a,
          0x07df72c3caf1ad92, 0xfde1c8e84dbc0e83, 0x38a659e6185243e4, 0xc79f62763e06dca8, 0x35ff0ef1812fc399,
          0x0d1f46a321fc6d36, 0x735d4e0ae84a83b9, 0x122002975bf5057c, 0xfe6c38e7da307c78, 0xf70eb49a72a6fc28,
          0x642ca2a0d2a539a3, 0x513167196e618209, 0x30d05e4e8ccde1de, 0x9abf93e39a7cc5b8, 0xe9adf9d04c7ef404,
          0xfa1e6a01f242d4c0, 0xc40a010cf0e9233e, 0x1d9fec7363f01677, 0x027bcdde7004728a, 0x5a932b79e6b1497e,
          0x054538e5d68dcb85, 0xb0f73e8c3fa333a2, 0xd545eb6f8da5eb2f, 0x66c643ef188771c8, 0xc2200aff43d79a34,
          0xccf149275cbb001c, 0xee2ef80c5e561394, 0x3eda0d4f045f2533, 0x63b9357ca010953e, 0x9f0bcc18a888ed67,
          0xcade078f14ddc5d9, 0x8b17e4fb158f102f, 0xce6a9300c3b014a4, 0x53f142dcc3728f3a, 0xe50eb5e461300424,
          0x11f5a364ebf462ca, 0x224e3f0f0cb09ab2, 0x4fe25e39a5abbb37, 0xc7cd86f38a56c70d, 0x4e8c38af46632bd0,
          0xbfb26dd54e4b286b, 0x8133d0b527534420, 0x3bc502653d70b2fa, 0x4c9984858dbbe5cf, 0x4eff3ebb3e3b8b12,
          0x70a3bb2463d77d30, 0xfdfb305e27f73e0e, 0xe276f88724651ee9, 0xabec6f31f447fd06, 0x1a7e124342131b59,
          0x1cbda39adbb56d59, 0x5f39681c8da98045, 0x5c745d21887f0b9d, 0xea328c83f4f1a388, 0xd8fd0005b3e4e2e8,
          0x5b2193f8032077ca, 0xa84f8d6b5b454946, 0x3b2e0e83267febdf, 0x07d9db888273ac4e, 0xaa28ee8c939893b7,
          0x8510f8c81a3e0045, 0xf4e8c1fc359df654, 0x7c142470cc831c0b, 0xf28b2c89a73dd77c, 0x2d86dd3a39d228a3,
          0xed7cf56bf85a8735, 0x5bd32c3f8af9a229, 0x3f1cc134ce94021f, 0x9e9362957c9a3b69, 0xc1bf07671f6cbbee,
          0x10cf004230096118, 0x3f062cf80d1ea5cc, 0xa0660aea04865010, 0x61f6e0640110e1fd, 0x2d65dc229d611d00,
          0xd4500d929df9be5f, 0x32698dfedcdf0707, 0xe7814cff411e7c96, 0xc8847a74e9fa8700, 0x33e494396330cf61,
          0xdf75fd4f49183b7d, 0xd2afdde6ec3e8454, 0x863892f35a7970d6, 0x0250f011386dd869, 0x3ed3c7f4f3c5d975,
          0xfd74a5e8d21a4530, 0x802c438f0ebe168f, 0xac744b662da38a81, 0x46c4a44917789159, 0xdca8986e6ed2c805,
          0x16cd9ca705a9a19b, 0x922750996db20a81, 0xb1356450194d2197, 0xc412a7e7d5f1f09d, 0xca7624dd1ec06042,
          0xa31edd060e4ef657, 0x4b30eccf93869d1c, 0xeb3bc9a0b092df9b, 0x77b4658fd0029203, 0xba9c1ad94eda6191,
          0x8ff7b4bc31a34b67, 0xec76da5d6d1d8ee0, 0xc3235613f2c885a7, 0x28ba7f9798cf7531, 0x6355f24eb99fbf2a,
          0xe7439006fbec6d72, 0x8cd9f3842d10a06f, 0xbb85c4b284e37cec, 0x5cd4e96b1b62963e, 0xb6315f6c17cae39f,
          0x999288a3adc8b3f9, 0x5d8f5c7925cedd36, 0x9e4e2d361c70ffa7, 0xe24d3bf3cae8a9e8, 0x71fc1e42c9cd9772,
          0x7bcd9b1b02d3f399, 0x7a86be9dbac4da3a, 0xd769d41675502587, 0xb909202ba8d6e8f8, 0xa7f273ad18d7fd8a,
          0x1e79956de27bdce0, 0xda8c1e5fca00fc11, 0xbefe481106e47d64, 0x9b975790c7acf980, 0x93e32871afe29aa0,
          0x559f65a2850ed142, 0x3f029ffbf5073e15, 0x102f8fdbf7c9d66e, 0xa2badb47fab6cdb6, 0x6d57114b0dc2618c,
          0x4da7db551a1e1812, 0xb729234a2713b3a6, 0x649dac78f747f6aa, 0x6e228a4f4e4f4cb0, 0xc60ff1da9742c60f,
          0xcbc41cff32593ffa, 0xc10172e3f5db5d18, 0xad1d81c2eda08e9a, 0xbe4c1a1546ece92f, 0x68df94d1d46cfa1a,
          0xfe41388364b3c928, 0xab6d4b89573ce7b8, 0x4bc23519622ea906, 0xc97052663277898a, 0xf06c3ea9ce40b2c0,
          0x5ec64b3481ad34ab, 0x68f222b29fe2357d, 0x10f4b4e7d7baf79e, 0x90c1a07867cd4765, 0xd71119281c7dc14e,
          0x536a87a01df23a2e, 0x762c9560ab13ac29, 0x3ea647caf5101255, 0x19d641b2f225a634, 0x503f53bbb32aa4e9,
          0x1f8ed9cec1d94d6e, 0x5193ed604a4693bf, 0x9add54d6f30e9d57, 0xe382b87bebbe223e, 0xfd7fb4a4737349f1,
          0x71bf04ace66206e4, 0x8a2c1f19cdc84f37, 0x3737dfbcba209b28, 0x81723dde9203d400, 0x8c08a8ee0b54daf6,
          0x0ed15c5e38fa7ab0, 0x0d5acd0fba4be3db, 0xbf228d0b531ad91e, 0xe2afa8812a221971, 0x91900c225c03c257,
          0x258439ae8624cd20, 0x339692b7d3d1adf9, 0x3e4931240aa39e5a, 0xa93f2967e638bf33, 0x3c04a4ceed5b0615,
          0x87cdab2048b75cf2, 0x1134642dfd530eeb, 0x4896ce81c4b86b01, 0xd021e67376451f47, 0x34fce5086cc007e1,
          0xe1078a2511296a5f, 0xc1b72ee97fad2b6b, 0x08fa35da3ead92fc, 0x9bea8dd195a949ec, 0x788884cbfba3ce28,
          0x36fc948d7b9264a3, 0x87bffb5d089183f7, 0x48437684996c07ff, 0x11999d39dcc078c9, 0x69670d91aa4da778,
          0x98e5fbfc2b0fd55c, 0x61cb8981a308a94c, 0xc0c0dfa86b151fa7, 0x68cd86f3eede6b0c, 0x640ac8d1dc02787a,
          0x4a72a49ddecc0a2e, 0x8db5bd944987714e, 0xd69c539955567346, 0x55df5a0facbb3092, 0x605e8ef4677805d3,
          0x170148d32bf22d78, 0x1ed5e55346c6554f, 0x432f745f925129f5, 0x0332e5b2a5be04a5, 0x0f74058aace4f0e6,
          0x1139c57c367a7444, 0x82e899c7b4ce4c5f, 0x135f8ee881cf3da2, 0xef8e48022aedced2, 0x19445b91591c9fab,
          0xa373d8360c02f96e, 0x616959e2889f9e37, 0x149ddd13effb3464, 0x6677d4d60fd40f1d, 0xf8d1a1ea795aeaaf,
          0xa6be625180a91689, 0xea08527be83518e0, 0xe75ab263274ef126, 0x4d507525b0d6846c, 0x8415ad6424ece82c,
          0xc80004da33763bca, 0x5fdab8da06759656, 0xdc1e308f19c5ed9a, 0x86f3b245763c5053, 0xadf20b368e4200da,
          0x2da6d5dd9850939a, 0xcfd6040271b88e43, 0xfcaa42cb815bbfb8, 0x1e541cb8e527cba4, 0x9f1f2a97652785b4,
          0x28e13f19c7961cb3, 0xeeeb0e875a471783, 0xbd5496a66e14f056, 0x5401f8068c1c56b9, 0x0ce5467b84435b1c,
          0x6e33130297c18a6e, 0xc2b5d20ed26d6df9, 0x9917960bbccf319a, 0xe0e7ebcae50f1c2e, 0xdde5286e5a6cb319,
          0x00bd14a7c139248c, 0xcd9bbd1752dd815f, 0xa4a96c155ed5ba72, 0x9ee2c819c3d20cfd, 0xed60aaeee6f0e3f8,
          0xdcd7e34b533e3611, 0x84a2a4960228d63e, 0x18843567699d6973, 0x0985952cc0e8f2ab, 0x4c0ef37c08085144,
          0x96e24e9346c5ad11, 0x8f3c81c2ae65cde9, 0xaec1a226eb3a4c98, 0xb9fd56733aa55128, 0x832910bc10270fa4,
          0xbcc6de743b8291a7, 0x8bbe2ea9abc28c3d, 0x326c9f75de6196e8, 0x4f6b52ab2b6c6a64, 0x17c6904531d59529,
          0xabb1fe56290a8d8d, 0x7cc4d04fcfba019e, 0x8ecb7d961ed71e1d, 0x7760502af173792b, 0x29bccc90e448e538,
          0x45d7b99614a81245, 0x2dfadae65d6b0cb7, 0x9ad43ef58e88de2a, 0x4b360a893d69e9c9, 0xa651ef5a859191ad,
          0xcfdb13055631f35b, 0xeb4c51f7f79a1bbf, 0x62bdaa44be894589, 0xec4a3d909c2b4daa, 0xc72319c984b771f8,
          0xe76b327e7509b5fa, 0x1882f37987a66ec1, 0x75c2ef4af62ce91a, 0xf662c525bd30133a, 0xd320dbf2421723bc,
          0x7834c1d2f523af64, 0x79c324a6749da986, 0x4b4ba1e783addd04, 0xab9d7e214db4c8b1, 0x1fac09024881452d,
          0xecc14181e165df86, 0x32d140d9bd2a5cd3, 0x4d708c9835e72d55, 0x07f91d5bd8ad3e46, 0x1f5371b130e86343,
          0x1352ac630c691bde, 0xd681bc0b67f53a66, 0xa935b657723555b3, 0xf388e8ca583bf32d, 0x66f89cc6fd934db7,
          0x8b95897b43ed7636, 0xb1a3c0207eab7478, 0x38b15b87a0a50229, 0xb7c346e3508d2fca, 0xf0faa692d7000bd8,
          0xc6b66893d6da7042, 0x9437d86bc2612c78, 0xdc5df5d565a35a61, 0x5cbe7a75fe6c71d8, 0xdaaa9d2531bab48b,
          0xa243c66195cef9d3, 0xa563c75c710ebf06, 0xf15678876b50f918, 0xefd60ba525520260, 0x26fb24c5a61ef887,
          0xcb8bcf6f7084f7eb, 0xace78572e9556fca, 0x2ff5634a210eafac, 0xdba1ad7f20667be2, 0x07014d0e553a5303,
          0xd6d884a7c4a93c64, 0x5b5668d7236eeb10, 0x1b1b8fc9b0c7dd78, 0xecd4b2c8f604e2c9, 0x7cdbce8cac9b9ca3,
          0x52073ebaf64b0fdf, 0x59eb5bd39dbf2171, 0x681086ad35fe4cb7, 0x28b28b76afc88f35, 0x200047b4d5373e2d,
          0x693190e6a94bb91a, 0x90704560d6c13ae6, 0x323ac8d31c975e39, 0x5b03e6ff45ea236e, 0x051b194ffe1b23de,
          0x3c75ac7d4cb7f6b7, 0x5a8a46defd53edc0, 0xbc8e52f79e7dad07, 0x5882ca99a9ed6e97, 0x6a55d80c7e6e55fe,
          0x394a5dc3c96c368d, 0x9fd66c2a1a4de3a6, 0x46d3c96403bb6cd9, 0x215851fdd6076fd2, 0x670770374ecf0c4e,
          0x0882b33047cb2167, 0xc557954260b74b03, 0xd3261c928371cbbb, 0xae2aafe0133f95aa, 0xf487e4c1a5fca6b1,
          0x0195f8bcbaf96a1e, 0x2e605e94d83826da, 0x2a6ef1fb9a939bff, 0x4489ffbe227f90bf, 0xadc0e228a7972b4d,
          0xfe8b27a539684115, 0x8de2ebff2ff6cede, 0xa7093a0000000000};
    };

    static std::optional<File> open(std::filesystem::path file) {
        auto filename = stringHash(file.relative_path().string());
        switch (filename) {
        case "index.html"_hash: return File(IndexHtml{});
        case "favicon.ico"_hash: return File{WebFrontIco{}};
        case "WebFront.js"_hash: return File{WebFrontJs{}};
        }
        return {};
    }
};

} // namespace webfront::filesystem