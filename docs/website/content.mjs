export const repo = 'https://github.com/ambroise-leclerc/WebFront';

export const snippets = {
clone: `git clone https://github.com/ambroise-leclerc/WebFront.git
cd WebFront
git checkout v0.7.0
mkdir build
cd build
cmake ..
cmake --build .`,
cpp: `#include "Life.hpp"
#include "LifeFrontend.hpp"
#include <webfront/WebFront.hpp>

int main() {
    webfront::demo::LifeApp app{"life-project"};
    app.cppFunction("life.step", &webfront::demo::nextLife);
    app.bootstrap("developer");
    app.show();
}`,
terminal: `wf create life
wf init
wf install react@19.3.1 react-dom@19.3.1
wf dev ui/main.tsx`,
bridge: `try {
    await webFront.ready;
    const step = webFront.cppFunction('life.step');
    const next = await step(cells);
    // Update your React state with next.
} catch (error) {
    console.error(error);
    // Stop playback and show the error in your UI.
}`,
test: `describe('the Life grid', () => {
    it('packs 1024 cells into 128 bytes', () => {
        const cells = new Uint8Array(32 * 32 / 8);
        for (const i of [330, 331, 332])
            cells[i >> 3] |= 1 << (i & 7);
        expect(cells.length).toBe(128);
        expect(cells[41]).toBe(0b00011100);
    });
});`,
manifest: `"test": {"specs": ["tests/life.spec.js"]}`,
typecheck: `wf install typescript@5.9.3 @types/react@19.3.0 @types/react-dom@19.3.0
wf check`,
tsconfig: `{
  "compilerOptions": {
    "strict": true,
    "target": "ES2022",
    "module": "ESNext",
    "moduleResolution": "Bundler",
    "jsx": "react-jsx",
    "lib": ["ES2022", "DOM", "DOM.Iterable"],
    "noEmit": true
  },
  "include": ["ui"]
}`,
embed: `wf embed ui/main.tsx --output generated/LifeUI.hpp`,
embedded: `#include "Life.hpp"
#include "LifeFrontend.hpp"
#include <webfront/WebFront.hpp>
#include "generated/LifeUI.hpp"

int main() {
    webfront::demo::LifeAppWithAssets<webfront::fs::LifeUI> app;
    app.cppFunction("life.step", &webfront::demo::nextLife);
    app.show();
}`,
rebuild: `cmake .. -DWEBFRONT_LIFE_ASSETS="$PWD/life-project/generated/LifeUI.hpp"
cmake --build . --target WebFrontLifeEmbedded
examples/WebFrontLifeEmbedded`,
cmake: `add_subdirectory(path/to/WebFront)
add_executable(MyApp main.cpp)
target_link_libraries(MyApp PRIVATE WebFront::WebFront)`,
react: `const [generation, setGeneration] = useState(0);
<button onClick={() => setGeneration(g => g + 1)}>
    {generation}
</button>`,
dom: `const output = document.querySelector('#generation');
if (output) output.textContent = '42';`,
async: `// Inside a webfront::Task coroutine:
auto title = co_await ui.jsFunctionSender<std::string>("getTitle")();`,
agentBuild: `cmake -S . -B build-agent -DCMAKE_BUILD_TYPE=Release \\
  -DWEBFRONT_EMBED_WKWEBVIEW=ON \\
  -DWEBFRONT_BUILD_AUTOMATION=ON -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build-agent --target webfront-agent webfront-wf --parallel 2`,
agentServe: `webfront-agent --project /absolute/project \\
  --environment /absolute/environment.tgz --integrity sha512-TRUSTED_DIGEST \\
  --allow check --allow build --allow test \\
  --allow preview --allow inspect --allow interact \\
  --serve --listen /absolute/private-session`,
agentConfig: `{
  "mcpServers": {
    "webfront": {
      "command": "/absolute/build-agent/webfront-agent",
      "args": ["--attach", "/absolute/private-session", "--mcp"]
    }
  }
}`,
agentCli: `webfront-agent --attach /absolute/private-session --operation check
webfront-agent --attach /absolute/private-session --operation test
webfront-agent --attach /absolute/private-session --operation preview --entry ui/main.tsx`,
agentTools: `webfront.session({})
// Use the returned session identifier:
webfront.start({session: SESSION, operation: "check"})
// Use the returned runId; poll until a terminal state:
webfront.result({session: SESSION, runId: RUN_ID})
// For a preview after a successful build:
webfront.start({session: SESSION, operation: "preview",
               arguments: {entry: "ui/main.tsx"}})
// After completion, inspect and use the returned pageId and element IDs:
webfront.start({session: SESSION, operation: "inspect"})
webfront.start({session: SESSION, operation: "fill",
               arguments: {pageId: PAGE_ID, elementId: "quantity", value: "3"}})
webfront.start({session: SESSION, operation: "click",
               arguments: {pageId: PAGE_ID, elementId: "calculate"}})
// Operations must run sequentially; inspect again to observe the result.`,
agentTask: `Add a quantity field and a Calculate button to the quotation screen.
Keep the price calculation in the existing C++ callback.
Give the controls stable IDs and show bridge failures in the UI.
Add a Jasmine journey: enter 3, calculate, expect a total of 75.
Run check, build and test; inspect the preview and try the controls.
Read each final run state. Report changes, test results and limitations.`,
consumerPage: `<!doctype html>
<html lang="en"><meta charset="utf-8"><title>Quotation</title>
<label for="quantity">Quantity</label>
<input id="quantity" type="number" value="1" min="1">
<button id="calculate" disabled>Calculate</button>
<output id="total" aria-live="polite"></output>
<p id="status" role="status"></p>
<script src="/WebFront.js"></script>
<script>
const quantity = document.getElementById('quantity');
const button = document.getElementById('calculate');
const total = document.getElementById('total');
const status = document.getElementById('status');
window.appReady = webFront.ready.then(() => {
    button.disabled = false;
    button.addEventListener('click', async () => {
        button.disabled = true;
        status.textContent = 'Calculating';
        total.textContent = '';
        try {
            const count = Number(quantity.value);
            if (!Number.isInteger(count) || count < 1 || count > 1000)
                throw new Error('Quantity must be between 1 and 1000');
            total.textContent = String(await webFront.cppFunction('quote.calculate')(count));
            status.textContent = 'Ready';
        } catch (error) {
            status.textContent = 'Error: ' + error.message;
        } finally { button.disabled = false; }
    });
});
window.appReady.catch(error => { status.textContent = 'Error: ' + error.message; });
</script></html>`,
consumerSpec: `// tests/ui/quote.spec.js — classic JavaScript, loaded into the real page.
describe('quotation journey', () => {
    beforeAll(async () => { await window.appReady; });
    const element = id => document.getElementById(id);
    function until(predicate) {
        if (predicate()) return Promise.resolve();
        return new Promise((resolve, reject) => {
            const observer = new MutationObserver(() => {
                if (predicate()) { cleanup(); resolve(); }
            });
            const timer = setTimeout(() => {
                cleanup(); reject(new Error('UI did not settle within 2 seconds'));
            }, 2000);
            function cleanup() { clearTimeout(timer); observer.disconnect(); }
            observer.observe(document.body, {
                subtree: true, childList: true, characterData: true, attributes: true
            });
        });
    }
    it('enters a quantity and displays the C++ price', async () => {
        element('quantity').value = '3';
        element('quantity').dispatchEvent(new Event('input', {bubbles: true}));
        element('calculate').click();
        await until(() => !element('calculate').disabled);
        expect(element('status').textContent).toBe('Ready');
        expect(element('total').textContent).toBe('75');
    });
    it('shows an error for an invalid quantity', async () => {
        element('quantity').value = '0';
        element('calculate').click();
        await until(() => !element('calculate').disabled);
        expect(element('status').textContent).toContain('Error:');
        expect(element('total').textContent).toBe('');
    });
});`,
consumerCpp: `#include <webfront/WebFront.hpp>
#include <webfront/testing/Jasmine.hpp>
#include <stdexcept>

using App = webfront::WebFrontWithFrontend<webfront::frontend::WKWebViewFrontend>;

int main() {
    App app{{.port = "0", .documentRoot = "ui"}};
    app.cppFunction("quote.calculate", [](int count) {
        if (count < 1 || count > 1000)
            throw std::invalid_argument("Invalid quantity");
        return count * 25;
    });
    webfront::testing::JasmineTest tests{app};
    app.show();
    app.shutdown();
    return tests.exitCode();
}`,
consumerCmake: `cmake_minimum_required(VERSION 3.31)
project(Quote LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(WEBFRONT_EMBED_WKWEBVIEW ON CACHE BOOL "" FORCE)
add_subdirectory(WebFront)
enable_testing()
add_executable(Quote main.cpp)
target_link_libraries(Quote PRIVATE WebFront::WebFront)
webfront_use_native_window(Quote)
webfront_add_jasmine_test(NAME QuoteJourney TARGET Quote
    SPECS tests/ui/quote.spec.js FRONTEND WKWebView TIMEOUT 60)
set_tests_properties(QuoteJourney PROPERTIES
    WORKING_DIRECTORY "\${CMAKE_CURRENT_SOURCE_DIR}")`,
consumerRun: `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure`,
domCpp: `app.onUIStarted([](auto ui) {
    auto doc = ui.document();
    auto root = doc.island();
    auto button = doc.element("button");
    auto label = doc.text("Click me");
    button.append(label);
    button.listen("click", [label](std::string) { label.setText("Clicked"); });
    root.append(button);
});`,
forms: `#include <webfront/components/forms/Form.hpp>
struct Settings { int speed{5}; bool wrap{true}; };
auto description = webfront::forms::Description<Settings>{
    .name = "Settings",
    .fields = {
        webfront::forms::field("speed", &Settings::speed, "Speed",
                              webfront::forms::Bounds{1, 10}),
        webfront::forms::field("wrap", &Settings::wrap, "Wrap")
    }
};
// Use the description with forms::form(), forms::inspector(),
// or forms::typescriptDeclaration().`,
migration: `# WebFront 0.7.0: use the prefixed options explicitly.
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \\
  -DWEBFRONT_EMBED_CEF=OFF -DWEBFRONT_ENABLE_TESTING=ON
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure`
};

