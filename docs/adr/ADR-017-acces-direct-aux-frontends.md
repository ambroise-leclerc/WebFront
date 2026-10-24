# ADR-017 : Accéder aux frontends par leur API directe sans détrôner le chemin HTML

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-24 | Proposée | Ambroise Leclerc | Décision initiale (#363) : transport direct facultatif par adaptateur, format de l'ADR-003 inchangé, HTML normatif ; premières mesures WebKitGTK et correction de deux coûts du chemin chaud du bridge. | À renseigner après intégration. |

## État et chronologie

**Proposée** le **2026-10-24** (issue #363). Cette décision prépare l'ajout de nouveaux frontends, notamment Android,
iOS et d'autres plateformes mobiles. Elle ne modifie ni l'API publique ni le format binaire : le seul code livré est
un benchmark et la correction de deux coûts qu'il a révélés dans le chemin HTML. Elle passera à « Acceptée » quand
les critères de la section dédiée seront démontrés.

## Contexte

Aujourd'hui, tous les frontends suivent le même chemin : le serveur embarqué sert la page par HTTP sur la boucle
locale, puis `WebFront.js` ouvre un WebSocket vers ce même serveur et y échange les messages binaires de
l'[ADR-003](ADR-003-protocole-binaire-et-bornes.md). Les adaptateurs de l'[ADR-007](ADR-007-frontends-explicites.md)
(CEF, WebView2, WKWebView, WebKitGTK) n'ouvrent qu'une fenêtre sur une URL `http://127.0.0.1:<port>/`.

Ce chemin HTML est le cœur de WebFront et doit le rester :

- c'est le seul qui fonctionne avec un navigateur quelconque, local ou distant, sans code propre au moteur ;
- l'évolution visée est de supporter des micro-contrôleurs très légers qui servent leur interface à un navigateur
  distant (téléphone, poste) : aucun frontend local n'y est envisageable, seul le HTML servi par HTTP/WebSocket existe ;
- le navigateur système, défaut de `webfront::WebFront`, n'offre aucune autre voie.

Les moteurs embarqués offrent pourtant tous une API directe, qui ne passe ni par un socket ni par HTTP :

| Moteur | Ressources | JavaScript → natif (avec réponse) | Natif → JavaScript |
| --- | --- | --- | --- |
| WebKitGTK 6.0 | `webkit_web_context_register_uri_scheme` | message handler avec réponse (2.40+) | `webkit_web_view_evaluate_javascript` |
| WKWebView (macOS, iOS) | `WKURLSchemeHandler` | `WKScriptMessageHandlerWithReply` (macOS 11, iOS 14) | `evaluateJavaScript`, `callAsyncJavaScript` |
| WebView2 | `SetVirtualHostNameToFolderMapping`, `WebResourceRequested` | `chrome.webview.postMessage`, `WebMessageReceived` | `PostWebMessageAsJson`/`AsString`, `ExecuteScriptAsync` |
| CEF | `CefSchemeHandlerFactory` | `CefMessageRouter`, messages de processus | `ExecuteJavaScript`, messages de processus |
| Android WebView | `WebViewAssetLoader`, `shouldInterceptRequest` | `WebViewCompat.addWebMessageListener` | `postWebMessage`, `evaluateJavascript` |

Sur mobile, un serveur local n'est pas aisé :

- Android refuse par défaut le trafic HTTP en clair aux applications qui ciblent l'API 28 ou plus ; il faut une `network_security_config` qui
  l'autorise pour la boucle locale. iOS impose une exception App Transport Security (`NSAllowsLocalNetworking`).
- Le système suspend l'application en arrière-plan : les sockets d'écoute et les connexions ne survivent pas
  forcément au retour au premier plan, et le cycle de vie du serveur se distingue alors de celui de la vue.
- Un port de la boucle locale est accessible à toute autre application du même appareil. Le contrôle d'origine de
  l'[ADR-008](ADR-008-contenu-de-confiance-et-origines.md) filtre les navigateurs, pas un processus natif qui forge
  l'en-tête `Origin` : sur un appareil partagé entre applications, un canal propre à la vue réduit cette exposition.

## Mesures (WebKitGTK, 2026-10-24)

Le benchmark `test/web/WebKitGTKTransportBenchmark.cpp` (CTest `WebKitGTKTransportBenchmark`, labels
`web-integration` et `performance`) ouvre une seule fenêtre WebKitGTK servie par WebFront et exécute les mêmes
échos de chaînes par les deux chemins (`test/web/transport-benchmark.html`) :

- **HTML** : `webFront.cppFunction("echo")` pour JavaScript → C++, `ui.jsFunctionSender<std::string>` pour
  C++ → JavaScript, chaque appel attendant le précédent ;
- **direct** : un message handler avec réponse (`window.webkit.messageHandlers.wfecho.postMessage`) et
  `webkit_web_view_evaluate_javascript`, enregistrés par le benchmark sur la vue de l'adaptateur avant la navigation ;
- « concurrent » : 64 appels JavaScript → C++ en vol, ce qui mesure le débit plutôt qu'un aller-retour.

Chaque valeur est la médiane, sur 5 exécutions précédées d'un échauffement, du temps moyen par appel, puis la médiane
de trois lancements du programme. Machine : AMD Ryzen 9 3900X (24 threads), Linux 7.0, GCC 15.2 en Release,
WebKitGTK 2.52.6, Xvfb avec rendu logiciel (`GSK_RENDERER=cairo`), comme en CI. La résolution de `performance.now()`
de WebKit est d'environ une milliseconde ; chaque lot dure au moins 80 ms.

| Sens | Mode | Octets | Appels | HTML (µs/appel) | Direct (µs/appel) | HTML/direct |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| JS → C++ | séquentiel | 16 | 1000 | 128 | 81 | 1,6 |
| JS → C++ | concurrent | 16 | 1000 | 63 | 12 | 5,3 |
| C++ → JS | séquentiel | 16 | 1000 | 178 | 69 | 2,6 |
| JS → C++ | séquentiel | 1024 | 1000 | 145 | 97 | 1,5 |
| JS → C++ | concurrent | 1024 | 1000 | 72 | 17 | 4,2 |
| C++ → JS | séquentiel | 1024 | 1000 | 189 | 75 | 2,5 |
| JS → C++ | séquentiel | 32768 | 200 | 705 | 420 | 1,7 |
| JS → C++ | concurrent | 32768 | 200 | 460 | 120 | 3,8 |
| C++ → JS | séquentiel | 32768 | 200 | 860 | 370 | 2,3 |

Le premier appel possible survient 8 à 9 ms après le début de la navigation par le bridge (connexion WebSocket
établie à 7 ou 8 ms), 6 à 7 ms par le canal direct. Dans les deux cas la page elle-même vient du serveur HTTP : ce
chiffre ne mesure pas un démarrage sans serveur, que seul un schéma de ressources natif permettrait.

Lecture :

- Le chemin HTML ajoute de 50 à 115 µs par aller-retour séquentiel jusqu'à 1 Kio (300 à 500 µs à 32 Kio) et divise
  le débit concurrent par 3,8 à 5,3. Pour une
  interface (une image à 60 Hz dure 16,7 ms), les deux chemins tiennent largement le budget : **la performance seule
  ne justifie pas d'abandonner le HTML sur poste de travail**. Le canal direct se justifie par les contraintes de
  plateforme et d'exposition ci-dessus, et pour des flux à haut débit.
- L'écart mesuré majore le gain d'un futur transport direct : le benchmark répond sur le thread GTK, sans le codec
  de l'ADR-003 ni le passage au contexte d'exécution du serveur qu'un vrai transport ajoutera. Ces deux coûts sont à
  mesurer avec le même benchmark lors de l'implémentation.
- Le benchmark a révélé deux coûts dans le chemin chaud du bridge, corrigés avec des tests de régression : chaque
  encodage de paramètre écrivait sur `std::cout` (`Messages.hpp`), et `Frame::addBuffer` formatait un dump
  hexadécimal de chaque tampon envoyé avant que `log::debug()` ne teste le niveau (21,7 Mo alloués pour un tampon de
  1 Mio). Avant correction, l'écho de 32 Kio coûtait 2380 µs (JS → C++) et 2528 µs (C++ → JS) par le chemin HTML,
  soit 6 fois le chemin direct.
- Non mesurés : CEF, WebView2, WKWebView, Android et iOS. Aucune extrapolation n'est faite ; ces mesures font partie
  des critères d'acceptation.

## Décision

### Le chemin HTML reste normatif

HTTP pour les ressources et WebSocket pour les messages restent le chemin par défaut, complet et testé de tous les
frontends, y compris d'un frontend qui offre un accès direct. Le navigateur système, les clients distants et les
futures cibles micro-contrôleurs n'utilisent que lui. Le cœur (`include/webfront/` hors `frontend/`) n'acquiert
aucune dépendance à un transport direct, et le budget de ressources de l'[ADR-016](ADR-016-bootstrap-comme-socle.md)
ne grossit pas pour lui.

### L'accès direct est une capacité facultative de l'adaptateur

Un adaptateur peut offrir deux capacités, indépendantes et facultatives :

1. **Ressources directes** : servir le même système de fichiers composé (`fs::Multi`, [ADR-002](ADR-002-systemes-de-fichiers-composables.md))
   par le schéma ou l'hôte virtuel natif du moteur, sans socket. Les règles de premier fournisseur gagnant, de
   `WebFront.js` non surchargeable et de page de repli s'appliquent à l'identique.
2. **Messages directs** : transporter les messages binaires de l'ADR-003, octet pour octet, par le canal natif du
   moteur. Le transport n'est qu'un porteur d'octets : `Messages.hpp` et le codec de `WebFront.js` restent les seuls
   codecs. Les octets passent en `ArrayBuffer` ou tableau typé quand le moteur le permet, sinon en base64, choix
   mesuré par adaptateur. Aucun format JSON ni objet natif propre au moteur n'est introduit.

### Une interface de transport sous `WebLink` et dans `WebFront.js`

Côté C++, `WebLink` ne dépendra que d'une interface de transport (envoyer des octets, recevoir un message, fermer,
signaler une déconnexion), dont le WebSocket actuel devient une implémentation. Côté JavaScript, `WebFront.js`
choisira le transport direct lorsqu'un objet de canal, injecté par l'adaptateur au début du document et avant tout
script de la page, est présent ; sinon le WebSocket. Le handshake et `webFront.ready` gardent leur sémantique.

Les contrats ne changent pas avec le transport : Promise systématique côté JavaScript, appel non typé sans retour et
`std::future`/sender typés côté C++, échec en cas de déconnexion ([ADR-004](ADR-004-appels-asynchrones-et-erreurs.md)) ;
tâches possédées, annulées et jointes ([ADR-006](ADR-006-taches-structurees-et-arret.md)) ; cycle de vie et action
de fin de session de l'ADR-007.

### Fils d'exécution

Les messages directs arrivent sur le thread du toolkit, en général le thread principal. Le transport les remet au
contexte d'exécution du serveur avant tout décodage applicatif ; les callbacks applicatifs ne s'exécutent jamais sur
le thread de l'interface sans décision explicite. Inversement, les envois C++ → JavaScript repassent par le thread
du toolkit lorsque le moteur l'exige (WebKitGTK, WKWebView, WebView2). Les règles de complétion de
[ASYNC_EXECUTION](../ASYNC_EXECUTION.md) restent celles du chemin HTML.

### Origines

Un canal direct n'est accepté que du document principal de l'origine de l'application, avec le filtrage natif du
moteur (règles d'origine d'Android, source de WebView2, cadre principal de WebKit) ; une origine refusée échoue comme
une connexion WebSocket refusée. Comme l'ADR-008, cette restriction n'est pas un bac à sable.

### Mobile

Les futurs frontends Android et iOS seront d'abord directs (ressources et messages) ; le serveur HTTP y reste
disponible sur option, pour le débogage par un navigateur ou l'accès distant. Ils satisfont `FrontendType` et
héritent des scénarios de cycle de vie partagés.

## Critères d'acceptation

Chacun fera l'objet d'une issue dédiée :

1. L'interface de transport existe dans `WebLink` et `WebFront.js` ; le WebSocket en est une implémentation et toutes
   les suites existantes passent sans modification de leurs attentes.
2. Un premier transport direct (WebKitGTK ou WKWebView) passe la suite Jasmine partagée par le canal direct, et le
   benchmark mesure ce transport complet (codec et changement de thread compris) face au chemin HTML.
3. Les mesures CEF, WebView2 et WKWebView sont ajoutées à cette ADR avec leur configuration.
4. Un prototype Android ou iOS charge ses ressources et échange ses messages sans serveur local.

## Options écartées et motifs

- **Remplacer le HTML par l'accès direct.** Perd le navigateur système, les clients distants et les micro-contrôleurs,
  et ferait dépendre le cœur de chaque moteur.
- **Garder le serveur local comme seule voie sur mobile.** Exige des exceptions de sécurité réseau, subit la
  suspension en arrière-plan et expose un port aux autres applications de l'appareil.
- **Un format propre au canal direct (JSON, objets natifs).** Crée un second codec par moteur, diverge de l'ADR-003
  et perd les tableaux typés et les bitsets compacts ; les mesures ne montrent aucun besoin qui le justifie.
- **Exposer à l'application les API natives des moteurs.** Rompt la portabilité du code applicatif et le contrat
  `FrontendType` ; l'accès direct reste interne à l'adaptateur.
- **Un canal synchrone (interfaces JavaScript bloquantes).** Contredit l'asynchronisme de l'ADR-004 et bloquerait le
  thread de rendu.

## Conséquences

- **Bénéfice :** des frontends mobiles sans serveur local, un canal non exposé aux autres processus et un débit
  concurrent de 3,8 à 5,3 fois supérieur (mesuré sur WebKitGTK), sans changer le code applicatif ni le protocole.
- **Bénéfice immédiat :** le chemin HTML, qui reste celui de tous, est plus rapide (32 Kio : de 2380 à 705 µs).
- **Coût accepté :** deux transports à tester pour chaque moteur direct ; la suite Jasmine et les scénarios de cycle
  de vie devront s'exécuter sur chacun.
- **Risque :** divergence de comportement entre transports (ordre des messages, déconnexion, taille maximale des
  messages du moteur). Les contrats sont exprimés au niveau de l'interface de transport et prouvés par les mêmes
  suites sur les deux chemins.

## Prescriptions d'implémentation et de génération de code

- Ne jamais rendre le chemin HTML facultatif, ni faire dépendre le cœur d'un moteur ou d'un transport direct.
- Ne pas introduire de codec, de type filaire ni de sémantique d'appel propre à un transport ; toute évolution du
  format suit l'ADR-003 sur tous les transports à la fois.
- Remettre les messages directs au contexte d'exécution du serveur ; ne pas exécuter de callback applicatif sur le
  thread du toolkit.
- Filtrer le canal direct par origine et cadre principal ; ne pas présenter ce filtrage comme un bac à sable.
- Mesurer avec `WebKitGTKTransportBenchmark` (ou son équivalent pour le moteur) avant et après tout changement du
  chemin chaud ; un benchmark échoue sur une réponse incorrecte, jamais sur un seuil de temps.
- Ne pas formater de journal coûteux sans tester d'abord son niveau (`log::is`) sur le chemin des messages.

## Sources et vérification

- Benchmark : [WebKitGTKTransportBenchmark.cpp](../../test/web/WebKitGTKTransportBenchmark.cpp),
  [transport-benchmark.html](../../test/web/transport-benchmark.html), enregistrement dans
  [WebKitGTKSupport.cmake](../../cmake/WebKitGTKSupport.cmake) ; exécution :
  `ctest --test-dir <build> -R WebKitGTKTransportBenchmark --output-on-failure` dans une configuration
  `-DWEBFRONT_EMBED_WEBKITGTK=ON -DENABLE_TESTING=ON` ; le rapport JSON est écrit dans le répertoire de build de
  `test/web`.
- Régressions du chemin chaud : `Encoding bridge messages writes nothing to the standard output`
  ([MessagesTests](../../test/unit/MessagesTests.cpp)) et `FrameBufferNotFormattedWhenDebugDisabled`
  ([FrameAllocationTest](../../test/unit/FrameAllocationTest.cpp)).
- Voir [FRONTENDS](../FRONTENDS.md), l'ADR-003, l'ADR-004, l'ADR-007 et l'ADR-008.
