/// @date 01/03/2022 22:39:42
/// @author Ambroise Leclerc
/// @brief Provides access to local or virtual file system.
#pragma once

#include "../details/C++23Support.hpp"
#include "tooling/Logger.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <sstream>
#include <string_view>

namespace webfront::filesystem {


class IndexFS {
public:
    IndexFS(std::filesystem::path /*docRoot*/) {}
    IndexFS() = delete;
    IndexFS(const IndexFS&) = default;
    IndexFS(IndexFS&&) = default;
    IndexFS& operator=(const IndexFS&) = default;
    IndexFS& operator=(IndexFS&&) = default;

    class File {
    public:
        File(std::span<const uint64_t> input, size_t fileSize, std::string contentEncoding = "") :
            data(input), readIndex(0), lastReadCount(0), size(fileSize), eofBit(false), badBit(false), encoding(std::move(contentEncoding)) {}
        File() = delete;
        
        File& read(std::span<char> s) { return read(s.data(), s.size()); }
        [[nodiscard]] bool isEncoded() const { return !encoding.empty(); }
        [[nodiscard]] std::string_view getEncoding() const { return encoding; }

     
        // fstream interface
        [[nodiscard]] bool bad() const { return badBit; }
        [[nodiscard]] bool eof() const { return eofBit; }
        [[nodiscard]] bool fail() const { return false; }
        [[nodiscard]] size_t gcount() const { return lastReadCount; }
        [[nodiscard]] size_t tellg() const { return readIndex; }
        [[nodiscard]] explicit operator bool() const { return !fail(); }
        [[nodiscard]] bool operator!() const { return eof() || bad(); }
        void seekg(size_t index) { readIndex = index; }
        void clear() { eofBit = false; badBit = false; }
        File& get(char* s, size_t count) { return read(s, count); }
        File& read(char* s, size_t count) {
            constexpr auto bytesPerInt = sizeof(decltype(data)::value_type);
            UIntByte chunk;
            for (size_t index = 0; index < count; ++index) {
                if (eof()) {
                    badBit = true;
                    lastReadCount = index;
                    return *this;
                }
                
                if (readIndex % bytesPerInt == 0) chunk = convert(data[readIndex / bytesPerInt]);

                s[index] = static_cast<char>(chunk.byte[readIndex % bytesPerInt]);
                readIndex++;
                if (readIndex == size) eofBit = true;
            }
            lastReadCount = count;
            return *this;
        }
        
    private:
        std::span<const uint64_t> data;
        size_t readIndex, lastReadCount, size;
        bool eofBit, badBit;
        const std::string encoding;
    
        union UIntByte {
            uint64_t uInt;
            uint8_t byte[sizeof(uInt)];
        };

        static UIntByte convert(uint64_t input) {
            UIntByte result;
            uint64_t bigEndian;
            if constexpr (std::endian::native == std::endian::little) 
                bigEndian = std::byteswap(input);
            else
                bigEndian = input;
            std::memcpy(result.byte, &bigEndian, sizeof(bigEndian));
            return result;
        }
    };

    struct IndexHtml {
        static constexpr size_t dataSize {143};
        static constexpr std::array<uint64_t, 18> data{
          0x3c21444f43545950, 0x452068746d6c3e0a, 0x3c68746d6c3e0a20, 0x203c686561643e0a, 0x202020203c6d6574,
          0x6120636861727365, 0x743d227574662d38, 0x223e0a202020203c, 0x7469746c653e5765, 0x6266726f6e743c2f,
          0x7469746c653e0a20, 0x203c2f686561643e, 0x0a20203c73637269, 0x7074207372633d22, 0x57656246726f6e74,
          0x2e6a73223e3c2f73, 0x63726970743e0a3c, 0x2f68746d6c3e0a00
        };  
    };