export const copy = {
    "fr": {
        "lang": "fr",
        "title": "Les applications graphiques C++ à l’ère de l’IA",
        "desc": "Un framework graphique C++ et TypeScript complet pour Windows, macOS et Linux : compilation, contrôle des types, prévisualisation et tests d’IHM automatisés, conçu pour le travail avec des agents d’IA.",
        "nav": [
            "Présentation",
            "Exemple",
            "Le livre",
            "Documentation"
        ],
        "eyebrow": "FRAMEWORK C++23 · Opensource",
        "hero": [
            "Les applications",
            "graphiques C++",
            "à l’ère de l’IA."
        ],
        "intro": "WebFront réunit la puissance du C++ et les possibilités graphiques de TypeScript dans un framework complet pour Windows, macOS et Linux. Il accompagne la création de l’application autant que son évolution.",
        "start": "Commencer avec WebFront",
        "explore": "Essayer le Jeu de la vie",
        "strip": [
            "Header-only · C++23",
            "Aucun environnement JavaScript à installer",
            "Aucun environnement de développement à distribuer"
        ],
        "demoLabel": "EXEMPLE / JEU DE LA VIE",
        "demoTitle": "Le Jeu de la vie",
        "demoText": "Cet exemple associe une fonction de calcul C++ à une interface React. Le composant affiche la grille et demande au C++ de calculer chaque génération. La démonstration ci-dessous permet d’examiner quelques motifs avant de construire l’application.",
        "demoNote": "La démonstration du site calcule les générations en JavaScript. Dans l’application native du livre, le même affichage attend le résultat d’un appel C++.",
        "play": "Lancer",
        "pause": "Pause",
        "step": "Un pas",
        "reset": "Recommencer",
        "pattern": "Motif",
        "patterns": [
            "Galaxie",
            "Planeur",
            "Clignotant"
        ],
        "gen": "Génération",
        "pop": "Cellules vivantes",
        "view": "Vue plane",
        "speed": "Vitesse",
        "whyLabel": "PRINCIPES",
        "whyTitle": "Organisation d’une application WebFront",
        "why": [
            [
                "Le programme C++",
                "Une fonction enregistrée avec `cppFunction` devient accessible depuis TypeScript. Le pont binaire transporte ses arguments et son résultat de manière asynchrone. Le code métier peut ainsi rester indépendant de la présentation."
            ],
            [
                "La présentation",
                "Une interface peut être écrite en TypeScript, en CSS et, si nécessaire, avec React. Les mêmes sources servent sur Windows, macOS et Linux, dans une fenêtre native ou un navigateur système."
            ],
            [
                "Les outils de développement",
                "Le terminal `wf` installe les dépendances, compile les sources TSX, vérifie les types et exécute les tests. La commande `wf dev` actualise la prévisualisation à chaque enregistrement. Ces outils ne demandent pas d’installation séparée de Node.js."
            ],
            [
                "La distribution",
                "La commande `wf embed` produit un en-tête C++ contenant les ressources de l’interface et ses dépendances web. Après recompilation, ces ressources sont disponibles hors ligne dans l’application."
            ],
            [
                "L’automatisation",
                "Un lanceur facultatif donne accès au contrôle des types, à la construction, aux tests et à une inspection limitée de la prévisualisation. Un harnais d’IA peut l’utiliser par MCP local ou par une interface JSON en ligne de commande."
            ]
        ],
        "walkLabel": "DÉVELOPPEMENT",
        "walkTitle": "Développer l’interface dans la fenêtre de l’application",
        "walkText": "L’exemple `WebFrontLifeDeveloper` contient le terminal de développement. Les commandes suivantes y créent le projet et ouvrent la prévisualisation. Les fichiers TSX et CSS se modifient avec l’éditeur habituel.",
        "watch": "Voir la session de développement",
        "bookLabel": "APPRENDRE",
        "bookTitle": "Le livre WebFront",
        "bookText": "Le livre présente le développement d’une application avec WebFront, depuis son modèle C++ jusqu’à sa distribution. Le Jeu de la vie fournit un exemple assez simple pour étudier la présentation, les appels asynchrones, les tests de parcours et le travail avec un harnais d’IA.",
        "bookAction": "Lire le premier chapitre",
        "bookAside": "Du modèle de l’application à sa distribution.",
        "bookHint": "Des sections complémentaires expliquent le DOM, les composants React et l’état, ainsi que les coroutines C++ et les règles de durée de vie.",
        "reference": "Documentation et contributions",
        "refText": "Les guides du dépôt donnent les contrats détaillés des API et les limites des moteurs pris en charge. Ils complètent les exemples du livre.",
        "refCards": [
            "Guide de l’environnement",
            "Exécution asynchrone",
            "Moteurs natifs",
            "État du projet",
            "Décisions d’architecture",
            "Contribuer sur GitHub"
        ],
        "footer": "Framework C++23 pour la création d’applications graphiques.",
        "source": "Code source",
        "privacy": "Sans mesure d’audience ni cookies publicitaires. La progression du livre reste dans ce navigateur.",
        "book": "Le livre",
        "contents": "Parcours",
        "optional": "Notions complémentaires",
        "lesson": "CHAPITRE",
        "mission": "Exercice",
        "quiz": "Vérifier sa compréhension",
        "correct": "Oui.",
        "wrong": "Ce n’est pas la bonne réponse. L’explication se trouve dans le chapitre.",
        "previous": "Précédent",
        "next": "Continuer",
        "done": "Marquer comme lu",
        "read": "Lu",
        "back": "Accueil",
        "search": "Rechercher un chapitre…",
        "noResults": "Aucun chapitre trouvé.",
        "progress": "Progression",
        "sourceDocs": "Aller à la documentation source",
        "need": "Avant de commencer",
        "toc": "Dans ce chapitre",
        "real": "L’exemple natif",
        "copy": "Copier",
        "copied": "Copié",
        "copyFail": "Sélectionnez le code pour le copier.",
        "skip": "Aller au contenu",
        "fallback": "WebFront demande un compilateur C++23 et des dépendances C++ gérées par CMake. L’affichage utilise un navigateur ou un moteur web : WebView2, WKWebView, WebKitGTK, ou CEF en option. Le déploiement de ce moteur dépend de la plateforme ; aucun environnement de développement JavaScript n’est requis.",
        "portabilityLabel": "PORTABILITÉ",
        "portabilityTitle": "Plateformes prises en charge",
        "portabilityText": "WebFront est actuellement qualifié sur les plateformes de bureau. L’utilisation d’un navigateur distant pourrait également convenir à certains équipements ; cette possibilité demande encore un travail de portage et de validation.",
        "portabilityCards": [
            [
                "Windows, macOS et Linux",
                "Les exemples emploient un moteur natif lorsque ses dépendances sont présentes, ou le navigateur système. Le déploiement du moteur dépend de la plateforme et doit être prévu avec celui de l’application."
            ],
            [
                "Équipements embarqués",
                "Les cibles embarquées et les microcontrôleurs ne sont pas qualifiés. Leur support du C++23, la mémoire disponible et la pile réseau doivent être examinés. Le transport distant et ses règles de sécurité restent à concevoir et à valider."
            ]
        ],
        "releases": {
            "title": "WebFront 0.7.0 est disponible",
            "text": "La version 0.6.0 introduit le DOM C++, les formulaires, l’adaptateur React et l’automatisation locale. La version 0.7.0 rend obligatoire le préfixe `WEBFRONT_` des options CMake et corrige plusieurs cas d’arrêt de l’agent.",
            "details": "Notes d’utilisation",
            "agent": "Développer avec une IA",
            "tests": "Tester un vrai parcours"
        },
        "agentLabel": "AUTOMATISATION ET TESTS",
        "agentTitle": "Faire évoluer une application avec un harnais",
        "agentText": "Un harnais peut intervenir dès la création d’un écran, puis accompagner ses évolutions : il modifie les sources, demande le contrôle des types et la construction, et exécute les parcours Jasmine. Les vérifications restent attachées au projet et servent ensuite à sa maintenance. Le chapitre correspondant décrit la configuration et les limites de cette méthode.",
        "notationLabel": "Conventions de notation",
        "notation": "Les noms de fonctions, types, commandes, options et fichiers sont composés en chasse fixe, par exemple `describe`, `wf test` ou `main.cpp`. L’italique introduit un *terme* lors de sa définition. Les blocs de code donnent les extraits à saisir ; le texte qui les précède précise leur emplacement et leurs prérequis.",
        "lifecycleLabel": "CONCEVOIR UNE APPLICATION",
        "lifecycleTitle": "Un environnement pour le cycle de développement",
        "lifecycleIntro": "Le choix d’un framework détermine aussi la manière de travailler sur une application. Avec WebFront, la création de l’interface, sa prévisualisation, les vérifications et la préparation des ressources distribuées s’effectuent dans un environnement commun. Cette continuité permet de faire évoluer le programme et sa présentation sans reconstruire une chaîne d’outils pour chaque étape.",
        "lifecycleSteps": [
            [
                "Créer",
                "Le projet rassemble les sources de l’interface, son manifeste et ses dépendances verrouillées. Les fonctions C++ et les composants de présentation peuvent être conçus ensemble dès le début de l’application."
            ],
            [
                "Développer",
                "Le terminal `wf` compile le TypeScript et le TSX. La prévisualisation est actualisée à l’enregistrement des sources ; une erreur laisse la dernière version valide disponible."
            ],
            [
                "Vérifier et maintenir",
                "Le contrôle des types et les parcours Jasmine accompagnent les modifications. Un harnais d’IA peut demander ces opérations et exploiter leurs résultats structurés, tandis que les tests restent réutilisables en CI."
            ],
            [
                "Distribuer",
                "La commande `wf embed` prépare les ressources de l’interface pour leur inclusion dans le programme C++. La version distribuée fonctionne sans l’environnement de développement ; le déploiement du moteur de rendu reste propre à la plateforme."
            ]
        ],
        "caseTitle": "SiliconForge : une application conçue avec WebFront",
        "caseText": "SiliconForge est aujourd’hui la seule application publiée utilisant WebFront. Ce simulateur de microcontrôleurs a été développé autour du framework dès l’origine. Il associe un moteur C++ à une interface permettant de charger un firmware, d’observer les broches et les registres, et de commander l’exécution simulée.",
        "caseExperience": "Son auteur souligne la rapidité de développement obtenue avec WebFront. Ce retour d’expérience concerne la construction d’une application complète : le travail porte à la fois sur le modèle, la présentation et les vérifications, dans le même environnement.",
        "caseLink": "Voir SiliconForge",
        "compatibilityTitle": "Intégrer un programme existant",
        "compatibilityText": "Cette organisation convient également à un programme C++ déjà développé. On peut lui ajouter une interface sans lui imposer un nouveau modèle de fonctionnement : le calcul et les données conservent leur organisation, et les fonctions nécessaires sont rendues accessibles à la présentation.",
        "introDetail": "Compilation TypeScript, contrôle des types, prévisualisation, tests automatisés de l’IHM et préparation des ressources à distribuer font partie du même environnement. Conçu pour le travail avec des agents d’IA, WebFront permet de leur confier une réalisation qu’ils peuvent construire, examiner et tester."
    },
    "en": {
        "lang": "en-GB",
        "title": "C++ graphical applications in the age of AI",
        "desc": "A complete C++ and TypeScript graphical framework for Windows, macOS and Linux: compilation, type checking, preview and automated UI tests, designed for work with AI agents.",
        "nav": [
            "Overview",
            "Example",
            "The book",
            "Documentation"
        ],
        "eyebrow": "C++23 FRAMEWORK · OPEN SOURCE",
        "hero": [
            "C++ graphical",
            "applications",
            "in the age of AI."
        ],
        "intro": "WebFront brings together the power of C++ and the graphical possibilities of TypeScript in a complete framework for Windows, macOS and Linux. It supports both the creation of an application and its subsequent evolution.",
        "start": "Get started with WebFront",
        "explore": "Try the Game of Life",
        "strip": [
            "Header-only · C++23",
            "No JavaScript environment to install",
            "No development environment to distribute"
        ],
        "demoLabel": "EXAMPLE / GAME OF LIFE",
        "demoTitle": "Conway’s Game of Life",
        "demoText": "This example combines a C++ calculation with a React interface. The component displays the grid and asks C++ to calculate each generation. The demonstration below lets us examine several patterns before building the application.",
        "demoNote": "The website demonstration calculates generations in JavaScript. In the book’s native application, the same presentation waits for a C++ call result.",
        "play": "Play",
        "pause": "Pause",
        "step": "Step",
        "reset": "Reset",
        "pattern": "Pattern",
        "patterns": [
            "Galaxy",
            "Glider",
            "Blinker"
        ],
        "gen": "Generation",
        "pop": "Living cells",
        "view": "Plan view",
        "speed": "Speed",
        "whyLabel": "PRINCIPLES",
        "whyTitle": "Organisation of a WebFront application",
        "why": [
            [
                "The C++ programme",
                "A function registered with `cppFunction` becomes accessible from TypeScript. The binary bridge transports its arguments and result asynchronously. Application logic can therefore remain independent of presentation."
            ],
            [
                "Presentation",
                "An interface can be written in TypeScript, CSS and, where appropriate, React. The same sources serve Windows, macOS and Linux, in a native window or the system browser."
            ],
            [
                "Development tools",
                "The `wf` terminal installs dependencies, compiles TSX sources, checks types and runs tests. `wf dev` updates the preview whenever a file is saved. These tools require no separate Node.js installation."
            ],
            [
                "Distribution",
                "`wf embed` produces a C++ header containing interface resources and web dependencies. After recompilation, those resources are available offline within the application."
            ],
            [
                "Automation",
                "An optional launcher provides type checking, building, testing and limited preview inspection. An AI harness can use it through local MCP or a JSON command-line interface."
            ]
        ],
        "walkLabel": "DEVELOPMENT",
        "walkTitle": "Developing the interface in the application window",
        "walkText": "The `WebFrontLifeDeveloper` example contains the development terminal. The following commands create the project and open its preview. Edit the TSX and CSS files in your usual editor.",
        "watch": "Watch the development session",
        "bookLabel": "LEARN",
        "bookTitle": "The WebFront Book",
        "bookText": "The book develops an application with WebFront, from its C++ model to distribution. The Game of Life provides an example small enough to study presentation, asynchronous calls, journey tests and work with an AI harness.",
        "bookAction": "Read the first chapter",
        "bookAside": "From the application model to distribution.",
        "bookHint": "Background sections explain the DOM, React components and state, and C++ coroutines and lifetime rules.",
        "reference": "Documentation and contributions",
        "refText": "The repository guides give detailed API contracts and the limitations of supported engines. They supplement the book’s examples.",
        "refCards": [
            "Developer environment",
            "Asynchronous execution",
            "Native engines",
            "Project status",
            "Architecture decisions",
            "Contribute on GitHub"
        ],
        "footer": "A C++23 framework for building graphical applications.",
        "source": "Source code",
        "privacy": "No analytics or advertising cookies. Book progress stays in this browser.",
        "book": "The book",
        "contents": "Learning path",
        "optional": "Background concepts",
        "lesson": "CHAPTER",
        "mission": "Exercise",
        "quiz": "Check your understanding",
        "correct": "Yes.",
        "wrong": "That’s not the right answer. The chapter explains why.",
        "previous": "Previous",
        "next": "Continue",
        "done": "Mark as read",
        "read": "Read",
        "back": "Home",
        "search": "Find a chapter…",
        "noResults": "No chapters found.",
        "progress": "Progress",
        "sourceDocs": "Read the source documentation",
        "need": "Before you begin",
        "toc": "In this chapter",
        "real": "The native example",
        "copy": "Copy",
        "copied": "Copied",
        "copyFail": "Select the code to copy it.",
        "skip": "Skip to content",
        "fallback": "WebFront requires a C++23 compiler and C++ dependencies managed by CMake. Rendering uses a browser or web engine: WebView2, WKWebView, WebKitGTK, or optionally CEF. Engine deployment depends on the platform; no JavaScript development environment is required.",
        "portabilityLabel": "PORTABILITY",
        "portabilityTitle": "Supported platforms",
        "portabilityText": "WebFront is currently qualified on desktop platforms. A remote browser could also suit some devices, but that use still requires porting and validation.",
        "portabilityCards": [
            [
                "Windows, macOS and Linux",
                "The examples use a native engine when its dependencies are present, or the system browser. Engine deployment depends on the platform and must be planned with application deployment."
            ],
            [
                "Embedded equipment",
                "Embedded targets and microcontrollers are not qualified. Their C++23 support, available memory and network stack require examination. Remote transport and its security rules remain to be designed and validated."
            ]
        ],
        "releases": {
            "title": "WebFront 0.7.0 is available",
            "text": "Version 0.6.0 introduces the C++ DOM, forms, the React adapter and local automation. Version 0.7.0 requires the `WEBFRONT_` prefix on CMake options and fixes several agent shutdown cases.",
            "details": "Usage notes",
            "agent": "Develop with AI",
            "tests": "Test a real journey"
        },
        "agentLabel": "AUTOMATION AND TESTING",
        "agentTitle": "Evolving an application with a harness",
        "agentText": "A harness can participate when a screen is first created and throughout subsequent changes: it edits sources, requests type checks and builds, and runs Jasmine journeys. Checks remain with the project for later maintenance. The corresponding chapter describes configuration and the method’s limitations.",
        "notationLabel": "Notation conventions",
        "notation": "Function names, types, commands, options and filenames appear in monospace, for example `describe`, `wf test` or `main.cpp`. Italics introduce a *term* at its definition. Code blocks give the fragments to enter; the preceding text states their location and prerequisites.",
        "lifecycleLabel": "APPLICATION DEVELOPMENT",
        "lifecycleTitle": "An environment for the development lifecycle",
        "lifecycleIntro": "Choosing a framework also determines how an application is developed. WebFront provides a common environment for creating the interface, previewing it, checking behaviour and preparing distributable resources. This continuity lets the programme and its presentation evolve without assembling a separate toolchain for each stage.",
        "lifecycleSteps": [
            [
                "Create",
                "The project brings together interface sources, a manifest and locked dependencies. C++ functions and presentation components can be designed together from the start of the application."
            ],
            [
                "Develop",
                "The `wf` terminal compiles TypeScript and TSX. Saving sources updates the preview; an error leaves the last successful version available."
            ],
            [
                "Test and maintain",
                "Type checks and Jasmine journeys accompany changes. An AI harness can request these operations and use their structured results, while the tests remain reusable in CI."
            ],
            [
                "Distribute",
                "`wf embed` prepares interface resources for inclusion in the C++ programme. The distributed version runs without the development environment; rendering-engine deployment remains platform-specific."
            ]
        ],
        "caseTitle": "SiliconForge: an application built with WebFront",
        "caseText": "SiliconForge is currently the only published application using WebFront. This microcontroller simulator was developed around the framework from the outset. It combines a C++ engine with an interface for loading firmware, examining pins and registers, and controlling simulated execution.",
        "caseExperience": "Its author reports a particularly rapid development process with WebFront. This experience concerns the construction of a complete application: work on the model, presentation and verification takes place within the same environment.",
        "caseLink": "Explore SiliconForge",
        "compatibilityTitle": "Integrating an existing programme",
        "compatibilityText": "This organisation also suits an existing C++ programme. An interface can be added without imposing a new operating model: computation and data retain their organisation, and the required functions are made accessible to the presentation.",
        "introDetail": "TypeScript compilation, type checking, preview, automated UI tests and preparation of distributable resources share the same environment. Designed for work with AI agents, WebFront lets you entrust them with an implementation they can build, examine and test."
    }
};

