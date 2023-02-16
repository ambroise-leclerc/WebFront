#include <doctest/doctest.h>
#include <JsFunction.hpp>
#include <networking/NetworkingMock.hpp>

#include <algorithm>

using namespace webfront;
using namespace std;

std::vector<uint8_t> encodedBuffer;

bool checkType(size_t index, msg::CodedType type) { return encodedBuffer[index] == static_cast<uint8_t>(type); }

bool checkSize8(size_t index, uint8_t size) { return encodedBuffer[index] == size; }

bool checkSize16(size_t index, uint16_t size) {
    uint16_t comp {};
    copy_n(&encodedBuffer[index], 2, reinterpret_cast<uint8_t*>(&comp));
    return comp == size;
}

bool checkString(size_t index, string_view text) {
    string_view comp{reinterpret_cast<const char*>(&encodedBuffer[index]), text.size()};
    return text.compare(comp) == 0;
}

bool checkNumber(size_t index, double number) {
    double comp {};
    copy_n(&encodedBuffer[index], 8, reinterpret_cast<uint8_t*>(&comp));
    return comp == number;
}

template<typename WebFront>
struct WebLinkMock {
    WebLinkMock(WebLinkId id, WebFront& wf) : webLinkId(id), webFront(wf) {}
    WebLinkId webLinkId;
    WebFront& webFront;
    void sendFrame(websocket::Frame<typename WebFront::Net> frame) {
        encodedBuffer.clear();
        size_t bufCount = 0;
        for (auto& buf : frame.toBuffers()) {
            cout << bufCount << ": " << utils::hexDump(span(reinterpret_cast<const byte*>(buf.data()), buf.size())) << "\n";
            if (++bufCount > 2)
                for (auto byte : span(reinterpret_cast<const uint8_t*>(buf.data()), buf.size())) encodedBuffer.push_back(byte);
        }
    }
};

struct WebFrontMock {
    using Net = networking::NetworkingMock;
    WebLinkMock<WebFrontMock> getLink(WebLinkId id) { return WebLinkMock<WebFrontMock>{id, *this}; }
};

SCENARIO("JsFunction") {
    WebFrontMock webFront;
    WebLinkId id{1};

    GIVEN("A print JsFunction") {
        JsFunction print("print", webFront, id);
        std::string text{"maFunction"};
        const char* bigText =
          "Longtemps, je me suis couché de bonne heure. Parfois, à peine ma bougie éteinte, mes yeux se fermaient si vite que je n’avais pas le temps de me "
          "dire : « Je m’endors. » Et, une demi-heure après, la pensée qu’il était temps de chercher le sommeil m’éveillait ; je voulais poser le volume que "
          "je croyais avoir encore dans les mains et souffler ma lumière ; je n’avais pas cessé en dormant de faire des réflexions sur ce que je venais de "
          "lire, mais ces réflexions avaient pris un tour un peu particulier ; il me semblait que j’étais moi-même ce dont parlait l’ouvrage : une église, un "
          "quatuor, la rivalité de François Ier et de Charles Quint. Cette croyance survivait pendant quelques secondes à mon réveil ; elle ne choquait pas ma "
          "raison mais pesait comme des écailles sur mes yeux et les empêchait de se rendre compte que le bougeoir n’était plus allumé. Puis elle commençait à "
          "me devenir inintelligible, comme après la métempsycose les pensées d’une existence antérieure ; le sujet du livre se détachait de moi, j’étais "
          "libre de m’y appliquer ou non ; aussitôt je recouvrais la vue et j’étais bien étonné de trouver autour de moi une obscurité, douce et reposante "
          "pour mes yeux, mais peut-être plus encore pour mon esprit, à qui elle apparaissait comme une chose sans cause, incompréhensible, comme une chose "
          "vraiment obscure. Je me demandais quelle heure il pouvait être ; j’entendais le sifflement des trains qui, plus ou moins éloigné, comme le chant "
          "d’un oiseau dans une forêt, relevant les distances, me décrivait l’étendue de la campagne déserte où le voyageur se hâte vers la station prochaine "
          "; et le petit chemin qu’il suit va être gravé dans son souvenir par l’excitation qu’il doit à des lieux nouveaux, à des actes inaccoutumés, à la "
          "causerie récente et aux adieux sous la lampe étrangère qui le suivent encore dans le silence de la nuit, à la douceur prochaine du retour.";

        WHEN("print is called with a boolean (true) parameter") {
            print(true);
            THEN("encoded data should be") {
                REQUIRE(checkType(0, msg::CodedType::smallString));
                REQUIRE(checkSize8(1, 5));
                REQUIRE(checkString(2, "print"));
                REQUIRE(checkType(7, msg::CodedType::booleanTrue));
            }
        }

        WHEN("print is called with a bunch of different types of parameters") {
            print(false, "text data", 45, text, bigText);
            THEN("encoded data should be") {
                REQUIRE(checkType(0, msg::CodedType::smallString));
                REQUIRE(checkSize8(1, 5));
                REQUIRE(checkString(2, "print"));
                REQUIRE(checkType(7, msg::CodedType::booleanFalse));
                REQUIRE(checkType(8, msg::CodedType::smallString));
                REQUIRE(checkSize8(9, 9));
                REQUIRE(checkString(10, "text data"));
                REQUIRE(checkType(19, msg::CodedType::number));
                REQUIRE(checkNumber(20, 45));
                REQUIRE(checkType(28, msg::CodedType::smallString));
                REQUIRE(checkSize8(29, 10));
                REQUIRE(checkString(30, "maFunction"));
                REQUIRE(checkType(40, msg::CodedType::string));
                REQUIRE(checkSize16(41, 1980));
            }
        }
    }
}
