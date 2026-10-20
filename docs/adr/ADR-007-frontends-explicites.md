# ADR-007 : Choisir explicitement le frontend et partager son contrat de cycle de vie

## Versions

| Version | Date | État | Auteur | Évolution | Commit |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-05-31 | Acceptée | Ambroise Leclerc | Création initiale : abstraction générique du frontend et action de fin de session. | [4eac25f](https://github.com/ambroise-leclerc/WebFront/commit/4eac25f19ec5bc3bad1ec8cf86dab8e03d2d3b82) |
| 1.1 | 2026-07-22 | Acceptée | Ambroise Leclerc | Sélection explicite ; le navigateur système reste le défaut, indépendamment de CEF. | [9a7c566](https://github.com/ambroise-leclerc/WebFront/commit/9a7c56677de28862cae8f8689e3e27e0da5d1163) |
| 1.2 | 2026-10-16 | Acceptée | Ambroise Leclerc | Frontend WebView2 optionnel sous Windows. | [53058ad](https://github.com/ambroise-leclerc/WebFront/commit/53058ade9ae10f0eedbc62eb77a1b3b157b3b711) |
| 1.3 | 2026-10-17 | Acceptée | Ambroise Leclerc | Construction d’URL, sélection et contrat de frontend partagés. | [9013a75](https://github.com/ambroise-leclerc/WebFront/commit/9013a75d9e5c164795227789e7be196bfd48c8ea) |
| 1.4 | 2026-10-17 | Acceptée | Ambroise Leclerc | Configuration CMake centralisée et câblage commun des tests navigateur. | [121899e](https://github.com/ambroise-leclerc/WebFront/commit/121899ef12276e10e1c05d12e375ee6c8b7ea113) |
| 1.5 | 2026-10-18 | Acceptée | Ambroise Leclerc | Scénarios de cycle de vie communs aux frontends embarqués. | [a3ba113](https://github.com/ambroise-leclerc/WebFront/commit/a3ba1134b1f3ad03dc01f24fd59e2b47f47c3466) |
| 1.6 | 2026-10-18 | Acceptée | Ambroise Leclerc | Contrat de frontend complété et couverture des échecs par mocks. | [99c3c57](https://github.com/ambroise-leclerc/WebFront/commit/99c3c576e22cc601f3e3a8f31436c30807b36ca7) |
| 1.7 | 2026-10-19 | Acceptée | Ambroise Leclerc | Frontend WKWebView optionnel sous macOS. | [93551a8](https://github.com/ambroise-leclerc/WebFront/commit/93551a88c31cbf4748b42013a066cf265a1c9034) |
| 1.8 | 2026-10-19 | Acceptée | Ambroise Leclerc | Frontend WebKitGTK optionnel sous Linux. | [e20203e](https://github.com/ambroise-leclerc/WebFront/commit/e20203ea88d79bd141a1b64d6fe0f987143d73ff) |
| 1.9 | 2026-10-19 | Acceptée | Ambroise Leclerc | Alias NativeWebViewFrontend avec sélection explicite par plateforme. | [7be859f](https://github.com/ambroise-leclerc/WebFront/commit/7be859f18957e770cc0db8382e7d9641689ba403) |

## Statut et date

**Acceptée.** Création initiale : **2026-05-31** (version 1.0).
Dernière évolution : **2026-10-19** (version 1.9).
Le corps de l’ADR décrit la décision dans sa dernière version ; les versions antérieures
et leurs changements sont identifiés dans le tableau ci-dessus.

## Contexte

L’affichage peut être un navigateur système ou une fenêtre embarquée. Installer un moteur
et sélectionner son utilisation sont deux choix distincts. Les moteurs ont des contraintes
de thread, d’initialisation, de fermeture et de déploiement différentes.

## Décision

`frontend::FrontendType` décrit l’adaptateur ; `BasicWF` reçoit le frontend comme paramètre.
`webfront::WebFront` utilise toujours le navigateur système. Les options d’intégration rendent
CEF, WebView2, WKWebView ou WebKitGTK disponibles sans changer ce choix par défaut.

`WebFrontWithFrontend<T>` sélectionne un adaptateur. L’alias optionnel `NativeWebViewFrontend`
désigne WebView2 sous Windows, WKWebView sous macOS et WebKitGTK sous Linux. Il exige l’option
du moteur correspondant ; il ne garantit pas un rendu identique entre moteurs.

Initialiser avant création du listener ; une initialisation qui lève peut être retentée.
Pour les moteurs embarqués, construire et ouvrir sur le thread principal, selon les exigences
du toolkit (notamment COM compatible STA pour WebView2). `open()` s’exécute chez l’appelant,
avec le serveur sur un thread distinct. `Action::closeServerAfterOpen` entraîne l’arrêt et la
jonction après fermeture ; `keepServerRunning` laisse le serveur actif au retour. Si l’ouverture
lève, `show()`/`openAndRun()` arrêtent et joignent avant de relancer l’exception d’origine.

Partager construction d’URL, choix CMake et scénarios de cycle de vie. Un frontend indisponible
reste déclaré et échoue avec un diagnostic indiquant l’option nécessaire.

## Alternatives et arbitrage

- Choisir automatiquement CEF quand il est compilé : ancienne ambiguïté corrigée en faveur
  d’une sélection explicite.
- Masquer toutes les différences de moteurs : impossible pour runtime, toolkit, rendu et
  politiques de navigation ; documenter les capacités par adaptateur.
- Dupliquer les règles d’URL et les tests dans chaque moteur : crée des dérives, remplacées
  par les utilitaires et scénarios partagés.

## Conséquences

- **Bénéfice :** démarrage léger et choix d’affichage indépendant du serveur et du bridge.
- **Coût accepté :** matrice de moteurs, runtimes et plateformes à maintenir.
- **Risque :** blocage ou fuite lors d’une ouverture ratée ; contrat d’arrêt commun, mocks
  de défaillance et tests de cycle de vie avec limites de temps.

## Règles d’implémentation et de génération de code

- Respecter `FrontendType`, les actions et les surcharges d’ouverture ; préférer la surcharge
  recevant `ContentPolicy` lorsqu’un adaptateur doit l’appliquer.
- Réutiliser `detail::applicationUrl()` pour l’adresse, le port et la référence de page.
- Ne pas ajouter un téléchargement de moteur ou un runtime obligatoire au chemin par défaut.
- Ajouter les scénarios partagés de fermeture/échec lors d’un nouveau frontend et documenter
  les contraintes de plateforme sans annoncer une vérification non effectuée.

## Historique Git

- **2026-05-31** — [4eac25f](https://github.com/ambroise-leclerc/WebFront/commit/4eac25f19ec5bc3bad1ec8cf86dab8e03d2d3b82) : abstraction générique des frontends.
- **2026-07-22** — [9a7c566](https://github.com/ambroise-leclerc/WebFront/commit/9a7c56677de28862cae8f8689e3e27e0da5d1163) : sélection du navigateur rendue explicite.
- **2026-10-16** — [53058ad](https://github.com/ambroise-leclerc/WebFront/commit/53058ade9ae10f0eedbc62eb77a1b3b157b3b711) : WebView2 optionnel sous Windows.
- **2026-10-17** — [9013a75](https://github.com/ambroise-leclerc/WebFront/commit/9013a75d9e5c164795227789e7be196bfd48c8ea) : contrat, URL et sélection partagés.
- **2026-10-17** — [121899e](https://github.com/ambroise-leclerc/WebFront/commit/121899ef12276e10e1c05d12e375ee6c8b7ea113) : configuration CMake centralisée.
- **2026-10-18** — [a3ba113](https://github.com/ambroise-leclerc/WebFront/commit/a3ba1134b1f3ad03dc01f24fd59e2b47f47c3466) : couverture de cycle de vie partagée.
- **2026-10-18** — [99c3c57](https://github.com/ambroise-leclerc/WebFront/commit/99c3c576e22cc601f3e3a8f31436c30807b36ca7) : contrat et mocks de cycle de vie complétés.
- **2026-10-19** — [93551a8](https://github.com/ambroise-leclerc/WebFront/commit/93551a88c31cbf4748b42013a066cf265a1c9034) : WKWebView optionnel.
- **2026-10-19** — [e20203e](https://github.com/ambroise-leclerc/WebFront/commit/e20203ea88d79bd141a1b64d6fe0f987143d73ff) : WebKitGTK optionnel.
- **2026-10-19** — [7be859f](https://github.com/ambroise-leclerc/WebFront/commit/7be859f18957e770cc0db8382e7d9641689ba403) : alias NativeWebView optionnel.

## Références et vérification

- [Contrat](../FRONTENDS.md), [concept](../../include/frontend/Frontend.hpp),
  [configuration](../../cmake/Frontends.cmake).
- Tests : [FrontendContractTests](../../test/FrontendContractTests.cpp),
  [frontend scenarios](../../test/frontends/CMakeLists.txt), [webtest](../../webtest/CMakeLists.txt).
- Voir [ADR-008](ADR-008-contenu-de-confiance-et-origines.md).
