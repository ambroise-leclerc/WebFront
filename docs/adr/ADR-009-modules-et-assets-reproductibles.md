# ADR-009 : Servir localement les modules et régénérer les assets de façon reproductible

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2023-03-16 | Acceptée | Ambroise Leclerc | Création initiale : ressources web locales embarquées avec IndexFS. | [dd463b4](https://github.com/ambroise-leclerc/WebFront/commit/dd463b44407f3dca13f2f55d3f10d904e2653dc4) |
| 1.1 | 2023-06-09 | Acceptée | Ambroise Leclerc | Distribution locale de React via un filesystem dédié. | [683a57c](https://github.com/ambroise-leclerc/WebFront/commit/683a57caf162f9abf04d93b1e08e58fb23f3d013) |
| 1.2 | 2023-06-11 | Acceptée | Ambroise Leclerc | Distribution locale de Babel pour les exemples JSX. | [450d03c](https://github.com/ambroise-leclerc/WebFront/commit/450d03c93c7dff30a0e66d991879c758c0bf02af) |
| 1.3 | 2026-07-16 | Acceptée | Ambroise Leclerc | Démonstration de référence en modules ES natifs. | [f3c8aa7](https://github.com/ambroise-leclerc/WebFront/commit/f3c8aa75c5766d4c504a04fb582de874ae4bcdd7) |
| 1.4 | 2026-07-21 | Acceptée | Ambroise Leclerc | Bridge embarqué sans compression dépendante de la plateforme. | [6f10761](https://github.com/ambroise-leclerc/WebFront/commit/6f107614b021de24a27707566223d227a013d80d) |
| 1.5 | 2026-10-16 | Acceptée | Ambroise Leclerc | Contrôle des fins de ligne LF des sources et en-têtes embarqués. | [520ca23](https://github.com/ambroise-leclerc/WebFront/commit/520ca23fa694519d9f428ed3661e64dcfdf906a2) |
| 1.6 | 2026-10-19 | Acceptée | Ambroise Leclerc | Négociation gzip/identity, décodage validé et budget de sortie. | [4be7358](https://github.com/ambroise-leclerc/WebFront/commit/4be73581ee6b9f569cb6e50aced73e27562aa38c) |
| 2.0 | 2026-10-19 | Acceptée | Ambroise Leclerc | Génération gzip reproductible avec compresseur vendored ; distributions actualisées et runtime React ESM local. | [b031ab5](https://github.com/ambroise-leclerc/WebFront/commit/b031ab58ea9cc32290d701094c1333bab455e67f) |

## Statut et date

**Acceptée.** Création initiale : **2023-03-16** (version 1.0).
Dernière évolution : **2026-10-19** (version 2.0).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

Le minimum démontrable est une interface en modules ES natifs. React et Babel sont des
exemples optionnels ; le consommateur C++ ne doit pas installer Node/npm ou contacter un CDN
pour lancer son application. Les sources lisibles doivent rester synchronisées avec leurs
en-têtes embarqués.

## Décision

Conserver les modules ES natifs comme démonstration de base et servir les ressources depuis
les providers locaux. React, Babel, Jasmine et xterm sont distribués avec versions et licences
traçables ; React partage un runtime ESM local pour DOM, hooks et JSX.

Les sources lisibles vivent dans `src/` et `third_party/`, les en-têtes générés sont versionnés.
La maintenance utilise `tools/assets/`, les versions exactes et le lockfile. Les distributions
JS/CSS volumineuses sont compressées avec le fflate vendored, paramètres fixes dont `mtime=0`.
Les petits scripts du bridge et des composants restent en identity. Les contrôles CTest
régénèrent et comparent hors ligne après configuration des dépendances.

HTTP négocie gzip et identity. Un client acceptant gzip reçoit les octets stockés ; sinon,
si identity est acceptable, décoder et servir la taille décodée sans `Content-Encoding`.
Respecter `q=0`, `Vary`, MIME et `Content-Length`. Le décodage valide le flux et applique un
budget de sortie par défaut de 4 MiB, y compris HEAD. Une préférence incompatible donne 406 ;
un échec de décodage ou dépassement du budget donne 500.

## Alternatives et arbitrage

- CDN ou bundler obligatoire chez le consommateur : incompatible avec le parcours autonome.
- Modifier directement les tableaux générés : non reproductible et perdu à la régénération.
- Compresseur gzip dépendant du système : remplacé par un compresseur vendored déterministe.
- Revenir à Brotli : demanderait une dépendance de décodage distincte, actuellement différée.

## Conséquences

- **Bénéfice :** exécution locale et contrôle de fraîcheur des ressources réellement distribuées.
- **Coût accepté :** distributions et en-têtes volumineux versionnés, outils de maintenance dédiés.
- **Risque :** gzip→identity décode synchroniquement sur le thread I/O, à chaque requête, sans
  cache. Le budget borne la sortie, pas le temps ; cache et déplacement du travail restent futurs.

## Règles d’implémentation et de génération de code

- Éditer les sources puis régénérer ; garder LF et les licences amont.
- Rafraîchir xterm uniquement avec `cmake/UpdateXterm.cmake` ; suivre `EMBEDDED_ASSETS.md` pour
  les versions, lockfile et autres distributions.
- Ne pas ajouter une dépendance npm ou réseau au lancement du programme C++.
- Pour un nouvel asset, tester route, MIME, encodage, taille, lectures partielles et fraîcheur.
- Pour modifier la négociation, tester gzip, identity, exclusions et flux corrompus ; ne pas
  affirmer que HEAD ou les ressources décodées sont déjà mis en cache.

## Historique Git

- **2023-03-16** — [dd463b4](https://github.com/ambroise-leclerc/WebFront/commit/dd463b44407f3dca13f2f55d3f10d904e2653dc4) : Création initiale : ressources web locales embarquées avec IndexFS.
- **2023-06-09** — [683a57c](https://github.com/ambroise-leclerc/WebFront/commit/683a57caf162f9abf04d93b1e08e58fb23f3d013) : Distribution locale de React via un filesystem dédié.
- **2023-06-11** — [450d03c](https://github.com/ambroise-leclerc/WebFront/commit/450d03c93c7dff30a0e66d991879c758c0bf02af) : Distribution locale de Babel pour les exemples JSX.
- **2026-07-16** — [f3c8aa7](https://github.com/ambroise-leclerc/WebFront/commit/f3c8aa75c5766d4c504a04fb582de874ae4bcdd7) : démonstration native ES modules.
- **2026-07-21** — [6f10761](https://github.com/ambroise-leclerc/WebFront/commit/6f107614b021de24a27707566223d227a013d80d) : bridge embarqué sans compression dépendante de la plateforme.
- **2026-10-16** — [520ca23](https://github.com/ambroise-leclerc/WebFront/commit/520ca23fa694519d9f428ed3661e64dcfdf906a2) : contrôle des fins de ligne LF.
- **2026-10-19** — [4be7358](https://github.com/ambroise-leclerc/WebFront/commit/4be73581ee6b9f569cb6e50aced73e27562aa38c) : négociation gzip avec repli identity et limites de décodage.
- **2026-10-19** — [b031ab5](https://github.com/ambroise-leclerc/WebFront/commit/b031ab58ea9cc32290d701094c1333bab455e67f) : mise à jour des distributions et génération gzip reproductible.

## Références et vérification

- [Maintenance des assets](../EMBEDDED_ASSETS.md), [contrat HTTP](../HTTP_ASSETS.md),
  [génération](../../cmake/WebFrontAssets.cmake).
- Tests : `Asset.*Fresh`, `EmbeddedGzipContents`, [HTTPEncodingTests](../../test/HTTPEncodingTests.cpp),
  [React dans le navigateur](../../webtest/js/react-demo-specs.mjs).
- Voir [ADR-002](ADR-002-systemes-de-fichiers-composables.md).