// Blocks contain a bilingual heading, bilingual prose and an optional snippet key.
export const chapters = [
    {
        "id": "start",
        "title": [
            "Partir du C++",
            "Starting with C++"
        ],
        "desc": [
            "Construire une application avec WebFront : le modèle C++ et les étapes du développement.",
            "Building an application with WebFront: the C++ model and development stages."
        ],
        "doc": "README.md",
        "blocks": [
            [
                [
                    "Le développement d’une application",
                    "Developing an application"
                ],
                [
                    "WebFront prend en charge la création d’applications graphiques et l’outillage nécessaire à leur développement. L’application C++, sa présentation et ses tests se construisent ensemble. L’environnement intégré accompagne ensuite leurs modifications et prépare les ressources à distribuer.\n\nSiliconForge, un simulateur de microcontrôleurs conçu dès l’origine autour de WebFront, constitue une première application publiée. Le Jeu de la vie utilisé ici est un exemple pédagogique plus réduit : il nous permet d’examiner chaque mécanisme séparément.\n\nNous commencerons par le modèle de simulation en C++, puis nous construirons son interface. La même séparation permet aussi d’enrichir un programme existant sans modifier son organisation.",
                    "WebFront supports the creation of graphical applications and provides tools for their development. The C++ application, its presentation and its tests are developed together. The integrated environment then supports their evolution and prepares the resources for distribution.\n\nSiliconForge, a microcontroller simulator designed around WebFront from the outset, is a first published application. The Game of Life used here is a smaller teaching example that lets us examine each mechanism separately.\n\nWe shall begin with the C++ simulation model and then build its interface. The same separation also allows an existing programme to be extended without changing its organisation."
                ]
            ],
            [
                [
                    "Le programme de départ",
                    "The example programme"
                ],
                [
                    "Nous prendrons pour exemple le Jeu de la vie. Une grille de 32 × 32 cellules évolue selon deux règles : une cellule naît si elle a trois voisines ; une cellule vivante survit si elle en a deux ou trois. Les bords de la grille se rejoignent.\n\nLe fichier `Life.hpp` contient le calcul de la génération suivante. Son résultat est un `std::bitset<1024>`. Ce calcul ne dépend d’aucune bibliothèque d’interface ; nous le conserverons lorsque nous ajouterons une fenêtre.",
                    "We shall use Conway’s Game of Life as our example. A grid of 32 × 32 cells follows two rules: a cell is born when it has three neighbours; a living cell survives when it has two or three. The edges of the grid wrap round.\n\n`Life.hpp` calculates the next generation and returns a `std::bitset<1024>`. This computation has no dependency on a user interface library. We shall retain it when adding a window."
                ]
            ],
            [
                [
                    "Construire les exemples",
                    "Building the examples"
                ],
                [
                    "La construction demande Git, CMake 3.31 ou une version ultérieure, et un compilateur C++23. La première configuration télécharge les dépendances C++ ; elle nécessite donc un accès à Internet.\n\nLes commandes ci-dessous sélectionnent la version 0.7.0 et construisent les exemples avec les options par défaut. Elles ne demandent pas le téléchargement de CEF.",
                    "Building requires Git, CMake 3.31 or later, and a C++23 compiler. The first configuration downloads the C++ dependencies and therefore needs Internet access.\n\nThe following commands select version 0.7.0 and build the examples with the default options. They do not request a CEF download."
                ],
                "clone"
            ],
            [
                [
                    "Exécuter la simulation",
                    "Running the simulation"
                ],
                [
                    "Depuis le répertoire de construction, exécutez `examples/WebFrontLifeConsole`. Avec Visual Studio, le chemin est `examples\\Debug\\WebFrontLifeConsole.exe`. Cette première version permet d’observer la simulation avant d’en modifier la présentation.\n\nDans un projet CMake existant, WebFront s’ajoute comme sous-projet. La cible de l’application doit être liée à `WebFront::WebFront`, comme dans l’extrait suivant.",
                    "From the build directory, run `examples/WebFrontLifeConsole`. With Visual Studio, use `examples\\Debug\\WebFrontLifeConsole.exe`. This initial version lets us examine the simulation before changing its presentation.\n\nIn an existing CMake project, add WebFront as a subproject and link the application target to `WebFront::WebFront`, as shown below."
                ],
                "cmake"
            ]
        ],
        "mission": [
            "Dans `examples/life/Life.hpp`, examinez `nextLife`. Déterminez ce que deviennent trois cellules alignées, puis comparez votre réponse au motif Clignotant de la démonstration.",
            "Examine `nextLife` in `examples/life/Life.hpp`. Work out what happens to three cells in a row, then compare your answer with the demonstration’s Blinker pattern."
        ],
        "q": [
            "La simulation doit-elle dépendre de React ?",
            "Does the simulation need to depend on React?"
        ],
        "answers": [
            [
                "Non, elle reste en C++.",
                "No, it stays in C++."
            ],
            [
                "Oui, React doit calculer les générations.",
                "Yes, React must calculate generations."
            ]
        ],
        "right": 0,
        "links": [
            [
                "SiliconForge",
                "https://github.com/ambroise-leclerc/SiliconForge"
            ]
        ]
    },
    {
        "id": "window",
        "title": [
            "Ouvrir une fenêtre",
            "Opening a window"
        ],
        "desc": [
            "Enregistrement d’une fonction C++ et chargement de l’environnement de développement.",
            "Registering a C++ function and loading the development environment."
        ],
        "doc": "examples/life/LifeDeveloper.cpp",
        "blocks": [
            [
                [
                    "Enregistrer une fonction C++",
                    "Registering a C++ function"
                ],
                [
                    "Le programme suivant reprend les en-têtes `Life.hpp` et `LifeFrontend.hpp` de l’exemple. Il doit être construit dans ce projet, où ces en-têtes sont disponibles.\n\nL’appel à `cppFunction` enregistre le calcul sous le nom `life.step`. L’appel à `bootstrap` charge l’environnement de développement ; `show` ouvre la fenêtre. Le chemin `life-project` désigne le répertoire où seront placées les sources de l’interface.",
                    "The following programme uses `Life.hpp` and `LifeFrontend.hpp` from the example. Build it within that project, where the supporting headers are available.\n\n`cppFunction` registers the calculation under the name `life.step`. `bootstrap` loads the development environment and `show` opens the window. The path `life-project` identifies the directory that will hold the interface sources."
                ],
                "cpp"
            ],
            [
                [
                    "Choisir le mode d’affichage",
                    "Choosing the display frontend"
                ],
                [
                    "Exécutez `examples/WebFrontLifeDeveloper` depuis le répertoire de construction. Avec Visual Studio, utilisez `examples\\Debug\\WebFrontLifeDeveloper.exe`.\n\nLes exemples recherchent WebView2 sur Windows, WKWebView sur macOS 13.3 ou ultérieur, et WebKitGTK 6.0 sur Linux. Si le moteur natif n’est pas disponible, ils ouvrent le navigateur système.\n\nCe choix appartient au type d’application : `webfront::WebFront` emploie toujours le navigateur système ; `NativeApp` sélectionne une fenêtre native. Une fenêtre native doit être créée et affichée sur le thread principal.",
                    "Run `examples/WebFrontLifeDeveloper` from the build directory. With Visual Studio, use `examples\\Debug\\WebFrontLifeDeveloper.exe`.\n\nThe examples look for WebView2 on Windows, WKWebView on macOS 13.3 or later, and WebKitGTK 6.0 on Linux. If no native engine is available, they open the system browser.\n\nThe application type determines this choice: `webfront::WebFront` always uses the system browser; `NativeApp` selects a native window. Create and display native windows on the main thread."
                ]
            ],
            [
                [
                    "Chargement des outils",
                    "Loading the tools"
                ],
                [
                    "Au premier lancement, le bootstrap télécharge une version déterminée de l’environnement et vérifie son intégrité SHA-512. Les lancements suivants utilisent le cache local.\n\nLe terminal ne demande ni Node.js ni gestionnaire de paquets installé séparément. Il dispose toutefois d’un accès au projet et peut exécuter son code. Il convient donc de l’utiliser avec un projet de confiance.",
                    "On its first run, the bootstrap downloads a pinned environment version and checks its SHA-512 integrity. Later runs use the local cache.\n\nThe terminal requires neither a separate Node.js installation nor a package manager on the host. It does, however, have access to the project and can execute its code. Use it with a trusted project."
                ]
            ]
        ],
        "mission": [
            "Exécutez `WebFrontLifeDeveloper`. Repérez son terminal et déterminez si l’affichage emploie un moteur natif ou le navigateur système.",
            "Run `WebFrontLifeDeveloper`. Locate its terminal and determine whether it uses a native engine or the system browser."
        ],
        "q": [
            "À quel moment le réseau est-il nécessaire ?",
            "When is network access needed?"
        ],
        "answers": [
            [
                "À chaque génération.",
                "For every generation."
            ],
            [
                "Pour les premiers téléchargements des outils et dépendances.",
                "For the initial tools and dependency downloads."
            ]
        ],
        "right": 1
    },
    {
        "id": "interface",
        "title": [
            "Créer l’interface",
            "Creating the interface"
        ],
        "desc": [
            "Création du projet, installation des dépendances et compilation de l’interface.",
            "Creating the project, installing dependencies and compiling the interface."
        ],
        "doc": "docs/DEVELOPER_ENVIRONMENT.md",
        "blocks": [
            [
                [
                    "Créer le projet",
                    "Creating the project"
                ],
                [
                    "Les commandes suivantes sont destinées au terminal de WebFront. Elles effectuent successivement quatre opérations :\n\n- `wf create life` crée les sources de l’exemple dans `ui/`, sans écraser un répertoire existant.\n- `wf init` crée le manifeste du projet.\n- `wf install` installe les paquets et inscrit leurs versions exactes dans le fichier de verrouillage.\n- `wf dev` compile le fichier TSX et ouvre sa prévisualisation.",
                    "Enter the following commands in the WebFront terminal. They perform four operations in order:\n\n- `wf create life` creates example sources in `ui/`, without overwriting an existing directory.\n- `wf init` creates the project manifest.\n- `wf install` installs packages and records their exact versions in the lock file.\n- `wf dev` compiles the TSX file and opens its preview."
                ],
                "terminal"
            ],
            [
                [
                    "Modifier les sources",
                    "Editing the sources"
                ],
                [
                    "Ouvrez `life-project/ui/main.tsx` et `style.css` dans votre éditeur. Lorsqu’un fichier est enregistré, `wf dev` reconstruit l’interface et actualise la prévisualisation.\n\nSi la compilation échoue, le diagnostic est affiché et la dernière version valide reste visible. Pour arrêter la surveillance, utilisez Ctrl+C. La commande `wf build` effectue une compilation unique, sans surveillance.",
                    "Open `life-project/ui/main.tsx` and `style.css` in your editor. Whenever a file is saved, `wf dev` rebuilds the interface and updates the preview.\n\nIf compilation fails, a diagnostic is displayed and the last successful version remains visible. Use Ctrl+C to stop watching. `wf build` performs a single build without watching for changes."
                ]
            ],
            [
                [
                    "Dépendances et contrôle des types",
                    "Dependencies and type checking"
                ],
                [
                    "Les paquets doivent être utilisables dans un navigateur. Les scripts d’installation et les extensions natives ne sont pas exécutés.\n\nLe fichier de verrouillage permet de rétablir les mêmes dépendances avec `wf restore`. Sur une machine déjà provisionnée, `wf restore --offline` vérifie les éléments du cache.\n\nLa compilation TSX enlève les annotations de types. Pour vérifier les types eux-mêmes, il faut exécuter séparément `wf check`.",
                    "Packages must be usable in a browser. Installation scripts and native add-ons are not executed.\n\nThe lock file allows the same dependencies to be restored with `wf restore`. On an already provisioned machine, `wf restore --offline` checks the cached files.\n\nTSX compilation removes type annotations. Checking the types themselves requires a separate `wf check` operation."
                ]
            ]
        ],
        "mission": [
            "Modifiez le titre de l’interface et la couleur des cellules. Introduisez ensuite une erreur TSX, observez le diagnostic et corrigez la source.",
            "Change the interface title and cell colour. Then introduce a TSX error, examine the diagnostic and correct the source."
        ],
        "q": [
            "La commande `wf dev` vérifie-t-elle les types TypeScript ?",
            "Does `wf dev` check TypeScript types?"
        ],
        "answers": [
            [
                "Oui.",
                "Yes."
            ],
            [
                "Non. Cette vérification demande `wf check`.",
                "No. Type checking requires `wf check`."
            ]
        ],
        "right": 1
    },
    {
        "id": "bridge",
        "title": [
            "Appeler le C++ depuis TypeScript",
            "Calling C++ from TypeScript"
        ],
        "desc": [
            "Le protocole d’appel, la représentation des données et le traitement des erreurs.",
            "The calling protocol, data representation and error handling."
        ],
        "doc": "docs/ASYNC_EXECUTION.md",
        "blocks": [
            [
                [
                    "Attendre le résultat d’un appel",
                    "Waiting for a call result"
                ],
                [
                    "La propriété `webFront.ready` est une promesse qui se résout après l’acquittement de la connexion WebSocket. Le programme attend cette connexion avant d’appeler `life.step` avec la grille courante.\n\nUn appel JavaScript vers C++ renvoie toujours une `Promise`, y compris lorsque la fonction C++ renvoie `void`. Il faut attendre cette promesse ou traiter son rejet. L’extrait suivant appartient à une fonction `async` ; `cells` désigne la grille du starter.",
                    "`webFront.ready` is a promise that resolves after the WebSocket handshake acknowledgement. The programme waits for this connection before calling `life.step` with the current grid.\n\nA JavaScript call to C++ always returns a `Promise`, even when the C++ function returns `void`. Await the promise or handle its rejection. The following fragment belongs inside an `async` function; `cells` is the starter’s grid."
                ],
                "bridge"
            ],
            [
                [
                    "Représenter la grille",
                    "Representing the grid"
                ],
                [
                    "Un `std::bitset<1024>` est transmis sous la forme d’un `Uint8Array` de 128 octets. Dans chaque octet, le bit de poids faible représente la première cellule. La taille doit être exacte ; les éventuels bits de remplissage doivent être nuls.\n\nLe pont accepte les scalaires, les chaînes, les tuples, les tableaux numériques typés et les bitsets compactés. Il ne transporte pas des objets JavaScript arbitraires. La longueur des chaînes est encodée sur 16 bits.",
                    "A `std::bitset<1024>` is transmitted as a 128-byte `Uint8Array`. Within each byte, the least-significant bit represents the first cell. The size must be exact and any padding bits must be zero.\n\nThe bridge supports scalars, strings, tuples, numeric typed arrays and packed bitsets. It does not transport arbitrary JavaScript objects. String lengths are encoded in 16 bits."
                ]
            ],
            [
                [
                    "Ordre des appels et erreurs",
                    "Call ordering and errors"
                ],
                [
                    "Il ne doit y avoir qu’un calcul de génération en cours. Attendez sa réponse avant de demander le suivant. Si une remise à zéro intervient entre-temps, la réponse ancienne ne doit plus modifier la grille ; le starter emploie à cette fin un numéro de révision.\n\nUne fonction absente, une exception C++ ou une déconnexion provoque le rejet de la promesse. Le programme doit alors arrêter la lecture et afficher l’erreur.\n\nDans l’autre direction, `ui.jsFunction(name)` ignore le résultat, tandis que `ui.jsFunction<R>(name)` renvoie un `std::future<R>`. Les callbacks s’exécutent sur le thread serveur : un traitement bloquant y retarde les autres opérations.",
                    "Only one generation calculation should be outstanding. Wait for its reply before requesting another. If a reset occurs in the meantime, the old reply must no longer update the grid; the starter uses a revision number for this purpose.\n\nA missing function, a C++ exception or disconnection rejects the promise. The programme must then stop playback and display the error.\n\nIn the opposite direction, `ui.jsFunction(name)` ignores the result, whereas `ui.jsFunction<R>(name)` returns a `std::future<R>`. Callbacks execute on the server thread, where blocking work delays other operations."
                ]
            ]
        ],
        "mission": [
            "Examinez `nextGeneration` dans le starter. Identifiez l’attente de connexion, la validation de la réponse et le rejet d’une réponse devenue caduque.",
            "Examine `nextGeneration` in the starter. Identify the connection wait, reply validation and rejection of an outdated reply."
        ],
        "q": [
            "Que renvoie un appel JavaScript vers une fonction C++ de résultat `void` ?",
            "What does a JavaScript call to a C++ function returning `void` produce?"
        ],
        "answers": [
            [
                "Rien.",
                "Nothing."
            ],
            [
                "Une Promise qui peut rejeter.",
                "A Promise that can reject."
            ]
        ],
        "right": 1
    },
    {
        "id": "patterns",
        "title": [
            "Choisir un modèle d’interface",
            "Choosing an interface model"
        ],
        "desc": [
            "Les possibilités offertes par le DOM C++, les formulaires et l’adaptateur React.",
            "The facilities provided by the C++ DOM, forms and React adapter."
        ],
        "doc": "docs/DOM.md",
        "blocks": [
            [
                [
                    "Les possibilités de la version 0.6",
                    "Facilities introduced in version 0.6"
                ],
                [
                    "La version 0.6.0 ajoute plusieurs moyens de construire la présentation d’une application C++. Un panneau simple peut être créé directement en C++. Un formulaire peut être déduit d’une description de ses champs. Une interface plus importante peut être écrite en TypeScript avec React.\n\nLe choix dépend de la quantité de présentation à programmer et de l’endroit où l’on souhaite en conserver la description. Ces trois méthodes peuvent employer le même code métier.",
                    "Version 0.6.0 adds several ways to construct the presentation of a C++ application. A simple panel can be created directly in C++. A form can be generated from a description of its fields. A larger interface can be written in TypeScript with React.\n\nThe choice depends on the amount of presentation code required and where its description should be maintained. All three approaches can use the same application logic."
                ],
                null
            ],
            [
                [
                    "Construire un panneau en C++",
                    "Building a panel in C++"
                ],
                [
                    "La page de secours embarquée contient une zone vide appelée `root`. Nous appellerons *île* une telle zone, dont la page prête le contenu au code C++.\n\nL’appel à `ui.document()` donne accès au document de la connexion. L’extrait suivant, placé avant `app.show()`, crée un bouton et modifie son texte lors d’un clic. Les écritures sont regroupées avant leur envoi au navigateur.\n\nLes opérations sur ce document doivent s’exécuter sur le contexte serveur : dans un callback, dans `onUIStarted`, ou dans une tâche WebFront. Un rechargement de la page crée un nouveau document.",
                    "The embedded fallback page contains an empty region named `root`. We shall call such a region an *island*: the page lends its contents to C++ code.\n\n`ui.document()` provides access to the connection’s document. The following code, placed before `app.show()`, creates a button and changes its text on a click. Writes are batched before being sent to the browser.\n\nDocument operations must execute on the server context: in a callback, in `onUIStarted`, or in a WebFront task. Reloading the page creates a new document."
                ],
                "domCpp"
            ],
            [
                [
                    "Décrire les champs d’un formulaire",
                    "Describing form fields"
                ],
                [
                    "Un descripteur rassemble le nom des champs, leurs libellés, leurs bornes et leurs fonctions d’accès. À partir de cette description, trois fonctions produisent des représentations différentes :\n\n- `forms::form()` construit un formulaire modifiable.\n- `forms::inspector()` construit une vue de lecture.\n- `forms::typescriptDeclaration()` produit une déclaration TypeScript utilisable avec `wf check`.\n\nL’exemple emploie les descripteurs disponibles en C++23. Une couche de réflexion C++26 existe séparément et est qualifiée avec GCC 16. La génération d’une déclaration ne modifie pas les types que le pont binaire sait transporter.",
                    "A descriptor collects field names, labels, bounds and accessors. Three functions use this description to produce different representations:\n\n- `forms::form()` constructs an editable form.\n- `forms::inspector()` constructs a read-only view.\n- `forms::typescriptDeclaration()` produces a TypeScript declaration for use with `wf check`.\n\nThe example uses C++23 descriptors. A separate C++26 reflection layer is qualified with GCC 16. Generating a declaration does not change the types supported by the binary bridge."
                ],
                "forms"
            ],
            [
                [
                    "Associer React et un panneau C++",
                    "Combining React with a C++ panel"
                ],
                [
                    "Le paquet facultatif `@webfront/react` fournit `createStore`, `useStore` et `Island`. Le store transmet des instantanés de l’état C++, regroupés pour chaque navigateur. Le composant `Island` prête un sous-arbre vide au code C++.\n\nLa propriété de ce sous-arbre doit rester explicite. React ne doit pas modifier les nœuds que C++ y construit. Avant le démontage du composant, les deux parties doivent suivre le protocole de libération décrit dans le guide React.",
                    "The optional `@webfront/react` package supplies `createStore`, `useStore` and `Island`. A store sends C++ state snapshots, coalesced for each browser. An `Island` component lends an empty subtree to C++.\n\nOwnership of that subtree must remain explicit. React must not modify the nodes created there by C++. Before unmounting the component, both parties must follow the release protocol described in the React guide."
                ],
                null
            ],
            [
                [
                    "Connexions et événements",
                    "Connections and events"
                ],
                [
                    "La fonction `app.broadcast(name, options)(...)` envoie un appel aux navigateurs prêts. L’encodage est partagé. L’option `maxPendingWrites` permet de conserver seulement le dernier état en attente pour un navigateur trop lent ; elle convient, par exemple, à une vue de télémétrie qui n’a pas à recevoir tous les états intermédiaires. La fonction `forEachUI` visite les connexions prêtes.\n\nUne valeur `UI` identifie l’application, la connexion et sa génération. Une ancienne valeur ne peut donc pas atteindre une connexion ultérieure qui réutiliserait le même identifiant. Elle ne doit jamais être utilisée après la destruction de l’application.\n\nPour écrire une suite de dialogues en C++, une tâche peut attendre un événement avec `co_await node.next(...)`. La tâche reste possédée par WebFront.",
                    "`app.broadcast(name, options)(...)` sends a call to ready browsers, sharing the encoded frame. `maxPendingWrites` can retain only the latest held state for a slow browser. This is useful for telemetry that need not display every intermediate state. `forEachUI` visits ready connections.\n\nA `UI` value identifies the application, connection and connection generation. An old value therefore cannot reach a later connection that reuses the same identifier. Never use it after the application has been destroyed.\n\nA task can express a sequence of C++ dialogues by waiting for an event with `co_await node.next(...)`. The task remains owned by WebFront."
                ],
                null
            ],
            [
                [
                    "Options CMake de la version 0.7",
                    "CMake options in version 0.7"
                ],
                [
                    "À partir de la version 0.7.0, les options de test et d’instrumentation portent obligatoirement le préfixe `WEBFRONT_`. Utilisez `WEBFRONT_ENABLE_TESTING`, `WEBFRONT_ENABLE_FUZZING`, `WEBFRONT_ENABLE_COVERAGE` et `WEBFRONT_ENABLE_SANITIZER_*`.\n\nLes anciennes variables `ENABLE_*`, y compris celles présentes dans un cache existant, ne configurent plus WebFront. Lors d’une migration, indiquez explicitement les nouvelles options.\n\nLes versions de la bibliothèque et de l’environnement de développement sont indépendantes : la bibliothèque 0.7.0 épingle l’environnement 0.6.3. Les API restent expérimentales.",
                    "From version 0.7.0, testing and instrumentation options require the `WEBFRONT_` prefix. Use `WEBFRONT_ENABLE_TESTING`, `WEBFRONT_ENABLE_FUZZING`, `WEBFRONT_ENABLE_COVERAGE` and `WEBFRONT_ENABLE_SANITIZER_*`.\n\nThe old `ENABLE_*` variables, including existing cache entries, no longer configure WebFront. Set the new options explicitly when upgrading.\n\nThe library and development environment have independent versions: library 0.7.0 pins environment 0.6.3. The APIs remain experimental."
                ],
                "migration"
            ]
        ],
        "mission": [
            "Pour un écran de votre application, choisissez entre un panneau C++, un formulaire décrit et un composant React. Indiquez qui possède chaque sous-arbre du document.",
            "For one screen in your application, choose between a C++ panel, a described form and a React component. State who owns each document subtree."
        ],
        "q": [
            "Quelle option active les tests de WebFront 0.7.0 ?",
            "Which option enables WebFront 0.7.0 tests?"
        ],
        "answers": [
            [
                "`ENABLE_TESTING`",
                "`ENABLE_TESTING`"
            ],
            [
                "`WEBFRONT_ENABLE_TESTING`",
                "`WEBFRONT_ENABLE_TESTING`"
            ]
        ],
        "right": 1,
        "links": [
            [
                "DOM",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/DOM.md"
            ],
            [
                "FORMS",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/FORMS.md"
            ],
            [
                "REACT",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/REACT.md"
            ],
            [
                "ASYNC_EXECUTION",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/ASYNC_EXECUTION.md"
            ]
        ]
    },
    {
        "id": "tests",
        "title": [
            "Tester un parcours utilisateur",
            "Testing a user journey"
        ],
        "desc": [
            "Écrire des tests Jasmine, puis les exécuter dans la page d’une application C++.",
            "Writing Jasmine tests and running them in a C++ application’s page."
        ],
        "doc": "docs/CONSUMER_TESTING.md",
        "blocks": [
            [
                [
                    "Les éléments d’un test Jasmine",
                    "The elements of a Jasmine test"
                ],
                [
                    "Jasmine est une bibliothèque de tests JavaScript. Un test décrit une opération et vérifie le résultat qu’elle doit produire. Les exemples de ce chapitre emploient les fonctions suivantes :\n\n- `describe` regroupe les tests qui concernent un même comportement.\n- `it` définit un cas de test.\n- `expect` vérifie une valeur ou une propriété du résultat.\n- `beforeAll` prépare les conditions communes à une suite.\n- `beforeEach` rétablit les conditions nécessaires avant chaque cas.\n\nUn test asynchrone peut employer `async` et `await` pour attendre une réponse C++. Nous commencerons par un test du format des données, avant de tester un parcours dans une interface réelle.",
                    "Jasmine is a JavaScript testing library. A test describes an operation and checks the result it should produce. This chapter uses the following functions:\n\n- `describe` groups tests concerning the same behaviour.\n- `it` defines one test case.\n- `expect` checks a value or property of the result.\n- `beforeAll` prepares conditions shared by a suite.\n- `beforeEach` establishes the conditions required before each case.\n\nAn asynchronous test can use `async` and `await` to wait for a C++ reply. We shall begin with a data-format test, then test a journey in a real interface."
                ],
                null
            ],
            [
                [
                    "Vérifier le format de la grille",
                    "Checking the grid format"
                ],
                [
                    "Dans le terminal de WebFront, installez Jasmine avec `wf install jasmine-core@7.0.2`. Créez ensuite le fichier `tests/life.spec.js` dans `life-project`.\n\nLe test suivant vérifie le compactage de trois cellules dans un tableau d’octets. Il porte sur la représentation JavaScript de la grille ; il ne vérifie pas encore les règles du Jeu de la vie calculées en C++.",
                    "In the WebFront terminal, install Jasmine with `wf install jasmine-core@7.0.2`. Then create `tests/life.spec.js` in `life-project`.\n\nThe following test checks the packing of three cells into a byte array. It concerns the JavaScript grid representation; it does not yet check the Conway rules calculated in C++."
                ],
                "test"
            ],
            [
                [
                    "Déclarer les fichiers de test",
                    "Declaring the test files"
                ],
                [
                    "Ajoutez la propriété suivante à la racine de `webfront.project.json`, en conservant les virgules nécessaires au JSON. Cette recette n’est pas créée par `wf init`.\n\nExécutez `wf test`. Jasmine est chargé dans la fenêtre et la commande échoue lorsqu’une assertion est fausse. Pour vérifier ce comportement, remplacez temporairement `128` par `127` dans le test, puis rétablissez la valeur correcte.",
                    "Add the following property at the root of `webfront.project.json`, retaining the commas required by JSON. `wf init` does not create this recipe.\n\nRun `wf test`. Jasmine is loaded in the window and the command fails when an assertion is false. To check this behaviour, temporarily replace `128` with `127` in the test, then restore the correct value."
                ],
                "manifest"
            ],
            [
                [
                    "Vérifier les types TypeScript",
                    "Checking TypeScript types"
                ],
                [
                    "Créez un fichier `tsconfig.json` à la racine de `life-project`, avec la configuration ci-dessous. Le contrôle des types utilisera la version de TypeScript inscrite dans le fichier de verrouillage.\n\nLa vérification des types et l’exécution des tests ont des rôles distincts. La première vérifie la cohérence statique du programme ; les seconds observent son comportement à l’exécution.",
                    "Create `tsconfig.json` at the root of `life-project`, using the configuration below. Type checking will use the TypeScript version recorded in the lock file.\n\nType checking and test execution serve different purposes. The former checks the programme’s static consistency; the latter observes its runtime behaviour."
                ],
                "tsconfig"
            ],
            [
                [
                    "Installer les déclarations de types",
                    "Installing type declarations"
                ],
                [
                    "Le starter importe React et React DOM. Leur contrôle en mode strict demande les déclarations de types correspondantes, en plus du compilateur TypeScript. Les commandes suivantes les installent, puis exécutent `wf check`.",
                    "The starter imports React and React DOM. Strict checking requires their type declarations as well as the TypeScript compiler. The following commands install them and run `wf check`."
                ],
                "typecheck"
            ],
            [
                [
                    "Tester le projet ou l’application",
                    "Testing the project or the application"
                ],
                [
                    "La commande `wf test` exécute les spécifications déclarées dans le manifeste du projet de développement, avec la version verrouillée de `jasmine-core`.\n\nPour tester la page d’une application C++ existante, on peut aussi employer `webfront_add_jasmine_test`. Ce helper charge Jasmine et les tests dans la page effective de l’application. Ses scripts, ses éléments DOM et ses callbacks C++ habituels restent disponibles.\n\nCette seconde méthode s’intègre à CTest. Elle ne demande ni terminal développeur, ni manifeste `wf`, ni runtime JavaScript sur l’hôte. Elle ne dépend pas de MCP.",
                    "`wf test` runs the specifications declared in the development project’s manifest, using its locked `jasmine-core` version.\n\nTo test an existing C++ application’s page, we can also use `webfront_add_jasmine_test`. This helper loads Jasmine and the tests into the application’s actual page. Its normal scripts, DOM elements and C++ callbacks remain available.\n\nThis second method integrates with CTest. It requires no developer terminal, `wf` manifest or host JavaScript runtime, and does not depend on MCP."
                ],
                null
            ],
            [
                [
                    "L’écran de l’exemple",
                    "The example screen"
                ],
                [
                    "Considérons un petit écran de devis. L’utilisateur saisit une quantité, puis demande le calcul. Le programme appelle la fonction C++ `quote.calculate` et affiche le total obtenu.\n\nCréez `ui/index.html` avec le contenu suivant. Le bouton reste désactivé pendant l’appel ; une erreur est affichée dans la page. La promesse `appReady` appartient à cet exemple : ce n’est pas une propriété définie par WebFront. Les tests l’utiliseront pour attendre que la page ait installé ses gestionnaires.",
                    "Consider a small quotation screen. The user enters a quantity and requests a calculation. The programme calls the C++ function `quote.calculate` and displays the returned total.\n\nCreate `ui/index.html` with the following contents. The button remains disabled during the call, and errors are displayed in the page. The `appReady` promise belongs to this example; it is not a WebFront property. Tests will use it to wait until the page has installed its handlers."
                ],
                "consumerPage"
            ],
            [
                [
                    "Écrire le test du parcours",
                    "Writing the journey test"
                ],
                [
                    "Créez `tests/ui/quote.spec.js`. Le premier cas saisit trois unités, actionne le bouton et vérifie que le total affiché vaut `75`. Le second saisit une quantité invalide et vérifie la présence d’une erreur.\n\nLe calcul C++ n’est pas simulé. Le premier cas exerce donc le gestionnaire du clic, le pont, le callback C++ et la mise à jour de l’affichage. L’attente se termine lorsque le bouton redevient disponible, avec une limite de deux secondes.\n\nLes événements produits par ce test sont synthétiques. Ils ne vérifient ni la disposition des pixels, ni les périphériques d’entrée natifs, ni l’ensemble des propriétés d’accessibilité. Avec des champs contrôlés par React, adaptez également la méthode de saisie.",
                    "Create `tests/ui/quote.spec.js`. The first case enters three units, activates the button and checks that the displayed total is `75`. The second enters an invalid quantity and checks for an error.\n\nThe C++ calculation is not mocked. The first case therefore exercises the click handler, bridge, C++ callback and display update. Waiting ends when the button becomes available again, with a two-second limit.\n\nThe events generated by this test are synthetic. They do not check pixel layout, native input devices or every accessibility property. React-controlled fields also require an appropriate input method."
                ],
                "consumerSpec"
            ],
            [
                [
                    "Préparer l’application C++",
                    "Preparing the C++ application"
                ],
                [
                    "Le fichier `main.cpp` ci-dessous utilise WKWebView et demande macOS 13.3 ou une version ultérieure. Le port `0` laisse le système choisir un port disponible. La racine documentaire `ui` est relative au répertoire de travail défini dans CMake.\n\nL’objet `JasmineTest` doit exister avant l’appel à `show`. Après fermeture de la fenêtre, `shutdown` draine les opérations et `exitCode()` fournit le statut à renvoyer.\n\nLe helper CMake définit `WEBFRONT_JASMINE_TESTING` sur cette cible. Sans cette macro, `JasmineTest` est inerte. Une cible destinée à la distribution doit rester distincte de la cible instrumentée.",
                    "The following `main.cpp` uses WKWebView and requires macOS 13.3 or later. Port `0` lets the operating system choose an available port. The document root `ui` is relative to the working directory set in CMake.\n\nThe `JasmineTest` object must exist before `show` is called. After the window closes, `shutdown` drains operations and `exitCode()` provides the status to return.\n\nThe CMake helper defines `WEBFRONT_JASMINE_TESTING` on this target. Without that macro, `JasmineTest` is inert. Keep a distribution target separate from the instrumented target."
                ],
                "consumerCpp"
            ],
            [
                [
                    "Enregistrer le test dans CMake",
                    "Registering the test in CMake"
                ],
                [
                    "Placez le dépôt WebFront 0.7.0 dans le sous-répertoire `WebFront/`. Le fichier `main.cpp` et le `CMakeLists.txt` suivant se trouvent à la racine du projet ; la page et les tests conservent les chemins indiqués plus haut.\n\nL’argument `FRONTEND` vérifie que le moteur demandé est activé. Le programme C++ choisit lui aussi explicitement ce moteur. À la configuration, le helper télécharge et vérifie `jasmine-core` 7.0.2.\n\nLes fichiers de spécifications sont des scripts JavaScript classiques. Si les sources sont écrites en TypeScript ou organisées en modules, compilez-les au préalable.",
                    "Place the WebFront 0.7.0 checkout in the `WebFront/` subdirectory. Put `main.cpp` and the following `CMakeLists.txt` at the project root; retain the page and test paths given above.\n\nThe `FRONTEND` argument checks that the requested engine is enabled. The C++ programme also selects that engine explicitly. At configuration time, the helper downloads and verifies `jasmine-core` 7.0.2.\n\nSpecification files are classic JavaScript scripts. If their sources use TypeScript or modules, compile them beforehand."
                ],
                "consumerCmake"
            ],
            [
                [
                    "Exécuter et interpréter le résultat",
                    "Running the test and interpreting its result"
                ],
                [
                    "Construisez l’application, puis exécutez CTest avec les commandes suivantes. Le rapport JSON du test se trouve sous `build/webfront-jasmine/QuoteJourney/`.\n\nLe test échoue en cas d’assertion fausse, de suite focalisée, d’absence de spécification exécutée, de rapport absent, d’erreur applicative ou de dépassement du délai. Vérifiez-le en remplaçant `75` par `74`, puis corrigez le test.\n\nSur Linux, choisissez CEF ou WebKitGTK et installez `xvfb-run`. Sur Windows, choisissez WebView2. Le navigateur système ne convient pas à ce mode de test. Après le provisionnement des dépendances, l’exécution ne demande aucun accès au réseau public.",
                    "Build the application and run CTest with the following commands. The test’s JSON report is written under `build/webfront-jasmine/QuoteJourney/`.\n\nThe test fails on a false assertion, a focused suite, zero executed specifications, a missing report, an application error or a timeout. Check this by replacing `75` with `74`, then restore the test.\n\nOn Linux, choose CEF or WebKitGTK and install `xvfb-run`. On Windows, choose WebView2. The system browser is unsuitable for this testing mode. Once dependencies are provisioned, execution needs no public network access."
                ],
                "consumerRun"
            ],
            [
                [
                    "Choisir d’autres parcours",
                    "Choosing further journeys"
                ],
                [
                    "Un bon test de parcours part d’un état connu et vérifie un résultat observable. Dans Life, on peut sélectionner un motif, avancer d’une génération, vérifier la grille et remettre la simulation à zéro. Dans un outil de réglage, on peut modifier une valeur, l’enregistrer, recharger la page et vérifier sa persistance.\n\nChaque cas doit rétablir ses conditions initiales et attendre un signal de fin déterminé. Les erreurs C++ et les déconnexions méritent des cas distincts. Le clavier, le focus, les dialogues natifs et la présentation demandent des vérifications complémentaires.",
                    "A useful journey test starts from a known state and checks an observable result. In Life, it might select a pattern, advance one generation, check the grid and reset the simulation. In a settings tool, it might change a value, save it, reload the page and check persistence.\n\nEach case should establish its initial conditions and wait for a defined completion signal. Give C++ errors and disconnections separate cases. Keyboard input, focus, native dialogues and presentation require additional checks."
                ],
                null
            ]
        ],
        "mission": [
            "Construisez l’exemple de devis. Remplacez le résultat attendu `75` par `74` et vérifiez l’échec de CTest, puis rétablissez la valeur correcte. Ajoutez un cas qui calcule deux devis successifs.",
            "Build the quotation example. Replace the expected result `75` with `74` and check that CTest fails, then restore the correct value. Add a case that calculates two successive quotations."
        ],
        "q": [
            "Que vérifie le parcours Devis ?",
            "What does the quotation journey verify?"
        ],
        "answers": [
            [
                "La saisie, le clic, le pont, le calcul C++ et le total visible.",
                "Input, click, bridge, C++ calculation and visible total."
            ],
            [
                "Tous les pixels et tous les périphériques natifs.",
                "Every pixel and every native input device."
            ]
        ],
        "right": 0,
        "links": [
            [
                "Jasmine",
                "https://jasmine.github.io/"
            ],
            [
                "main.cpp",
                "/examples/quote/main.cpp"
            ],
            [
                "CMakeLists.txt",
                "/examples/quote/CMakeLists.txt"
            ],
            [
                "ui/index.html",
                "/examples/quote/ui/index.html"
            ],
            [
                "quote.spec.js",
                "/examples/quote/tests/ui/quote.spec.js"
            ],
            [
                "Consumer testing",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/CONSUMER_TESTING.md"
            ]
        ]
    },
    {
        "id": "automation",
        "title": [
            "Développer avec un harnais d’IA",
            "Developing with an AI harness"
        ],
        "desc": [
            "Configuration de l’automatisation locale et utilisation de ses résultats pendant le développement.",
            "Configuring local automation and using its results during development."
        ],
        "doc": "docs/AUTOMATION.md",
        "blocks": [
            [
                [
                    "Le rôle du harnais",
                    "The role of the harness"
                ],
                [
                    "Un *harnais* est le programme qui donne à un modèle ses outils et organise leur exécution. Dans le cas qui nous intéresse, il permet au modèle de modifier les sources, puis de vérifier les conséquences de cette modification.\n\nLes outils de l’éditeur ou du harnais écrivent les fichiers. WebFront fournit des opérations de contrôle des types, de compilation de l’interface, de test et d’inspection de la prévisualisation. Le modèle peut ainsi corriger une erreur à partir d’un diagnostic, puis demander une nouvelle vérification.",
                    "A *harness* is the programme that gives a model its tools and arranges their execution. Here, it allows the model to edit sources and then check the consequences of those edits.\n\nThe editor’s or harness’s tools write the files. WebFront provides type checking, interface compilation, testing and preview inspection. The model can therefore correct an error from a diagnostic and request another check."
                ],
                null
            ],
            [
                [
                    "L’intérêt d’un environnement commun",
                    "The value of a common environment"
                ],
                [
                    "Lorsqu’un agent travaille sur une interface native, le projet doit lui fournir les moyens de construire le programme, de piloter ses contrôles et d’interpréter les résultats. Ces moyens existent dans de nombreuses bibliothèques d’IHM, mais leur assemblage et leur adaptation restent souvent à la charge du projet.\n\nWebFront réunit dans son environnement la compilation TSX, le contrôle des types, Jasmine et la prévisualisation. Le même callback C++ est appelé lors d’une utilisation normale et pendant le test. Une modification peut donc être accompagnée immédiatement d’un contrôle exécutable.\n\nL’intérêt pratique est de réduire les interventions où le développeur relance une commande, lit une sortie et la retransmet à l’agent. Il dépend du projet, de ses tests et du modèle employé ; il ne constitue pas un facteur d’accélération garanti.\n\nCette méthode intervient dès la création d’une application, puis lors de son évolution. Les fonctions et les tests ajoutés pendant les premières itérations deviennent les éléments de travail de la maintenance et de la distribution.",
                    "When an agent works on a native interface, the project must provide ways to build the programme, operate its controls and interpret the results. Many GUI libraries provide such facilities, but assembling and adapting them often remains project work.\n\nWebFront brings TSX compilation, type checking, Jasmine and preview into one environment. The same C++ callback is used during normal operation and during tests. A change can therefore be accompanied immediately by an executable check.\n\nThe practical benefit is fewer occasions on which a developer must rerun a command, read its output and relay it to the agent. The benefit depends on the project, its tests and the model; it is not a guaranteed speed-up factor.\n\nThis method applies from the creation of an application through its subsequent evolution. Functions and tests added in the first iterations become the working material for maintenance and distribution."
                ],
                null
            ],
            [
                [
                    "Construire le lanceur",
                    "Building the launcher"
                ],
                [
                    "Les commandes suivantes construisent `webfront-agent` et le pilote `webfront-wf` depuis le dépôt 0.7.0, sur macOS. Sous Windows ou Linux, activez le moteur embarqué correspondant.\n\nLe lanceur demande une archive d’environnement 0.6.3 ou ultérieure contenant l’API d’automatisation v1, ainsi qu’une empreinte SHA-512 de confiance. Cette archive peut provenir de la distribution vérifiée ou d’une construction décrite dans le guide source.\n\nLe manifeste, le fichier de verrouillage et les dépendances du projet doivent être préparés avant de connecter l’agent. Les outils d’automatisation ne proposent ni `install` ni `restore`.",
                    "The following commands build `webfront-agent` and the `webfront-wf` driver from the 0.7.0 checkout on macOS. Enable the appropriate embedded engine on Windows or Linux.\n\nThe launcher requires an environment archive at version 0.6.3 or later containing automation API v1, together with a trusted SHA-512 digest. Use the verified distribution or build it as described in the source guide.\n\nPrepare the project manifest, lock file and dependencies before connecting the agent. Automation tools provide neither `install` nor `restore`."
                ],
                "agentBuild"
            ],
            [
                [
                    "Partager une fenêtre de développement",
                    "Sharing a development window"
                ],
                [
                    "La commande suivante ouvre une fenêtre qui reste utilisable par le développeur. Remplacez les chemins et `sha512-TRUSTED_DIGEST` par leurs valeurs réelles ; cette chaîne n’est pas une empreinte valide. Le répertoire de session doit être nouveau, privé et situé hors du projet.\n\n- `--serve` conserve la fenêtre et son terminal ouverts.\n- `--listen` permet à un client local de s’y attacher.\n- Chaque option `--allow` autorise l’opération nommée.\n\nAccordez seulement les opérations nécessaires au travail prévu. Une vérification de types et de tests ne demande pas les autorisations d’inspection ou d’interaction.",
                    "The following command opens a window that remains available to the developer. Replace the paths and `sha512-TRUSTED_DIGEST` with their real values; that string is not a valid digest. The session directory must be new, private and outside the project.\n\n- `--serve` keeps the window and terminal open.\n- `--listen` allows a local client to attach.\n- Each `--allow` option permits the named operation.\n\nGrant only the operations required for the work. Type checking and test execution do not require inspection or interaction permissions."
                ],
                "agentServe"
            ],
            [
                [
                    "Configurer le client MCP",
                    "Configuring the MCP client"
                ],
                [
                    "Le lanceur expose MCP sur l’entrée et la sortie standard d’un processus local. Le client démarre ici un adaptateur qui s’attache à la fenêtre déjà ouverte. Il ne se connecte pas à un service HTTP.\n\nL’exemple reprend la forme courante d’une configuration `mcpServers`. Adaptez son emplacement aux conventions de votre client et indiquez le chemin réel du binaire. Fermer le client détache l’adaptateur sans fermer la fenêtre du développeur. Le client ne peut pas élargir les permissions accordées par le propriétaire de la session.",
                    "The launcher exposes MCP over a local process’s standard input and output. Here, the client starts an adapter that attaches to the already open window. It does not connect to an HTTP service.\n\nThe example uses the common `mcpServers` configuration format. Use the location expected by your client and the actual executable path. Closing the client detaches the adapter without closing the developer’s window. The client cannot broaden permissions granted by the session owner."
                ],
                "agentConfig"
            ],
            [
                [
                    "Utiliser la ligne de commande",
                    "Using the command line"
                ],
                [
                    "Un harnais qui sait lancer un processus peut employer l’interface JSON en ligne de commande. L’option `--schema` décrit le contrat ; `--agent-help` fournit des instructions courtes.\n\nChaque commande attend son résultat final. Le statut de sortie indique notamment :\n\n- `0` : réussite ;\n- `1` : échec de l’opération ;\n- `70` : échec du bootstrap ;\n- `71` : expiration du délai natif.\n\nLes commandes de construction portent sur l’interface. La compilation C++ de l’application reste à organiser dans le harnais.",
                    "A harness that can start a process can use the JSON command-line interface. `--schema` describes the contract; `--agent-help` provides short instructions.\n\nEach command waits for its final result. Exit statuses include:\n\n- `0`: success;\n- `1`: operation failure;\n- `70`: bootstrap failure;\n- `71`: native deadline expiry.\n\nBuild commands compile the interface. The harness must arrange the application’s C++ compilation separately."
                ],
                "agentCli"
            ],
            [
                [
                    "Suivre une opération MCP",
                    "Following an MCP operation"
                ],
                [
                    "Le client dispose de quatre outils :\n\n- `webfront.session` obtient les informations de session.\n- `webfront.start` lance une opération et renvoie son identifiant `runId`.\n- `webfront.result` consulte l’état et le résultat de cette opération.\n- `webfront.cancel` demande son annulation.\n\nL’extrait ci-dessous décrit une suite d’appels d’outils ; il ne doit pas être collé dans le JavaScript de la page. Après chaque lancement, attendez un état terminal : `succeeded`, `failed`, `cancelled`, `timeout` ou `disconnected`.\n\nL’acceptation d’une requête ne signifie pas que l’opération a réussi. Les diagnostics de compilation peuvent indiquer le fichier, la ligne et la colonne ; Jasmine fournit un rapport dans `report`.",
                    "The client has four tools:\n\n- `webfront.session` obtains session information.\n- `webfront.start` starts an operation and returns its `runId`.\n- `webfront.result` reads that operation’s state and result.\n- `webfront.cancel` requests cancellation.\n\nThe fragment below describes tool calls; do not paste it into the page’s JavaScript. After each start, wait for a terminal state: `succeeded`, `failed`, `cancelled`, `timeout` or `disconnected`.\n\nAcceptance of a request does not mean the operation succeeded. Compiler diagnostics may identify the file, line and column; Jasmine supplies its report in `report`."
                ],
                "agentTools"
            ],
            [
                [
                    "Exemple : ajouter un écran de devis",
                    "Example: adding a quotation screen"
                ],
                [
                    "Le devis du chapitre précédent peut servir de premier exercice. La demande ci-dessous précise le comportement attendu, l’emplacement du calcul et le test à produire.\n\nLe harnais modifie les fichiers avec ses outils habituels, puis demande à WebFront de vérifier les types, de construire l’interface et d’exécuter les tests. Il peut ensuite inspecter la prévisualisation et essayer les contrôles.\n\nLe développeur examine les sources modifiées et les assertions. Le parcours Jasmine reste dans le dépôt : il pourra être exécuté par la CI et signaler une régression ultérieure.",
                    "The quotation screen from the preceding chapter is a suitable first exercise. The request below specifies the expected behaviour, where the calculation belongs and the test to produce.\n\nThe harness edits files with its usual tools, then asks WebFront to check types, build the interface and run tests. It can then inspect the preview and try the controls.\n\nThe developer reviews the modified sources and assertions. The Jasmine journey remains in the repository, where CI can execute it and detect a later regression."
                ],
                "agentTask"
            ],
            [
                [
                    "Autres travaux adaptés à cette méthode",
                    "Further applications of the method"
                ],
                [
                    "La correction d’une erreur TypeScript est un cas simple. Le modèle lit le diagnostic, modifie la source et relance `wf check`, puis les tests concernés. Le développeur n’a plus à lui recopier chaque message de compilation.\n\nPour un panneau de réglages, une description des champs C++ permet de générer le formulaire et les déclarations TypeScript. On peut alors demander au modèle d’ajouter les tests de bornes et de valeurs invalides.\n\nPour une régression fonctionnelle, on commence par reproduire le défaut dans un test Jasmine. La correction est ensuite évaluée sur ce cas et sur les tests existants. Cette démarche laisse un contrôle durable, indépendant de la conversation avec le modèle.",
                    "Correcting a TypeScript error is a simple case. The model reads the diagnostic, edits the source and reruns `wf check`, followed by relevant tests. The developer no longer has to relay every compiler message.\n\nFor a settings panel, a description of C++ fields can generate the form and TypeScript declarations. The model can then add boundary and invalid-value tests.\n\nFor a functional regression, first reproduce the defect in a Jasmine test. Evaluate the correction against that case and the existing tests. This leaves a lasting check that is independent of the conversation with the model."
                ],
                null
            ],
            [
                [
                    "Étendue de l’inspection",
                    "Scope of inspection"
                ],
                [
                    "L’opération `inspect` décrit les éléments visibles qui possèdent un identifiant explicite. Elle fournit des textes et libellés de taille limitée, ainsi que les erreurs observées. L’opération `fill` accepte les champs texte et nombre ordinaires ; `click` accepte notamment les boutons et les cases à cocher.\n\nUne action doit reprendre le `pageId` de l’inspection. Après une navigation, cette révision devient caduque et l’action est refusée. L’inspection ne fournit ni capture d’écran, ni sélecteur arbitraire, ni arbre d’accessibilité complet. Les interactions DOM sont synthétiques.\n\nLe terminal et l’agent ne peuvent pas exécuter simultanément des opérations incompatibles. Un observateur `wf dev` peut toutefois rester ouvert pendant qu’il attend une modification.",
                    "`inspect` describes visible elements that have explicit identifiers. It provides bounded text and labels, together with observed errors. `fill` accepts ordinary text and number fields; `click` accepts buttons and checkboxes among other controls.\n\nAn action must use the inspected `pageId`. Navigation invalidates that revision, so a subsequent action is refused. Inspection supplies no screenshots, arbitrary selectors or full accessibility tree. DOM interactions are synthetic.\n\nThe terminal and agent cannot execute incompatible operations concurrently. A `wf dev` watcher may, however, remain open while waiting for a change."
                ],
                null
            ],
            [
                [
                    "Résultats observés et limites",
                    "Observed results and limitations"
                ],
                [
                    "La qualification locale du 29 octobre 2026 a porté sur cinq modèles installés. Les 30 sources finales ont passé une vérification indépendante des types et des tests. Dans 29 conversations sur 30, le modèle a lui-même observé les deux réussites. Le rapport donne le détail des tâches et du cas incomplet. Ces observations ne sont pas une comparaison de productivité avec une autre bibliothèque d’IHM.\n\nLe lanceur est facultatif et local. Il ne fournit ni shell, ni évaluation JavaScript libre, ni lecture arbitraire des fichiers. Le projet et ses tests exécutent néanmoins du code avec l’autorité de l’application. Pour du code inconnu, employez un environnement isolé sans secrets. Sur WKWebView et WebKitGTK, les données du navigateur ne sont pas isolées par processus.",
                    "The local qualification of 29 October 2026 used five installed models. All 30 final sources passed independent type and test checks. In 29 of the 30 conversations, the model itself observed both successful results. The report details the tasks and incomplete case. These observations are not a productivity comparison with another GUI library.\n\nThe launcher is optional and local. It supplies no shell, unrestricted JavaScript evaluation or arbitrary file reads. Projects and tests still execute code with application authority. Use an isolated environment without secrets for unfamiliar code. WKWebView and WebKitGTK do not isolate browser storage per process."
                ],
                null
            ]
        ],
        "mission": [
            "Demandez au harnais d’ajouter un comportement observable et un test de parcours correspondant. Faites exécuter le test avant et après la correction, puis examinez les sources modifiées et les résultats.",
            "Ask the harness to add an observable behaviour and a corresponding journey test. Run the test before and after the correction, then review the modified sources and results."
        ],
        "q": [
            "L’acceptation d’un appel à `webfront.start` indique-t-elle que les tests ont réussi ?",
            "Does acceptance of a `webfront.start` call indicate that the tests passed?"
        ],
        "answers": [
            [
                "Oui.",
                "Yes."
            ],
            [
                "Non. Il faut consulter le résultat final associé au `runId`.",
                "No. Read the final result associated with the `runId`."
            ]
        ],
        "right": 1,
        "links": [
            [
                "MCP / CLI",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/AUTOMATION.md"
            ],
            [
                "Qualification · 2026-10-29",
                "https://github.com/ambroise-leclerc/WebFront/blob/v0.7.0/docs/validation/automation-models-2026-10-29.md"
            ]
        ]
    },
    {
        "id": "embed",
        "title": [
            "Embarquer et distribuer",
            "Embedding and distribution"
        ],
        "desc": [
            "Produire un en-tête de ressources et construire une application autonome.",
            "Producing a resource header and building a standalone application."
        ],
        "doc": "README.md",
        "blocks": [
            [
                [
                    "Générer l’en-tête de ressources",
                    "Generating the resource header"
                ],
                [
                    "La commande `wf embed` compile l’interface et regroupe ses ressources dans un en-tête C++ compressé. Les notices des dépendances web sont incluses. Exécutez la commande suivante dans le terminal de l’application.\n\nLa génération de cet en-tête ne recompile pas le programme C++. Cette seconde étape sera effectuée avec CMake.",
                    "`wf embed` compiles the interface and packages its resources in a compressed C++ header. Notices for web dependencies are included. Run the following command in the application terminal.\n\nGenerating this header does not recompile the C++ programme. We shall perform that second step with CMake."
                ],
                "embed"
            ],
            [
                [
                    "Employer les ressources générées",
                    "Using the generated resources"
                ],
                [
                    "Le programme de distribution n’a plus besoin du répertoire de projet ni de l’appel à `bootstrap`. Il utilise à leur place le fournisseur de ressources généré.\n\nL’extrait reprend les en-têtes de l’exemple Life et suppose que `generated/LifeUI.hpp` est accessible par le chemin d’inclusion. Le dépôt fournit la cible correspondante, `WebFrontLifeEmbedded`.",
                    "The distribution programme no longer needs the project directory or the call to `bootstrap`. It uses the generated resource provider instead.\n\nThis fragment uses the Life example’s headers and assumes `generated/LifeUI.hpp` is on the include path. The repository supplies the corresponding `WebFrontLifeEmbedded` target."
                ],
                "embedded"
            ],
            [
                [
                    "Construire la version de distribution",
                    "Building the distribution version"
                ],
                [
                    "Fermez la fenêtre de développement, puis exécutez ces commandes dans le même répertoire de construction. Avec Visual Studio, l’exécutable se trouve dans `examples\\Debug\\WebFrontLifeEmbedded.exe`.\n\nL’application peut alors fonctionner hors ligne, sans les sources de l’interface ni le cache des outils. Le moteur de rendu reste une dépendance de la plateforme : son déploiement doit être prévu séparément. `wf embed` ne produit pas un installeur.",
                    "Close the development window and run these commands in the same build directory. With Visual Studio, the executable is `examples\\Debug\\WebFrontLifeEmbedded.exe`.\n\nThe application can then run offline without interface sources or the tools cache. The rendering engine remains a platform dependency whose deployment must be arranged separately. `wf embed` does not produce an installer."
                ],
                "rebuild"
            ]
        ],
        "mission": [
            "Lancez la version embarquée depuis un répertoire vide et sans réseau. Vérifiez que votre interface apparaît. Consultez ensuite les exigences du moteur natif choisi avant de préparer un installeur.",
            "Run the embedded version from an empty directory without network access. Check that your interface appears. Then read the chosen native engine’s requirements before preparing an installer."
        ],
        "q": [
            "La commande `wf embed` installe-t-elle le moteur de rendu chez l’utilisateur ?",
            "Does `wf embed` install the rendering engine on the user’s machine?"
        ],
        "answers": [
            [
                "Oui, automatiquement.",
                "Yes, automatically."
            ],
            [
                "Non, son déploiement reste à prévoir.",
                "No, deployment is still application work."
            ]
        ],
        "right": 1
    },
    {
        "id": "dom",
        "optional": true,
        "title": [
            "Le DOM et le navigateur",
            "The DOM and the browser"
        ],
        "desc": [
            "La représentation du document et la propriété de ses éléments.",
            "Document representation and ownership of its elements."
        ],
        "doc": "docs/DOM.md",
        "blocks": [
            [
                [
                    "La structure du document",
                    "Document structure"
                ],
                [
                    "Le *DOM* est la représentation en mémoire d’un document HTML. Ses éléments forment un arbre : un bouton, par exemple, peut être enfant d’un panneau et contenir un nœud de texte.\n\nJavaScript peut modifier cet arbre. Le navigateur recalcule alors la présentation des éléments concernés. Les règles CSS déterminent notamment les couleurs, les dimensions et la disposition.",
                    "The *DOM* is the in-memory representation of an HTML document. Its elements form a tree: a button, for example, may belong to a panel and contain a text node.\n\nJavaScript can modify this tree. The browser then updates the affected elements’ presentation. CSS rules determine properties such as colours, dimensions and layout."
                ]
            ],
            [
                [
                    "Modifier un élément",
                    "Modifying an element"
                ],
                [
                    "L’extrait suivant recherche l’élément dont l’identifiant est `generation`, puis remplace son texte s’il existe. La vérification évite de déréférencer un résultat absent.\n\nDans un composant React, on modifie généralement l’état du composant plutôt que son DOM. React conserve ainsi la responsabilité des éléments qu’il a créés.",
                    "The following fragment finds the element whose identifier is `generation` and replaces its text if it exists. The check avoids dereferencing a missing result.\n\nInside a React component, we normally update component state rather than its DOM. React thereby retains responsibility for the elements it created."
                ],
                "dom"
            ],
            [
                [
                    "Le document accessible depuis C++",
                    "Accessing the document from C++"
                ],
                [
                    "Depuis la version 0.6.0, `ui.document()` permet de construire des sous-arbres en C++ dans les îles prêtées par la page. La page de secours fournit l’île `root`. Une page personnalisée installe `/webfront-dom.mjs` et déclare ses propres îles.\n\nL’API ne permet pas de sélectionner librement les éléments d’un composant React. Les opérations doivent s’exécuter sur le contexte serveur. Le chapitre consacré aux modèles d’interface présente un exemple de bouton complet.",
                    "Since version 0.6.0, `ui.document()` allows C++ to build subtrees within islands lent by the page. The fallback page supplies the `root` island. A custom page installs `/webfront-dom.mjs` and declares its own islands.\n\nThe API does not permit unrestricted selection of React component elements. Operations must run on the server context. The interface-model chapter presents a complete button example."
                ],
                "domCpp"
            ]
        ],
        "mission": [
            "Dans votre interface, ajoutez un paragraphe puis faites varier son contenu avec un état React. Vérifiez que le bouton reste utilisable au clavier.",
            "Add a paragraph to your interface, then change its content using React state. Check that the button remains keyboard-accessible."
        ],
        "q": [
            "Qui dessine le document HTML ?",
            "What renders the HTML document?"
        ],
        "answers": [
            [
                "Le moteur du navigateur.",
                "The browser engine."
            ],
            [
                "Le compilateur C++.",
                "The C++ compiler."
            ]
        ],
        "right": 0
    },
    {
        "id": "react",
        "optional": true,
        "title": [
            "Composants React et état",
            "React components and state"
        ],
        "desc": [
            "Description d’un composant, modification de son état et intégration avec C++.",
            "Describing a component, updating its state and integrating it with C++."
        ],
        "doc": "docs/REACT.md",
        "blocks": [
            [
                [
                    "Décrire un composant",
                    "Describing a component"
                ],
                [
                    "Un composant React décrit une partie de l’interface à partir de ses propriétés et de son état. Le JSX ressemble à du HTML, mais fait partie du programme JavaScript ou TypeScript.\n\nLorsqu’un état change, React recalcule la description du composant et applique les modifications nécessaires au DOM. La fonction de rendu doit donc décrire la présentation correspondant à l’état courant.",
                    "A React component describes part of an interface from its properties and state. JSX resembles HTML but belongs to the JavaScript or TypeScript programme.\n\nWhen state changes, React recalculates the component description and applies the necessary DOM updates. The rendering function should therefore describe the presentation corresponding to the current state."
                ]
            ],
            [
                [
                    "Modifier un état",
                    "Updating state"
                ],
                [
                    "Cet extrait appartient à un composant fonctionnel et suppose que `useState` a été importé depuis React. La fonction `g => g + 1` calcule la nouvelle valeur à partir de la précédente.\n\nPour modifier une grille conservée dans l’état, créez une nouvelle valeur et transmettez-la au setter. Une modification directe du tableau existant ne décrit pas correctement le changement à React.",
                    "This fragment belongs inside a function component and assumes `useState` has been imported from React. The function `g => g + 1` calculates a new value from the previous one.\n\nTo update a grid held in state, create a new value and pass it to the setter. Mutating the existing array does not correctly describe the change to React."
                ],
                "react"
            ],
            [
                [
                    "Recevoir une réponse asynchrone",
                    "Receiving an asynchronous reply"
                ],
                [
                    "Un appel C++ peut se terminer après une remise à zéro ou après le démontage du composant qui l’a demandé. Avant de modifier l’état à la suite d’un `await`, vérifiez que la réponse appartient encore à la session courante.\n\nLes abonnements créés dans `useEffect` doivent être accompagnés d’une fonction de nettoyage. Le starter Life illustre ces deux obligations.",
                    "A C++ call may finish after a reset or after the requesting component has unmounted. Before updating state following an `await`, check that the reply still belongs to the current session.\n\nSubscriptions created in `useEffect` require a clean-up function. The Life starter illustrates both obligations."
                ]
            ],
            [
                [
                    "L’adaptateur WebFront",
                    "The WebFront adapter"
                ],
                [
                    "Le paquet facultatif `@webfront/react` fournit `createStore`, `useStore` et `Island`. Il est distribué séparément de React et des ressources du cœur WebFront. L’exemple `examples/life/react` associe une interface React à un panneau construit en C++.\n\nLe démontage demande une libération coordonnée : React doit attendre que le propriétaire C++ ait terminé son nettoyage avant de retirer l’île. Le guide React décrit ce protocole.",
                    "The optional `@webfront/react` package provides `createStore`, `useStore` and `Island`. It is distributed separately from React and WebFront’s core resources. `examples/life/react` combines a React interface with a panel built in C++.\n\nUnmounting requires coordinated release: React must wait until the C++ owner has completed its clean-up before removing the island. The React guide describes this protocol."
                ],
                null
            ]
        ],
        "mission": [
            "Ajoutez un compteur de clics dans un composant, puis remettez-le à zéro avec un second bouton. Essayez une mise à jour fonctionnelle.",
            "Add a click counter to a component, then reset it with a second button. Try a functional state update."
        ],
        "q": [
            "Faut-il modifier directement le tableau conservé dans l’état ?",
            "Should you directly mutate the array stored in state?"
        ],
        "answers": [
            [
                "Oui.",
                "Yes."
            ],
            [
                "Non, créez une nouvelle valeur.",
                "No, create a new value."
            ]
        ],
        "right": 1
    },
    {
        "id": "cpp",
        "optional": true,
        "title": [
            "C++ moderne et asynchronisme",
            "Modern C++ and asynchrony"
        ],
        "desc": [
            "Propriété des callbacks, suspension des tâches et annulation locale.",
            "Callback ownership, task suspension and local cancellation."
        ],
        "doc": "docs/ASYNC_EXECUTION.md",
        "blocks": [
            [
                [
                    "Durée de vie des callbacks",
                    "Callback lifetime"
                ],
                [
                    "Un callback enregistré peut être appelé après le retour de la fonction qui l’a créé. Ses captures doivent donc rester valides pendant toute la durée de son enregistrement.\n\nLa capture par référence d’une variable locale n’est correcte que si cette variable vit assez longtemps. L’application possède les callbacks enregistrés ; les objets auxquels ils accèdent doivent avoir une durée de vie compatible.",
                    "A registered callback may be called after the function that created it has returned. Its captures must therefore remain valid for the entire registration lifetime.\n\nCapturing a local variable by reference is correct only if that variable lives long enough. The application owns registered callbacks; the objects they access need compatible lifetimes."
                ]
            ],
            [
                [
                    "Suspension d’une coroutine",
                    "Suspending a coroutine"
                ],
                [
                    "Une coroutine peut suspendre son exécution avec `co_await` en attendant la fin d’une opération. Cette suspension ne crée pas de thread et ne déplace pas automatiquement un calcul coûteux hors du thread courant.\n\nLes tâches applicatives WebFront renvoient `webfront::Task`. Elles sont démarrées par `app.spawn(task)` ou `ui.spawn(task)`. WebFront les possède et les draine avant sa destruction.",
                    "A coroutine can suspend execution with `co_await` while waiting for an operation to finish. Suspension neither creates a thread nor automatically moves expensive computation away from the current thread.\n\nWebFront application tasks return `webfront::Task`. Start them with `app.spawn(task)` or `ui.spawn(task)`. WebFront owns them and drains them before destruction."
                ]
            ],
            [
                [
                    "Résultat et annulation",
                    "Results and cancellation"
                ],
                [
                    "L’extrait suivant suppose une valeur `ui` valide et une fonction JavaScript enregistrée sous le nom `getTitle`. Le sender typé propage le jeton d’arrêt de la coroutine. Pour demander l’arrêt individuel d’une tâche, conservez son `TaskHandle`.\n\nL’annulation termine localement l’attente ; elle n’arrête pas du JavaScript déjà lancé. Les futures C++ et les promesses JavaScript n’exposent pas cette annulation. Les appels ordinaires n’ont pas de délai d’expiration automatique.",
                    "The following fragment assumes a valid `ui` value and a JavaScript function registered as `getTitle`. The typed sender propagates the coroutine’s stop token. Retain the task’s `TaskHandle` to request individual cancellation.\n\nCancellation ends the local wait; it does not stop JavaScript that has already started. C++ futures and JavaScript promises do not expose this cancellation. Ordinary calls have no automatic timeout."
                ],
                "async"
            ]
        ],
        "mission": [
            "Pour chaque capture de callback, indiquez l’objet propriétaire de la donnée et expliquez pourquoi cette donnée sera encore valide lors de l’appel.",
            "For each callback capture, identify the object that owns the data and explain why the data will remain valid when called."
        ],
        "q": [
            "L’expression `co_await` crée-t-elle un thread ?",
            "Does `co_await` create a thread?"
        ],
        "answers": [
            [
                "Non.",
                "No."
            ],
            [
                "Oui.",
                "Yes."
            ]
        ],
        "right": 0
    }
];
