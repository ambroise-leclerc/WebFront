# ADR-007 : Choisir explicitement l’interface d’affichage et son cycle de vie

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-05-31 | Acceptée | Ambroise Leclerc | Décision initiale : abstraction générique du frontend et action de fin de session. | [4eac25f](https://github.com/ambroise-leclerc/WebFront/commit/4eac25f19ec5bc3bad1ec8cf86dab8e03d2d3b82) |
| 1.1 | 2026-07-22 | Acceptée | Ambroise Leclerc | Sélection explicite ; le navigateur système reste le défaut, indépendamment de CEF. | [9a7c566](https://github.com/ambroise-leclerc/WebFront/commit/9a7c56677de28862cae8f8689e3e27e0da5d1163) |
| 1.2 | 2026-10-16 | Acceptée | Ambroise Leclerc | Interface WebView2 facultative sous Windows. | [53058ad](https://github.com/ambroise-leclerc/WebFront/commit/53058ade9ae10f0eedbc62eb77a1b3b157b3b711) |
| 1.3 | 2026-10-17 | Acceptée | Ambroise Leclerc | Construction d’URL, sélection et contrat de frontend partagés. | [9013a75](https://github.com/ambroise-leclerc/WebFront/commit/9013a75d9e5c164795227789e7be196bfd48c8ea) |
| 1.4 | 2026-10-17 | Acceptée | Ambroise Leclerc | Configuration CMake centralisée et câblage commun des tests navigateur. | [121899e](https://github.com/ambroise-leclerc/WebFront/commit/121899ef12276e10e1c05d12e375ee6c8b7ea113) |
| 1.5 | 2026-10-18 | Acceptée | Ambroise Leclerc | Scénarios de cycle de vie communs aux frontends embarqués. | [a3ba113](https://github.com/ambroise-leclerc/WebFront/commit/a3ba1134b1f3ad03dc01f24fd59e2b47f47c3466) |
| 1.6 | 2026-10-18 | Acceptée | Ambroise Leclerc | Contrat d’interface d’affichage complété et couverture des échecs par mocks. | [99c3c57](https://github.com/ambroise-leclerc/WebFront/commit/99c3c576e22cc601f3e3a8f31436c30807b36ca7) |
| 1.7 | 2026-10-19 | Acceptée | Ambroise Leclerc | Interface WKWebView facultative sous macOS. | [93551a8](https://github.com/ambroise-leclerc/WebFront/commit/93551a88c31cbf4748b42013a066cf265a1c9034) |
| 1.8 | 2026-10-19 | Acceptée | Ambroise Leclerc | Interface WebKitGTK facultative sous Linux. | [e20203e](https://github.com/ambroise-leclerc/WebFront/commit/e20203ea88d79bd141a1b64d6fe0f987143d73ff) |
| 1.9 | 2026-10-19 | Acceptée | Ambroise Leclerc | Alias NativeWebViewFrontend avec sélection explicite par plateforme. | [7be859f](https://github.com/ambroise-leclerc/WebFront/commit/7be859f18957e770cc0db8382e7d9641689ba403) |
| 1.10 | 2026-10-20 | Acceptée | Ambroise Leclerc | WKWebView respecte les bundles agents LSUIElement ; les exécutables de test ne créent plus d’icônes Dock. | À renseigner après intégration. |

## État et chronologie

**Acceptée.** Décision initiale : **2026-05-31** (version 1.0).
Dernière révision : **2026-10-20** (version 1.10).
Le présent texte expose la décision en vigueur ; le tableau retrace ses états antérieurs.

## Contexte

L’interface peut s’ouvrir dans le navigateur système ou une fenêtre intégrée. Qu’un moteur
soit disponible n’implique pas qu’il soit choisi : chacun impose ses contraintes de thread,
d’initialisation, de fermeture et de déploiement.

## Décision

Le concept `frontend::FrontendType` définit l’adaptateur ; `BasicWF` reçoit le frontend en
paramètre.
`webfront::WebFront` utilise toujours le navigateur système. Les options d’intégration rendent
CEF, WebView2, WKWebView ou WebKitGTK disponibles sans changer ce choix par défaut.

`WebFrontWithFrontend<T>` sélectionne un adaptateur. L’alias optionnel `NativeWebViewFrontend`
désigne WebView2 sous Windows, WKWebView sous macOS et WebKitGTK sous Linux. Il exige l’option
du moteur correspondant ; il ne garantit pas un rendu identique entre moteurs.

Que l’initialisation précède la mise en écoute ; si elle lève, une nouvelle tentative reste
possible.
Pour les moteurs embarqués, construire et ouvrir sur le thread principal, selon les exigences
du toolkit (notamment COM compatible STA pour WebView2). `open()` s’exécute chez l’appelant,
avec le serveur sur un thread distinct. `Action::closeServerAfterOpen` entraîne l’arrêt et la
jonction après fermeture ; `keepServerRunning` laisse le serveur actif au retour. Si l’ouverture
lève, `show()`/`openAndRun()` arrêtent et joignent avant de relancer l’exception d’origine.

Sous macOS, WKWebView respecte `LSUIElement=true` du bundle hôte : la session utilise
la politique d’activation Accessory et conserve une fenêtre sans icône Dock ni barre de
menus applicative. Un hôte ordinaire garde la politique Regular. La politique antérieure
est restaurée à la fermeture. Les tests WKWebView sont des bundles agents identifiés,
afin d’éviter l’accumulation d’icônes d’exécutables anonymes au fil de CTest.

Que la construction des URL, les choix CMake et les scénarios de cycle de vie soient partagés.
Un frontend indisponible reste déclaré et échoue avec un diagnostic indiquant l’option requise.

## Options écartées et motifs

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

## Prescriptions d’implémentation et de génération de code

- Que soient respectés `FrontendType`, les actions et les surcharges d’ouverture ; que celle
  recevant `ContentPolicy` prévale lorsqu’un adaptateur doit appliquer cette politique.
- Que `detail::applicationUrl()` détermine l’adresse, le port et la référence de page.
- Qu’aucun téléchargement de moteur ni environnement d’exécution obligatoire ne grève le
  parcours par défaut.
- Ajouter les scénarios partagés de fermeture/échec lors d’un nouveau frontend et documenter
  les contraintes de plateforme sans annoncer une vérification non effectuée.


## Sources et vérification

- [Contrat](../FRONTENDS.md), [concept](../../include/webfront/frontend/Frontend.hpp),
  [configuration](../../cmake/Frontends.cmake).
- Tests : [FrontendContractTests](../../test/unit/FrontendContractTests.cpp),
  [frontend scenarios](../../test/cmake/frontends/CMakeLists.txt), [webtest](../../test/web/CMakeLists.txt).
- Voir [ADR-008](ADR-008-contenu-de-confiance-et-origines.md).
