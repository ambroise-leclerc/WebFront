# ADR-016 : L'environnement bootstrap comme socle de WebFront

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-22 | Acceptée | Ambroise Leclerc | Décision initiale : environnement bootstrap comme architecture de base, liste normative du cœur, scripts autorisés, indépendance vis-à-vis de Node.js. | [404fed2](https://github.com/ambroise-leclerc/WebFront/commit/404fed2c2145d4faba8efee035e10522ff87a91f) |
| 1.1 | 2026-10-23 | Acceptée | Ambroise Leclerc | Garde-fou `MinimalCoreAssets` (#314) : la liste est vérifiée par CTest, budget de 150 Kio d'octets décodés ; le favicon devient `FaviconData`, généré par CMake pur depuis `js/core/favicon.ico`. | [c0ee031](https://github.com/ambroise-leclerc/WebFront/commit/c0ee03131d3104721f8a24224533e753a5f6ad13) |
| 1.2 | 2026-10-23 | Acceptée | Ambroise Leclerc | Auto-hébergement (#315) : le dépôt est un projet `wf` dont les scripts construisent l'environnement, le site et la vérification du pin avec l'environnement d'amorçage épinglé ; octets identiques à la construction de l'époque, preuve de reconstruction du bundle bootstrap. | — |
| 1.3 | 2026-10-24 | Acceptée | Ambroise Leclerc | Tests, régénération et CI sans Node.js ni npm (#318) : les en-têtes du cœur sont produits et vérifiés par un générateur CMake pur (`js/core/EmbedAsset.cmake`), qui remplace `wf run embed-core` et le générateur Node.js ; les paquets du registre de test sont téléchargés et vérifiés au configure ; les tests JavaScript deviennent des specs Jasmine exécutées par `wf test` ; garde-fou global `NoNodeDependency`. | — |
| 1.4 | 2026-10-24 | Acceptée | Ambroise Leclerc | Qualification du parcours (#316) : ADR-013, ADR-014 et ADR-015 passent à « Acceptée ». | [#348](https://github.com/ambroise-leclerc/WebFront/pull/348) |
| 1.5 | 2026-10-24 | Acceptée | Ambroise Leclerc | Documentation consolidée (#317) : la formulation courante décrit directement l'environnement et ses commandes (`wf init`, `install`, `dev`, `check`, `test`, `embed`), sans comparaison avec d'autres outils ; les versions et décisions antérieures de ce tableau et de l'historique Git restent l'historique de la décision. | [#317](https://github.com/ambroise-leclerc/WebFront/issues/317) |
| 1.6 | 2026-10-26 | Acceptée | Ambroise Leclerc | Ajout du module DOM optionnel `DomInterpreterData`, commande bornée de l’ADR-003 (#380) ; budget inchangé. | — |

## Statut et date

**Acceptée le 2026-10-22** (épique #303, issue #304). Cette décision fixe la direction
architecturale et les règles que les issues suivantes de l'épique doivent respecter. Les mécanismes
déjà proposés (ADR-013, ADR-014, ADR-015) sont passés à « Acceptée » le 2026-10-24 avec les preuves
multiplateformes de #316. Aucun code n'est modifié par
cette décision ; les retraits qu'elle annonce sont réalisés par #311 à #319.

## Contexte

L'étape développeur du jeu de la vie (`examples/life/LifeDeveloper.cpp`) montre le modèle visé :
`NativeApp::bootstrap("developer")` sert une petite page embarquée, télécharge une archive
d'environnement vérifiée SHA-512, monte un terminal dans le navigateur, installe des paquets du registre npm
(`wf install`), compile TS/TSX dans un Worker (`wf dev`, `wf build`), vérifie les types (`wf check`), exécute les specs (`wf test`) et génère un provider
propre à l'application (`wf embed`), compilé ensuite dans un exécutable de distribution.

Le dépôt livre encore en parallèle environ 2,5 Mo d'en-têtes pré-générés sous
`include/webfront/system/` (dont `BabelData.hpp`, 1,5 Mo), environ 3,4 Mo de sources vendorisées
dans `third_party/`, des providers (`BabelFS`, `ReactFS`, `JasmineFS`) qui ne servent qu'aux tests
et un second système de terminal et d'outils (`Terminal.hpp`, `ToolManager.hpp`, `ToolStore`,
`PackageFS`, `TerminalFS`, `ToolManagerFS`) avec un autre `wf`. Régénérer ou tester exige Node.js.

## Décision

L'environnement bootstrap devient l'architecture de base : un outil de développement complet
(manifeste de projet, install, run, test, build, embed) fourni par une archive
téléchargée et vérifiée, tandis que le code source de WebFront redevient minimaliste.

### Liste normative des ressources du cœur

`include/webfront/` ne contient, comme ressources web embarquées, que :

| Ressource | Rôle |
| --- | --- |
| `WebFrontJsData` | Bridge `WebFront.js` |
| `DomInterpreterData` | Module optionnel `/webfront-dom.mjs`, chargé explicitement par les pages DOM |
| `FallbackIndexHtmlData` | Page de repli |
| `FallbackModuleData` | Module de repli |
| `FaviconData` (alias `IndexFS::WebFrontIco`, source `js/core/favicon.ico`) | Icône par défaut |
| `DeveloperIndexHtmlData` | Page du bootstrap développeur |
| `DeveloperBootstrapData` | Chargeur bootstrap (`bootstrap.bundle.mjs`) |
| `DeveloperReleaseData` | Release épinglée (URL et SHA-512) |

Toute autre ressource web (bibliothèques, runner de test, pages de test, terminal, providers
applicatifs) est produite par un script autorisé et livrée par l'archive d'environnement ou par un
paquet npm verrouillé par intégrité. Cette liste est normative : le test garde-fou `MinimalCoreAssets` (#314) la
compare aux en-têtes de ressources de `include/webfront/` et impose un budget de 150 Kio d'octets décodés
(somme des `dataSize` après décodage gzip, indépendante de l'encodage) ;
ajouter une ressource exige de modifier cette ADR (nouvelle version) et ce test dans le même
changement. Le C++ header-only (filesystems, serveur, bridge, frontends) n'est pas concerné.

### Script autorisé par WebFront

Code JS/TS exécuté par l'environnement vérifié, dans un Worker, avec une API WebFront bornée
(`context`) et des permissions déclarées dans le manifeste de projet. Il n'y a ni API Node
implicite, ni shell, ni exécution des scripts d'installation npm.

### Portée des permissions

Les permissions du manifeste encadrent **uniquement** les opérations offertes par l'API
`context` : fichiers du projet via le bridge, publication, `context.fetch` vers des origines
listées. Elles ne restreignent pas les capacités propres du Worker (`fetch` global,
`importScripts`, WebSocket…) et ne constituent pas une frontière de sécurité, conformément à
l'[ADR-008](ADR-008-contenu-de-confiance-et-origines.md) : le code téléchargé et les scripts
autorisés ont l'autorité du bridge et du projet désigné. Un durcissement ultérieur (CSP
`connect-src` servie avec le script du Worker) reste possible mais n'est pas promis.

### Indépendance vis-à-vis de l'environnement

Aucun usage (utilisateur, contributeur, CI) ne requiert Node.js ou npm : seuls restent un
compilateur C++23, CMake et le moteur du frontend choisi. Les en-têtes du cœur sont régénérés et
vérifiés sans Node (#318), **par CMake seul** : `js/core/EmbedAsset.cmake` rend l'en-tête de chaque
ressource (sept sources de moins de 40 Kio, quelques dixièmes de seconde) et compare ses octets exacts à ceux du fichier
versionné. Le choix d'un générateur CMake pur plutôt que d'un script exécuté par `wf` tient à ce que la fraîcheur des
en-têtes doit être vérifiable par un build qui n'a pas de navigateur (la CI CEF-off, les sanitizers) ; une seule
implémentation du rendu subsiste, la compression gzip n'ayant plus d'usage dans le cœur (elle reste celle de `wf embed`,
dans l'environnement, avec le `fflate` verrouillé). Un test Catch2 décode chaque en-tête avec les données C++ qu'il
déclare et le compare au fichier source ; un test CMake vérifie les sommes de contrôle des entrées des bundles
(`bundles.json`) : une source modifiée sans reconstruction est détectée sans navigateur. La reconstruction effective
sources → bundle est prouvée dans la suite navigateur par `wf run` (`SelfBuildRefreshMatchesSources`), puis bundle →
en-tête par CMake. Le pilote C++ non interactif exécute les scripts d'assemblage et les specs (`wf test`) dans le navigateur
à partir d'une version d'amorçage épinglée (#321). Les paquets de test sont téléchargés par CMake au configure, listés par nom,
version et intégrité SHA-512 (`test/web/registry-packages.json`), puis servis hors ligne par un registre que `wf run
test-registry` construit ; aucun test ne dépend du réseau public ensuite. Un garde-fou global (`NoNodeDependency`) audite
tous les fichiers suivis, ses exceptions étant listées et motivées dans `test/cmake/NodeGuardExceptions.cmake`. Aucune
source tierce n'est vendorisée et `third_party/` a disparu (#319) ; les bibliothèques sont des paquets npm verrouillés, jamais
committés.

### Sources vendorisées et artefacts générés

Sont **interdites** les sources tierces copiées dans le dépôt. Sont **permises** les dépendances
incorporées dans un artefact généré autorisé, désigné nominativement. Exception nommée : le bundle
bootstrap (`bootstrap.bundle.mjs` → `DeveloperBootstrapData.hpp`), qui incorpore notamment le
décompresseur. Il est accompagné de son lock, de ses notices de licence et d'une preuve de
reconstruction (#315). Toute autre exception exige une révision de cette ADR.

### Auto-hébergement de l'environnement (#315)

Le dépôt est lui-même un projet `wf` : `js/developer/webfront.project.json` (dépendances de construction exactes, scripts, permissions) et
`js/developer/webfront-packages.lock.json` (intégrités SHA-512) sont dans `js/developer/`, désignés par `--manifest-dir`
(#354) ; la racine reste une liste fermée. Les scripts `js/developer/scripts/` (`build.mjs`, `site.mjs`, `pin.mjs`) s'exécutent dans l'environnement
d'amorçage épinglé (`cmake/WebFrontBootstrap.cmake`) par le pilote `webfront-wf`, avec l'esbuild-wasm verrouillé dans le
Worker du script : ils lisent les sources du projet et les paquets verrouillés, n'écrivent que sous `permissions.publish`
(répertoires `build-*` et sorties versionnées nommées) et n'ont ni shell ni API Node. Ils produisent `environment.tgz`,
`environment.json`, `provision.json`, `bootstrap.bundle.mjs` et `DeveloperReleaseData.hpp` **octet pour octet** comme la
construction qui a précédé (la 0.6.1 publiée), deux constructions successives étant identiques ; les CTest `SelfBuild*` l'exigent
à chaque exécution et le job `wf-package` de `DeveloperDistribution.yml` compare aussi les deux constructions. La preuve de reconstruction du bundle bootstrap exigée plus haut est
`SelfBuildRefreshMatchesSources` : `wf run build --refresh-bootstrap` régénère `bootstrap.bundle.mjs`, `packages.bundle.mjs`,
leurs sommes de contrôle, identiques aux fichiers versionnés, et `EmbedAsset.cmake` en rend les deux en-têtes
`DeveloperIndexHtmlData.hpp` et `DeveloperBootstrapData.hpp`, identiques eux aussi. Les opérations d'hôte de la
publication (Pages, prerelease, dépôt `gh-pages`) restent des scripts shell et des étapes de workflow : elles n'entrent pas
dans l'API `context`. L'environnement 0.6.1 construit ainsi par la 0.6.0 corrige aussi `runNonInteractive`, qui perdait la
sortie d'une commande avant sa fin ; la 0.6.0 reste immuable.

### Distribution

`NativeAppWithAssets<Provider>` s'exécute sans racine projet, cache, réseau, callbacks
développeur ni outillage. L'outillage de construction n'est jamais embarqué dans l'application.

## Révision des décisions existantes

- **[ADR-009](ADR-009-modules-et-assets-reproductibles.md)** (v2.1) : les assets embarqués sont
  limités à la liste normative ; `BabelFS`, `ReactFS`, `JasmineFS` et leurs sources vendorisées
  sont retirés (#312, #319). Les tests migrent vers `wf test` (#311).
- **[ADR-010](ADR-010-composants-et-outils-optionnels.md)** (v1.7) : `Terminal.hpp`,
  `ToolManager.hpp`, `ToolStore`, `PackageFS`, `TerminalFS`, `ToolManagerFS` et xterm embarqué
  sont retirés (#313, effectué). Le terminal et `wf` de l'environnement téléchargé les remplacent ; les
  abonnements UI multiples et l'injection du navigateur appelant restent des contrats du cœur.
- **[ADR-013](ADR-013-bootstrap-developpeur-a-la-demande.md),
  [ADR-014](ADR-014-construction-javascript-dans-le-browser.md),
  [ADR-015](ADR-015-ressources-generees-par-application.md)** sont **Acceptées** (#316, 2026-10-24) :
  parcours complet validé sous CEF, WebKitGTK, WebView2 et WKWebView, échantillon de paquets npm,
  restauration hors ligne et release publiée et vérifiée.

### Trajectoire de dépréciation

1. Les composants historiques restent fonctionnels et testés tant que l'équivalent de
   l'environnement n'est pas couvert par `wf test` et le registre local.
2. Les remplaçants sont livrés et documentés (#305 à #310), puis les tests migrent (#311).
3. Retrait des providers de test, puis des composants (#312, #313), documenté dans le CHANGELOG
   comme changement incompatible ; le garde-fou (#314) interdit leur retour.

## Alternatives et conséquences

- Conserver deux systèmes (composants C++ et environnement) : écarté, double maintenance de
  deux terminaux, deux `wf` et deux modèles d'installation.
- Garder Node.js pour la régénération et les tests : écarté, contraire à l'autonomie visée.
- Régénérer les en-têtes du cœur par un script `wf` (`wf run embed-core`, essayé dans #315) : écarté par #318, car il exige un
  navigateur pour vérifier la fraîcheur ; un seul rendu, en CMake, sert la génération et la vérification.
- Garder les bibliothèques vendorisées pour les tests : écarté, le poids du dépôt et la
  provenance floue ne se justifient pas face à des paquets verrouillés par intégrité.
- **Coût accepté :** le premier lancement du mode développeur nécessite l'accès à la release ou
  un provisioning local ; les consommateurs des providers historiques doivent migrer ; le
  bootstrap devient un point critique à qualifier sur chaque moteur.
- **Risque :** l'environnement téléchargé porte l'autorité du bridge ; l'intégrité SHA-512
  protège l'origine des octets, pas leur comportement.

## Règles et vérification

- Ne pas ajouter de ressource web à `include/webfront/` hors liste normative ; ne pas ajouter
  de dépendance Node.js ou npm à configure, build, CTest ou CI.
- Ne pas présenter les permissions du manifeste comme une isolation.
- Conserver les contrats existants : bridge asynchrone, Promises qui rejettent, bornes du
  protocole ([ADR-003](ADR-003-protocole-binaire-et-bornes.md)), intégrité SHA-512, publication
  transactionnelle via `developer::Store`.
- Vérification : test garde-fou de la liste normative et du budget (#314), régénération des
  en-têtes sans Node (#318), absence de `third_party/` (#319), qualification multiplateforme
  (#316).

## Références

- Épique #303 ; [ADR-008](ADR-008-contenu-de-confiance-et-origines.md),
  [ADR-009](ADR-009-modules-et-assets-reproductibles.md),
  [ADR-010](ADR-010-composants-et-outils-optionnels.md),
  [ADR-013](ADR-013-bootstrap-developpeur-a-la-demande.md),
  [ADR-014](ADR-014-construction-javascript-dans-le-browser.md),
  [ADR-015](ADR-015-ressources-generees-par-application.md),
  [Environnement développeur](../DEVELOPER_ENVIRONMENT.md).
