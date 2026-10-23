# ADR-016 : L'environnement bootstrap comme socle de WebFront

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-22 | Acceptée | Ambroise Leclerc | Décision initiale : environnement bootstrap comme architecture de base, liste normative du cœur, scripts autorisés, indépendance vis-à-vis de Node.js. | [404fed2](https://github.com/ambroise-leclerc/WebFront/commit/404fed2c2145d4faba8efee035e10522ff87a91f) |

## Statut et date

**Acceptée le 2026-10-22** (épique #303, issue #304). Cette décision fixe la direction
architecturale et les règles que les issues suivantes de l'épique doivent respecter. Elle ne
déclare **pas** les mécanismes déjà proposés (ADR-013, ADR-014, ADR-015) comme acceptés : leur
passage à « Acceptée » attend les preuves multiplateformes de #316. Aucun code n'est modifié par
cette décision ; les retraits qu'elle annonce sont réalisés par #311 à #319.

## Contexte

L'étape développeur du jeu de la vie (`examples/life/LifeDeveloper.cpp`) montre le modèle visé :
`NativeApp::bootstrap("developer")` sert une petite page embarquée, télécharge une archive
d'environnement vérifiée SHA-512, monte un terminal dans le navigateur, installe des paquets npm
sans Node (`wf install`), compile TS/TSX dans un Worker (`wf preview`) et génère un provider
propre à l'application (`wf embed`), compilé ensuite dans un exécutable de distribution.

Le dépôt livre encore en parallèle environ 2,5 Mo d'en-têtes pré-générés sous
`include/webfront/system/` (dont `BabelData.hpp`, 1,5 Mo), environ 3,4 Mo de sources vendorisées
dans `third_party/`, des providers (`BabelFS`, `ReactFS`, `JasmineFS`) qui ne servent qu'aux tests
et un second système de terminal et d'outils (`Terminal.hpp`, `ToolManager.hpp`, `ToolStore`,
`PackageFS`, `TerminalFS`, `ToolManagerFS`) avec un autre `wf`. Régénérer ou tester exige Node.js.

## Décision

L'environnement bootstrap devient l'architecture de base : un outil de développement complet
« à la node.js » (manifeste de projet, install, run, test, build, embed) fourni par une archive
téléchargée et vérifiée, tandis que le code source de WebFront redevient minimaliste.

### Liste normative des ressources du cœur

`include/webfront/` ne contient, comme ressources web embarquées, que :

| Ressource | Rôle |
| --- | --- |
| `WebFrontJsData` | Bridge `WebFront.js` |
| `FallbackIndexHtmlData` | Page de repli |
| `FallbackModuleData` | Module de repli |
| favicon (`WebFrontIco`, servi par `IndexFS`) | Icône par défaut |
| `DeveloperIndexHtmlData` | Page du bootstrap développeur |
| `DeveloperBootstrapData` | Chargeur bootstrap (`bootstrap.bundle.mjs`) |
| `DeveloperReleaseData` | Release épinglée (URL et SHA-512) |

Toute autre ressource web (bibliothèques, runner de test, pages de test, terminal, providers
applicatifs) est produite par un script autorisé et livrée par l'archive d'environnement ou par un
paquet npm verrouillé par intégrité. Cette liste est normative : un test garde-fou (#314) la
compare aux fichiers de ressources de `include/webfront/system/` et impose un budget de taille ;
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
vérifiés sans Node (#318) : le pilote C++ non interactif exécute les scripts d'assemblage dans le
navigateur à partir d'une version d'amorçage épinglée (#321), puis compare les octets produits aux
en-têtes versionnés. Les paquets de test viennent d'un registre local alimenté par CMake au
configure ; aucun test ne dépend du réseau public ensuite. Aucune source tierce n'est vendorisée
et `third_party/` disparaît (#319) ; les bibliothèques sont des paquets npm verrouillés, jamais
committés.

### Sources vendorisées et artefacts générés

Sont **interdites** les sources tierces copiées dans le dépôt. Sont **permises** les dépendances
incorporées dans un artefact généré autorisé, désigné nominativement. Exception nommée : le bundle
bootstrap (`bootstrap.bundle.mjs` → `DeveloperBootstrapData.hpp`), qui incorpore notamment le
décompresseur. Il est accompagné de son lock, de ses notices de licence et d'une preuve de
reconstruction (#315). Toute autre exception exige une révision de cette ADR.

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
  [ADR-015](ADR-015-ressources-generees-par-application.md)** restent **Proposées**. Preuves
  manquantes avant acceptation : validation CEF, WebKitGTK, WebView2 et WKWebView du parcours
  complet, compatibilité d'un échantillon de paquets npm, restauration hors ligne, publication
  vérifiée d'une release (#316).

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
