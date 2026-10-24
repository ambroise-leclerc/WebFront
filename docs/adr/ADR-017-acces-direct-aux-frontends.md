# ADR-017 : Accéder aux frontends par leur API directe sans détrôner le chemin HTML

## Versions

| Version | Date | État | Auteur | Évolution | Révision Git |
| --- | --- | --- | --- | --- | --- |
| 1.0 | 2026-10-24 | Proposée | Ambroise Leclerc | Décision initiale (#363) : transport direct facultatif par adaptateur, format de l'ADR-003 inchangé, HTML normatif ; premières mesures WebKitGTK et WebView2 (dont un tampon partagé), évaluation du défaut par plateforme et correction de deux coûts du chemin chaud du bridge. | À renseigner après intégration. |

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

## Mesures (WebKitGTK et WebView2, 2026-10-24)

Le benchmark `test/web/WebKitGTKTransportBenchmark.cpp` (CTest `WebKitGTKTransportBenchmark`, labels
`web-integration` et `performance`) ouvre une seule fenêtre WebKitGTK servie par WebFront et exécute les mêmes
échos de chaînes par les deux chemins (`test/web/transport-benchmark.html`) :

- **HTML** : `webFront.cppFunction("echo")` pour JavaScript → C++, `ui.jsFunctionSender<std::string>` pour
  C++ → JavaScript, chaque appel attendant le précédent ;
- **direct** : un message handler avec réponse (`window.webkit.messageHandlers.wfecho.postMessage`) et
  `webkit_web_view_evaluate_javascript`, enregistrés par le benchmark sur la vue de l'adaptateur avant la navigation ;
- « concurrent » : 64 appels JavaScript → C++ en vol, ce qui mesure le débit plutôt qu'un aller-retour.

Chaque valeur est la médiane, sur 5 exécutions précédées d'un échauffement, du temps moyen par appel, puis la médiane
de trois lancements du programme (`ctest -R WebKitGTKTransportBenchmark` trois fois, en conservant chaque rapport JSON).
Machine : AMD Ryzen 9 3900X (24 threads), Linux 7.0, GCC 15.2 en Release, WebKitGTK 2.52.6, Xvfb avec rendu logiciel
(`GSK_RENDERER=cairo`), comme en CI. La résolution de `performance.now()` de WebKit est d'environ une milliseconde.
Ce tableau a été mesuré avec 1000 appels par lot (200 à 32 Kio) : les lots concurrents de 16 et 1024 octets duraient donc
12 à 17 ms, soit une incertitude de 6 à 8 % sur ces deux lignes. Le benchmark fait désormais 8000 appels (800 à 32 Kio)
et son rapport donne la durée du lot le plus court (`shortestBatchMilliseconds`) ; les mesures Linux sont à refaire avec
ces nombres sur la machine de référence avant l'acceptation de l'ADR.

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
- Non mesurés : CEF, WKWebView, Android et iOS. Aucune extrapolation n'est faite ; ces mesures font partie des
  critères d'acceptation. Les mesures WebView2 ci-dessous montrent qu'on ne peut pas extrapoler d'un moteur à l'autre.

### Mesures WebView2 (Windows)

`test/web/WebView2TransportBenchmark.cpp` (CTest `WebView2TransportBenchmark`, label `performance`) exécute la même page
dans une fenêtre WebView2, avec les mêmes échos. Le canal direct est enregistré sur le navigateur de l'adaptateur avant la
navigation (observateur interne `Session::viewReady`) :

- **direct** (chaînes) : `chrome.webview.postMessage` / `WebMessageReceived` / `PostWebMessageAsString` pour
  JavaScript → C++ (WebView2 n'a pas de réponse par appel : l'appel porte un identifiant) et `ExecuteScript` pour C++ → JavaScript ;
- **partagé** (octets) : un `ICoreWebView2SharedBuffer` de 128 Kio partagé avec la page (`PostSharedBufferToScript`),
  une moitié écrite par JavaScript, l'autre par C++ ; les messages ne disent que quelle moitié est prête. Séquentiel
  seulement : le tampon n'a qu'un propriétaire à la fois. Chaque côté vérifie tous les octets reçus.

Machine : CurmudgeonWindows, Windows 11 (10.0.22631), MSVC, Release, WebView2 Evergreen 155.0.4283.62, SDK 1.0.3537.50.
Même méthode : médiane de 5 exécutions, puis médiane de trois lancements du programme ; le lot le plus court a duré
82 à 86 ms.

| Sens | Mode | Octets | Appels | HTML (µs/appel) | Direct, chaînes | HTML/direct | Partagé | HTML/partagé |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| JS → C++ | séquentiel | 16 | 8000 | 180 | 91 | 2,0 | 92 | 2,0 |
| JS → C++ | concurrent | 16 | 8000 | 51 | 12 | 4,3 | — | — |
| C++ → JS | séquentiel | 16 | 8000 | 178 | 225 | 0,8 | 87 | 2,0 |
| JS → C++ | séquentiel | 1024 | 8000 | 192 | 106 | 1,8 | 94 | 2,0 |
| JS → C++ | concurrent | 1024 | 8000 | 52 | 21 | 2,5 | — | — |
| C++ → JS | séquentiel | 1024 | 8000 | 191 | 246 | 0,8 | 87 | 2,2 |
| JS → C++ | séquentiel | 32768 | 800 | 651 | 953 | 0,7 | 112 | 5,8 |
| JS → C++ | concurrent | 32768 | 800 | 344 | 484 | 0,7 | — | — |
| C++ → JS | séquentiel | 32768 | 800 | 672 | 1210 | 0,6 | 106 | 6,3 |

Le premier appel possible survient 90 à 109 ms après le début de la navigation par le bridge (connexion prête à 89 à
108 ms), 83 à 98 ms par le canal direct : ici encore la page vient du serveur HTTP.

Lecture :

- Sous WebView2, le canal de messages en chaînes **n'est pas plus rapide que le HTML** dès que les messages grossissent
  (JSON/UTF-16 aux deux extrémités, 1,4 à 1,8 fois plus lent à 32 Kio) ni pour C++ → JavaScript
  (`ExecuteScript` réanalyse un script à chaque appel). Il ne gagne que pour les petits appels JavaScript → C++
  (un facteur 2) et en débit concurrent de petits messages (2,5 à 4,3).
- Seul le tampon partagé, qui évite toute conversion par une chaîne, est partout plus rapide et presque indépendant de
  la taille (de 87 à 112 µs de 16 octets à 32 Kio), soit 2 fois le HTML pour les petits messages et 6 fois à 32 Kio. Un
  transport direct WebView2 doit donc être conçu autour de lui, pas autour de `PostWebMessageAsString`. Ce
  benchmark mesure l'échange d'octets, pas encore le codec ni le changement de thread (voir la lecture WebKitGTK).
- À l'inverse, WebKitGTK gagne même avec des chaînes. Le gain d'un canal direct dépend donc du moteur, de la
  forme du canal et de la taille, et se mesure par adaptateur.

## Autres possibilités des API natives

Hors des canaux de messages mesurés ci-dessus, les API des moteurs offrent des possibilités que le HTML sur boucle
locale n'a pas. Seule la première (WebView2) est mesurée ici ; les autres sont lues dans la documentation des moteurs et
restent à qualifier par un test avant d'être retenues :

| Possibilité | Moteurs | Intérêt pour WebFront | État |
| --- | --- | --- | --- |
| Mémoire partagée sans copie | WebView2 (`CreateSharedBuffer`, `PostSharedBufferToScript`) ; CEF (messages de processus à région de mémoire partagée, à confirmer pour la version épinglée) | Messages binaires sans chaîne intermédiaire : 6 fois le HTML à 32 Kio sous WebView2 | WebView2 mesuré |
| Messages `ArrayBuffer` | Android (`WebMessageCompat` de type `TYPE_ARRAY_BUFFER`, androidx.webkit 1.8.0 et `WEB_MESSAGE_ARRAY_BUFFER`) ; CEF (`CefV8Value::CreateArrayBuffer`) | Porte les octets de l'ADR-003 sans base64 | à qualifier |
| Script injecté au début du document | WebView2 (`AddScriptToExecuteOnDocumentCreated`), WebKitGTK (`WebKitUserScript`), WKWebView (`WKUserScript`) | Fournit l'objet de canal avant tout script de la page, comme l'exige la décision plus bas | à qualifier |
| Réponse par appel | WebKitGTK 2.40+ et WKWebView (message handler avec réponse) | Un appel JavaScript → C++ n'a pas à gérer lui-même ses identifiants ; WebView2 n'en a pas (mesuré avec un identifiant) | WebKitGTK mesuré |
| Réponses de ressources calculées | `WebResourceRequested` (WebView2), `WKURLSchemeHandler`, `CefSchemeHandlerFactory`, `webkit_web_context_register_uri_scheme`, `shouldInterceptRequest` | Sert `fs::Multi` sans socket, avec les mêmes règles de premier fournisseur | à qualifier |
| Hôte virtuel sur dossier | WebView2 (`SetVirtualHostNameToFolderMapping`) | Plus simple, mais contourne `fs::Multi` : réservé au développement | écarté pour la production |
| Objets hôtes synchrones | WebView2 (`AddHostObjectToScript`) | Aucun : contredit l'asynchronisme de l'[ADR-004](ADR-004-appels-asynchrones-et-erreurs.md) | écarté |

Conclusion de cette comparaison : les gains durables viennent des canaux binaires (tampon partagé, `ArrayBuffer`) et des
ressources servies sans socket, pas du simple remplacement du WebSocket par un canal de chaînes.

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

### L'accès direct par défaut des frontends natifs ?

Question posée : puisqu'un frontend natif n'a pas besoin d'un serveur, l'accès direct ne devrait-il pas être son défaut,
ce qui éviterait les autorisations réseau ? Évaluation, d'après les mesures ci-dessus et la documentation :

- **Autorisations évitées.** Un socket d'écoute, même sur la boucle locale, demande `android.permission.INTERNET` à une
  application Android, l'exception de trafic en clair (`network_security_config`) à partir de l'API 28 et, sur iOS,
  l'exception `NSAllowsLocalNetworking` ; l'application est en outre suspendue en arrière-plan. Sur poste de travail,
  un écouteur lié à `127.0.0.1` ne déclenche en pratique pas l'invite du pare-feu Windows (qui concerne les écouteurs
  ouverts à toutes les interfaces), mais un antivirus, une stratégie d'entreprise ou un autre processus de l'utilisateur
  peuvent encore le voir ; le port par défaut `9002` entre aussi en conflit entre deux applications. Ces points sont à
  vérifier sur chaque plateforme cible et non démontrés par ce benchmark.
