# ADR-009 : Servir les modules localement et régénérer les ressources à l’identique

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2023-03-16 | Acceptée | Ambroise Leclerc | Décision initiale : ressources web locales embarquées avec IndexFS. | [dd463b4](https://github.com/ambroise-leclerc/WebFront/commit/dd463b44407f3dca13f2f55d3f10d904e2653dc4) |
| 1.1 | 2023-06-09 | Acceptée | Ambroise Leclerc | Distribution locale de React via un filesystem dédié. | [683a57c](https://github.com/ambroise-leclerc/WebFront/commit/683a57caf162f9abf04d93b1e08e58fb23f3d013) |
| 1.2 | 2023-06-11 | Acceptée | Ambroise Leclerc | Distribution locale de Babel pour les exemples JSX. | [450d03c](https://github.com/ambroise-leclerc/WebFront/commit/450d03c93c7dff30a0e66d991879c758c0bf02af) |
| 1.3 | 2026-07-16 | Acceptée | Ambroise Leclerc | Démonstration de référence en modules ES natifs. | [f3c8aa7](https://github.com/ambroise-leclerc/WebFront/commit/f3c8aa75c5766d4c504a04fb582de874ae4bcdd7) |
| 1.4 | 2026-07-21 | Acceptée | Ambroise Leclerc | Pont embarqué sans compression dépendante de la plateforme. | [6f10761](https://github.com/ambroise-leclerc/WebFront/commit/6f107614b021de24a27707566223d227a013d80d) |
| 1.5 | 2026-10-16 | Acceptée | Ambroise Leclerc | Contrôle des fins de ligne LF des sources et en-têtes embarqués. | [520ca23](https://github.com/ambroise-leclerc/WebFront/commit/520ca23fa694519d9f428ed3661e64dcfdf906a2) |
| 1.6 | 2026-10-19 | Acceptée | Ambroise Leclerc | Négociation gzip/identity, décodage validé et budget de sortie. | [4be7358](https://github.com/ambroise-leclerc/WebFront/commit/4be73581ee6b9f569cb6e50aced73e27562aa38c) |
| 2.0 | 2026-10-19 | Acceptée | Ambroise Leclerc | Génération gzip reproductible avec compresseur intégré ; distributions actualisées et runtime React ESM local. | [b031ab5](https://github.com/ambroise-leclerc/WebFront/commit/b031ab58ea9cc32290d701094c1333bab455e67f) |
| 2.1 | 2026-10-22 | Acceptée | Ambroise Leclerc | Assets embarqués limités à la liste normative de l'ADR-016 ; trajectoire de retrait de BabelFS, ReactFS, JasmineFS et des sources vendorisées. | [404fed2](https://github.com/ambroise-leclerc/WebFront/commit/404fed2c2145d4faba8efee035e10522ff87a91f) |
| 2.2 | 2026-10-24 | Acceptée | Ambroise Leclerc | Les retraits annoncés sont réalisés (#312, #313, #318) : les en-têtes du cœur sont rendus et vérifiés par CMake seul (`js/core/EmbedAsset.cmake`), sans compression ni Node.js ; la compression gzip reproductible est celle de `wf embed`. | — |

## État et chronologie

**Acceptée.** Décision initiale : **2023-03-16** (version 1.0).
Dernière révision : **2026-10-24** (version 2.2).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

La démonstration minimale repose sur les modules ES natifs. React et Babel restent
facultatifs : qu’une application C++ puisse démarrer sans installer Node ou npm ni joindre
un CDN. Les sources lisibles et leurs en-têtes embarqués doivent coïncider.

## Décision

Que les modules ES natifs demeurent la démonstration de référence et que les fournisseurs
locaux servent les ressources. React, Babel, Jasmine et xterm sont distribués avec des versions
et licences traçables ; React partage un environnement ESM local pour DOM, hooks et JSX.

Les sources lisibles résident dans `js/` ; les en-têtes générés sont versionnés.
La maintenance utilise les versions exactes et le lockfile. Les distributions
JS/CSS volumineuses sont compressées avec le `fflate` verrouillé, paramètres fixes dont `mtime=0`.
Les petits scripts du bridge et des composants restent en identity. Les contrôles CTest
régénèrent et comparent hors ligne après configuration des dépendances.

HTTP négocie gzip et identity. Un client acceptant gzip reçoit les octets stockés ; sinon,
si identity est acceptable, décoder et servir la taille décodée sans `Content-Encoding`.
Respecter `q=0`, `Vary`, MIME et `Content-Length`. Le décodage valide le flux et applique un
budget de sortie par défaut de 4 MiB, y compris HEAD. Une préférence incompatible donne 406 ;
un échec de décodage ou dépassement du budget donne 500.

## Options écartées et motifs

- CDN ou bundler obligatoire chez le consommateur : incompatible avec le parcours autonome.
- Modifier directement les tableaux générés : non reproductible et perdu à la régénération.
- Compresseur gzip dépendant du système : remplacé par un compresseur vendored déterministe.
- Revenir à Brotli : demanderait une dépendance de décodage distincte, actuellement différée.

## Conséquences

- **Bénéfice :** exécution locale et contrôle de fraîcheur des ressources réellement distribuées.
- **Coût accepté :** distributions et en-têtes volumineux versionnés, outils de maintenance dédiés.
- **Risque :** gzip→identity décode synchroniquement sur le thread I/O, à chaque requête, sans
  cache. Le budget borne la sortie, pas le temps ; cache et déplacement du travail restent futurs.

### Révision de l'ADR-016

Les assets embarqués dans `include/webfront/` se limitent à la liste normative de
[ADR-016](ADR-016-bootstrap-comme-socle.md). `BabelFS`, `ReactFS`, `JasmineFS`, leurs en-têtes
générés ont été retirés (#312) et `third_party/` l'a été par #319 ; les bibliothèques sont des
paquets npm verrouillés fournis par l'environnement. Les en-têtes de la liste normative sont rendus par
`js/core/EmbedAsset.cmake` (#318), sans compression : les règles de génération gzip ci-dessous décrivent l'historique de la
décision et ne s'appliquent plus qu'aux providers produits par `wf embed`.

## Prescriptions d’implémentation et de génération de code

- Que les sources soient modifiées avant régénération ; préserver les fins de ligne LF et les
  licences d’origine.
- Régénérer les en-têtes du cœur avec la cible `webfront-embedded-assets` ; suivre `EMBEDDED_ASSETS.md`
  pour les bundles de l'environnement et leurs sommes de contrôle.
- Que le lancement du programme C++ n’exige ni npm ni connexion réseau.
- Pour un nouvel asset, tester route, MIME, encodage, taille, lectures partielles et fraîcheur.
- Pour modifier la négociation, tester gzip, identity, exclusions et flux corrompus ; ne pas
  affirmer que HEAD ou les ressources décodées sont déjà mis en cache.

## Sources et vérification

- [Maintenance des assets](../EMBEDDED_ASSETS.md), [contrat HTTP](../HTTP_ASSETS.md),
  [génération](../../js/core/WebFrontAssets.cmake).
- Tests : `Asset.*Fresh`, `EmbeddedGzipContents`, [HTTPEncodingTests](../../test/unit/HTTPEncodingTests.cpp),
  [React/TSX et application Life dans le navigateur](../../test/web/developer-specs.mjs).
- Voir [ADR-002](ADR-002-systemes-de-fichiers-composables.md) et [ADR-016](ADR-016-bootstrap-comme-socle.md).