    struct WebFrontIco {
        static constexpr size_t dataSize {766};
        static constexpr std::array<uint64_t, 96> data{
            0x0000010001002020, 0x100001000400e802, 0x0000160000002800, 0x0000200000004000, 0x0000010004000000,
            0x0000000000000000, 0x0000000000001000, 0x0000000000003834, 0x3200e09b1200ead9, 0xb0006c676200ae9f,
            0x7500e6b24200524e, 0x4800e1a429009d7b, 0x2f00bfbab900857e, 0x6f00fbf8ef00b887, 0x2000866d3800e8c2,
            0x6d0063594600bb23, 0x0000000000000000, 0x0000000032bbb900, 0x0000000000000000, 0x00000000009b2000,
            0x0000000000000000, 0x000000000002a000, 0x000000006d8cc8d6, 0x0000000000036000, 0x006000d111717111,
            0x1d00000000000000, 0x00006c1777777777, 0x7110000000000000, 0x0006117777777777, 0x7777f00000000000,
            0x00f1777777777777, 0x77771f0000000000, 0x0017777171717177, 0x7777710000000000, 0x0117111717171717,
            0x1717171000000060, 0xf111711111111111, 0x1171111f00600000, 0xc111111111111111, 0x1111111100000600,
            0x1111111217be7e1b, 0xe71111116000000d, 0x111111eb7ebb111b, 0x21111111d000060d, 0x111111227b2b5112,
            0x211111118060000c, 0x111115b5eb72211b, 0xb2251111c0000668, 0x11111221b21eb11b, 0x2ee5111180600608,
            0x11117be5b711be1b, 0xe1111111c060066d, 0x1111eb1221112b1b, 0xe7111111d660066f, 0x1111be7be1117b5b,
            0xbbbbb111f0600666, 0x1111111111111111, 0x1111111166600666, 0xd717171717171711, 0x7171771d66600f66,
            0x6777777177717777, 0x1777717666606666, 0x6377777777777777, 0x7777773666f006ff, 0xf685575777575777,
            0x57775a6f6f6666f6, 0xff3a575555757555, 0x5555affff6f066ff, 0x3f3fa55555555557, 0x555af33636f6f6ff,
            0xf333334555555555, 0xea333333636faff3, 0xf3333333a445444a, 0x3a33333ff664b36f, 0xf333a3aaaaaaaaaa,
            0xaa3a3333f332b243, 0x363f333333333333, 0x33333fffaa2bbbb9, 0x9999999999999999, 0x999999999bbbc000,
            0x0003800000010000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000,
            0x0000000000000002, 0x1000001310000005, 0x0000002418000008, 0x9000004890000040, 0x500000905f800000,
            0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000000000000000, 0x0000800000008000,
            0x0001e00000070000};
    };