- **Performance.** Elle ne tranche pas : le canal de chaînes de WebView2 est plus lent que le HTML, celui de WebKitGTK plus
  rapide. Elle ne justifie un défaut direct que si le transport est conçu autour des canaux binaires.
- **Outils.** L'environnement de développement (terminal, chargement des modules, navigateur système) a besoin du
  serveur HTTP ; le chemin HTML ne peut donc pas disparaître d'un projet de bureau, et un défaut direct ne vaut que pour
  la version distribuée.

Recommandation, sans modifier l'état « Proposée » :

| Cible | Défaut recommandé | Condition |
| --- | --- | --- |
| Android, iOS | direct | Contrainte de plateforme, pas de choix de performance. |
| WebView2, WKWebView, WebKitGTK, CEF (distribution embarquée) | HTML jusqu'à preuve, puis direct | Critères 1 à 3 ci-dessous remplis, mesures codec et changement de thread comprises, sans régression de la suite Jasmine ; sous WebView2 seulement avec le tampon partagé. |
| Navigateur système, clients distants, micro-contrôleurs | HTML | Seul chemin existant. |

Dans tous les cas, le chemin HTML reste disponible par une option explicite, et le choix du défaut est une décision
d'adaptateur qui ne change ni l'API applicative ni le format.

## Critères d'acceptation

Chacun fera l'objet d'une issue dédiée :

1. L'interface de transport existe dans `WebLink` et `WebFront.js` ; le WebSocket en est une implémentation et toutes
   les suites existantes passent sans modification de leurs attentes.
2. Un premier transport direct (WebKitGTK ou WKWebView) passe la suite Jasmine partagée par le canal direct, et le
   benchmark mesure ce transport complet (codec et changement de thread compris) face au chemin HTML.
3. Les mesures CEF et WKWebView sont ajoutées à cette ADR avec leur configuration (WebKitGTK et WebView2 sont mesurés,
   les chiffres Linux à refaire avec les nombres d'appels actuels), et le défaut de chaque adaptateur est arrêté d'après elles.
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
  `test/web`. La partie commune est dans [TransportBenchmark.hpp](../../test/web/TransportBenchmark.hpp).
- Benchmark WebView2 : [WebView2TransportBenchmark.cpp](../../test/web/WebView2TransportBenchmark.cpp), enregistrement
  dans [WebView2Support.cmake](../../cmake/WebView2Support.cmake) ; exécution :
  `ctest --test-dir <build> -R WebView2TransportBenchmark --output-on-failure` dans une configuration
  `-DWEBFRONT_EMBED_WEBVIEW2=ON -DENABLE_TESTING=ON` sous Windows, avec le runtime Evergreen.
- Régressions du chemin chaud : `Encoding bridge messages writes nothing to the standard output`
  ([MessagesTests](../../test/unit/MessagesTests.cpp)) et `FrameBufferNotFormattedWhenDebugDisabled`
  ([FrameAllocationTest](../../test/unit/FrameAllocationTest.cpp)).
- Voir [FRONTENDS](../FRONTENDS.md), l'ADR-003, l'ADR-004, l'ADR-007 et l'ADR-008.