    struct WebFrontJs0_1_1 {
        static constexpr size_t dataSize{3531};
        static constexpr std::string_view encoding {"gzip"};
        static constexpr std::array<uint64_t, 442> data{
          0x1f8b0808b7650f64, 0x000357656246726f, 0x6e742e6a7300d51b, 0x6b6fe336f27b81fe, 0x07d61f6a79ad556c,
          0x279be6e2f5b5dd6d, 0x16e861af5b347bed, 0x072330648bb6b52b, 0x4b8244e7d136fffd, 0x66f8904889949d14,
          0x3de0845dc4a66686, 0xc399e1bc489f9c9c, 0x90efa2905132f9e6, 0x64343e998c261332, 0x995c9e8d2fcf265f,
          0x7e7182afc33ddb66, 0x05f97eb72cb2b8a4, 0xe43d5d25b458c9b7, 0xcb22a66bf21b5dbe, 0x2bb29491551253fc,
          0x9315f4cb2f6ec382, 0x2c16f49ed1342ac9, 0x8c786c1b97e4ebaf, 0x09fe0daa3703f2e7, 0x9fc45befd3158bb3,
          0x947803f2c7975f10, 0x78105fc05cb390c5, 0x2ba45183453e5956, 0xa0f834413f2c3fd1, 0x150b4aca7e2e3296,
          0xb1879c7e58c35c35, 0x063ede1fc0638e00, 0x8bc52599df904712, 0xa7250bd315cdd6e4, 0xfba2081f90e5d6bc,
          0x240a2a3c986c3925, 0x8f8316f136d61a44, 0xe9e1c272980687e2, 0x35f124a7b96233d8, 0x86e587bb14d8ce69,
          0xc11e82559824ded2, 0x27f96040a2797e83, 0xf3c11f98725acf57, 0x50b62f52530a625a, 0x09a4802560872491,
          0x27e40304b0245fcd, 0x66a4a7807b280b31, 0x96ee9364602e976d, 0x8bec8ea4f48e7c04, 0xecaba2c80aaff736,
          0x09cb92282bb80d93, 0x3d253d3224d7ac88, 0xd38d07330fe13b18, 0x469a311282e980f4, 0x8bfd8a81a0e01fce,
          0xd21b4c1d5a36d667, 0x087cb1403b1296a6, 0x939c9108c4562344, 0xb5d451aa64265746, 0xbe55f6b32a286c10,
          0xe4f392788b85095f, 0x7ff3f9ba715a5dde, 0x8f030fbf8a9d50e6, 0x402a1226d5de0dda, 0x5bbe236afdb0cc27,
          0xeb22db810184abcf, 0x95a6504b3880c061, 0xb1d9ef60e7954142, 0xd30ddbf2754c06b5, 0xb9c530e3c827096e,
          0x212025c17c409cc2, 0xbbd724813fc361cb, 0x0a0013a87fe5c568, 0xad8838304014d857, 0x61310052409cf3af,
          0x597299c42b2a0c58, 0x2c019888758de113, 0x16f3f84672069fb4, 0xb752538f86e5b20c, 0x55ba0a9964efc09c,
          0x5c218f520b7774b9, 0x464f055f3597a346, 0x0dd7734b8b12dfce, 0xc08e76e1a7acb844, 0xe677718a9fc6a80c,
          0xb6dac2a76a6355f4, 0x56d92edf337a9546, 0x7198a6b42c3d436a, 0x487cff63ca4e2740, 0x1bade63f317ee1cb,
          0xf0e6a3fbf17804cf, 0x7a7da3cb49215d68, 0x38170245d01ab47d, 0x01879f8f6eb8358c, 0xeedfbd03aba69ca9,
          0x2089194b28987403, 0x663cae6196f10600, 0x4c84a1f6726a2887, 0x7b6bfe4e8e6bf215, 0xe3a6b7e64373f9a7,
          0x27c8f7d00a9017a2, 0x06a64e0c6080838f, 0x39387e73c3a6e02d, 0x6e05758b6e380109, 0xa2d634903cf3d824,
          0x3f82213c56bb1b97, 0x0b61ef7d9c7e4667, 0x44db8bd6df1a4bd7, 0x5fcc8d2fbd7d1aa7, 0x318bc324fe9d469a,
          0x34ccf1e951b4b621, 0x38dc6df819bcac26, 0x287df4383a097c94, 0xcc4ca46af8402d2a, 0x1d9e0bcc186889ed,
          0x27cade870f14ddc5, 0xc98b17e4bb158f10, 0x2f4eac4980b1b114, 0xa6d7f242dcc3728f, 0x3ae30eb5e1613004,
          0x64090d926ce3f552, 0xca123e7f15186035, 0x4d9fc4bd724157b7, 0x6ff6eb35e715771d, 0xdf7062c47b359e38,
          0x91fe5d6edafbb441, 0xd28a5c82ad3d7946, 0x4472cf5893b42367, 0xabcf94495c50dc35, 0xffeef5efcacb9393,
          0x240337bacd4a76d9, 0x87ad7f17a7517617, 0xe020aa23c8b382f9, 0xa4afb2bfc52818f7, 0x3be608b214129ad4,
          0xc8e2e82dd5dcae4b, 0x657de5a07134a502, 0x1769d1a835213e0b, 0x7dd6659c86c5c347, 0x11b1fb210a66c9c5,
          0xd1776266e90720ee, 0x35493f76ae6d9564, 0x253d72711834f9db, 0xe02e2cdf26149c63, 0x1ba82985de9c4f71,
          0x43ded642e02311fc, 0x0112c9830ff0119d, 0xf5547c1453e0980f, 0x3916e4176596ce7a, 0x03f3b5181ed8e448,
          0x93921ee6ab6fe12b, 0x8a2daae9961fc57c, 0xd1941f8e601a674a, 0x810fdf907a951c6c, 0x303d407f07ce3edc,
          0x1cab219e01c4b025, 0xc4c6f82164e1aff0, 0x55ca0c6aa6d02630, 0xc48208b303270b88, 0x881f6c28e33bd21b,
          0xd910a086ea90ec3c, 0x0882c696d6e6bf09, 0x7661eedd93d93fc9, 0x7dc03299508fcf07, 0x411e62865c306f02,
          0xdb73d41f0c824f59, 0x9c7a7dd2b7aab9bc, 0x8b2195219ee4dc2a, 0x0fce5a08063e06a7, 0x0d9ae69001a69f2f,
          0x4e2eede0f8a0a1cb, 0xdd28820164197a74, 0x08b478e49c573d06, 0x21938c884996b5b5, 0xf1456671a562baa9,
          0xa4f1005834739e03, 0x440ddb943c81c96c, 0xb2550c8c45904195, 0xb4805c124b9c9e6f, 0xe3e15b95edc8797b,
          0x8083098dfa6a5399, 0x7a1eddaf96b0b13f, 0x3b50b92627ba2619, 0x1456f273b7460fe8, 0x08774096a3cbb1c8,
          0xf680281117f9784f, 0x9b8a199f7badd0d7, 0x7c8464b57578820f, 0xbf1915eb2de49333, 0x5fcd68dd19eaf92b,
          0x623ed5c58c55c93b, 0xe57efeaa9cf3b008, 0x77e5db6c9fb2e709, 0x5b1040df760d8965, 0x83c6e9c43bb3d9eb,
          0x21c2ac7838c43c3e, 0x8230cae35fa59288, 0xa7adc877f95d9f5c, 0xf80dce3b75874f87, 0xfef05985dcffa978,
          0x739879b5e9a9e871, 0xfc1643c9cd97727d, 0xc99b1b82d2f3997a, 0x86bd3550ecb1d0e6, 0x3a8ddac286d57098,
          0xeb10b282768d8e8f, 0xcacfb5625cf72b7a, 0xe4557b13dfa382ad, 0x193dbe9401f8237c, 0xfc81220ec87bcfd6,
          0x2f2f208f89f88057, 0xd16870afe29a98cb, 0x9e65e20e1de10efd, 0x087ceea20f1c1408, 0x2ff79bf7d9c6bd45,
          0x75b78ff3bb94ed52, 0x571d4b8d89e3149b, 0x4ed7ab22ce194eee, 0x3246944e29a06624, 0xca56bcfb237b5557,
          0x09c56f5e4f0038e3, 0x87781d4821e31f07, 0x60457f99450f4198, 0x436e1cbdddc649e4, 0x0912b6099ae6a8d9,
          0xcbb4d360844e7fc6, 0xfd4d192dcca6afe1, 0x1f8433c8d6eb92b6, 0x73384c3cc42b9e73, 0xdc66714446984baa,
          0x41329a3699ac5ca2, 0x9a787e336d43e0b4, 0xc05ec94b3441ad01, 0x55377991d88f6944, 0xefc57cdaf7d7baf7,
          0x9e92e1b07ee7cc47, 0x65d71119a81d7dcd, 0x4e576a87a847e475, 0x5cec582a81aa33ac, 0x293e167bcaed1012,
          0x5519d641b2f225a6, 0x34503f3b4a047c6a, 0x6906f9bedc7a08dd, 0xe51635d90e6764dc, 0x01795c76d35acf3b,
          0x745e9d0be2eeedf8, 0x1571f0ffd1924ecd, 0x25a5fbdd12a89a8b, 0x19920bb27c60b424, 0x3f5e5d5d7df3ea8c,
          0xac930ccaf5744372, 0xa801181158c72f50, 0x59db3b24737ee6e9, 0xebc136683b3f385e, 0x1afff82bd23833a5,
          0x51ee20fe8a1aa825, 0x923117092921c275, 0x107cd52058d1ea48, 0xd28c692652f2ee79, 0xd4c35bd159812151,
          0x6cea9973f627e5cc, 0x9ce6b7868780d4f9, 0xb0caa0e270791544, 0x392e977c000b89a4, 0x7faa703dc9d2294c,
          0x7130857f42cce7ec, 0x8a3e8e68f8cb1150, 0xc007e1e1878a2511, 0x29acafe0d331e93f, 0x3e2d57d611f4d563,
          0x848ad6947f53bd31, 0xbe302d89edf344b8, 0xbc3f6c9b62cd4fd9, 0xb827c9d67af8bbdb, 0xc61039788f34667b,
          0x80645bf88fc4dccc, 0xa102d3259fbbec88, 0x54876c8a8759aee7, 0x5f78a8e606db4c0c, 0x3c3591ca040cfa9d,
          0xb6d6f2716aaef9e8, 0xe678ef3581696acc, 0x713357508f4597ee, 0xe4081f797a33afb9, 0x34d6f652261dc639,
          0x59774e655659dd19, 0x95bdb51e3ab53096, 0x687e45f4277805e0, 0x8b10a4e9d7f9167c, 0x6fc9a89571d9e7d0,
          0x0bdd9764dc48fd60, 0x9a66d9622f41e93d, 0x5d81212b39bc7940, 0x4e319f4d1e3c9120, 0xfae631ad37d7d7c4,
          0x97237ae0373ed1b5, 0x23928056bbb37106, 0x71a8c8ac8fcfd5d1, 0x39a80d8340a52d2c, 0x2d1b1cf1d3f38ea2,
          0xf3f8c4fbae8899d9, 0x1d75f503f543077e, 0x74a87a9e8efaabab, 0xaf581760aa45a23a, 0x82d41d7a0d06f8b9,
          0x96ecd889533cc746, 0x7497c05a13b21357, 0xb49271624f674106, 0x20b09e499711586e, 0x6b2835a8b3b4faf6,
          0x8079cc286ed7749c, 0x972dca7d4e4db3aa, 0x2ed8788a0058b700, 0xd318a94828a0eeb3, 0x3541401caee310bf,
          0xaa70e0c0ad5f5f0f, 0xaa0fdcaa9365d2af, 0x0e959bb293425a34, 0xe86b225594755f97, 0xe7564797c27e692d,
          0x4ddd28e0ada81af1, 0x0d6c7258e532968a, 0xf609c77638dc6e26, 0x042d83157d0b5ba5, 0x1d161b6bf9591596,
          0x0bbccf319ee2dfd7, 0xadcb1f38dcb8cba1, 0x1e243c07a497e234, 0xb8c284b1e65c96da, 0x780b7e9116b25578,
          0x6175e53cc59110a3, 0x0684fe7638537773, 0xf8717ce5ebf1a529, 0x1f97086251524b01, 0xd46b1f0e63cb59da,
          0xd1737309c5ad2cc0, 0xe8f2ab4c0ef37c08, 0x08494296e24e9336, 0x4b9300b22c4fa0b0, 0x6b6939dd3558d464,
          0x6da38499dbaf96d3, 0x296d86269a42c10b, 0x71f240caefec4d1f, 0x2630f675d1db92ba, 0x36cec427ad83b036,
          0xd4e91150a23fad49, 0xcd56d8586417a790, 0x4531d595699ab1fe, 0x56190a4d8d7cc4b0, 0x4fdfa9005fa7d58a,
          0x0b47d9b1f5e8f808, 0x06a5113f97b786c1, 0xcb0c498eb48e5374, 0x9b3b9002b5a2a8c3, 0x5e0f7957cbc66d27,
          0xb54fbd23a2b7cab2, 0xb522e24e5a7ab269, 0xd4bb5461c4d6dc11, 0xa0a2c0aa216d8d0f, 0x0129eafe5e77e357,
          0xac5795c857a92891, 0x07d20e24e7ca92ec, 0x742423a5f0361eff, 0xfb9a4c5e9d43ad3e, 0x81e07c7a4453b7e6,
          0x3ae377257b8e740d, 0x7bb1e2925e5c0a9b, 0xe9906d73a18b315e, 0x3c9a62e9fa9157b2, 0xbc6192521a95aae1,
          0x72a0d00b60af5d85, 0xabad7e214db4c8b1, 0x1fac0902a6821db5, 0x7007078586977d3b, 0xca4483e4f1556144,
          0xd7e13e61ce9cb751, 0x1d54776c415b7d8c, 0x3ea6e16261d0c786, 0x26a458fb14d236bc, 0xad03791742b58e04,
          0x1c59aa65ef355c4d, 0x7bdbf94474659c9d, 0xf903dbf0398dfb27, 0x6fdd637699b83774, 0x789f3914ac12a66b,
          0x65c3cd74485fb098, 0x4b0552be0f3b3787, 0x6a7c5186d73795bc, 0x8658c076b645bbac, 0xd64513a2bcc15e17,
          0x0d63c193e34d1f1f, 0xbc6c6415c6f9ab57, 0xa7cf1687abad7ab4, 0x24c6e76e511c9171, 0x38a573fa74e9b829,
          0xb79cc32f61ba11de, 0xe1124c3ec5bbf5c2, 0x688994006c935d56, 0x60530ffcc3f9d9e7, 0x3738c2fb75cef342,
          0xb9f4b637e597fa31, 0xa51087574e7568eb, 0x1f0a685ffc20a0cb, 0xa14a676ae01a4a78, 0x4a9caa42b663856e,
          0x3be27b1d6263e731, 0x19f6b83b8f9d0e2c, 0xf27002f164be5d67, 0x464e2557a4d4818f, 0x6efdd2c31f6df507,
          0xd39dbf2171e81086, 0xab35fe4cb728b28b, 0xc35e911f6b40004e, 0xe841df88cff1498b, 0x41b44a5d9ad580c4,
          0xab03b38e76642ea3, 0xa33ccd71e995b333, 0x60fe5fa43ee256b0, 0x91f154bf31e2cd53, 0xcfd9c71c587faba5,
          0xe6a8ba7faa1d58d5, 0x51eade73a0f5001b, 0x581533d6ee76a356, 0x0565f073abe6cf51, 0x6c0d27b3d934795a,
          0xb3a96e344dba1a4d, 0x8f261bd8652bf370, 0x45f55b1fbc495fe6, 0xe06ebc9e199c1005, 0xa13047ab30e7f547,
          0x954260b74b43d300, 0xf22c376e79dbaaf2, 0x163df153a9567f48, 0x1e5cca4f1a1b5089, 0xdfb495afe0700137,
          0x75898d63a2ad52fd, 0x8e8cff8cacba018f, 0xbfafa8bec8df5660, 0x7154cd57194df58b, 0x276539b8835a16c5,
          0xd7ff5ff6cedea709, 0x3a00000000000000};
    };    

    static std::optional<File> open(std::filesystem::path file) {
        auto filename = file.relative_path().string();
        log::debug("IndexFS open {} -> filename:{}", file.string(), filename);
        if (filename == "index.html") return File(IndexHtml::data, IndexHtml::dataSize);
        else if (filename == "favicon.ico") return File(WebFrontIco::data, WebFrontIco::dataSize);
        else if (filename == "WebFront.js")
            return File{WebFrontJs0_1_1::data, WebFrontJs0_1_1::dataSize, std::string(WebFrontJs0_1_1::encoding)};
        return {};
    }

};

class NativeFS {};

} // namespace webfront::filesystem
